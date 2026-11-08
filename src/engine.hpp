// The engine: every statement of libpg runs through execute(), on a connection
// in libpq pipeline mode. Internal to the library.
#pragma once
#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pg/pool.hpp"

#ifndef LIBPQ_HAS_PIPELINING
#error "libpg needs libpq 14 or newer (pipeline mode)"
#endif

namespace pg::detail {

// A prepared statement on the server: its name, result column types, and the
// descriptions of those columns once all are known.
struct prepared {
    std::string name;
    std::vector<oid> columns;
    std::vector<std::string> column_names;  // the result columns' names, in order
    std::vector<oid> param_types;           // the parameter types the server resolved
    std::vector<type_desc> described;  // the result column types, once all are known; views column_names; guarded by session::mu
};

// Cache key (statement text and parameter types) -> prepared statement, least
// recently used first out.
class stmt_cache {
public:
    static constexpr std::size_t capacity = 256;

    // The cached statement, which becomes the most recently used; null when absent.
    std::shared_ptr<prepared> find(std::string_view key);

    // Adds a statement; returns the name of the one it pushed out, if any.
    std::optional<std::string> add(std::string key, std::shared_ptr<prepared> p);

    std::optional<std::string> erase(std::string_view key);

private:
    struct entry {
        std::string key;
        std::shared_ptr<prepared> p;
    };
    std::list<entry> lru_;
    // The keys view the strings in lru_ nodes, which never move.
    std::unordered_map<std::string_view, std::list<entry>::iterator> index_;
};

// Types the server told us about, by OID; the descriptions view the names
// stored next to them.
class type_cache {
public:
    const type_desc* find(oid id) const;
    void add(oid id, std::string name, char category, char kind);

private:
    struct known {
        std::string name;
        type_desc d;
    };
    std::unordered_map<oid, known> types_;
};

// Parameter arrays for PQsendQueryPrepared.
struct bind_buffers {
    std::vector<const char*> values;
    std::vector<int> lengths, formats;
};

struct ticket;

// How busy a session's pipeline looks: written under session::mu by whoever moves its
// queue, read by the pool without that lock.
struct load {
    const ticket* head = nullptr;              // the front ticket as last recorded; guarded by session::mu
    std::atomic<std::int64_t> head_since{0};   // when the front round trip became the front; 0 when none
    std::atomic<std::int64_t> rtt_ns{0};       // moving average of full round trips of tickets that found the queue empty, 0 before the first
    std::atomic<std::int64_t> busy_until{0};   // latest due of the round trips in flight; 0 when none

    // Records when the front round trip became the front and the latest due of the
    // tickets in flight; the queue's front or contents changed.
    void queue_changed(const std::deque<ticket*>& queue);
    // The front ticket t completes at `now`: a ticket that found the queue empty feeds
    // rtt_ns, and its statement's learned duration is updated.
    void completed(const ticket& t, std::int64_t now);
    // When a ticket of learned slot `slot` sent at `sent` is expected to finish; 0 for slot < 0.
    static std::int64_t due(int slot, std::int64_t sent) {
        return slot < 0 ? 0 : sent + learned[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
    }
    // The learned slot of a statement's cache-key hash.
    static int slot_of(std::size_t hash) { return static_cast<int>(hash & (learned.size() - 1)); }
    // Likely running a slow statement: its front round trip has been the front, or its
    // known-slow statements are due to stay busy, for more than `limit`. Never slow
    // before any round trip was measured (`best` is the best rtt of the idle sessions).
    bool slow(std::int64_t now, std::int64_t best, std::int64_t limit) const {
        const std::int64_t front = head_since.load(std::memory_order_relaxed);
        const std::int64_t busy = busy_until.load(std::memory_order_relaxed);
        return best > 0 && ((front != 0 && now - front > limit) || busy - now > limit);
    }

    // Round-trip time of recent executions per statement text, by hash; collisions
    // only blur the estimate.
    static std::array<std::atomic<std::int64_t>, 4096> learned;
};

// A pooled connection: in pipeline and nonblocking mode, with its caches.
struct session {
    explicit session(PGconn* conn, std::chrono::milliseconds deadline_per_round_trip)
        : c(conn), deadline(deadline_per_round_trip) {}
    ~session() { PQfinish(c); }
    session(const session&) = delete;
    session& operator=(const session&) = delete;

    PGconn* c;
    stmt_cache cache;
    type_cache types;
    std::chrono::milliseconds deadline;  // per round trip; 0 = none
    std::uint64_t next_name = 0;
    std::vector<std::string> closes;  // statements to deallocate in front of the next operation
    bool begin_pending = false;       // a transaction started; BEGIN goes in front of the next statement
    bool dirty = false;               // a SET/RESET/DISCARD ran in this lease: the next lease starts with RESET ALL
    bool reset_pending = false;       // RESET ALL goes in front of the next statement
    int depth = 0;                    // open transaction guards
    bool broken = false;              // an operation stopped mid-pipeline: no further operation runs
    bool discard = false;             // the pool closes the connection instead of reusing it
    bool exclusive = false;           // leased to one borrower; guarded by mu
    std::chrono::steady_clock::time_point last_used = std::chrono::steady_clock::now();  // set by the pool on release

