#pragma once
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "sql.hpp"

namespace pg {

class conn;

// A write-shape member for a column with a DEFAULT: unset writes DEFAULT, a
// value writes that value (std::optional<T> as T can write NULL).
template <class T>
class field {
public:
    field() = default;
    field(T v) : v_(std::move(v)) {}
    field& operator=(T v) {
        v_ = std::move(v);
        return *this;
    }
    bool is_set() const { return v_.has_value(); }
    const T& value() const { return *v_; }
    void reset() { v_.reset(); }

private:
    std::optional<T> v_;
};

namespace detail {
template <class T> inline constexpr bool is_field = false;
template <class T> inline constexpr bool is_field<field<T>> = true;
// Whether a write-shape member writes a value (an unset field writes DEFAULT), and that value.
template <class T> bool is_set(const T&) { return true; }
template <class T> bool is_set(const field<T>& f) { return f.is_set(); }
template <class T> const T& written(const T& v) { return v; }
template <class T> const T& written(const field<T>& f) { return f.value(); }

std::int64_t exec_on(conn& c, const sql& q);

// A column insert_all writes: its name, what its member type reads, that type's name.
struct copy_column {
    std::string_view name;
    bool (*accepts)(oid);
    std::string_view type;
    bool operator==(const copy_column&) const = default;
};
// Checks the types of table's columns `cols` (as the server describes SELECT
// "a", "b" FROM "t"), then runs COPY "t" ("a", "b") FROM STDIN (FORMAT binary)
// with `data` as the stream; returns the rows copied.
std::int64_t copy_on(conn& c, std::string_view table, std::span<const copy_column> cols, const std::string& data);

template <class T>
constexpr std::string_view type_name() {
    std::string_view p = __PRETTY_FUNCTION__;
    p.remove_prefix(p.find("T = ") + 4);
    return p.substr(0, p.find_first_of(";]"));
}
}  // namespace detail

// A struct one result row decodes into:
//   static constexpr std::size_t pg_columns = N;
//   static constexpr std::array<std::string_view, N> pg_names;  // the result column names
//   template <class F> void pg_each(F&& f);                    // f(member) in column order
// A result whose column names differ from pg_names is rejected before decoding.
template <class T>
concept read_shape = std::is_default_constructible_v<T> && requires(T& t) {
    { T::pg_columns } -> std::convertible_to<std::size_t>;
    requires std::same_as<std::remove_cv_t<decltype(T::pg_names)>, std::array<std::string_view, T::pg_columns>>;
    t.pg_each([](auto&) {});
};

// A struct that is one row to INSERT into its table:
//   using pg_table = <table tag>;                               // pg_table::name
//   static constexpr std::array<std::string_view, N> pg_names;  // column names
//   template <class F> void pg_each(F&& f) const;              // f(member) in pg_names order
// Members are T, std::optional<T> (NULL when empty) or pg::field<T> (DEFAULT when unset).
template <class T>
concept write_shape = requires(const T& t) {
    { T::pg_table::name } -> std::convertible_to<std::string_view>;
    { T::pg_names.size() } -> std::convertible_to<std::size_t>;
    t.pg_each([](const auto&) {});
};

// INSERT INTO "t" ("a", ...) VALUES ($1, DEFAULT, ...), or DEFAULT VALUES for a
// shape without columns. Returns the number of rows inserted.
template <write_shape W>
std::int64_t insert(conn& c, const W& row) {
    sql cols, values;
    std::size_t i = 0;
    row.pg_each([&](const auto& m) {
        const sql sep = i ? sql{", "} : sql{};
        cols += sep + ident(W::pg_names[i++]);
        values += sep + (detail::is_set(m) ? sql{"{}", detail::written(m)} : sql{"DEFAULT"});
    });
    if (i == 0) return detail::exec_on(c, sql{"INSERT INTO {} DEFAULT VALUES", ident(W::pg_table::name)});
    return detail::exec_on(c, sql{"INSERT INTO {} ({}) VALUES ({})", ident(W::pg_table::name), cols, values});
}

// Inserts every row with one binary COPY "t" ("a", "b"), atomic on its own.
// A pg::field column is written iff it is set in the first row; a later row
// that disagrees throws, and so does a row set with no column at all. The
// member types are checked against the target columns (as described by the
// server for SELECT "a", "b" FROM "t") before any row is sent. An empty range
// returns 0 without a round trip.
template <std::ranges::contiguous_range R>
    requires write_shape<std::ranges::range_value_t<R>>
std::int64_t insert_all(conn& c, const R& rows) {
    using W = std::ranges::range_value_t<R>;
    const std::span<const W> all(rows);
    if (all.empty()) return 0;
    std::array<bool, W::pg_names.size()> take{};
    std::vector<detail::copy_column> cols;
    std::size_t i = 0;
    all[0].pg_each([&](const auto& m) {
        using V = std::remove_cvref_t<decltype(detail::written(m))>;
        if ((take[i] = detail::is_set(m))) cols.push_back({W::pg_names[i], &detail::codec_of<V>::accepts, detail::type_name<V>()});
        ++i;
    });
    detail::array_builder data;  // the COPY stream: header, then per row its field count and fields
    data.body.assign("PGCOPY\n\377\r\n\0\0\0\0\0\0\0\0\0", 19);
    for (std::size_t r = 0; r < all.size(); ++r) {
        detail::put_be(cols.size(), 2, data.body);
        i = 0;
        all[r].pg_each([&](const auto& m) {
            if (detail::is_set(m) != take[i])
                throw error(error_kind::other, "insert_all: row " + std::to_string(r) + " sets field " +
                                                   std::string(W::pg_names[i]) + " unlike the first row");
            if (take[i++]) data.add(detail::written(m));
        });
    }
    detail::put_be(0xffff, 2, data.body);
    return detail::copy_on(c, W::pg_table::name, cols, data.body);
}

}  // namespace pg
