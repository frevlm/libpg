#include "pg/connection.hpp"

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <utility>

#include "engine.hpp"

namespace pg {

namespace {

using detail::session;

// A thread waiting for a connection. The pool hands it either a session, or (s == nullptr)
// the right to open one with `dsn`; `generation` is the pool generation of the handoff.
struct waiter {
    std::condition_variable cv;
    bool ready = false;
    session* s = nullptr;
    std::string dsn;
    std::uint64_t generation = 0;
};

// The global pool. `open` counts live connections, leased or idle, of the
// current `generation`; connect() starts a new generation. Waiters queue in
// arrival order and are served directly by release(), so idle is only
// non-empty while nobody waits.
struct pool {
    std::mutex mu;
    std::string dsn;
    int max_conns = 8;
    int open = 0;
    std::uint64_t generation = 0;
    std::vector<session*> idle;
    std::deque<waiter*> waiters;
};

struct lease {
    session* s;
    std::uint64_t generation;
};

// Never destroyed, so that threads still running at exit can use it.
pool& the_pool() {
    static pool* p = new pool;
    return *p;
}

// Closes the connection and throws pg::error carrying `why`.
[[noreturn]] void fail_connection(PGconn* c, const std::string& why) {
    PQfinish(c);
    throw error(why);
}

// Connects, applies the session settings, then switches to pipeline and
// nonblocking mode for the engine.
session* open_session(const std::string& dsn) {
    PGconn* c = PQconnectdb(dsn.c_str());
    if (PQstatus(c) != CONNECTION_OK) fail_connection(c, detail::conn_message(c));
    // SET after connecting overrides PGDATESTYLE, DSN options and database defaults.
    const detail::result r{PQexec(c, "SET TimeZone = 'UTC'; SET DateStyle = 'ISO, MDY'; "
                                     "SET IntervalStyle = 'postgres'; SET extra_float_digits = 3")};
    if (!r || PQresultStatus(r.get()) != PGRES_COMMAND_OK) fail_connection(c, detail::conn_message(c));
    if (!PQenterPipelineMode(c) || PQsetnonblocking(c, 1) != 0) fail_connection(c, detail::conn_message(c));
    return new session(c);
}

// Wakes the oldest waiter with a session, or with the right to open one.
void hand_off(pool& p, session* s) {
    waiter* w = p.waiters.front();
    p.waiters.pop_front();
    w->s = s;
    w->generation = p.generation;
    if (!s) w->dsn = p.dsn;
    w->ready = true;
    w->cv.notify_one();
}

// A connection slot of the current generation became free.
void free_slot(pool& p) {
    if (p.waiters.empty()) --p.open;
    else hand_off(p, nullptr);
}

// Gives back a connection slot reserved by acquire() that did not materialize.
void give_back_slot(std::uint64_t generation) {
    pool& p = the_pool();
    std::lock_guard lk(p.mu);
    if (generation == p.generation) free_slot(p);
}

// Waits in line; returns with a session or a slot to open one in.
lease wait_in_line(pool& p, std::unique_lock<std::mutex>& lk) {
    waiter w;
    p.waiters.push_back(&w);
    w.cv.wait(lk, [&] { return w.ready; });
    if (w.s) return {w.s, w.generation};
    lk.unlock();
    try {
        return {open_session(w.dsn), w.generation};
    } catch (...) {
        give_back_slot(w.generation);
        throw;
    }
}

lease acquire() {
    pool& p = the_pool();
    std::unique_lock lk(p.mu);
    const bool has_slot = p.open < p.max_conns;
    if (!p.waiters.empty() || (p.idle.empty() && !has_slot)) return wait_in_line(p, lk);
    if (!p.idle.empty()) {
        session* s = p.idle.back();
        p.idle.pop_back();
        return {s, p.generation};
    }
    ++p.open;
    const std::string dsn = p.dsn;
    const std::uint64_t generation = p.generation;
    lk.unlock();
    try {
        return {open_session(dsn), generation};
    } catch (...) {
        give_back_slot(generation);
        throw;
    }
}

bool reusable(const session& s) {
    return !s.broken && PQstatus(s.c) == CONNECTION_OK && PQtransactionStatus(s.c) == PQTRANS_IDLE;
}

// A BEGIN still pending was never sent: dropping it leaves the connection idle.
void release(session* s, std::uint64_t generation) {
    pool& p = the_pool();
    s->begin_pending = false;
    const bool healthy = reusable(*s);
    std::unique_lock lk(p.mu);
    const bool current = generation == p.generation;
    if (healthy && current) {
        if (p.waiters.empty()) p.idle.push_back(s);
        else hand_off(p, s);
        return;
    }
    if (current) free_slot(p);
    lk.unlock();
    delete s;
}

}  // namespace

void connect(std::string dsn, int max_conns) {
    if (max_conns < 1) throw error("connect: max_conns must be at least 1");
    pool& p = the_pool();
    std::vector<session*> dropped;
    {
        std::lock_guard lk(p.mu);
        dropped.swap(p.idle);
        p.dsn = std::move(dsn);
        p.max_conns = max_conns;
        p.open = 0;
        ++p.generation;
        // Waiters of the old generation may open connections of the new one.
        while (!p.waiters.empty() && p.open < p.max_conns) {
            ++p.open;
            hand_off(p, nullptr);
        }
    }
    for (session* s : dropped) delete s;
}

namespace detail {

void check_columns(const PGresult* r, std::size_t want) {
    const auto got = static_cast<std::size_t>(PQnfields(r));
    if (got != want)
        throw error("query returned " + std::to_string(got) + " columns, expected " + std::to_string(want));
}

std::int64_t affected_rows(const PGresult* r) {
    // PQcmdTuples only reads the result; its parameter is not const for historical reasons.
    return std::strtoll(PQcmdTuples(const_cast<PGresult*>(r)), nullptr, 10);
}

}  // namespace detail

conn::conn() {
    const lease l = acquire();
    s_ = l.s;
    c_ = l.s->c;
    generation_ = l.generation;
}

conn::~conn() { release(s_, generation_); }

detail::result conn::run(std::string_view sql, const detail::params& p) {
    const detail::statement st{sql, &p};
    detail::outcome o = detail::execute(*s_, {&st, 1});
    if (o.failure) throw *o.failure;
    return std::move(o.results.front());
}

std::int64_t conn::exec_params(std::string_view sql, const detail::params& p) {
    const detail::result r = run(sql, p);
    return detail::affected_rows(r.get());
}

transaction::transaction(conn& c) : c_(c) {
    if (!detail::idle(*c_.s_)) throw error("transaction: connection is not idle");
    c_.s_->begin_pending = true;
}

transaction::~transaction() {
    // A BEGIN still pending was never sent: there is nothing to roll back.
    if (!open_ || std::exchange(c_.s_->begin_pending, false)) return;
    try {
        c_.exec("ROLLBACK");
    } catch (...) {
        // The connection is broken; the pool discards it when the lease ends.
    }
}

void transaction::commit() {
    open_ = false;
    if (std::exchange(c_.s_->begin_pending, false)) return;
    // COMMIT of an aborted transaction succeeds on the wire but rolls back.
    const bool aborted = PQtransactionStatus(c_.raw()) == PQTRANS_INERROR;
    c_.exec("COMMIT");
    if (aborted) throw error("transaction: a statement failed, transaction was rolled back");
}

}  // namespace pg
