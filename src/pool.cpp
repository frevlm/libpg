#include "pg/pool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <utility>

#include "engine.hpp"
#include "prefetch.hpp"

namespace pg {
namespace detail {

using clock = std::chrono::steady_clock;

// A borrower waiting in line. It is handed a session, or (s == nullptr) a
// slot to open a new connection in, or it is timed out at its deadline. The
// pool sets `s` and `state` under its lock; the borrower watches `state`
// without the lock.
struct waiter {
    using state_t = std::atomic_signed_lock_free::value_type;
    enum : state_t { asleep, served, timed_out };
    std::atomic_signed_lock_free state{asleep};
    clock::time_point deadline;
    session* s = nullptr;
};

// A waiter to notify after the pool lock is released, so the woken borrower
// does not run into the lock still held. Declared before the lock it follows.
struct wakeups {
    std::shared_ptr<waiter> w;
    void add(std::shared_ptr<waiter> x) { w = std::move(x); }
    ~wakeups() {
        if (w) w->state.notify_one();
    }
};

// `open` counts sessions alive or being opened; `leased` those lent out or
// being opened for a borrower. Waiters are served in arrival order, directly
// by whoever frees a session or a slot, so `idle` is only non-empty while
// nobody waits.
//
// Waiters block without a timeout; the keeper thread, started by the first
// waiter, times them out.
struct pool_state {
    mutable std::mutex mu;
    std::condition_variable keeper_cv;
    std::thread keeper;
    bool stopping = false;
    int open = 0;
    int leased = 0;
    std::vector<session*> idle;
    // Read-only sessions of speculation: kept apart from the borrowable ones,
    // never leased, outside max_conns. `ro_open` counts them alive or being
    // opened.
    std::vector<session*> ro;
    int ro_open = 0;
    std::deque<std::shared_ptr<waiter>> waiters;
    std::atomic<std::uint64_t> borrows{0};
    std::uint64_t connect_failures = 0;
    int failures = 0;  // consecutive connect failures
    bool breaker_open = false;
    std::chrono::milliseconds cooldown{0};
    clock::time_point retry_at;
    // The base types of COPY columns by table and column list.
    std::unordered_map<std::string, std::vector<copy_target>> copy_types;
    prefetch_table prefetch;
};

prefetch_table& prefetch_of(pool_state& st) { return st.prefetch; }

// A statement in flight on a session it owns a reference to.
struct flight {
    session* s = nullptr;
    pending_ptr pd;
};

// A statement sent by conn::send and its outcome once read.
struct sent {
    sql q;                        // owns the parameters r views
    rendered r;                   // r = q.render() after q is set; sent is never moved
    statement stmt;               // views r
    pool_state* st = nullptr;
    const pool_options* opts = nullptr;  // to re-run a stale floating statement
    bool held = false;            // on the conn's own (leased) session
    flight f;                     // where it travels
    std::optional<outcome> out;   // set by finish()
    bool taken = false;           // a reader consumed the result
};

namespace {

// The pools this thread holds a connection of.
thread_local std::vector<const pool*> held_pools;

constexpr std::chrono::milliseconds max_cooldown{30'000};

// Serves the head of the line.
void hand_off(pool_state& st, session* s, wakeups& wk) {
    std::shared_ptr<waiter> w = std::move(st.waiters.front());
    st.waiters.pop_front();
    w->s = s;
    ++st.leased;
    w->state.store(waiter::served, std::memory_order_release);
    wk.add(std::move(w));
}

// A connection slot became free: the oldest waiter gets it, if any.
void free_slot(pool_state& st, wakeups& wk) {
    if (st.waiters.empty()) --st.open;
    else hand_off(st, nullptr, wk);
}

// Waits until the waiter is served or timed out.
waiter::state_t await(waiter& w) {
    for (;;) {
        const waiter::state_t v = w.state.load(std::memory_order_acquire);
        if (v != waiter::asleep) return v;
        w.state.wait(waiter::asleep, std::memory_order_acquire);
    }
}

// Times out the waiters whose deadline passed.
void expire_waiters(pool_state& st, clock::time_point now) {
    std::erase_if(st.waiters, [&](const std::shared_ptr<waiter>& w) {
        if (w->deadline > now) return false;
        w->state.store(waiter::timed_out, std::memory_order_release);
        w->state.notify_one();
        return true;
    });
}

// The earliest deadline in line. A waiter arriving later has a deadline of at
// least now + acquire_timeout, so with nobody in line that is the next wake.
clock::time_point next_deadline(const pool_state& st, clock::time_point now, std::chrono::milliseconds timeout) {
    clock::time_point next = now + timeout;
    for (const auto& w : st.waiters) next = std::min(next, w->deadline);
    return next;
}

void keep_time(pool_state& st, std::chrono::milliseconds timeout) {
    std::unique_lock lk(st.mu);
    while (!st.stopping) {
        const auto now = clock::now();
        expire_waiters(st, now);
        st.keeper_cv.wait_until(lk, next_deadline(st, now, timeout));
    }
}

session* wait_in_line(pool_state& st, const pool_options& o, std::unique_lock<std::mutex>& lk) {
    if (!st.keeper.joinable()) st.keeper = std::thread(keep_time, std::ref(st), o.acquire_timeout);
    auto w = std::make_shared<waiter>();
    w->deadline = clock::now() + o.acquire_timeout;
    st.waiters.push_back(w);
    lk.unlock();
    if (await(*w) == waiter::timed_out) throw error(error_kind::connect_failed, "timed out waiting for a free connection");
    return w->s;
}

// The session of `v` (the idle or the read-only ones) to take and whether every
// one is slow. A session whose front round trip has been the front for more
// than twice the best round-trip time, or whose known-slow statement is due to
// stay busy past twice the best round-trip time, is likely running a slow
// statement: nothing piles onto it.
std::pair<std::vector<session*>::iterator, bool> pick_idle(std::vector<session*>& v, std::int64_t now) {
    std::int64_t best = 0;
    for (session* s : v) {
        const std::int64_t rtt = s->busy.rtt_ns.load(std::memory_order_relaxed);
        if (rtt > 0 && (best == 0 || rtt < best)) best = rtt;
    }
    // Twice the best round trip, at least 1 ms: below that, scheduling noise
    // on a busy client looks like a slow statement.
    const std::int64_t limit = std::max<std::int64_t>(2 * best, 1'000'000);
    const auto slow = [&](const session* s) { return s->busy.slow(now, best, limit); };
    auto pick = v.end();
    for (auto it = v.begin(); it != v.end(); ++it)
        if (!slow(*it) && (pick == v.end() ||
                           (*it)->refs.load(std::memory_order_relaxed) <
                               (*pick)->refs.load(std::memory_order_relaxed)))
            pick = it;
    if (pick != v.end()) return {pick, false};
    // Every session is slow: the youngest head.
    for (auto it = v.begin(); it != v.end(); ++it)
        if (pick == v.end() || (*it)->busy.head_since.load(std::memory_order_relaxed) >
                                         (*pick)->busy.head_since.load(std::memory_order_relaxed))
            pick = it;
    return {pick, true};
}

// An idle session a floating statement may share: it stays in idle and takes no
// lease. Nullptr asks the caller to take a lease instead, spreading load.
session* attach(pool_state& st, const pool_options& o) {
    std::lock_guard lk(st.mu);
    if (st.idle.empty()) return nullptr;
    const auto [it, stalled] = pick_idle(st.idle, now_ns());
    const bool full = st.open >= o.max_conns;
    if ((stalled || (*it)->refs.load(std::memory_order_relaxed) > 1) && !full) return nullptr;
    // Counted as an owner before the pool lock is released, so that nobody
    // deletes it under us; settle() undoes this.
    ref(**it);
    return *it;
}

// An idle session, or nullptr for a slot to open one in; waits in line when
// neither is free or others wait already. The session with the fewest owners is
// taken, so traffic spreads before piling onto one connection; a new
// connection is opened instead while the pool has room. A session likely busy
// with a slow statement is skipped unless every idle one is.
session* reserve(pool_state& st, const pool_options& o) {
    std::unique_lock lk(st.mu);
    if (!st.waiters.empty()) return wait_in_line(st, o, lk);
    if (st.idle.empty()) {
        if (st.open >= o.max_conns) return wait_in_line(st, o, lk);
        ++st.open;
        ++st.leased;
        return nullptr;
    }
    const auto [it, stalled_only] = pick_idle(st.idle, now_ns());
    const int refs = (*it)->refs.load(std::memory_order_relaxed);
    // Every idle session is stalled, but a leased or opening one may come
    // back: wait for it rather than piling onto a slow statement.
    if (stalled_only && st.open >= o.max_conns && st.idle.size() < static_cast<std::size_t>(st.open))
        return wait_in_line(st, o, lk);
    if (st.open < o.max_conns && (stalled_only || refs > 1)) {
        ++st.open;
        ++st.leased;
        return nullptr;
    }
    session* s = *it;
    st.idle.erase(it);
    ++st.leased;
    return s;
}

enum class gate { refused, normal, trial };

// Whether a new connection may be tried now. While the breaker is open, one
// try (the trial) is allowed per cooldown.
gate may_connect(pool_state& st) {
    if (!st.breaker_open) return gate::normal;
    const auto now = clock::now();
    if (now < st.retry_at) return gate::refused;
    st.retry_at = now + st.cooldown;
    return gate::trial;
}

// The breaker opens after enough consecutive failures; the cooldown doubles
// only when the trial connect fails, not for connects begun before it opened.
void note_connect_failure(pool_state& st, const pool_options& o, gate g) {
    ++st.connect_failures;
    ++st.failures;
    if (g == gate::trial) {
        st.cooldown = std::min(st.cooldown * 2, max_cooldown);
    } else if (st.breaker_open || st.failures < o.breaker_failures) {
        return;
    } else {
        st.cooldown = o.breaker_cooldown;
        st.breaker_open = true;
    }
    st.retry_at = clock::now() + st.cooldown;
}

void note_connect_success(pool_state& st) {
    st.failures = 0;
    st.breaker_open = false;
}

session* open_in_slot(pool_state& st, const pool_options& o, clock::time_point until) {
    gate g = gate::normal;
    {
        wakeups wk;
        std::lock_guard lk(st.mu);
        g = may_connect(st);
        if (g == gate::refused) {
            --st.leased;
            free_slot(st, wk);
            throw error(error_kind::connect_failed, "connection breaker is open after repeated connect failures");
        }
    }
    try {
        session* s = open_session(o, until);
        std::lock_guard lk(st.mu);
        note_connect_success(st);
        return s;
    } catch (const error&) {
        wakeups wk;
        std::lock_guard lk(st.mu);
        note_connect_failure(st, o, g);
        --st.leased;
        free_slot(st, wk);
        throw;
    }
}

// A session for a borrower: an idle one (probed when it sat unused too long;
// one that fails the probe is replaced in its slot) or a new one.
session* lend(pool_state& st, const pool_options& o) {
    const auto now = clock::now();
    const auto until = now + o.acquire_timeout;
    session* s = reserve(st, o);
    if (s && (now - s->last_used <= o.probe_after || ping(*s))) {
        set_exclusive(*s, true);
        return s;
    }
    if (s) unref(s);
    s = open_in_slot(st, o, until);
    set_exclusive(*s, true);
    return s;
}

// A conn floats from the start when an idle connection is there; otherwise it
// leases one exactly as lend() does.
session* lend_unless_idle(pool_state& st, const pool_options& o) {
    {
        std::lock_guard lk(st.mu);
        if (!st.idle.empty()) return nullptr;
    }
    return lend(st, o);
}

// A BEGIN still pending was never sent: dropping it leaves the connection idle.
// A lease that changed session settings leaves a RESET ALL in front of the next one.
void release(pool_state& st, session* s) {
    {
        std::lock_guard lk(s->mu);
        s->exclusive = false;
        s->last_used = clock::now();
        s->begin_pending = false;
        s->depth = 0;
        if (std::exchange(s->dirty, false)) s->reset_pending = true;
    }
    const bool keep = pg::detail::reusable(*s);
    wakeups wk;
    std::unique_lock lk(st.mu);
    --st.leased;
    if (keep) {
        if (st.waiters.empty()) st.idle.push_back(s);
        else hand_off(st, s, wk);
        return;
    }
    free_slot(st, wk);
    lk.unlock();
    unref(s);
}

void check_options(const pool_options& o) {
    if (o.max_conns < 1) throw error(error_kind::other, "pool: max_conns must be at least 1");
    if (o.acquire_timeout <= std::chrono::milliseconds::zero())
        throw error(error_kind::other, "pool: acquire_timeout must be positive");
    if (o.breaker_failures < 1) throw error(error_kind::other, "pool: breaker_failures must be at least 1");
    if (o.transact_attempts < 1) throw error(error_kind::other, "pool: transact_attempts must be at least 1");
}

std::int64_t affected_rows(const PGresult* r) {
    // PQcmdTuples only reads the result; its parameter is not const for historical reasons.
    return std::strtoll(PQcmdTuples(const_cast<PGresult*>(r)), nullptr, 10);
}

// Runs fixed statements without parameters as one operation.
void run_plain(session& s, std::initializer_list<std::string_view> sqls) {
    std::vector<statement> stmts;
    for (std::string_view q : sqls) stmts.push_back({q, {}, nullptr, false});
    outcome o = execute(s, stmts);
    if (o.failure) throw std::move(*o.failure);
}

std::string savepoint(int level) { return "libpg_sp" + std::to_string(level); }

// Rolls back the transaction (level 1) or to the savepoint of `level`.
void roll_back(session& s, int level) {
    if (level == 1) {
        if (!std::exchange(s.begin_pending, false)) run_plain(s, {"ROLLBACK"});
        return;
    }
    // Two operations: RELEASE cannot even be prepared until ROLLBACK TO has
    // left the aborted state.
    run_plain(s, {"ROLLBACK TO SAVEPOINT " + savepoint(level)});
    run_plain(s, {"RELEASE SAVEPOINT " + savepoint(level)});
}

}  // namespace

// Drops a broken session the pool still lists as idle or as a read-only
// session of speculation. When another borrower holds it, that borrower's
// release drops it instead.
void evict(pool_state& st, session* s) {
    bool removed = false;
    {
        wakeups wk;
        std::lock_guard lk(st.mu);
        const auto it = std::find(st.idle.begin(), st.idle.end(), s);
        const auto ro = std::find(st.ro.begin(), st.ro.end(), s);
        if (it != st.idle.end()) {
            st.idle.erase(it);
            free_slot(st, wk);
            removed = true;
        } else if (ro != st.ro.end()) {
            st.ro.erase(ro);
            --st.ro_open;
            removed = true;
        }
    }
    if (removed) unref(s);
}

// Drops the caller's owner of a finished floating statement, evicting it when
// it is broken.
void settle(pool_state& st, session* s) {
    if (is_broken(*s)) evict(st, s);
    unref(s);
}

namespace {

// Sends the floating statement without a lease when it can: attaches to an idle
// session, trying a few times when it is leased meanwhile, and leases one when
// attaching finds nothing or needs a round trip. A leased session is given back
// as soon as the statement is sent.
flight send_floating(pool_state& st, const pool_options& o, session*& slot, const statement& stmt) {
    session* s = slot ? std::exchange(slot, nullptr) : nullptr;
    if (!s) {
        for (int tries = 0; tries < 3; ++tries) {
            s = attach(st, o);
            if (!s) break;
            std::pair<pending_ptr, attach_result> r;
            try {
                r = execute_begin(*s, stmt, true);
            } catch (...) {
                settle(st, s);
                throw;
            }
            if (r.second == attach_result::sent) return {s, std::move(r.first)};
            settle(st, s);
            if (r.second == attach_result::needs_lease) break;
        }
        s = lend(st, o);
    }
    pending_ptr pd;
    try {
        pd = execute_begin(*s, stmt, false).first;
    } catch (...) {
        release(st, s);
        throw;
    }
    release(st, s);
    return {s, std::move(pd)};
}

// Waits for the flight's result and drops its reference; settles even when
// waiting throws.
[[gnu::always_inline]] inline outcome land(pool_state& st, flight f) {
    outcome out;
    try {
        out = execute_end(*f.s, std::move(f.pd));
    } catch (...) {
        settle(st, f.s);
        throw;
    }
    settle(st, f.s);
    return out;
}

}  // namespace

// Sends a floating statement, gives its connection back while it travels and
// waits for its results. A stale statement is retried once with a fresh lease.
outcome run_floating(pool_state& st, const pool_options& o, session*& slot, const statement& stmt) {
    for (int attempt = 0;; ++attempt) {
        outcome out = land(st, send_floating(st, o, slot, stmt));
        if (out.stale && attempt == 0) continue;
        return out;
    }
}

// Sends COMMIT, gives the connection back to the pool and waits for the
// answer, so the next borrower's statements travel right behind it.
void commit_floating(pool_state& st, const pool_options& o, session*& slot) {
    const statement commit{"COMMIT", {}, nullptr, false};
    outcome out = land(st, send_floating(st, o, slot, commit));
    if (out.failure) throw std::move(*out.failure);
}

namespace {

// The read-only session a speculation takes: the one with the fewest owners,
// unless it is stalled or shared and a new one may still be opened. Opens one
// then (outside the pool lock); a refused or failed connect is a skip. The
// caller counts itself as an owner of the result. Nullptr: nothing usable.
session* ro_session(pool_state& st, const pool_options& o) {
    const int cap = std::max(1, o.max_conns / 4);
    gate g = gate::normal;
    {
        std::lock_guard lk(st.mu);
        if (!st.ro.empty()) {
            const auto [it, stalled] = pick_idle(st.ro, now_ns());
            const bool shared = (*it)->refs.load(std::memory_order_relaxed) > 1;
            if (!stalled && (!shared || st.ro_open >= cap)) {
                ref(**it);
                return *it;
            }
        }
        if (st.ro_open >= cap || (g = may_connect(st)) == gate::refused) return nullptr;
        ++st.ro_open;
    }
    try {
        session* s = open_session(o, clock::now() + o.acquire_timeout, "-c default_transaction_read_only=on -c statement_timeout=250");
        std::lock_guard lk(st.mu);
        note_connect_success(st);
        ref(*s);
        st.ro.push_back(s);
        return s;
    } catch (const error&) {
        std::lock_guard lk(st.mu);
        note_connect_failure(st, o, g);
        --st.ro_open;
        return nullptr;
    }
}

}  // namespace

// The prefetch statements of a scope in flight as one operation on a read-only
// session: the server refuses every write in them.
struct speculation {
    std::vector<batch_item> items;
    std::shared_ptr<const void> keep;  // keeps the items' texts
    std::vector<statement> stmts;
    pool_state* st = nullptr;
    flight f;
    bool landed = false;

