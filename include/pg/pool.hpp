#pragma once
#include <libpq-fe.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "codec.hpp"
#include "error.hpp"
#include "shape.hpp"
#include "sql.hpp"

namespace pg {

struct pool_options {
    std::string dsn;
    int max_conns = 8;
    std::chrono::milliseconds statement_timeout{30'000};  // set at connect; 0 = none
    bool synchronous_commit = false;  // off: a server crash may lose the last ~0.6 s of commits, never corrupts data; on: COMMIT waits for the WAL flush
    std::chrono::milliseconds deadline{35'000};           // client cap per round trip; 0 = none
    std::chrono::milliseconds acquire_timeout{30'000};    // wait for a free connection
    std::chrono::milliseconds probe_after{30'000};        // probe an idle connection older than this
    int breaker_failures = 3;                             // consecutive connect failures to open
    std::chrono::milliseconds breaker_cooldown{1'000};    // doubles per failed retry, max 30 s
    int transact_attempts = 3;                            // closure transaction attempts
    // The DSN is a transaction pooler (PgBouncer, PgCat), which refuses startup
    // options: statement_timeout and synchronous_commit above are not sent, so
    // set them on the role (ALTER ROLE ... SET). Session state (SET, advisory
    // locks) does not outlive a transaction there.
    bool pooler = false;
    // Learns which reads follow a first read on a conn and fetches them with
    // the first read in one round trip (docs/adr/0025): batches run on up to
    // max(1, max_conns / 4) extra read-only connections, opened with a 250 ms
    // statement timeout. Reads in a transaction, locking reads and statements
    // that pin are never prefetched. No effect with `pooler`.
    bool prefetch = false;
};

struct pool_stats {
    int open = 0;     // connections, idle or leased
    int idle = 0;
    int leased = 0;   // lent out or being opened for a borrower
    int waiting = 0;  // borrowers waiting for a connection
    std::uint64_t borrows = 0;
    std::uint64_t connect_failures = 0;
    bool breaker_open = false;
    // Prefetch (pool_options::prefetch), counted when a conn's scope of reads ends.
    std::uint64_t prefetch_plans = 0;     // first reads sent with their learned followers
    std::uint64_t prefetch_hits = 0;      // reads answered from a prefetched result
    std::uint64_t prefetch_misses = 0;    // reads executed normally after a planned first read
    std::uint64_t prefetch_unused = 0;    // prefetched results dropped unread
    std::uint64_t prefetch_failures = 0;  // prefetch statements that failed
    std::uint64_t prefetch_skips = 0;     // plans not sent for lack of a free connection
};

class conn;

namespace detail {

struct session;
struct pool_state;
struct sent;
struct prefetch_scope;
struct prefetch_plan;

// Clears the result unless it is a view of one owned elsewhere (`owns` false).
struct result_deleter {
    bool owns = true;
    void operator()(PGresult* r) const {
        if (owns) PQclear(r);
    }
};
using result = std::unique_ptr<PGresult, result_deleter>;

// The parameter types the server resolved for a prepared statement.
using param_types = std::shared_ptr<const std::vector<oid>>;

// Checks the result columns a statement will produce before it executes;
// returns why they cannot be read, if they cannot.
using check_fn = std::optional<std::string> (*)(std::span<const type_desc>);

// The part of a result that is read: `rows` rows from row `row` (all of them
// when negative) and the columns from `col` on.
struct window {
    int row = 0;
    int rows = -1;
    int col = 0;
};

// A statement's result together with a view of its column types; `keep` holds
// what the view points into. The reply reads the result through `w`; its
// columns are the ones from w.col on.
struct reply {
    result r;
    std::span<const type_desc> columns;
    std::shared_ptr<const void> keep;
    window w;
};

// The rows of the result a window shows.
inline int rows_in(const PGresult* r, const window& w) { return w.rows < 0 ? PQntuples(r) : w.rows; }
inline int rows_in(const reply& rp) { return rows_in(rp.r.get(), rp.w); }

// Waits for a sent statement and consumes its result; a reader may take it once.
reply take(sent& x);

// The statement a sent one was made from.
const sql& sent_sql(const sent& x);

// The number of affected rows of a result.
std::int64_t affected_of(const reply& rp);

template <class... T>
struct row_type {
    using type = std::tuple<T...>;
};
template <class T>
struct row_type<T> {
    using type = T;
};

// Backs off before attempt `next` of a closure transaction.
void transact_backoff(int next);

session& session_of(conn& c);

// Sends COMMIT, gives the connection back to the pool and waits for the
// answer, so the next borrower's statements travel right behind it.
void commit_floating(pool_state& st, const pool_options& o, session*& slot);

// conn::run_text for library code that builds its statements itself; never pins.
reply run_text(conn& c, std::string_view text, std::span<const param> params, check_fn check);

}  // namespace detail

// A row of a query: std::tuple<T...>, or T itself when there is only one.
template <class... T>
using row_t = typename detail::row_type<T...>::type;

namespace detail {
template <class... T>
std::vector<row_t<T...>> rows_of(const reply& rp);
template <class... T>
row_t<T...> one_of(const reply& rp, const sql& q);
template <class... T>
std::optional<row_t<T...>> opt_of(const reply& rp, const sql& q);
}  // namespace detail

// A statement sent and not yet read. Each reader waits for the result and may
// be called once; with no T only affected() is available.
template <class... T>
class pending {
public:
    std::vector<row_t<T...>> all() {
        static_assert(sizeof...(T) > 0, "pending::all needs at least one column type");
        const detail::reply rp = detail::take(*s_);
        return detail::rows_of<T...>(rp);
    }

    row_t<T...> one() {
        static_assert(sizeof...(T) > 0, "pending::one needs at least one column type");
        const detail::reply rp = detail::take(*s_);
        return detail::one_of<T...>(rp, detail::sent_sql(*s_));
    }

    std::optional<row_t<T...>> opt() {
        static_assert(sizeof...(T) > 0, "pending::opt needs at least one column type");
        const detail::reply rp = detail::take(*s_);
        return detail::opt_of<T...>(rp, detail::sent_sql(*s_));
    }

    std::int64_t affected() {
        const detail::reply rp = detail::take(*s_);
        return detail::affected_of(rp);
    }

private:
    friend class conn;
    explicit pending(std::shared_ptr<detail::sent> s) : s_(std::move(s)) {}
    std::shared_ptr<detail::sent> s_;
};

// A user-owned set of connections to one database. Not copyable, not movable;
// it must outlive every conn borrowed from it.
class pool {
public:
    explicit pool(pool_options o);
    ~pool();
    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;

    pool_stats stats() const;
    // For a readiness probe: connections are open and the breaker is closed;
    // with none open, tries one. Never waits in line for a busy pool.
    bool ready();
    const pool_options& options() const { return opts_; }

    // Borrows a connection, runs f(conn&) in a transaction and commits; returns
    // what f returned. Retries with a fresh connection on errors whose
    // disposition says so, up to options().transact_attempts.
    // A reference returned by f is returned as a copy of the referenced value.
    template <class F>
    auto transact(F&& f) -> std::remove_cvref_t<std::invoke_result_t<F, conn&>>;

private:
    friend class conn;
    friend class transaction;
    pool_options opts_;
    std::unique_ptr<detail::pool_state> st_;
};

// A connection borrowed from a pool: the only thing that executes statements.
// It leases a connection only while it needs one to itself (a transaction,
// raw(), session-state statements, or when the pool has no idle connection at
// construction); otherwise its statements share the pool's connections.
// Borrowing a second conn of the same pool on the same thread throws
// nested_borrow.
class conn {
public:
    explicit conn(pool& p);
    ~conn();
    conn(const conn&) = delete;
    conn& operator=(const conn&) = delete;

    // Runs a statement; returns the number of affected rows.
    std::int64_t exec(const sql& q);

    // Every row, as std::tuple<T...>, as T for one column, or decoded into a
    // read shape. Column types are checked before the statement executes.
    template <class... T>
    std::vector<row_t<T...>> query(const sql& q);

    // Exactly one row; none throws not_found, more than one pg::error.
    template <class... T>
    row_t<T...> query_one(const sql& q);

    // Zero or one row; more than one throws pg::error.
    template <class... T>
    std::optional<row_t<T...>> query_opt(const sql& q);

    // Sends the statement and returns at once; the result is read through the
    // returned pending. Outside a transaction, statements sent this way run
    // concurrently with each other and with later statements of the conn (no
    // order between them). In a transaction they run in order on its connection;
    // one nobody read is checked by commit(), which throws its error.
    // Every sent statement is waited for at the latest when the conn ends; the
    // error of one nobody read outside a transaction is lost.
    template <class... T>
    pending<T...> send(const sql& q);

    // A transaction guard is open on the connection.
    bool in_transaction() const;

    // The libpq connection. It is in pipeline mode and nonblocking: the blocking
    // calls (PQexec, PQprepare, PQexecPrepared ...) are not allowed on it.
    PGconn* raw();

    pool& owner() { return *p_; }

private:
    friend class transaction;
    friend detail::session& detail::session_of(conn&);
    friend std::vector<detail::copy_target> detail::copy_targets(conn&, std::string_view,
                                                                std::span<const std::string_view>);
    friend void detail::forget_copy_targets(conn&, std::string_view, std::span<const std::string_view>);
    friend std::int64_t detail::copy_rows(conn&, std::string_view, std::string_view);
    friend detail::reply detail::run_text(conn&, std::string_view, std::span<const detail::param>, detail::check_fn);

    // Runs text already numbered $1..$n with its parameters on a floating or held connection, like
    // run(const sql&) does; `pins` is the pin verdict of the text. With `types`, it receives the
    // parameter types the server resolved.
    detail::reply run_text(std::string_view text, std::span<const detail::param> params, detail::check_fn check,
                           bool pins, detail::param_types* types = nullptr);
    detail::reply run(const sql& q, detail::check_fn check);
    std::shared_ptr<detail::sent> start(const sql& q, detail::check_fn check);
    // start() without ending the prefetch scope.
    std::shared_ptr<detail::sent> send_unscoped(const sql& q, detail::check_fn check);
    // run() without ending the prefetch scope.
    detail::reply run_unscoped(const rendered& r, detail::check_fn check, bool pins,
                               detail::param_types* types = nullptr);
    // A statement sent with send() has not finished yet.
    bool sending() const;
    // run() with pool_options::prefetch on.
    detail::reply prefetched(const sql& q, rendered& r, detail::check_fn check, bool pins);
    // Runs the first read of a scope together with the followers its plan holds.
    detail::reply run_plan(const detail::prefetch_plan& pl, const sql& q, const rendered& r, detail::check_fn check);
    // Ends the scope of prefetched reads: learns from it and drops its results.
    void end_scope() {
        pf_idle_ = false;
        if (pf_) close_scope();
    }
    void close_scope();
    std::optional<error> settle_sent(bool held_only);
    detail::session& hold();  // leases a connection if floating, returns it
    detail::session& pin();   // hold(), then keeps the connection until the conn ends
    bool keeps(bool pins) const { return pinned_ || in_transaction() || pins; }  // runs on the conn's own connection
    detail::session& own(bool pins) { return pins ? pin() : hold(); }
    detail::pool_state& state();

    pool* p_;
    detail::session* s_;   // the leased connection; nullptr while floating
    bool pinned_ = false;  // keeps s_ until the conn ends (raw(), session-state statement)
    std::vector<std::shared_ptr<detail::sent>> sent_;  // statements sent and not yet finished
    std::unique_ptr<detail::prefetch_scope> pf_;       // reads since the last write; prefetch only
    bool pf_idle_ = false;                             // the scope is not observed (a sampled-out root)
};

// A transaction scope on a connection. The outermost guard begins a READ
// COMMITTED transaction (BEGIN travels with the next statement); a nested one
// sets a savepoint. Unless commit() was called, the destructor rolls back (to
// the savepoint, for a nested guard). Guards must end innermost first.
class transaction {
public:
    explicit transaction(conn& c);
    ~transaction();
    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;

    // COMMIT, or RELEASE SAVEPOINT. The connection lost, or the deadline
    // passed, while COMMIT was in flight throws commit_unknown; a transaction a failed statement aborted
    // is rolled back and throws pg::error.
    void commit();

private:
    conn& c_;
    int level_;
    bool open_ = true;
};

namespace detail {

std::string column_mismatch(std::size_t col, const type_desc& t);

template <class... T>
std::optional<std::string> check_count(std::span<const type_desc> cols) {
    if (cols.size() == sizeof...(T)) return std::nullopt;
    return "query returned " + std::to_string(cols.size()) + " columns, expected " + std::to_string(sizeof...(T));
}

template <class T>
std::optional<std::string> check_one(std::span<const type_desc> cols, std::size_t i) {
    if (accepts<T>(cols[i])) return std::nullopt;
    return column_mismatch(i, cols[i]);
}

template <class R, class = void>
struct has_pg_names : std::false_type {};
template <class R>
struct has_pg_names<R, std::void_t<decltype(R::pg_names)>> : std::true_type {};

template <class R>
std::optional<std::string> check_shape(std::span<const type_desc> cols) {
    if (cols.size() != R::pg_columns)
        return "query returned " + std::to_string(cols.size()) + " columns, the row shape has " +
               std::to_string(R::pg_columns);
    R probe{};
    std::size_t i = 0;
    std::optional<std::string> bad;
    probe.pg_each([&](auto& m) {
        if (!bad) {
            if constexpr (has_pg_names<R>::value) {
                if (cols[i].column != R::pg_names[i])
                    bad = "column " + std::to_string(i + 1) + " is \"" + std::string(cols[i].column) +
                          "\", the row shape expects \"" + std::string(R::pg_names[i]) + "\"";
            }
        }
        if (!bad) bad = check_one<std::remove_cvref_t<decltype(m)>>(cols, i);
        ++i;
    });
    return bad;
}

// The check_fn of query<T...>.
template <class... T>
std::optional<std::string> check_row(std::span<const type_desc> cols) {
    if constexpr (sizeof...(T) == 1 && (read_shape<T> && ...)) return (check_shape<T>(cols), ...);
    else {
        if (auto bad = check_count<T...>(cols)) return bad;
        std::size_t i = 0;
        std::optional<std::string> bad;
        ((bad = bad ? bad : check_one<T>(cols, i), ++i), ...);
        return bad;
    }
}

// The check_fn of send<T...>: none when nothing is read.
template <class... T>
constexpr check_fn check_for() {
    if constexpr (sizeof...(T) == 0) return nullptr;
    else return &check_row<T...>;
}

template <class T>
T cell(const reply& rp, int row, int col) {
    const PGresult* r = rp.r.get();
    row += rp.w.row;
    const int at = col + rp.w.col;
    const char* v = PQgetisnull(r, row, at) ? nullptr : PQgetvalue(r, row, at);
    const auto n = static_cast<std::size_t>(PQgetlength(r, row, at));
    return decode_value<T>(v, n, rp.columns[static_cast<std::size_t>(col)]);
}

template <class R>
R decode_shape(const reply& rp, int row) {
    R out{};
    int col = 0;
    out.pg_each([&](auto& m) { m = cell<std::remove_cvref_t<decltype(m)>>(rp, row, col++); });
    return out;
}

template <class... T>
row_t<T...> decode_row(const reply& rp, int row) {
    if constexpr (sizeof...(T) == 1 && (read_shape<T> && ...)) return decode_shape<T...>(rp, row);
    else if constexpr (sizeof...(T) == 1) return cell<T...>(rp, row, 0);
    else {
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return std::tuple<T...>{cell<T>(rp, row, static_cast<int>(I))...};
        }(std::index_sequence_for<T...>{});
    }
}

[[noreturn]] void throw_row_count(std::size_t rows, const sql& q);

template <class... T>
std::vector<row_t<T...>> rows_of(const reply& rp) {
    const int n = rows_in(rp);
    std::vector<row_t<T...>> rows;
    rows.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) rows.push_back(decode_row<T...>(rp, i));
    return rows;
}

template <class... T>
row_t<T...> one_of(const reply& rp, const sql& q) {
    const auto n = static_cast<std::size_t>(rows_in(rp));
    if (n != 1) throw_row_count(n, q);
    return decode_row<T...>(rp, 0);
}

template <class... T>
std::optional<row_t<T...>> opt_of(const reply& rp, const sql& q) {
    const auto n = static_cast<std::size_t>(rows_in(rp));
    if (n > 1) throw_row_count(n, q);
    if (n == 0) return std::nullopt;
    return decode_row<T...>(rp, 0);
}

}  // namespace detail

template <class... T>
std::vector<row_t<T...>> conn::query(const sql& q) {
    static_assert(sizeof...(T) > 0, "query needs at least one column type");
    return detail::rows_of<T...>(run(q, &detail::check_row<T...>));
}

template <class... T>
row_t<T...> conn::query_one(const sql& q) {
    return detail::one_of<T...>(run(q, &detail::check_row<T...>), q);
}

template <class... T>
std::optional<row_t<T...>> conn::query_opt(const sql& q) {
    return detail::opt_of<T...>(run(q, &detail::check_row<T...>), q);
}

template <class... T>
pending<T...> conn::send(const sql& q) {
    return pending<T...>(start(q, detail::check_for<T...>()));
}

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
            if (!dispose(e.kind).retry_closure || attempt >= opts_.transact_attempts) throw;
        }
        detail::transact_backoff(attempt + 1);
    }
}

}  // namespace pg
