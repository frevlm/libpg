#pragma once
#include <libpq-fe.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "shape.hpp"  // with sql.hpp, codec.hpp and error.hpp

namespace pg {

struct pool_options {
    // Session settings travel in the DSN: options=-c statement_timeout=30s -c synchronous_commit=off
    std::string dsn;
    int max_conns = 8;
    std::chrono::milliseconds acquire_timeout{30'000};  // wait for a free connection, then throw
    int transact_attempts = 3;
};

struct pool_stats {
    int open = 0;  // connections, idle or in use
    int idle = 0;
    int waiting = 0;            // borrowers waiting for a connection
    std::uint64_t borrows = 0;  // connections handed out, shared or held
};

class conn;

namespace detail {

struct wire;
struct prepared;
struct lot;

struct result_deleter {
    void operator()(PGresult* r) const { PQclear(r); }
};

// One round trip queued on a wire. Reading it fills its last result and its first error.
struct ticket {
    std::string key;  // the statement text, then NUL and the parameter type OIDs
    std::unique_ptr<PGresult, result_deleter> res;
    std::optional<error> err;
    bool done = false;
};

// Fills the ticket through its conn unless it is filled; throws its error.
const PGresult* await(conn* c, ticket& t);

// Checks result column types and names: why they do not fit, empty when they do.
using check_fn = std::string (*)(std::span<const oid> types, std::span<const std::string> names);

std::string count_error(std::size_t got, std::size_t want);
std::string column_error(std::size_t i, const std::string& name, oid type, std::string_view cxx_type);
[[noreturn]] void row_count_error(int rows, const std::string& key);
std::int64_t affected_of(const PGresult* r);
void backoff(int attempt);  // sleeps before attempt + 1 of pool::transact

template <class T>
std::string check_column(std::size_t i, oid type, const std::string& name) {
    return codec_of<T>::accepts(type) ? std::string() : column_error(i, name, type, type_name<T>());
}

template <class R>
std::string check_shape(std::span<const oid> types, std::span<const std::string> names) {
    if (types.size() != R::pg_columns) return count_error(types.size(), R::pg_columns);
    std::string bad;
    std::size_t i = 0;
    R probe{};
    probe.pg_each([&](auto& m) {
        if (bad.empty() && names[i] != R::pg_names[i])
            bad = "column " + std::to_string(i + 1) + " is \"" + names[i] + "\", the row shape expects \"" +
                  std::string(R::pg_names[i]) + "\"";
        if (bad.empty()) bad = check_column<std::remove_cvref_t<decltype(m)>>(i, types[i], names[i]);
        ++i;
    });
    return bad;
}

template <class... T>
std::string check_row(std::span<const oid> types, std::span<const std::string> names) {
    if constexpr (sizeof...(T) == 1 && (read_shape<T> && ...)) {
        return (check_shape<T>(types, names), ...);
    } else {
        if (types.size() != sizeof...(T)) return count_error(types.size(), sizeof...(T));
        std::string bad;
        std::size_t i = 0;
        ((bad = bad.empty() ? check_column<T>(i, types[i], names[i]) : bad, ++i), ...);
        return bad;
    }
}

template <class... T>
constexpr check_fn check_for() {
    if constexpr (sizeof...(T) == 0) return nullptr;
    else return &check_row<T...>;
}

template <class T>
T value_at(const PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) {
        if constexpr (is_optional<T>) return std::nullopt;
        else throw error(error_kind::other, "column " + std::to_string(col + 1) + " \"" + PQfname(r, col) +
                                                "\" is NULL; read it as std::optional");
    }
    return codec_of<T>::decode({PQgetvalue(r, row, col), static_cast<std::size_t>(PQgetlength(r, row, col))});
}

template <class R>
R shape_at(const PGresult* r, int row) {
    R out{};
    int col = 0;
    out.pg_each([&](auto& m) { m = value_at<std::remove_cvref_t<decltype(m)>>(r, row, col++); });
    return out;
}

template <class... T> struct row_type { using type = std::tuple<T...>; };
template <class T> struct row_type<T> { using type = T; };

}  // namespace detail

// A row: std::tuple<T...>, or T itself for one type (a read shape included).
template <class... T>
using row_t = typename detail::row_type<T...>::type;

namespace detail {

template <class... T>
row_t<T...> row_at(const PGresult* r, int row) {
    if constexpr (sizeof...(T) == 1 && (read_shape<T> && ...)) {
        return (shape_at<T>(r, row), ...);
    } else if constexpr (sizeof...(T) == 1) {
        return (value_at<T>(r, row, 0), ...);
    } else {
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return std::tuple<T...>{value_at<T>(r, row, static_cast<int>(I))...};
        }(std::index_sequence_for<T...>{});
    }
}

template <class... T>
std::vector<row_t<T...>> rows_of(const PGresult* r) {
    static_assert(sizeof...(T) > 0, "name the column types to read");
    std::vector<row_t<T...>> out;
    out.reserve(static_cast<std::size_t>(PQntuples(r)));
    for (int i = 0; i < PQntuples(r); ++i) out.push_back(row_at<T...>(r, i));
    return out;
}

template <class... T>
std::optional<row_t<T...>> opt_of(const PGresult* r, const std::string& key) {
    if (PQntuples(r) > 1) row_count_error(PQntuples(r), key);
    if (PQntuples(r) == 0) return std::nullopt;
    return row_at<T...>(r, 0);
}

template <class... T>
row_t<T...> one_of(const PGresult* r, const std::string& key) {
    if (PQntuples(r) != 1) row_count_error(PQntuples(r), key);
    return row_at<T...>(r, 0);
}

}  // namespace detail