    ~speculation() {
        if (landed || !f.s) return;
        try {
            land(*st, std::move(f));
        } catch (...) {
            // Its outcome is not wanted; waiting only keeps its ticket alive.
        }
    }
};

std::shared_ptr<speculation> speculate(pool_state& st, const pool_options& o, std::vector<batch_item> items,
                                       std::shared_ptr<const void> keep) {
    auto x = std::make_shared<speculation>();
    x->items = std::move(items);
    x->keep = std::move(keep);
    x->stmts.reserve(x->items.size());
    for (const batch_item& b : x->items) x->stmts.push_back({b.text, b.params, nullptr, false});
    x->st = &st;
    for (int tries = 0; tries < 3; ++tries) {
        session* s = ro_session(st, o);
        if (!s) return nullptr;
        std::pair<pending_ptr, attach_result> r;
        try {
            r = execute_begin_contained(*s, x->stmts);
        } catch (...) {
            settle(st, s);
            throw;
        }
        if (r.second == attach_result::sent) {
            x->f = {s, std::move(r.first)};
            return x;
        }
        settle(st, s);
    }
    return nullptr;
}

landed_batch land_speculation(speculation& x) {
    landed_batch out;
    const std::size_t n = x.items.size();
    out.failed = n;
    if (x.landed) return out;
    x.landed = true;
    outcome o;
    try {
        o = land(*x.st, std::move(x.f));
    } catch (error& e) {
        out.failure = std::move(e);
        return out;
    } catch (...) {
        out.failure = error(error_kind::other, "prefetch: the batch could not be read");
        return out;
    }
    if (o.failure) {
        out.failure = std::move(*o.failure);
        out.failed = std::min(o.failed, n);
    }
    out.results = std::move(o.results);
    out.results.resize(n);
    return out;
}

std::int64_t affected_of(const reply& rp) { return affected_rows(rp.r.get()); }

const sql& sent_sql(const sent& x) { return x.q; }

// Waits for a sent statement's result and records it; never throws.
void finish(sent& x) {
    if (x.out) return;
    try {
        x.out = land(*x.st, std::move(x.f));
    } catch (error& e) {
        x.out = outcome{};
        x.out->failure = e;
    }
    if (x.out->stale && !x.held) {
        session* none = nullptr;
        try {
            x.out = run_floating(*x.st, *x.opts, none, x.stmt);
        } catch (error& e) {
            x.out = outcome{};
            x.out->failure = e;
        }
    }
    if (x.out->failure && x.out->failure->sql.empty()) x.out->failure->sql = std::string(x.stmt.sql);
}

reply take(sent& x) {
    finish(x);
    if (x.taken) throw error(error_kind::other, "pending: the result was already read");
    x.taken = true;
    if (x.out->failure) throw error(*x.out->failure);
    column_desc cols = std::move(x.out->columns.front());
    return {std::move(x.out->results.front()), columns_of(cols), std::move(cols), {}};
}

session& session_of(conn& c) {
    c.end_scope();
    session& s = c.pin();
    drain(s);
    return s;
}

std::int64_t exec_on(conn& c, const sql& q) { return c.exec(q); }

bool in_transaction(conn& c) { return c.in_transaction(); }

namespace {

// The key of a table's column list in the COPY type cache.
std::string copy_key(std::string_view table, std::span<const std::string_view> names) {
    std::string key(table);
    for (std::string_view name : names) {
        key.push_back('\0');
        key.append(name);
    }
    return key;
}

// A text[] literal of the names, quoting and escaping each.
std::string text_array(std::span<const std::string_view> names) {
    std::string array = "{";
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) array += ',';
        array += '"';
        for (char ch : names[i]) {
            if (ch == '"' || ch == '\\') array += '\\';
            array += ch;
        }
        array += '"';
    }
    array += '}';
    return array;
}

std::int64_t send_inserts(conn& c, std::span<const sql> rows) {
    std::vector<pg::pending<>> pendings;
    pendings.reserve(rows.size());
    for (const sql& q : rows) pendings.push_back(c.send(q));
    std::int64_t n = 0;
    for (auto& p : pendings) n += p.affected();
    return n;
}

}  // namespace

std::vector<copy_target> copy_targets(conn& c, std::string_view table, std::span<const std::string_view> names) {
    pool_state& st = c.state();
    const std::string key = copy_key(table, names);
    {
        std::lock_guard lk(st.mu);
        if (const auto it = st.copy_types.find(key); it != st.copy_types.end()) return it->second;
    }
    const sql q{
        "SELECT b.oid::int8, b.typcategory::text, b.typtype::text "
        "FROM unnest({}::text[]) WITH ORDINALITY AS c(name, i) "
        "JOIN pg_attribute a ON a.attrelid = {}::regclass AND a.attname = c.name AND NOT a.attisdropped "
        "JOIN pg_type t ON t.oid = a.atttypid "
        "JOIN pg_type b ON b.oid = CASE WHEN t.typtype = 'd' THEN t.typbasetype ELSE t.oid END "
        "ORDER BY c.i",
        text_array(names), quote_ident(table)};
    const auto rows = c.query<std::int64_t, std::string, std::string>(q);
    std::vector<copy_target> got;
    if (rows.size() != names.size()) return got;
    got.reserve(rows.size());
    for (const auto& row : rows) {
        const std::string& category = std::get<1>(row);
        const std::string& kind = std::get<2>(row);
        got.push_back({static_cast<oid>(std::get<0>(row)), static_cast<char>(category.empty() ? 0 : category[0]),
                       static_cast<char>(kind.empty() ? 0 : kind[0])});
    }
    {
        std::lock_guard lk(st.mu);
        st.copy_types.emplace(key, got);
    }
    return got;
}

void forget_copy_targets(conn& c, std::string_view table, std::span<const std::string_view> names) {
    pool_state& st = c.state();
    std::lock_guard lk(st.mu);
    st.copy_types.erase(copy_key(table, names));
}

std::int64_t copy_rows(conn& c, std::string_view copy_sql, std::string_view body) {
    if (c.in_transaction()) return copy_in(c.hold(), copy_sql, body);
    transaction tx(c);
    const std::int64_t n = copy_in(c.hold(), copy_sql, body);
    tx.commit();
    return n;
}

std::int64_t insert_rows(conn& c, std::span<const sql> rows) {
    if (c.in_transaction()) return send_inserts(c, rows);
    transaction tx(c);
    const std::int64_t n = send_inserts(c, rows);
    tx.commit();
    return n;
}

std::string column_mismatch(std::size_t col, const type_desc& t) {
    const std::string type = t.name.empty() ? "oid " + std::to_string(t.id) : std::string(t.name);
    return "result column " + std::to_string(col + 1) + " has type " + type +
           ", which the requested C++ type cannot read";
}

void throw_row_count(std::size_t rows, const sql& q) {
    if (rows == 0) {
        not_found e("query returned no row");
        e.sql = q.render().text;
        throw e;
    }
    throw error(error_kind::other, "query returned " + std::to_string(rows) + " rows, expected one", {},
                q.render().text);
}

void transact_backoff(int next) {
    const auto seed = static_cast<unsigned>(clock::now().time_since_epoch().count());
    std::minstd_rand rng(seed ^ static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    const int base = 5 << std::min(next - 2, 6);  // ms: 5, 10, 20 ... 320
    std::this_thread::sleep_for(std::chrono::milliseconds(base + static_cast<int>(rng() % static_cast<unsigned>(base))));
}

}  // namespace detail

pool::pool(pool_options o) : opts_(std::move(o)), st_(std::make_unique<detail::pool_state>()) {
    detail::check_options(opts_);
}

pool::~pool() {
    {
        std::lock_guard lk(st_->mu);
        if (st_->leased != 0) {
            std::fputs("pg::pool destroyed while connections are borrowed\n", stderr);
            std::terminate();
        }
        for (detail::session* s : st_->idle) detail::unref(s);
        for (detail::session* s : st_->ro) detail::unref(s);
        st_->stopping = true;
    }
    st_->keeper_cv.notify_one();
    if (st_->keeper.joinable()) st_->keeper.join();
}

pool_stats pool::stats() const {
    pool_stats out;
    {
        std::lock_guard lk(st_->mu);
        out = {st_->open,
               static_cast<int>(st_->idle.size()),
               st_->leased,
               static_cast<int>(st_->waiters.size()),
               st_->borrows.load(std::memory_order_relaxed),
               st_->connect_failures,
               st_->breaker_open};
    }
    detail::prefetch_counts(st_->prefetch, out);
    return out;
}

bool pool::ready() {
    const pool_stats s = stats();
    if (s.breaker_open) return false;
    if (s.open > 0) return true;
    try {
        conn c(*this);
        c.exec(sql{"SELECT 1"});
        return true;
    } catch (const error&) {
        return false;
    }
}

conn::conn(pool& p) : p_(&p) {
    if (std::ranges::find(detail::held_pools, &p) != detail::held_pools.end())
        throw nested_borrow("this thread already holds a connection of this pool");
    s_ = detail::lend_unless_idle(*p.st_, p.opts_);
    p.st_->borrows.fetch_add(1, std::memory_order_relaxed);
    detail::held_pools.push_back(&p);
}

conn::~conn() {
    end_scope();
    if (pf_) detail::recycle(std::move(pf_));
    settle_sent(false);
    std::erase(detail::held_pools, p_);
    if (s_) detail::release(*p_->st_, s_);
}

detail::session& conn::hold() {
    if (!s_) s_ = detail::lend(*p_->st_, p_->opts_);
    return *s_;
}

detail::session& conn::pin() {
    detail::session& s = hold();
    pinned_ = true;
    return s;
}

detail::pool_state& conn::state() { return *p_->st_; }

std::shared_ptr<detail::sent> conn::start(const sql& q, detail::check_fn check) {
    end_scope();
    return send_unscoped(q, check);
}

std::shared_ptr<detail::sent> conn::send_unscoped(const sql& q, detail::check_fn check) {
    auto x = std::make_shared<detail::sent>();
    x->q = q;
    x->r = x->q.render();
    x->stmt = detail::statement{x->r.text, x->r.params, check, false};
    x->st = p_->st_.get();
    x->opts = &p_->opts_;
    std::erase_if(sent_, [](const std::shared_ptr<detail::sent>& s) { return s->out.has_value(); });
    const bool pins = detail::pins_of(x->q, x->r.text);
    if (keeps(pins)) {
        detail::session& s = own(pins);
        x->f = {&s, detail::execute_begin(s, x->stmt, false).first};
        x->held = true;
    } else {
        x->f = detail::send_floating(*p_->st_, p_->opts_, s_, x->stmt);
    }
    sent_.push_back(x);
    return x;
}

std::optional<error> conn::settle_sent(bool held_only) {
    for (const auto& x : sent_) {
        if (held_only && !x->held) continue;
        detail::finish(*x);
    }
    std::optional<error> first;
    std::erase_if(sent_, [&](const std::shared_ptr<detail::sent>& x) {
        if ((held_only && !x->held) || !x->out) return false;
        if (!x->taken && x->out->failure && !first) first = *x->out->failure;
        return true;
    });
    return first;
}

[[gnu::always_inline]] inline detail::reply conn::run_text(std::string_view text, std::span<const detail::param> params,
                             detail::check_fn check, bool pins, detail::param_types* types) {
    const detail::statement st{text, params, check, types != nullptr};
    const bool held = keeps(pins);
    try {
        detail::outcome o = held ? detail::execute(own(pins), {&st, 1})
                                 : detail::run_floating(*p_->st_, p_->opts_, s_, st);
        if (o.failure) {
            if (held) {
                if (auto unread = settle_sent(true)) throw *unread;
            }
            throw std::move(*o.failure);
        }
        if (types && !o.types.empty()) *types = std::move(o.types.front());
        detail::column_desc cols = std::move(o.columns.front());
        return {std::move(o.results.front()), detail::columns_of(cols), std::move(cols), {}};
    } catch (error& e) {
        if (e.sql.empty()) e.sql = std::string(text);
        throw;
    }
}

detail::reply conn::run(const sql& q, detail::check_fn check) {
    rendered r = q.render();
    const bool pins = detail::pins_of(q, r.text);
    if (p_->opts_.prefetch && !p_->opts_.pooler) [[unlikely]] return prefetched(q, r, check, pins);
    return run_text(r.text, r.params, check, pins);
}

detail::reply conn::run_unscoped(const rendered& r, detail::check_fn check, bool pins, detail::param_types* types) {
    return run_text(r.text, r.params, check, pins, types);
}

bool conn::sending() const {
    return std::any_of(sent_.begin(), sent_.end(), [](const std::shared_ptr<detail::sent>& x) { return !x->out; });
}

namespace detail {
reply run_text(conn& c, std::string_view text, std::span<const param> params, check_fn check) {
    c.end_scope();
    return c.run_text(text, params, check, false);
}
}  // namespace detail

std::int64_t conn::exec(const sql& q) { return detail::affected_rows(run(q, nullptr).r.get()); }

bool conn::in_transaction() const { return s_ && s_->depth > 0; }

PGconn* conn::raw() {
    end_scope();
    detail::session& s = pin();
    detail::drain(s);
    return s.c;
}

transaction::transaction(conn& c) : c_(c), level_(0) {
    c.end_scope();
    detail::session& s = c.hold();
    level_ = s.depth + 1;
    if (level_ == 1 && !detail::idle(s)) throw error(error_kind::other, "transaction: connection is not idle");
    if (level_ == 1) s.begin_pending = true;
    else detail::run_plain(s, {"SAVEPOINT " + detail::savepoint(level_)});
    s.depth = level_;
}

transaction::~transaction() {
    if (!open_) return;
    detail::session& s = *c_.s_;
    s.depth = level_ - 1;
    c_.settle_sent(true);
    try {
        detail::roll_back(s, level_);
    } catch (const error&) {
        // The connection is broken or left in a transaction; the pool discards it.
    }
}

void transaction::commit() {
    detail::session& s = *c_.s_;
    if (!open_) throw error(error_kind::other, "transaction: already committed");
    if (s.depth != level_) throw error(error_kind::other, "transaction: an inner transaction is still open");
    open_ = false;
    s.depth = level_ - 1;
    if (level_ == 1 && std::exchange(s.begin_pending, false)) return;
    if (level_ == 1 && !c_.pinned_ && !detail::in_error(s) && !detail::is_broken(s)) {
        std::optional<error> commit_failed;
        try {
            detail::commit_floating(*c_.p_->st_, c_.p_->opts_, c_.s_);
        } catch (const error& e) {
            commit_failed = e;
        }
        if (auto unread = c_.settle_sent(true)) throw *unread;
        if (commit_failed) {
            if (commit_failed->kind == error_kind::broken || commit_failed->kind == error_kind::deadline)
                throw commit_unknown(commit_failed->what());
            throw *commit_failed;
        }
        return;
    }
    if (auto unread = c_.settle_sent(true)) {
        detail::roll_back(s, level_);
        throw *unread;
    }
    if (detail::in_error(s)) {
        detail::roll_back(s, level_);
        throw error(error_kind::other, "transaction: a statement failed; the transaction was rolled back");
    }
    if (level_ > 1) return detail::run_plain(s, {"RELEASE SAVEPOINT " + detail::savepoint(level_)});
    if (detail::is_broken(s)) detail::throw_unusable();
    try {
        detail::run_plain(s, {"COMMIT"});
    } catch (const error& e) {
        if (e.kind == error_kind::broken || e.kind == error_kind::deadline) throw commit_unknown(e.what());
        throw;
    }
}

}  // namespace pg
