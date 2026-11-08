// The write-back cache (ADR 0011-0015): counters kept in memory, postings
// written to the database in batches by one background thread.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cell.hpp"
#include "codec.hpp"
#include "error.hpp"
#include "pool.hpp"
#include "shape.hpp"

namespace pg {

// A posting given up for good: why, and one line per insert and per delta, so
// that it can be booked by hand.
struct lost_posting {
    std::string reason;
    error_kind kind = error_kind::other;
    std::vector<std::string> description;
};

struct writeback_options {
    std::chrono::milliseconds interval{200};           // a batch is written this often
    std::size_t backlog_limit = 100'000;                // postings recorded but not settled
    std::chrono::milliseconds shutdown_timeout{10'000};  // the destructor's flush gives up after this
    std::chrono::milliseconds full_reconcile{60'000};   // full reconcile of watched counters
    std::function<void(const lost_posting&)> on_lost;   // runs on the writer thread; empty: stderr
    // The writer and the listener hold connections of their own that need the
    // server itself (LISTEN, session settings, backend pids); empty: the pool's
    // DSN. Set it when the pool goes through a transaction pooler.
    std::string direct_dsn;
};

struct writeback_stats {
    std::size_t backlog = 0;           // postings recorded but not settled
    std::uint64_t committed = 0;       // postings settled as committed
    std::uint64_t lost = 0;            // postings handed to on_lost
    std::uint64_t replays = 0;         // batches sent again after a replay disposition
    std::uint64_t reconciled = 0;      // slots reconciled because a notification named them
    std::uint64_t full_reconciles = 0; // full reconciles done
    std::uint64_t ignored = 0;         // notifications of this writeback's own writer, skipped
};

class writeback;

namespace detail {

template <class T>
inline constexpr int scale_v = -1;
template <int S>
inline constexpr int scale_v<decimal<S>> = S;

// A counter's value type: an integer or a fixed-point decimal.
template <class T>
concept counter_value = (std::is_integral_v<T> && !std::is_same_v<T, bool>) || is_decimal<T>;

template <class T>
std::int64_t to_units(const T& v) {
    if constexpr (is_decimal<T>) return v.units;
    else return static_cast<std::int64_t>(v);
}

template <class T>
T from_units(std::int64_t u) {
    if constexpr (is_decimal<T>) return T::from_units(u);
    else return static_cast<T>(u);
}

// A counter (column tag) without its C++ types: scale is S of decimal<S>, -1
// for an integer column.
struct counter_def {
    std::string_view table, column;
    std::span<const std::string_view> key_columns;
    int scale;
};

template <class Col>
inline constexpr counter_def counter_of{Col::pg_table::name, Col::pg_name,
                                        std::span<const std::string_view>(Col::pg_table::key_columns),
                                        scale_v<typename Col::pg_type>};

// The text form PostgreSQL gives a key value (`k::text`), which is also how
// notifications name keys.
template <class T>
std::string key_text(const T& v) {
    if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
    else if constexpr (std::is_integral_v<T>) return std::to_string(v);
    else if constexpr (std::is_convertible_v<const T&, std::string_view>) return std::string(std::string_view(v));
    else if constexpr (is_decimal<T> || std::is_same_v<T, uuid>) return v.to_string();
    else static_assert(sizeof(T) == 0, "writeback: unsupported key column type");
}

// Appends one key text, prefixed by its length (4 bytes, native byte order).
inline void append_key_text(std::string& out, std::string_view k) {
    const auto n = static_cast<std::uint32_t>(k.size());
    out.append(reinterpret_cast<const char*>(&n), sizeof n);
    out += k;
}

// A key's texts packed back to back, each prefixed by its length: the form
// slots are looked up by.
template <class Key, class... K, std::size_t... I>
std::string key_of(std::index_sequence<I...>, K&&... k) {
    std::string out;
    (append_key_text(out, key_text(static_cast<std::tuple_element_t<I, Key>>(std::forward<K>(k)))), ...);
    return out;
}

inline constexpr oid text_oid = 25;

// One array element of an inserted column: element type and bytes (none: NULL).
struct wb_element {
    oid type = text_oid;
    std::optional<std::string> bytes;
};

// One inserted row: the columns given (unset fields are left to DEFAULT).
struct wb_row {
    std::string_view table;
    std::vector<std::string_view> columns;
    std::vector<wb_element> values;
};

struct wb_delta {
    counter_def def;
    std::string key;  // packed by key_of
    std::int64_t units;
};

// Binary codecs send their own type; text ones send text, cast by the server.
template <class T>
wb_element element_of(const T& v) {
    using C = codec<std::decay_t<value_t<T>>>;
    static const oid type = C::param_binary ? C::param_oid : text_oid;
    [[maybe_unused]] static const oid known = array_oid(type);  // throws on every call until it succeeds
    wb_element e{type, std::nullopt};
    if constexpr (is_optional<std::remove_cvref_t<T>>) {
        if (v) C::encode(*v, e.bytes.emplace());
    } else {
        C::encode(v, e.bytes.emplace());
    }
    return e;
}

template <write_shape W>
wb_row row_of(const W& row) {
    wb_row r{W::pg_table::name, {}, {}};
    r.columns.reserve(W::pg_names.size());
    r.values.reserve(W::pg_names.size());
    std::size_t i = 0;
    row.pg_each([&](const auto& m) {
        const std::string_view name = W::pg_names[i++];
        using M = std::remove_cvref_t<decltype(m)>;
        if constexpr (is_field<M>) {
            if (!m.is_set()) return;
            r.columns.push_back(name);
            r.values.push_back(element_of(m.value()));
        } else {
            r.columns.push_back(name);
            r.values.push_back(element_of(m));
        }
    });
    return r;
}

// Test-only fault injection: the next batch the writer sends fails as if the
// connection were lost before sending it, or after the server committed it
// but before its result was read.
enum class wb_fault { none, lost_before_send, lost_after_commit, lost_after_copy };
void inject_fault(writeback& w, wb_fault f);

struct wb_state;

}  // namespace detail

template <class Col>
    requires detail::counter_value<typename Col::pg_type>
class cached;

// One atomic unit of write-back: rows to insert and deltas to counters, which
// commit together or are lost together. On a temporary, insert and add return
// it by value, moved, so that `submit(posting{}.insert(r).add<C>(d, k))` copies nothing.
class posting {
public:
    template <write_shape W>
    posting& insert(const W& row) & {
        rows_.push_back(detail::row_of(row));
        return *this;
    }
    template <write_shape W>
    posting insert(const W& row) && {
        insert(row);
        return std::move(*this);
    }

    template <class Col, class... K>
        requires detail::counter_value<typename Col::pg_type> && detail::key_args_for<typename Col::pg_table::key, K...>
    posting& add(typename Col::pg_type delta, K&&... key) & {
        deltas_.push_back({detail::counter_of<Col>,
                           detail::key_of<typename Col::pg_table::key>(std::index_sequence_for<K...>{}, std::forward<K>(key)...),
                           detail::to_units(delta)});
        return *this;
    }
    template <class Col, class... K>
        requires detail::counter_value<typename Col::pg_type> && detail::key_args_for<typename Col::pg_table::key, K...>
    posting add(typename Col::pg_type delta, K&&... key) && {
        add<Col>(delta, std::forward<K>(key)...);
        return std::move(*this);
    }

    bool empty() const { return rows_.empty() && deltas_.empty(); }

private:
    friend class writeback;
    template <class Col>
        requires detail::counter_value<typename Col::pg_type>
    friend class cached;

    posting& add_packed(const detail::counter_def& d, std::string key, std::int64_t units) & {
        deltas_.push_back({d, std::move(key), units});
        return *this;
    }

    std::vector<detail::wb_row> rows_;
    std::vector<detail::wb_delta> deltas_;
};

// Counters in memory over one pool. Each slot holds the committed value plus
// this process's recorded but unsettled deltas; one background thread writes
// postings in batches, a dedicated connection listens for changes of watched
// columns. on_lost must not call back into the same writeback.
class writeback {
public:
    explicit writeback(pool& p, writeback_options o = {});
    // Flushes for at most shutdown_timeout; postings still unsettled go to on_lost.
    ~writeback();
    writeback(const writeback&) = delete;
    writeback& operator=(const writeback&) = delete;

    // Reconciles Col's slots when the database announces changes to it; Col
    // must be a watched column (pg::migrate keeps its triggers).
    template <class Col>
    void watch() {
        watch_counter(detail::counter_of<Col>);
    }

    // The counter's value; a key not in memory is loaded (throws not_found
    // when the row does not exist).
    template <class Col, class... K>
        requires detail::counter_value<typename Col::pg_type> && detail::key_args_for<typename Col::pg_table::key, K...>
    typename Col::pg_type get(K&&... key) {
        return detail::from_units<typename Col::pg_type>(value(
            detail::counter_of<Col>,
            detail::key_of<typename Col::pg_table::key>(std::index_sequence_for<K...>{}, std::forward<K>(key)...)));
    }

    // Applies the posting's deltas to memory at once and queues it; over the
    // backlog limit throws backlog_full and records nothing.
    void submit(posting p);

    // Returns once every posting submitted before the call is settled.
    void flush();

    // Reconciles every loaded slot of every counter now.
    void reconcile();

    writeback_stats stats() const;

private:
    friend void detail::inject_fault(writeback&, detail::wb_fault);
    template <class Col>
        requires detail::counter_value<typename Col::pg_type>
    friend class cached;
    void watch_counter(const detail::counter_def& d);
    std::int64_t value(const detail::counter_def& d, const std::string& key);

    std::unique_ptr<detail::wb_state> st_;
};

// One cell of a write-back counter used like a variable: reads come from memory, `+=` and `-=`
// submit a posting with that single delta (see writeback::submit; may throw backlog_full).
// Postings that must commit together with inserted rows or other deltas use pg::posting.
template <class Col>
    requires detail::counter_value<typename Col::pg_type>
class cached {
public:
    using value_type = typename Col::pg_type;

    template <class... K>
        requires detail::key_args_for<typename Col::pg_table::key, K...>
    explicit cached(writeback& wb, K&&... key)
        : wb_(&wb),
          key_(detail::key_of<typename Col::pg_table::key>(std::index_sequence_for<K...>{}, std::forward<K>(key)...)) {}

    value_type get() const { return detail::from_units<value_type>(wb_->value(detail::counter_of<Col>, key_)); }
    operator value_type() const { return get(); }

    cached& operator+=(const value_type& d) { return add(detail::to_units(d)); }
    cached& operator-=(const value_type& d) { return add(-detail::to_units(d)); }

private:
    cached& add(std::int64_t units) {
        posting p;
        p.add_packed(detail::counter_of<Col>, key_, units);
        wb_->submit(std::move(p));
        return *this;
    }

    writeback* wb_;
    std::string key_;  // packed by detail::key_of
};

}  // namespace pg