// A statement sent and not yet read. Each reader may be called once.
template <class... T>
class pending {
public:
    std::vector<row_t<T...>> all() { return detail::rows_of<T...>(take()); }
    row_t<T...> one() { return detail::one_of<T...>(take(), t_->key); }
    std::optional<row_t<T...>> opt() { return detail::opt_of<T...>(take(), t_->key); }
    std::int64_t affected() { return detail::affected_of(take()); }

private:
    friend class conn;
    pending(std::shared_ptr<detail::ticket> t, conn* c) : t_(std::move(t)), c_(c) {}
    const PGresult* take() {
        if (taken_) throw error(error_kind::other, "a pending result is read once");
        taken_ = true;
        return detail::await(c_, *t_);
    }
    std::shared_ptr<detail::ticket> t_;
    conn* c_;
    bool taken_ = false;
};

// A user-owned set of connections to one database. It must outlive its conns.
class pool {
public:
    explicit pool(pool_options o);
    ~pool();
    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;

    pool_stats stats() const;
    const pool_options& options() const { return opts_; }

    // Runs f(conn&) in a transaction and commits; returns what f returned (a
    // reference as a copy). An error whose kind is retryable runs it again, up to
    // options().transact_attempts times, after a jittered exponential backoff.
    template <class F>
    auto transact(F&& f) -> std::remove_cvref_t<std::invoke_result_t<F, conn&>>;

private:
    friend class conn;
    pool_options opts_;
    std::unique_ptr<detail::lot> lot_;
};

// A handle that runs statements on a pool's connections; one per thread and pool.
// Outside a transaction a statement shares a connection with other threads'
// statements (pipelined, each with its own Sync) for as long as it runs, so two
// statements may run on different server sessions, as behind a transaction
// pooler. The conn holds a connection of its own while a transaction is open,
// while sent statements are unread, and after raw().
class conn {
public:
    explicit conn(pool& p);  // throws nested_borrow when this thread has a live conn of p
    ~conn();                 // reads every sent statement first
    conn(const conn&) = delete;
    conn& operator=(const conn&) = delete;

    // Runs a statement; returns the number of affected rows.
    std::int64_t exec(const sql& q) { return detail::affected_of(detail::await(this, *start(q, nullptr, true))); }

