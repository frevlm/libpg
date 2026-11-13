// A write-back cache: counters kept in memory, postings written to the database
// in batches by one background thread.
#pragma once
#include <functional>

#include "pool.hpp"

namespace pg {

// A posting given up for good: why, and one line per insert and per delta, so
// that it can be booked by hand.
struct lost_posting { std::string reason; error_kind kind = error_kind::other; std::vector<std::string> description; };

struct writeback_options {
    std::chrono::milliseconds interval{200};             // a batch is written this often
    std::chrono::milliseconds refresh{1'000};            // loaded slots older than this are re-read
    std::size_t backlog_limit = 100'000;                 // postings recorded but not settled
    std::chrono::milliseconds shutdown_timeout{10'000};  // the destructor's flush gives up after this
    std::function<void(const lost_posting&)> on_lost;    // runs on the writer thread; empty: stderr
};

struct writeback_stats {
    std::size_t backlog = 0;                              // postings recorded but not settled
    std::uint64_t committed = 0, lost = 0, replays = 0;  // postings committed; handed to on_lost; batches sent again
};

class writeback;

namespace detail {

// How a counter's value type is held as int64 units; `scale` is S of decimal<S>, else 0.
template <class T> struct units;
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
struct units<T> {
    static constexpr int scale = 0;
    static std::int64_t to(T v) { return v; }
    static T from(std::int64_t u) { return static_cast<T>(u); }
};
template <int S>
struct units<decimal<S>> {
    static constexpr int scale = S;
    static std::int64_t to(decimal<S> v) { return v.units; }
    static decimal<S> from(std::int64_t u) { return decimal<S>::from_units(u); }
};
template <class T> concept counter_value = requires { units<T>::scale; };

// A counter column without its C++ types.
struct counter { std::string_view table, column; std::span<const std::string_view> keys; int scale; };
template <class Col>
inline const counter counter_of{Col::pg_table::name, Col::pg_name, std::span<const std::string_view>(Col::pg_table::key_columns),
                                units<typename Col::pg_type>::scale};

// A counter column and as many key arguments as its table's key has columns.
template <class Col, class... A>
concept counter_for = counter_value<typename Col::pg_type> && std::tuple_size_v<typename Col::pg_table::key> == sizeof...(A);

// A key: its counter's address, then each value as its type OID, its length and its binary bytes.
std::string pack_param(std::string out, const param& p);
template <class Col, class... K, std::size_t... I>
std::string pack_key(std::index_sequence<I...>, K&&... k) {
    const counter* d = &counter_of<Col>;
    std::string out(reinterpret_cast<const char*>(&d), sizeof d);
    ((out = pack_param(std::move(out), to_param(static_cast<std::tuple_element_t<I, typename Col::pg_table::key>>(std::forward<K>(k))))), ...);
    return out;
}
template <class Col, class... K>
std::string key_of(K&&... k) { return pack_key<Col>(std::index_sequence_for<K...>{}, std::forward<K>(k)...); }

// One row to insert: the columns given (an unset field is left to DEFAULT) and their values.
struct wb_row { std::string_view table; std::vector<std::string_view> columns; std::vector<param> values; };
struct wb_delta { const counter* def; std::string key; std::int64_t units; };  // key: packed by key_of

template <write_shape W>
wb_row row_of(const W& row) {
    wb_row r{W::pg_table::name, {}, {}};
    std::size_t i = 0;
    row.pg_each([&](const auto& m) {
        const std::string_view name = W::pg_names[i++];
        if constexpr (is_field<std::remove_cvref_t<decltype(m)>>) {
            if (!m.is_set()) return;
            r.values.push_back(to_param(m.value()));
        } else {
            r.values.push_back(to_param(m));
        }
        r.columns.push_back(name);
    });
    if (r.columns.empty()) throw error(error_kind::other, "posting::insert: no column to insert");
    return r;
}

// Test-only: the next batch fails as if the connection were lost before it was
// sent (1) or after the server committed it (2).
void inject_fault(writeback& w, int fault);

struct wb_state;

}  // namespace detail

// One atomic unit of write-back: rows to insert and deltas to counters, which
// commit together or are lost together. On a temporary, insert and add return
// it by value, moved, so that `submit(posting{}.insert(r).add<C>(d, k))` copies nothing.
class posting {
public:
    template <write_shape W> posting& insert(const W& row) & { return rows_.push_back(detail::row_of(row)), *this; }
    template <write_shape W> posting insert(const W& row) && { return std::move(insert(row)); }

    template <class Col, class... K>
        requires detail::counter_for<Col, K...>
    posting& add(typename Col::pg_type delta, K&&... key) & {
        deltas_.push_back({&detail::counter_of<Col>, detail::key_of<Col>(std::forward<K>(key)...), detail::units<typename Col::pg_type>::to(delta)});
        return *this;
    }
    template <class Col, class... K>
        requires detail::counter_for<Col, K...>
    posting add(typename Col::pg_type delta, K&&... key) && {
        return std::move(add<Col>(delta, std::forward<K>(key)...));
    }

    bool empty() const { return rows_.empty() && deltas_.empty(); }

private:
    friend struct detail::wb_state;
    std::vector<detail::wb_row> rows_;
    std::vector<detail::wb_delta> deltas_;
};

// Counters in memory over one pool. A slot holds the database value as last
// read plus this process's recorded but unsettled deltas. on_lost must not call
// back into the same writeback.
class writeback {
public:
    explicit writeback(pool& p, writeback_options o = {});
    // Flushes for at most shutdown_timeout; postings still unsettled go to on_lost.
    ~writeback();
    writeback(const writeback&) = delete;
    writeback& operator=(const writeback&) = delete;

    // The counter's value; a key not in memory is loaded with one SELECT (throws
    // not_found when the row does not exist). Throws nested_borrow when the
    // calling thread holds a conn of the pool, as inside pool::transact, whether
    // the key is in memory or not.
    template <class Col, class... K>
        requires detail::counter_for<Col, K...>
    typename Col::pg_type get(K&&... key) {
        return detail::units<typename Col::pg_type>::from(value(detail::counter_of<Col>, detail::key_of<Col>(std::forward<K>(key)...)));
    }

    // Applies the posting's deltas to memory at once and queues it; over the
    // backlog limit throws backlog_full and records nothing.
    void submit(posting p);

    // Returns once every posting submitted before the call is settled.
    void flush();

    writeback_stats stats() const;

private:
    friend void detail::inject_fault(writeback&, int);
    std::int64_t value(const detail::counter& d, const std::string& key);

    std::unique_ptr<detail::wb_state> st_;
};

}  // namespace pg
