#pragma once
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
// value writes that value (use std::optional<T> as T to be able to write NULL).
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
template <class T>
inline constexpr bool is_field = false;
template <class T>
inline constexpr bool is_field<field<T>> = true;

// The wire type of one column of a binary COPY: a domain replaced by its base type.
struct copy_target {
    oid type = 0;
    char category = 0;  // pg_type.typcategory
    char kind = 0;      // pg_type.typtype
};

inline constexpr oid json_oid = 114, jsonb_oid = 3802;

// Whether bytes in the wire form of type `sent` (text_oid, 25, for text codecs) can travel in a
// binary COPY of a column whose base type is `t`.
inline bool copy_accepts(oid sent, const copy_target& t) {
    if (sent == t.type) return true;
    if (sent != 25) return false;
    return t.category == 'S' || t.kind == 'e' || t.type == json_oid || t.type == jsonb_oid;
}

// The target types of the named columns of a table, cached per pool; empty when a
// column does not exist.
std::vector<copy_target> copy_targets(conn& c, std::string_view table, std::span<const std::string_view> names);
void forget_copy_targets(conn& c, std::string_view table, std::span<const std::string_view> names);

// Runs one COPY in a transaction of its own when the conn is not in one.
std::int64_t copy_rows(conn& c, std::string_view copy_sql, std::string_view body);

// Whether the conn is inside a caller's transaction.
bool in_transaction(conn& c);

// Sends every INSERT as one operation, in a transaction of its own when needed.
std::int64_t insert_rows(conn& c, std::span<const sql> rows);

// The member type with pg::field unwrapped (an optional is kept).
template <class M>
struct write_base {
    using type = std::remove_cvref_t<M>;
};
template <class T>
struct write_base<field<T>> {
    using type = T;
};

// The value type of a write member: T, optional<T> and field forms unwrapped to T.
template <class M>
using write_value_t = value_t<typename write_base<std::remove_cvref_t<M>>::type>;

template <class V>
bool copy_encodable_one(const copy_target& t) {
    using C = codec<V>;
    return copy_accepts(C::param_binary ? C::param_oid : 25, t);
}

// Whether a write member of type M can travel in a binary COPY of the target type.
template <class M>
bool copy_encodable(const copy_target& t) {
    return copy_encodable_one<write_value_t<M>>(t);
}

inline void copy_i16(std::int16_t v, std::string& out) {
    const auto u = static_cast<std::uint16_t>(v);
    out.push_back(static_cast<char>(u >> 8));
    out.push_back(static_cast<char>(u & 0xff));
}

inline void copy_i32(std::int32_t v, std::string& out) {
    const auto u = static_cast<std::uint32_t>(v);
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((u >> s) & 0xff));
}

// The binary COPY body's signature, flags and header extension.
inline void put_copy_header(std::string& out) {
    out.append("PGCOPY\n\377\r\n\0", 11);
    out.append(8, '\0');
}

// The trailer that ends a binary COPY body.
inline void put_copy_trailer(std::string& out) { copy_i16(-1, out); }

// `COPY "table" ("a", "b") FROM STDIN (FORMAT binary)`.
inline std::string copy_statement(std::string_view table, std::span<const std::string_view> names) {
    std::string cols;
    for (std::size_t k = 0; k < names.size(); ++k) {
        if (k) cols += ", ";
        cols += quote_ident(names[k]);
    }
    return "COPY " + quote_ident(table) + " (" + cols + ") FROM STDIN (FORMAT binary)";
}

// Appends one length-prefixed COPY field.
template <class V>
void put_copy_value(const V& v, const copy_target& t, std::string& out) {
    using C = codec<V>;
    const std::size_t at = out.size();
    copy_i32(0, out);
    if (C::param_binary) C::encode(v, out);
    else {
        if (t.type == jsonb_oid) out.push_back('\x01');
        C::encode(v, out);
    }
    const auto n = static_cast<std::uint32_t>(out.size() - at - 4);
    out[at] = static_cast<char>((n >> 24) & 0xff);
    out[at + 1] = static_cast<char>((n >> 16) & 0xff);
    out[at + 2] = static_cast<char>((n >> 8) & 0xff);
    out[at + 3] = static_cast<char>(n & 0xff);
}