    // Every row, decoded as row_t<T...>. Result column types (and the names of
    // a read shape) are checked before the statement executes.
    template <class... T>
    std::vector<row_t<T...>> query(const sql& q) {
        const auto t = start(q, detail::check_for<T...>(), true);
        return detail::rows_of<T...>(detail::await(this, *t));
    }

    // Exactly one row: none throws not_found, more pg::error.
    template <class... T>
    row_t<T...> query_one(const sql& q) {
        const auto t = start(q, detail::check_for<T...>(), true);
        return detail::one_of<T...>(detail::await(this, *t), t->key);
    }

    // Zero or one row; more throws pg::error.
    template <class... T>
    std::optional<row_t<T...>> query_opt(const sql& q) {
        const auto t = start(q, detail::check_for<T...>(), true);
        return detail::opt_of<T...>(detail::await(this, *t), t->key);
    }

    // Queues the statement and returns; the result is read through the pending,
    // also after this conn ended. Outside a transaction, the error of a statement
    // nobody reads is dropped.
    template <class... T>
    pending<T...> send(const sql& q) {
        return pending<T...>(start(q, detail::check_for<T...>(), false), this);
    }

    bool in_transaction() const { return depth_ > 0; }

    // The connection, every sent statement read, out of pipeline mode for
    // blocking libpq calls. It stays with this conn and is closed when the conn ends.
    PGconn* raw();

    pool& owner() { return *p_; }

private:
    friend class transaction;
    friend const PGresult* detail::await(conn* c, detail::ticket& t);
    friend std::int64_t detail::copy_on(conn& c, std::string_view table, std::span<const detail::copy_column> cols,
                                        const std::string& data);

    std::shared_ptr<detail::ticket> start(const sql& q, detail::check_fn check, bool now);
    detail::prepared& lookup(detail::wire& w, std::unique_lock<std::mutex>& lk, const std::string& key,
                             std::span<const detail::param> params, detail::check_fn check);
    template <class F>
    std::shared_ptr<detail::ticket> queue(detail::wire& w, std::string key, F&& send);
    std::optional<error> control(const std::vector<std::string>& commands);
    void end_tx(int level, bool commit);
    detail::wire& hold();
    detail::wire& quiet();
    void settle();

    pool* p_;
    detail::wire* w_ = nullptr;  // held: in a transaction, with sent statements unread, or after raw()
    int depth_ = 0;  // open transaction guards
    int sent_ = 0;   // guards whose BEGIN or SAVEPOINT went out on w_
};

// A transaction scope. The outermost guard's BEGIN travels with the conn's next
// statement; a nested guard sets SAVEPOINT libpg_<depth>. Without commit() the
// destructor rolls back (to the savepoint). Guards end innermost first.
class transaction {
public:
    explicit transaction(conn& c) : c_(c), level_(++c.depth_) {}
    ~transaction();
    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;

    // Reads every unread statement of the conn; if the transaction failed, rolls
    // back and throws that error. Then COMMIT (RELEASE SAVEPOINT); the connection
    // lost while COMMIT is in flight throws commit_unknown.
    void commit();

private:
    conn& c_;
    int level_;
    bool open_ = true;
};

template <class F>
auto pool::transact(F&& f) -> std::remove_cvref_t<std::invoke_result_t<F, conn&>> {
    for (int attempt = 1;; ++attempt) {
        try {
            conn c(*this);
            transaction tx(c);
            if constexpr (std::is_void_v<std::invoke_result_t<F, conn&>>) {
                f(c);
                tx.commit();
                return;
            } else {
                std::remove_cvref_t<std::invoke_result_t<F, conn&>> r = f(c);
                tx.commit();
                return r;
            }
        } catch (const error& e) {
            if (!retryable(e.kind) || attempt >= opts_.transact_attempts) throw;
        }
        detail::backoff(attempt);
    }
}

}  // namespace pg