    // Round trips sent and not yet answered, oldest first; guarded by mu.
    std::mutex mu;                 // guards every member and every libpq call on c
    std::deque<struct ticket*> queue;
    bool reading = false;          // a thread is reading results for the queue
    // Owners: the pool's slot (1, from open_session) plus each statement in flight that
    // execute_begin or attach() counted. The last unref deletes the session.
    std::atomic<int> refs{1};
    load busy;

    // Buffers an operation fills and the next one reuses, keeping their capacity.
    bind_buffers bind;
};

// One round trip in flight: the results of its steps, filled by the reader.
struct ticket {
    int syncs = 0;                               // Syncs still to come
    int slot = -1;                               // learned_slot of its statement, -1 when not learned
    std::int64_t sent = 0;                       // steady_clock ns when it was queued
    std::int64_t due = 0;                        // sent + learned duration of its statement; else 0
    bool alone = false;                          // it was pushed onto an empty queue: its round trip is unqueued
    std::vector<result> got;
    std::optional<error> failure;                // the connection was lost or the deadline passed
    std::optional<std::chrono::steady_clock::time_point> until;  // deadline of this round trip
    std::atomic<int> state{0};                   // 0 waiting, 1 done, 2 asked to take over reading
    bool waited = false;                         // a thread sleeps on it; guarded by session::mu
};

std::int64_t now_ns();

inline void load::queue_changed(const std::deque<ticket*>& queue) {
    if (queue.empty()) {
        head = nullptr;
        head_since.store(0, std::memory_order_relaxed);
    } else if (queue.front() != head) {
        head = queue.front();
        head_since.store(std::max(head->sent, now_ns()), std::memory_order_relaxed);
    }
    std::int64_t busy = 0;
    for (const ticket* t : queue) busy = std::max(busy, t->due);
    busy_until.store(busy, std::memory_order_relaxed);
}

inline void load::completed(const ticket& t, std::int64_t now) {
    // A ticket pushed onto an empty queue waited for nothing: its round trip
    // is the network's, not the queue's, so it alone feeds rtt_ns.
    if (t.alone) {
        const std::int64_t sample = now - t.sent;
        const std::int64_t rtt = rtt_ns.load(std::memory_order_relaxed);
        rtt_ns.store(rtt == 0 ? sample : rtt + (sample - rtt) / 8, std::memory_order_relaxed);
    }
    if (t.slot < 0) return;
    // Folds the sample into the slot: the first sample, then an ewma of weight 1/4.
    std::atomic<std::int64_t>& l = learned[static_cast<std::size_t>(t.slot)];
    const std::int64_t sample = now - head_since.load(std::memory_order_relaxed);
    const std::int64_t cur = l.load(std::memory_order_relaxed);
    l.store(cur == 0 ? sample : cur + (sample - cur) / 4, std::memory_order_relaxed);
}

// One statement of an operation. The views must outlive execute(). With a
// `check`, the result column types are checked before anything executes.
struct statement {
    std::string_view sql;
    std::span<const param> params;
    check_fn check = nullptr;
    bool types = false;  // outcome::types gets the parameter types the server resolved
};

// The result column descriptions of one statement: a cached description shared
// with its prepared statement, or an owned vector for the uncached path.
using column_desc = std::shared_ptr<const std::vector<type_desc>>;

// The columns of a description, empty when the statement has none.
inline std::span<const type_desc> columns_of(const column_desc& c) {
    return c ? std::span<const type_desc>(*c) : std::span<const type_desc>{};
}

// What an operation produced: one result per statement, in order, with the
// column types of the checked statements. When a statement failed, `failure`
// is its error, `failed` its index, and the statements after it were not
// executed (their results are null).
struct outcome {
    std::vector<result> results;
    std::vector<column_desc> columns;
    std::vector<std::shared_ptr<const std::vector<oid>>> types;  // statement::types; empty when none asked
    std::size_t failed = 0;
    std::optional<error> failure;
    bool stale = false;  // the failed statement was stale and is no longer cached
};

// Runs the statements as one operation. Statements not yet prepared are
// first prepared and described in a round trip of their own, so that every
// check runs before anything executes; then all statements execute in one
// round trip ending in one Sync. A pending BEGIN and the closes of evicted
// statements travel in the first round trip. A statement the server no
// longer has, or whose result shape changed, is dropped from the cache; when
// the connection was idle the operation is retried once.
//
// Statement errors are returned. Client errors (unsendable parameters, a
// failed check) throw before anything executes; a lost connection or a
// passed deadline throws and marks the session broken.
outcome execute(session& s, std::span<const statement> stmts);

// A floating statement sent and not yet answered.
struct pending;
struct pending_deleter { void operator()(pending*) const; };
using pending_ptr = std::unique_ptr<pending, pending_deleter>;

// Sends one statement like execute() does (preparing it first if needed, in a
// round trip of its own) and returns without waiting for its execution
// round trip; counts one owner (ref) unless `attached`. Throws as execute()
// does. When the operation already finished (a failure in the prepare round),
// the pending carries the outcome and no ticket.
//
// With `attached`, the session is shared with other floating statements: it
// must already have the statement prepared and every checked type known, and
// no lease is taken; the result says whether it was sent, the session was
// leased meanwhile (`busy`), or a round trip would be needed (`needs_lease`).
// The caller retries elsewhere in the last two cases; refs stays untouched
// and the pending is null.
enum class attach_result { sent, busy, needs_lease };
std::pair<pending_ptr, attach_result> execute_begin(session& s, const statement& st, bool attached = false);

// Sends several statements in one round trip on a shared read-only session
// (one opened with default_transaction_read_only=on and a 250 ms
// statement_timeout; pool.cpp opens them), without a lease:
//   [prepare the statements not prepared there yet] Sync
//   the statements, Sync
// The server refuses every write in them, and the statement timeout bounds
// them. The session must report default_transaction_read_only=on when the
// operation is sent, or it is marked broken and discarded and nothing is sent
// (needs_lease). Nothing is described or checked: the outcome has results and
// no column descriptions. A statement error never marks the session for
// discard; after the operation lands, a session that no longer reports
// read-only, or is left in a transaction with nothing else queued, is given
// up. The caller counted itself as an owner; the statements must outlive
// execute_end.
std::pair<pending_ptr, attach_result> execute_begin_contained(session& s, std::span<const statement> stmts);

// Marks the session as leased to one borrower (or not), so attached
// statements leave it alone or may share it again. Locks mu.
void set_exclusive(session& s, bool on);

// Waits for the statement's results and applies them like execute(); returns
// the outcome. execute_begin counted one owner unless `attached`; the caller
// must unref once after execute_end (pool.cpp's settle() does it).
outcome execute_end(session& s, pending_ptr p);

// Counts one more owner.
inline void ref(session& s) { s.refs.fetch_add(1, std::memory_order_relaxed); }

// Drops one owner; deletes the session when it was the last.
void unref(session* s);

// Whether a statement may change or read state of its server session that a
// later statement on the same conn could depend on; such a statement keeps the
// conn on one connection. Only SELECT/INSERT/UPDATE/DELETE/WITH/VALUES/TABLE
// statements that mention none of advisory, set_config, currval, lastval,
// pg_backend_pid float.
bool pins_session(std::string_view sql);

// One empty round trip: whether the connection still answers.
bool ping(session& s);

// The steady clock in nanoseconds.
std::int64_t now_ns();

// Waits until every round trip in flight on the session is answered, so the
// caller may use the libpq connection directly.
void drain(session& s);

// The transaction on the session failed and waits for a rollback.
bool in_error(session& s);

// An earlier operation lost the connection or stopped mid-pipeline.
bool is_broken(session& s);

// Throws error_kind::broken for a session an earlier operation left unusable.
[[noreturn]] void throw_unusable();

// Runs a multi-statement script with the simple protocol: leaves pipeline
// mode for it and returns to it after. The connection is never reused. COPY
// and NUL characters in the script are rejected.
void run_script(session& s, std::string_view script);

// Runs one COPY FROM STDIN with the body given, leaving pipeline mode for it and
// entering it again before returning or throwing (unless the connection is
// lost). Drains the round trips in flight and sends a pending BEGIN first. The
// session stays reusable; returns the number of rows the server reported.
std::int64_t copy_in(session& s, std::string_view copy_sql, std::string_view body);

// No transaction is open or pending on the session.
bool idle(session& s);

// idle() for a caller already holding s.mu.
bool idle_locked(const session& s);

// The session may be lent to another borrower: not broken or discarded, the
// connection is up, and no transaction is open or pending.
bool reusable(session& s);

// The connection's last error message, without the trailing newline.
std::string conn_message(PGconn* c);

// Opens a session: connects (statement_timeout and `settings`, "-c name=value"
// items, travel in the startup options) and enters pipeline mode. The connect
// gives up at `until`, and at the DSN's connect_timeout if that is sooner.
// Throws error_kind::connect_failed.
session* open_session(const pool_options& o, std::chrono::steady_clock::time_point until,
                      std::string_view settings = {});

// The same, giving up after o.acquire_timeout.
session* open_session(const pool_options& o, std::string_view settings = {});

}  // namespace pg::detail