template <class B>
void put_copy_opt(const B& v, const copy_target& t, std::string& out) {
    if constexpr (is_optional<B>) {
        if (!v) {
            copy_i32(-1, out);
            return;
        }
        put_copy_value(*v, t, out);
    } else {
        put_copy_value(v, t, out);
    }
}

// Appends one row member as a COPY field; unset fields never reach here.
template <class M>
void put_copy_member(const M& m, const copy_target& t, std::string& out) {
    using B = typename write_base<std::remove_cvref_t<M>>::type;
    if constexpr (is_field<std::remove_cvref_t<M>>) put_copy_opt<B>(m.value(), t, out);
    else put_copy_opt<B>(m, t, out);
}
}  // namespace detail

// A struct one result row decodes into, column by column:
//   static constexpr std::size_t pg_columns = N;
//   template <class F> void pg_each(F&& f);   // f(member) for each member in column order
template <class T>
concept read_shape = std::is_default_constructible_v<T> && requires(T& t) {
    { T::pg_columns } -> std::convertible_to<std::size_t>;
    t.pg_each([](auto&) {});
};

// A struct that is one row to INSERT into its table:
//   using pg_table = <table tag>;                                // pg_table::name
//   static constexpr std::array<std::string_view, N> pg_names;  // column names
//   template <class F> void pg_each(F&& f) const;              // f(member) in pg_names order
// Members are T, std::optional<T> (NULL when empty) or pg::field<T> (DEFAULT when unset).
template <class T>
concept write_shape = requires(const T& t) {
    { T::pg_table::name } -> std::convertible_to<std::string_view>;
    { T::pg_names.size() } -> std::convertible_to<std::size_t>;
    t.pg_each([](const auto&) {});
};

class conn;

namespace detail {

// `INSERT INTO "table" ("a", "b") VALUES ($1, DEFAULT)` for the row;
// `INSERT INTO "table" DEFAULT VALUES` when the shape has no columns.
template <write_shape W>
sql insert_statement(const W& row) {
    if constexpr (W::pg_names.size() == 0) return sql{"INSERT INTO {} DEFAULT VALUES", ident(W::pg_table::name)};
    sql cols, values;
    std::size_t i = 0;
    row.pg_each([&](const auto& m) {
        const sql comma = i ? sql{", "} : sql{};
        cols += comma + ident(W::pg_names[i++]);
        values += comma;
        using M = std::remove_cvref_t<decltype(m)>;
        if constexpr (!is_field<M>) values += sql{"{}", m};
        else if (m.is_set()) values += sql{"{}", m.value()};
        else values += sql{"DEFAULT"};
    });
    return sql{"INSERT INTO {} ({}) VALUES ({})", ident(W::pg_table::name), cols, values};
}

std::int64_t exec_on(conn& c, const sql& q);

}  // namespace detail

// Inserts the row: unset fields take the column DEFAULT. Returns the number of
// rows inserted.
template <write_shape W>
std::int64_t insert(conn& c, const W& row) {
    return detail::exec_on(c, detail::insert_statement(row));
}

namespace detail {

// The columns a batch writes: a field set in the first row is taken; `uniform`
// is false when later rows disagree on which fields are set.
struct copy_columns {
    std::vector<char> take;  // the column goes into the COPY
    std::vector<std::string_view> names;
    bool uniform = true;
};

// Reads the field set-mask of the first row and checks every later row agrees.
template <write_shape W>
copy_columns columns_of(std::span<const W> rows) {
    constexpr std::size_t n = W::pg_names.size();
    copy_columns cols;
    cols.take.assign(n, 0);
    std::vector<char> is_field_member(n, 0), set_first(n, 0);
    std::size_t i = 0;
    rows.front().pg_each([&](const auto& m) {
        using M = std::remove_cvref_t<decltype(m)>;
        if constexpr (is_field<M>) {
            is_field_member[i] = true;
            set_first[i] = m.is_set();
        }
        ++i;
    });
    for (std::size_t r = 1; r < rows.size() && cols.uniform; ++r) {
        std::size_t j = 0;
        rows[r].pg_each([&](const auto& m) {
            using M = std::remove_cvref_t<decltype(m)>;
            if constexpr (is_field<M>) {
                if (is_field_member[j] && m.is_set() != set_first[j]) cols.uniform = false;
            }
            ++j;
        });
    }
    for (std::size_t k = 0; k < n; ++k) {
        cols.take[k] = !is_field_member[k] || set_first[k];
        if (cols.take[k]) cols.names.push_back(W::pg_names[k]);
    }
    return cols;
}

// Whether every taken member of the row encodes into its target type.
template <write_shape W>
bool all_encodable(const W& row, const copy_columns& cols, std::span<const copy_target> targets) {
    bool ok = true;
    std::size_t k = 0, col = 0;
    row.pg_each([&](const auto& m) {
        if (!cols.take[k++]) return;
        if (!copy_encodable<std::remove_cvref_t<decltype(m)>>(targets[col++])) ok = false;
    });
    return ok;
}

// One row of the binary COPY body: the column count, then every taken field.
template <write_shape W>
void put_copy_row(const W& row, const copy_columns& cols, std::span<const copy_target> targets, std::string& out) {
    copy_i16(static_cast<std::int16_t>(cols.names.size()), out);
    std::size_t k = 0, col = 0;
    row.pg_each([&](const auto& m) {
        if (cols.take[k++]) put_copy_member(m, targets[col++], out);
    });
}

// The binary COPY body of every row: the header, the rows, the trailer.
template <write_shape W>
std::string copy_body(std::span<const W> rows, const copy_columns& cols, std::span<const copy_target> targets) {
    std::string body;
    body.reserve(64 + rows.size() * (2 + 16 * targets.size()));
    put_copy_header(body);
    for (const W& row : rows) put_copy_row(row, cols, targets, body);
    put_copy_trailer(body);
    return body;
}

// The INSERT path: every row as one statement, sent as one operation.
template <write_shape W>
std::int64_t insert_each(conn& c, std::span<const W> rows) {
    std::vector<sql> stmts;
    stmts.reserve(rows.size());
    for (const W& row : rows) stmts.push_back(insert_statement(row));
    return insert_rows(c, stmts);
}

// Inserts every row with one binary COPY when each column can travel in binary
// form, else with pipelined INSERTs. Columns of pg::field members unset in
// every row take their DEFAULT; rows that differ in which fields are set go as
// INSERTs. An empty span inserts nothing.
template <write_shape W>
std::int64_t insert_all_span(conn& c, std::span<const W> rows) {
    if (rows.empty()) return 0;
    const copy_columns cols = columns_of(rows);
    if (!cols.uniform || cols.names.empty()) return insert_each(c, rows);
    const std::vector<copy_target> targets = copy_targets(c, W::pg_table::name, cols.names);
    if (targets.size() != cols.names.size() || !all_encodable(rows.front(), cols, targets))
        return insert_each(c, rows);
    const bool own = !in_transaction(c);
    try {
        return copy_rows(c, copy_statement(W::pg_table::name, cols.names), copy_body(rows, cols, targets));
    } catch (const error& e) {
        if (!own || e.sqlstate != "22P03") throw;
    }
    // The cached target types were wrong: forget them and INSERT instead.
    forget_copy_targets(c, W::pg_table::name, cols.names);
    return insert_each(c, rows);
}

}  // namespace detail

// Inserts every row of the range; see detail::insert_all_span. Atomic: outside
// a transaction it runs in one of its own.
template <std::ranges::contiguous_range R>
    requires write_shape<std::ranges::range_value_t<R>>
std::int64_t insert_all(conn& c, const R& rows) {
    return detail::insert_all_span(c, std::span<const std::ranges::range_value_t<R>>(rows));
}

}  // namespace pg
