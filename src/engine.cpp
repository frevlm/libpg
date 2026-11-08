#include "engine.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

namespace pg::detail {

std::shared_ptr<prepared> stmt_cache::find(std::string_view key) {
    const auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->p;
}

std::optional<std::string> stmt_cache::add(std::string key, std::shared_ptr<prepared> p) {
    // Prepared twice (two operations raced on a shared session): the new one is closed.
    if (index_.contains(key)) return std::move(p->name);
    lru_.push_front({std::move(key), std::move(p)});
    index_.emplace(lru_.front().key, lru_.begin());
    if (lru_.size() <= capacity) return std::nullopt;
    return erase(lru_.back().key);
}

std::optional<std::string> stmt_cache::erase(std::string_view key) {
    const auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    std::string name = std::move(it->second->p->name);
    const auto node = it->second;
    index_.erase(it);
    lru_.erase(node);
    return name;
}

const type_desc* type_cache::find(oid id) const {
    const auto it = types_.find(id);
    return it == types_.end() ? nullptr : &it->second.d;
}

void type_cache::add(oid id, std::string name, char category, char kind) {
    auto [it, fresh] = types_.try_emplace(id);
    if (!fresh) return;
    it->second.name = std::move(name);
    it->second.d = type_desc{id, it->second.name, category, kind};
}

std::string conn_message(PGconn* c) {
    std::string s = PQerrorMessage(c);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s.empty() ? "connection lost" : s;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool idle_locked(const session& s) {
    return !s.begin_pending && s.depth == 0 && (s.queue.empty() ? PQtransactionStatus(s.c) == PQTRANS_IDLE : true);
}

bool idle(session& s) {
    std::lock_guard lk(s.mu);
    return idle_locked(s);
}

bool reusable(session& s) {
    std::lock_guard lk(s.mu);
    return !s.broken && !s.discard && PQstatus(s.c) == CONNECTION_OK && idle_locked(s);
}

void set_exclusive(session& s, bool on) {
    std::lock_guard lk(s.mu);
    s.exclusive = on;
}

namespace {

using clock = std::chrono::steady_clock;

enum class step_kind { close, reset, begin, prepare, describe, execute, lookup };

// One protocol message of an operation that produces one result.
struct step {
    step_kind kind;
    std::size_t stmt = 0;  // the statement it belongs to; 0 for close, begin and lookup
    std::string name;      // the server side statement of a close or prepare
};

// The messages queued for one round trip and the Sync groups they form.
struct round {
    std::vector<step> steps;
    int syncs = 0;
};

// libpq 17 added PQsendPipelineSync (a Sync without a flush; drain() flushes) and
// PQsendClosePrepared. LIBPG_FORCE_LIBPQ16_PATH selects the older calls for testing.
#if defined(LIBPQ_HAS_SEND_PIPELINE_SYNC) && !defined(LIBPG_FORCE_LIBPQ16_PATH)
bool send_sync_message(PGconn* c) { return PQsendPipelineSync(c) == 1; }
#else
bool send_sync_message(PGconn* c) { return PQpipelineSync(c) == 1; }
#endif

#if defined(LIBPQ_HAS_CLOSE_PREPARED) && !defined(LIBPG_FORCE_LIBPQ16_PATH)
bool send_close_message(PGconn* c, const std::string& name) { return PQsendClosePrepared(c, name.c_str()) == 1; }
#else
// Statement names are s<number>: no quoting needed.
bool send_close_message(PGconn* c, const std::string& name) {
    const std::string sql = "DEALLOCATE " + name;
    return PQsendQueryParams(c, sql.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0) == 1;
}
#endif

// Marks the session broken and throws the connection's error.
[[noreturn]] void lost(session& s) {
    s.broken = true;
    s.discard = true;
    throw error(error_kind::broken, conn_message(s.c));
}

// Gives the session up: the round produced `got` results for `want` messages.
[[noreturn, gnu::noinline]] void out_of_step(session& s, std::size_t got, std::size_t want) {
    s.broken = true;
    s.discard = true;
    throw error(error_kind::other, "pipeline out of step: " + std::to_string(got) + " results for " +
                                       std::to_string(want) + " messages");
}

// Every message of the round produced exactly one result; otherwise the session is given up.
[[gnu::always_inline]] inline void check_in_step(session& s, std::size_t got, std::size_t want) {
    if (got != want) [[unlikely]] out_of_step(s, got, want);
}

// The deadline passed: cancels the running query and gives the connection up.
[[noreturn]] void expire(session& s) {
    if (PGcancel* k = PQgetCancel(s.c)) {
        char why[256];
        PQcancel(k, why, sizeof why);
        PQfreeCancel(k);
    }
    s.broken = true;
    s.discard = true;
    throw error(error_kind::deadline, "deadline passed; the query was cancelled");
}

void send_close(session& s, round& rd, std::string name) {
    if (!send_close_message(s.c, name)) lost(s);
    rd.steps.push_back({step_kind::close, 0, std::move(name)});
}

void send_sync(session& s, round& rd) {
    if (!send_sync_message(s.c)) lost(s);
    ++rd.syncs;
}

void send_begin(session& s, round& rd) {
    if (!PQsendQueryParams(s.c, "BEGIN", 0, nullptr, nullptr, nullptr, nullptr, 0)) lost(s);
    s.begin_pending = false;
    rd.steps.push_back({step_kind::begin, 0, {}});
}

void send_reset(session& s, round& rd) {
    if (!PQsendQueryParams(s.c, "RESET ALL", 0, nullptr, nullptr, nullptr, nullptr, 0)) lost(s);
    s.reset_pending = false;
    rd.steps.push_back({step_kind::reset, 0, {}});
}

// Queues a pending RESET ALL and the closes in a Sync group of their own, so
// that a failing close (DEALLOCATE of a missing name) cannot abort the
// statements; then a pending BEGIN.
void queue_preamble(session& s, round& rd) {
    if (s.reset_pending) send_reset(s, rd);
    for (std::string& name : std::exchange(s.closes, {})) send_close(s, rd, std::move(name));
    if (!rd.steps.empty()) send_sync(s, rd);
    if (s.begin_pending) send_begin(s, rd);
}

// When the current round trip must be over; nullopt without a deadline.
std::optional<clock::time_point> round_deadline(const session& s) {
    if (s.deadline.count() <= 0) return std::nullopt;
    return clock::now() + s.deadline;
}

// Waits until the socket is readable, or writable while output is pending;
// false when `until` passed. poll() only, so the caller may drop the lock.
bool wait_fd(int fd, bool output_pending, std::optional<clock::time_point> until) {
    for (;;) {
        int timeout = -1;
        if (until) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(*until - clock::now());
            if (left.count() <= 0) return false;
            timeout = static_cast<int>(left.count());
        }
        pollfd p{fd, static_cast<short>(POLLIN | (output_pending ? POLLOUT : 0)), 0};
        const int n = poll(&p, 1, timeout);
        if (n > 0) return true;
        if (n == 0) return false;
        if (errno != EINTR) return true;  // let PQconsumeInput report the error
    }
}

// wait_fd for a caller holding the lock: a passed deadline expires the session.
void wait_socket(session& s, bool output_pending, std::optional<clock::time_point> until) {
    if (!wait_fd(PQsocket(s.c), output_pending, until)) expire(s);
}

bool is_copy(ExecStatusType status) {
    return status == PGRES_COPY_IN || status == PGRES_COPY_OUT || status == PGRES_COPY_BOTH;
}

// A COPY never ends by itself: the connection is given up.
[[noreturn]] void copy_unsupported(session& s) {
    s.broken = true;
    s.discard = true;
    throw error(error_kind::other, "COPY is not supported");
}

// Enqueues the round's ticket and flushes; the round's last Sync was already sent.
[[gnu::always_inline]] inline void enqueue(session& s, const round& rd, ticket& t) {
    t.syncs = rd.syncs;
    t.until = round_deadline(s);
    t.sent = now_ns();
    t.due = load::due(t.slot, t.sent);
    t.alone = s.queue.empty();
    s.queue.push_back(&t);
    s.busy.queue_changed(s.queue);
    if (PQflush(s.c) < 0) {
        s.queue.pop_back();
        s.busy.queue_changed(s.queue);
        lost(s);
    }
}

// Pops the front ticket, marks it done and wakes its owner.
void complete(session& s) {
    ticket* t = s.queue.front();
    const std::int64_t now = now_ns();
    s.busy.completed(*t, now);
    s.queue.pop_front();
    s.busy.queue_changed(s.queue);
    t->state.store(1, std::memory_order_release);
    t->state.notify_one();
}

// Every ticket in the queue fails with e (the connection is gone).
void fail_all(session& s, const error& e) {
    while (!s.queue.empty()) {
        ticket* t = s.queue.front();
        s.queue.pop_front();
        t->failure = e;
        t->state.store(1, std::memory_order_release);
        t->state.notify_one();
    }
    s.busy.queue_changed(s.queue);
}

// Moves ready results to the tickets at the front of the queue, completing
// those whose last Sync arrived.
void dispatch(session& s) {
    int nulls = 0;
    while (!s.queue.empty() && !PQisBusy(s.c)) {
        result r{PQgetResult(s.c)};
        if (!r) {
            if (PQstatus(s.c) == CONNECTION_BAD || ++nulls > 1) lost(s);
            continue;
        }
        nulls = 0;
        if (is_copy(PQresultStatus(r.get()))) copy_unsupported(s);
        ticket& t = *s.queue.front();
        if (PQresultStatus(r.get()) != PGRES_PIPELINE_SYNC) t.got.push_back(std::move(r));
        else if (--t.syncs == 0) complete(s);
    }
}

// Reads until `t` is done; the caller holds lk and has set s.reading.
void read_until(session& s, std::unique_lock<std::mutex>& lk, ticket& t) {
    for (;;) {
        const int unsent = PQflush(s.c);
        if (unsent < 0) lost(s);
        dispatch(s);
        if (t.state.load(std::memory_order_relaxed) == 1) return;
        const auto until = s.queue.front()->until;  // the oldest round trip's deadline
        const int fd = PQsocket(s.c);
        lk.unlock();
        const bool ready = wait_fd(fd, unsent == 1, until);
        lk.lock();
        if (!ready) expire(s);
        if (!PQconsumeInput(s.c)) lost(s);
    }
}

// Waits for t: as the reader when nobody reads, else asleep until done or
// handed the reading.
void await_ticket(session& s, std::unique_lock<std::mutex>& lk, ticket& t) {
    for (;;) {
        if (t.state.load(std::memory_order_acquire) == 1) break;
        if (!s.reading) {
            s.reading = true;
            try {
                read_until(s, lk, t);
            } catch (const error& e) {
                s.reading = false;
                fail_all(s, e);  // includes t
                break;
            }
            s.reading = false;
            break;
        }
        int v = 2;
        t.state.compare_exchange_strong(v, 0);
        t.waited = true;
        lk.unlock();
        t.state.wait(0, std::memory_order_acquire);
        lk.lock();
    }
    // Hand the reading to the oldest ticket a thread sleeps on, if any. Tickets
    // ahead of it may have nobody waiting yet: a statement sent with conn::send
    // is read only when its result is asked for.
    if (!s.reading) {
        for (ticket* n : s.queue) {
            if (!n->waited) continue;
            int w = 0;
            if (n->state.compare_exchange_strong(w, 2)) n->state.notify_one();
            break;
        }
    }
    if (t.failure) throw *t.failure;
}

// Sends the round's last Sync, enqueues the ticket and waits for its results.
std::vector<result> finish(std::unique_lock<std::mutex>& lk, session& s, round& rd) {
    send_sync(s, rd);
    ticket t;
    enqueue(s, rd, t);
    await_ticket(s, lk, t);
    check_in_step(s, t.got.size(), rd.steps.size());
    return std::move(t.got);
}

bool succeeded(ExecStatusType status) {
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_EMPTY_QUERY;
}

bool has_state(const PGresult* r, const char* want) {
    const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    return state && std::strcmp(state, want) == 0;
}

// The error of a failed result: SQLSTATE and primary message, the statement apart.
error error_of(session& s, const PGresult* r, std::string_view sql, bool may_discard = true) {
    const char* primary = PQresultErrorField(r, PG_DIAG_MESSAGE_PRIMARY);
    std::string msg = primary ? primary : PQresStatus(PQresultStatus(r));
    const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    const error_kind kind = state ? classify(state) : error_kind::other;
    if (may_discard && dispose(kind).discard_conn) s.discard = true;
    return error(kind, msg, state ? state : "", std::string(sql));
}

std::optional<std::string> unsendable(const statement& st) {
    if (st.sql.find('\0') != std::string_view::npos) return "statement text contains a NUL character";
    const std::span<const param> ps = st.params;
    if (ps.size() > 65535) return "more than 65535 parameters";
    for (const param& p : ps) {
        if (!p.binary && p.bytes && p.bytes->find('\0') != std::string::npos)
            return "text parameter contains a NUL character";
    }
    return std::nullopt;
}

// The statement cache key: the text and the parameter type OIDs, which are
// part of the prepared statement.
void cache_key(const statement& st, std::string& k) {
    k.assign(st.sql);
    k += '\0';
    for (const param& p : st.params) k.append(reinterpret_cast<const char*>(&p.type), sizeof p.type);
}

// The cache key of a statement, in a buffer reused across calls; valid until
// the next call.
std::string_view cache_key(const statement& st) {
    thread_local std::string buf;
    cache_key(st, buf);
    return buf;
}

// The cache key hash of every statement.
void hashes_of(std::span<const statement> stmts, std::vector<std::size_t>& hashes) {
    hashes.resize(stmts.size());
    for (std::size_t i = 0; i < stmts.size(); ++i)
        hashes[i] = std::hash<std::string_view>{}(cache_key(stmts[i]));
}

// Everything one run of an operation works with.
struct op {
    std::span<const statement> stmts;
    std::vector<std::string> keys;  // owned keys of the cache misses; empty for a hit
    std::vector<std::size_t> hashes;
    std::vector<bool> fresh;  // prepared and described by this run; empty when nothing was
    outcome o;
    bool contained = false;   // see execute_begin_contained

    bool is_fresh(std::size_t i) const { return !fresh.empty() && fresh[i]; }
};

// Records the first failure of the operation.
void fail_step(session& s, op& w, const step& st, const PGresult* r) {
    if (w.o.failure) return;
    w.o.failure = error_of(s, r, w.stmts[st.stmt].sql, !w.contained);
    w.o.failed = st.stmt;
    w.o.stale = st.kind == step_kind::execute && (has_state(r, "26000") || (has_state(r, "0A000") && !w.is_fresh(st.stmt)));
    if (!w.o.stale) return;
    w.o.failure->kind = error_kind::stale_statement;
    // Statement gone (26000) needs no close; one whose result changed does.
    std::string key;
    cache_key(w.stmts[st.stmt], key);
    auto name = s.cache.erase(key);
    if (name && !has_state(r, "26000")) s.closes.push_back(std::move(*name));
}

// A close is done once it succeeded or the statement does not exist; otherwise
// (aborted, or refused inside an aborted transaction) it is queued again.
void settle_close(session& s, const step& st, const PGresult* r) {
    if (!succeeded(PQresultStatus(r)) && !has_state(r, "26000")) s.closes.push_back(st.name);
}

void remember(session& s, op& w, const step& st, const PGresult* r) {
    auto p = std::make_shared<prepared>();
    p->name = st.name;
    p->columns.reserve(static_cast<std::size_t>(PQnfields(r)));
    for (int i = 0; i < PQnfields(r); ++i) p->columns.push_back(PQftype(r, i));
    p->param_types.reserve(static_cast<std::size_t>(PQnparams(r)));
    for (int i = 0; i < PQnparams(r); ++i) p->param_types.push_back(PQparamtype(r, i));
    if (auto evicted = s.cache.add(std::move(w.keys[st.stmt]), std::move(p))) s.closes.push_back(std::move(*evicted));
}

// The statement changed session state that outlives the lease: its command tag
// is SET, RESET or DISCARD ... (SET LOCAL reports SET too and costs one
// needless reset). A set_config() call inside a SELECT is not seen.
bool changes_session(const PGresult* r) {
    const std::string_view tag = PQcmdStatus(const_cast<PGresult*>(r));
    return tag == "SET" || tag == "RESET" || tag.starts_with("DISCARD");
}

// Applies one step's result to the session and the operation.
void apply(session& s, op& w, const step& st, result r) {
    if (st.kind == step_kind::close) return settle_close(s, st, r.get());
    if (st.kind == step_kind::reset) {
        if (!succeeded(PQresultStatus(r.get()))) s.discard = true;
        return;
    }
    if (!succeeded(PQresultStatus(r.get()))) return fail_step(s, w, st, r.get());
    if (st.kind == step_kind::describe) remember(s, w, st, r.get());
    if (st.kind != step_kind::execute) return;
    if (changes_session(r.get())) s.dirty = true;
    w.o.results[st.stmt] = std::move(r);
}

void apply_all(session& s, op& w, const round& rd, std::vector<result> got) {
    for (std::size_t k = 0; k < got.size(); ++k) apply(s, w, rd.steps[k], std::move(got[k]));
}

void send_prepare(session& s, round& rd, std::size_t i, const statement& st, std::string name) {
    std::vector<Oid> types;
    types.reserve(st.params.size());
    for (const param& p : st.params) types.push_back(p.type);
    const std::string sql(st.sql);
    const int n = static_cast<int>(types.size());
    if (!PQsendPrepare(s.c, name.c_str(), sql.c_str(), n, types.data())) lost(s);
    if (!PQsendDescribePrepared(s.c, name.c_str())) lost(s);
    rd.steps.push_back({step_kind::prepare, i, name});
    rd.steps.push_back({step_kind::describe, i, std::move(name)});
}

// Prepares and describes, in one round trip, every statement not cached yet.
// Owned keys are kept only for the misses; a hit is looked up by the buffer.
void prepare_misses(std::unique_lock<std::mutex>& lk, session& s, op& w) {
    std::vector<std::size_t> misses;
    for (std::size_t i = 0; i < w.stmts.size(); ++i) {
        const std::string_view key = cache_key(w.stmts[i]);
        if (s.cache.find(key)) continue;
        bool seen = false;
        for (std::size_t m : misses) seen = seen || std::string_view(w.keys[m]) == key;
        if (seen) continue;
        if (w.keys.size() != w.stmts.size()) w.keys.resize(w.stmts.size());
        w.keys[i] = std::string(key);
        misses.push_back(i);
    }
    if (misses.empty()) return;
    w.fresh.resize(w.stmts.size());
    for (std::size_t i = 0; i < w.stmts.size(); ++i) {
        const std::string_view key = cache_key(w.stmts[i]);
        for (std::size_t m : misses) w.fresh[i] = w.fresh[i] || std::string_view(w.keys[m]) == key;
    }
    round rd;
    queue_preamble(s, rd);
    for (std::size_t i : misses) send_prepare(s, rd, i, w.stmts[i], "s" + std::to_string(++s.next_name));
    apply_all(s, w, rd, finish(lk, s, rd));
}

// Asks the server for the pg_type rows of the given OIDs.
void look_up_types(std::unique_lock<std::mutex>& lk, session& s, const std::vector<oid>& ids) {
    std::string list = "{";
    for (oid id : ids) list += (list.size() > 1 ? "," : "") + std::to_string(id);
    list += '}';
    const char* values[] = {list.c_str()};
    round rd;
    if (!PQsendQueryParams(s.c,
                           "SELECT oid::int8, typname::text, typcategory::text, typtype::text "
                           "FROM pg_type WHERE oid = ANY($1::oid[])",
                           1, nullptr, values, nullptr, nullptr, 0))
        lost(s);
    rd.steps.push_back({step_kind::lookup, 0, {}});
    const result r = std::move(finish(lk, s, rd).front());
    if (!succeeded(PQresultStatus(r.get()))) throw error_of(s, r.get(), {});
    for (int i = 0; i < PQntuples(r.get()); ++i) {
        const auto id = static_cast<oid>(std::strtoul(PQgetvalue(r.get(), i, 0), nullptr, 10));
        s.types.add(id, PQgetvalue(r.get(), i, 1), *PQgetvalue(r.get(), i, 2), *PQgetvalue(r.get(), i, 3));
    }
    // A type the server no longer has: remembered as unknown, never asked again.
    for (oid id : ids) s.types.add(id, "unknown", 0, 0);
}

const type_desc* known_type(const session& s, oid id) {
    if (const type_desc* t = builtin_type(id)) return t;
    return s.types.find(id);
}

// The column types of every checked statement, asking the server about
// types it did not describe to this session before, and remembering them.
void describe_columns(std::unique_lock<std::mutex>& lk, session& s, op& w,
                      std::span<const std::shared_ptr<prepared>> preps) {
    std::vector<oid> unknown;
    for (std::size_t i = 0; i < w.stmts.size(); ++i) {
        if (!w.stmts[i].check || !preps[i]->described.empty()) continue;
        for (oid id : preps[i]->columns)
            if (!known_type(s, id)) unknown.push_back(id);
    }
    if (!unknown.empty()) look_up_types(lk, s, unknown);
    for (std::size_t i = 0; i < w.stmts.size(); ++i) {
        if (!w.stmts[i].types) continue;
        w.o.types.resize(w.stmts.size());
        w.o.types[i] = std::shared_ptr<const std::vector<oid>>(preps[i], &preps[i]->param_types);
    }
    for (std::size_t i = 0; i < w.stmts.size(); ++i) {
        if (!w.stmts[i].check) continue;
        prepared& p = *preps[i];
        if (!p.described.empty()) {
            w.o.columns[i] = column_desc(preps[i], &p.described);
            continue;
        }
        std::vector<type_desc> built;
        built.reserve(p.columns.size());
        bool all = true;
        for (oid id : p.columns) {
            const type_desc* t = known_type(s, id);
            if (!t) {
                all = false;
                continue;
            }
            built.push_back(*t);
        }
        if (all) {
            p.described = std::move(built);
            w.o.columns[i] = column_desc(preps[i], &p.described);
        } else {
            w.o.columns[i] = std::make_shared<const std::vector<type_desc>>(std::move(built));
        }
    }
}

// The first failed check: the statement's index and why.
struct check_failure {
    std::size_t stmt;
    std::string why;
};

std::optional<check_failure> check_all(const op& w) {
    for (std::size_t i = 0; i < w.stmts.size(); ++i) {
        const statement& st = w.stmts[i];
        if (!st.check) continue;
        if (auto bad = st.check(columns_of(w.o.columns[i]))) return check_failure{i, std::move(*bad)};
    }
    return std::nullopt;
}

void fill(bind_buffers& b, std::span<const param> ps) {
    b.values.clear();
    b.lengths.clear();
    b.formats.clear();
    for (const param& p : ps) {
        b.values.push_back(p.bytes ? p.bytes->data() : nullptr);
        b.lengths.push_back(p.bytes ? static_cast<int>(p.bytes->size()) : 0);
        b.formats.push_back(p.binary ? 1 : 0);
    }
}

void send_execute(session& s, round& rd, std::size_t i, const statement& st, const std::string& name) {
    bind_buffers& b = s.bind;
    fill(b, st.params);
    const int n = static_cast<int>(b.values.size());
    if (!PQsendQueryPrepared(s.c, name.c_str(), n, b.values.data(), b.lengths.data(), b.formats.data(), 1)) lost(s);
    rd.steps.push_back({step_kind::execute, i, {}});
}

// Sends the execution round and enqueues its ticket.
void send_execution(session& s, op& w, std::span<const std::shared_ptr<prepared>> preps, round& rd, ticket& t) {
    rd.steps.reserve(w.stmts.size() + 1);
    queue_preamble(s, rd);
    for (std::size_t i = 0; i < w.stmts.size(); ++i) send_execute(s, rd, i, w.stmts[i], preps[i]->name);
    send_sync(s, rd);
    enqueue(s, rd, t);
}

void execute_all(std::unique_lock<std::mutex>& lk, session& s, op& w,
                 std::span<const std::shared_ptr<prepared>> preps) {
    round rd;
    ticket t;
    send_execution(s, w, preps, rd, t);
    await_ticket(s, lk, t);
    check_in_step(s, t.got.size(), rd.steps.size());
    apply_all(s, w, rd, std::move(t.got));
}

// The prepared statement of every statement. More distinct statements than
// the cache holds cannot run as one operation.
std::vector<std::shared_ptr<prepared>> resolve(session& s, const op& w) {
    std::vector<std::shared_ptr<prepared>> preps;
    preps.reserve(w.stmts.size());
    for (const statement& st : w.stmts) {
        std::shared_ptr<prepared> p = s.cache.find(cache_key(st));
        if (!p) throw error(error_kind::other, "operation has more distinct statements than the statement cache holds");
        preps.push_back(std::move(p));
    }
    return preps;
}

// Prepares what is missing, describes and checks the result shapes. A check
// that fails against a cached description (the table may have changed since)
// describes the statement again once before throwing; nothing executes.
std::vector<std::shared_ptr<prepared>> prepare_and_check(std::unique_lock<std::mutex>& lk, session& s, op& w,
                                                         bool may_redescribe = true) {
    prepare_misses(lk, s, w);
    if (w.o.failure) return {};
    std::vector<std::shared_ptr<prepared>> preps = resolve(s, w);
    describe_columns(lk, s, w, preps);
    if (auto bad = check_all(w)) {
        if (!may_redescribe || w.is_fresh(bad->stmt))
            throw error(error_kind::other, bad->why, {}, std::string(w.stmts[bad->stmt].sql));
        std::string key;
        cache_key(w.stmts[bad->stmt], key);
        if (auto name = s.cache.erase(key)) s.closes.push_back(std::move(*name));
        w.o.columns.assign(w.stmts.size(), {});
        w.fresh.clear();
        return prepare_and_check(lk, s, w, false);
    }
    return preps;
}

// Whether every result column type of the prepared statement is known.
bool types_known(const session& s, const prepared& p) {
    for (oid id : p.columns)
        if (!known_type(s, id)) return false;
    return true;
}

// Whether an attached statement can run without a round trip: prepared on this
// session, and every checked result type already known.
bool attach_ready(session& s, const op& w) {
    for (std::size_t i = 0; i < w.stmts.size(); ++i) {
        std::shared_ptr<prepared> p = s.cache.find(cache_key(w.stmts[i]));
        if (!p) return false;
        if (w.stmts[i].check && !types_known(s, *p)) return false;
    }
    return true;
}

// Resolves and checks an attached statement, with no round trip; returns busy
// when the session was leased meanwhile, needs_lease when it is not ready.
attach_result attach_prepare(std::unique_lock<std::mutex>& lk, session& s, op& w,
                             std::vector<std::shared_ptr<prepared>>& preps) {
    if (s.exclusive) return attach_result::busy;
    if (!attach_ready(s, w)) return attach_result::needs_lease;
    preps = resolve(s, w);
    describe_columns(lk, s, w, preps);
    if (check_all(w)) return attach_result::needs_lease;
    return attach_result::sent;
}

// Runs the operation.
outcome run_once(std::unique_lock<std::mutex>& lk, session& s, std::span<const statement> stmts) {
    op w{stmts, {}, {}, {}, {}, false};
    hashes_of(stmts, w.hashes);
    w.o.results.resize(stmts.size());
    w.o.columns.resize(stmts.size());
    w.o.failed = stmts.size();
    std::vector<std::shared_ptr<prepared>> preps = prepare_and_check(lk, s, w);
    if (w.o.failure) return std::move(w.o);
    execute_all(lk, s, w, preps);
    return std::move(w.o);
}

// Waits until a result is ready outside pipeline mode.
void await_result(session& s, std::optional<clock::time_point> until) {
    for (;;) {
        const int unsent = PQflush(s.c);
        if (unsent < 0) lost(s);
        if (unsent == 0 && !PQisBusy(s.c)) return;
        wait_socket(s, unsent == 1, until);
        if (!PQconsumeInput(s.c)) lost(s);
    }
}

}  // namespace

std::array<std::atomic<std::int64_t>, 4096> load::learned{};

struct pending {
    op w;
    round rd;
    ticket t;           // never moves: the session queue points at it
    bool done = false;  // the prepare round already decided the outcome
};

void pending_deleter::operator()(pending* p) const { delete p; }

outcome execute(session& s, std::span<const statement> stmts) {
    for (const statement& st : stmts) {
        if (auto why = unsendable(st)) throw error(error_kind::other, *why, {}, std::string(st.sql));
    }
    std::unique_lock lk(s.mu);
    if (s.broken) throw_unusable();
    if (stmts.empty()) return {};
    const bool was_idle = idle_locked(s);
    outcome o = run_once(lk, s, stmts);
    if (!o.stale || !was_idle) return o;
    return run_once(lk, s, stmts);
}

namespace {

std::pair<pending_ptr, attach_result> begin_op(session& s, std::span<const statement> stmts, bool attached) {
    for (const statement& st : stmts) {
        if (auto why = unsendable(st)) throw error(error_kind::other, *why, {}, std::string(st.sql));
    }
    std::unique_lock lk(s.mu);
    if (s.broken) throw_unusable();
    auto p = std::make_unique<pending>();
    p->w.stmts = stmts;
    hashes_of(p->w.stmts, p->w.hashes);
    p->w.o.results.resize(stmts.size());
    p->w.o.columns.resize(stmts.size());
    p->w.o.failed = stmts.size();
    std::vector<std::shared_ptr<prepared>> preps;
    if (attached) {
        const attach_result how = attach_prepare(lk, s, p->w, preps);
        if (how != attach_result::sent) return {nullptr, how};
    } else {
        preps = prepare_and_check(lk, s, p->w);
    }
    if (!attached) ref(s);  // an attached caller counted itself in attach()
    try {
        if (p->w.o.failure) {
            p->done = true;
            return {pending_ptr(p.release()), attach_result::sent};
        }
        p->t.slot = load::slot_of(p->w.hashes.front());
        send_execution(s, p->w, preps, p->rd, p->t);
        return {pending_ptr(p.release()), attach_result::sent};
    } catch (...) {
        if (!attached) s.refs.fetch_sub(1, std::memory_order_relaxed);
        throw;
    }
}

}  // namespace

std::pair<pending_ptr, attach_result> execute_begin(session& s, const statement& st, bool attached) {
    return begin_op(s, std::span<const statement>(&st, 1), attached);
}

namespace {

// Whether the server reports the session read-only by default (the startup
// option of a speculation session). A function that sets the parameter to off
// shows here. Needs mu.
bool reports_read_only(const session& s) {
    const char* v = PQparameterStatus(s.c, "default_transaction_read_only");
    return v && std::string_view(v) == "on";
}

}  // namespace

std::pair<pending_ptr, attach_result> execute_begin_contained(session& s, std::span<const statement> stmts) {
    for (const statement& st : stmts) {
        if (auto why = unsendable(st)) throw error(error_kind::other, *why, {}, std::string(st.sql));
    }
    std::unique_lock lk(s.mu);
    if (s.broken) throw_unusable();
    if (!reports_read_only(s)) {
        s.broken = true;
        s.discard = true;
        return {nullptr, attach_result::needs_lease};
    }
    auto p = std::make_unique<pending>();
    op& w = p->w;
    w.stmts = stmts;
    w.contained = true;
    w.o.results.resize(stmts.size());
    w.o.columns.resize(stmts.size());
    w.o.failed = stmts.size();
    w.keys.resize(stmts.size());
    std::vector<std::string> names(stmts.size());
    round& rd = p->rd;
    queue_preamble(s, rd);
    bool prepares = false;
    for (std::size_t i = 0; i < stmts.size(); ++i) {
        const std::string key(cache_key(stmts[i]));
        if (const auto cached = s.cache.find(key)) {
            names[i] = cached->name;
            continue;
        }
        for (std::size_t j = 0; j < i && names[i].empty(); ++j)
            if (w.keys[j] == key) names[i] = names[j];
        if (!names[i].empty()) continue;
        w.keys[i] = key;
        names[i] = "s" + std::to_string(++s.next_name);
        send_prepare(s, rd, i, stmts[i], names[i]);
        prepares = true;
    }
    if (prepares) send_sync(s, rd);
    for (std::size_t i = 0; i < stmts.size(); ++i) send_execute(s, rd, i, stmts[i], names[i]);
    send_sync(s, rd);
    enqueue(s, rd, p->t);
    return {pending_ptr(p.release()), attach_result::sent};
}

outcome execute_end(session& s, pending_ptr p) {
    std::unique_lock lk(s.mu);
    if (!p->done) {
        await_ticket(s, lk, p->t);
        check_in_step(s, p->t.got.size(), p->rd.steps.size());
        apply_all(s, p->w, p->rd, std::move(p->t.got));
        // A contained operation runs on a read-only session and leaves no
        // transaction open; a session that stopped reporting read-only, or that
        // nothing else is queued on and still is in a transaction, is given up.
        if (p->w.contained && (!reports_read_only(s) || (s.queue.empty() && PQtransactionStatus(s.c) != PQTRANS_IDLE))) {
            s.broken = true;
            s.discard = true;
        }
    }
    return std::move(p->w.o);
}

void unref(session* s) {
    if (s->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete s;
}

bool pins_session(std::string_view sql) { return pins_text(sql); }

void drain(session& s) {
    std::unique_lock lk(s.mu);
    if (s.queue.empty()) return;
    if (s.broken) throw_unusable();
    round rd;
    finish(lk, s, rd);
}

bool in_error(session& s) {
    std::lock_guard lk(s.mu);
    return PQtransactionStatus(s.c) == PQTRANS_INERROR;
}

bool is_broken(session& s) {
    std::lock_guard lk(s.mu);
    return s.broken;
}

[[noreturn]] void throw_unusable() {
    throw error(error_kind::broken, "connection is unusable: an earlier operation failed mid-pipeline");
}

bool ping(session& s) {
    std::unique_lock lk(s.mu);
    if (s.broken) return false;
    try {
        round rd;
        finish(lk, s, rd);
        return true;
    } catch (const error&) {
        return false;
    }
}

void run_script(session& s, std::string_view script) {
    std::unique_lock lk(s.mu);
    s.discard = true;
    if (s.broken) throw_unusable();
    if (script.find('\0') != std::string_view::npos)
        throw error(error_kind::other, "script contains a NUL character");
    // Drain every round trip still in flight first: the simple protocol
    // cannot share the connection with them.
    {
        round rd;
        finish(lk, s, rd);
    }
    const std::string text(script);
    if (!PQexitPipelineMode(s.c) || !PQsendQuery(s.c, text.c_str())) lost(s);
    std::optional<error> first;
    for (;;) {
        await_result(s, std::nullopt);
        const result r{PQgetResult(s.c)};
        if (!r) break;
        if (is_copy(PQresultStatus(r.get()))) copy_unsupported(s);
        if (!first && !succeeded(PQresultStatus(r.get()))) first = error_of(s, r.get(), script);
    }
    if (!PQenterPipelineMode(s.c)) lost(s);
    if (first) throw *first;
}

namespace {

// Drains the round trips in flight, then a pending preamble, so the simple
// protocol starts on a settled session.
void drain_and_preamble(session& s, std::unique_lock<std::mutex>& lk) {
    round rd;
    finish(lk, s, rd);
    round pre;
    queue_preamble(s, pre);
    if (!pre.steps.empty()) finish(lk, s, pre);
}

// Sends the body in chunks, flushing whenever the socket is full.
void put_body(session& s, std::string_view body, std::optional<clock::time_point> until) {
    std::size_t off = 0;
    while (off < body.size()) {
        const int n = static_cast<int>(std::min<std::size_t>(65536, body.size() - off));
        const int put = PQputCopyData(s.c, body.data() + off, n);
        if (put < 0) lost(s);
        if (put == 0) {
            if (PQflush(s.c) < 0) lost(s);
            wait_socket(s, true, until);
            continue;
        }
        off += static_cast<std::size_t>(n);
    }
    for (;;) {
        const int flushed = PQflush(s.c);
        if (flushed == 0) break;
        if (flushed < 0) lost(s);
        wait_socket(s, true, until);
    }
}

// Ends the COPY, flushing until the end message is out.
void end_copy(session& s, std::optional<clock::time_point> until) {
    for (;;) {
        const int put = PQputCopyEnd(s.c, nullptr);
        if (put < 0) lost(s);
        if (put == 1) break;
        if (PQflush(s.c) < 0) lost(s);
        wait_socket(s, true, until);
    }
}

// Reads the results after the COPY command's first one: the row count of the
// last COMMAND_OK and the first failure.
std::pair<std::int64_t, std::optional<error>> read_copy_results(session& s, std::optional<clock::time_point> until,
                                                               std::string_view copy_sql, std::int64_t count = 0,
                                                               std::optional<error> failure = {}) {
    for (;;) {
        await_result(s, until);
        const result r{PQgetResult(s.c)};
        if (!r) return {count, std::move(failure)};
        if (is_copy(PQresultStatus(r.get()))) continue;
        if (PQresultStatus(r.get()) == PGRES_COMMAND_OK) count = std::strtoll(PQcmdTuples(r.get()), nullptr, 10);
        else if (!failure) failure = error_of(s, r.get(), copy_sql);
    }
}

}  // namespace

std::int64_t copy_in(session& s, std::string_view copy_sql, std::string_view body) {
    std::unique_lock lk(s.mu);
    if (s.broken) throw_unusable();
    drain_and_preamble(s, lk);
    if (!PQexitPipelineMode(s.c)) lost(s);
    const std::string text(copy_sql);
    if (!PQsendQuery(s.c, text.c_str())) lost(s);
    const auto until = round_deadline(s);
    await_result(s, until);
    const result first{PQgetResult(s.c)};
    if (!first) lost(s);
    std::int64_t count = 0;
    std::optional<error> failure;
    if (PQresultStatus(first.get()) == PGRES_COPY_IN) {
        put_body(s, body, until);
        end_copy(s, until);
    } else if (PQresultStatus(first.get()) == PGRES_COMMAND_OK) {
        count = std::strtoll(PQcmdTuples(first.get()), nullptr, 10);
    } else {
        failure = error_of(s, first.get(), copy_sql);
    }
    auto [rows, more] = read_copy_results(s, until, copy_sql, count, std::move(failure));
    if (!PQenterPipelineMode(s.c)) lost(s);
    if (more) throw *more;
    return rows;
}

namespace {

struct conninfo_deleter {
    void operator()(PQconninfoOption* o) const { PQconninfoFree(o); }
};
using conninfo = std::unique_ptr<PQconninfoOption, conninfo_deleter>;

// The DSN's value for a keyword, empty when unset.
std::string dsn_value(const PQconninfoOption* opts, const char* keyword) {
    for (const PQconninfoOption* o = opts; o->keyword; ++o) {
        if (std::strcmp(o->keyword, keyword) == 0 && o->val) return o->val;
    }
    return {};
}

conninfo parse_dsn(const std::string& dsn) {
    char* msg = nullptr;
    conninfo info{PQconninfoParse(dsn.c_str(), &msg)};
    if (info) return info;
    std::string why = msg ? msg : "out of memory";
    PQfreemem(msg);
    while (!why.empty() && why.back() == '\n') why.pop_back();
    throw error(error_kind::connect_failed, why);
}

// The DSN's options followed by the pool's statement_timeout, synchronous_commit
// and the caller's settings. These are the session's RESET values.
std::string startup_options(const std::string& dsn_options, std::chrono::milliseconds statement_timeout,
                            bool synchronous_commit, std::string_view settings) {
    std::string ours = "-c statement_timeout=" + std::to_string(statement_timeout.count());
    if (!synchronous_commit) ours += " -c synchronous_commit=off";
    if (!settings.empty()) ours += " " + std::string(settings);
    return dsn_options.empty() ? ours : dsn_options + " " + ours;
}

// The earlier of `until` and the DSN's connect_timeout (seconds) from now.
clock::time_point connect_bound(const std::string& connect_timeout, clock::time_point until) {
    const long secs = std::strtol(connect_timeout.c_str(), nullptr, 10);
    if (secs <= 0) return until;
    return std::min(until, clock::now() + std::chrono::seconds(secs));
}

[[noreturn]] void connect_failed(PGconn* c, const std::string& why) {
    error e(error_kind::connect_failed, why);
    PQfinish(c);
    throw e;
}

// Drives PQconnectPoll until the connection is up or `until` passes.
void finish_connect(PGconn* c, clock::time_point until) {
    PostgresPollingStatusType st = PGRES_POLLING_WRITING;
    while (st != PGRES_POLLING_OK) {
        if (st == PGRES_POLLING_FAILED) connect_failed(c, conn_message(c));
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(until - clock::now());
        if (left.count() <= 0) connect_failed(c, "connection attempt timed out");
        pollfd p{PQsocket(c), static_cast<short>(st == PGRES_POLLING_READING ? POLLIN : POLLOUT), 0};
        if (p.fd < 0) connect_failed(c, conn_message(c));
        const int n = poll(&p, 1, static_cast<int>(std::min<long long>(left.count(), 1'000'000)));
        if (n < 0 && errno != EINTR) connect_failed(c, "poll failed while connecting");
        if (n != 0) st = PQconnectPoll(c);
    }
}

}  // namespace

session* open_session(const pool_options& o, clock::time_point until, std::string_view settings) {
    const conninfo info = parse_dsn(o.dsn);
    const std::string options =
        o.pooler ? std::string(dsn_value(info.get(), "options"))
                 : startup_options(dsn_value(info.get(), "options"), o.statement_timeout, o.synchronous_commit, settings);
    const char* keys[] = {"dbname", "options", nullptr};
    const char* values[] = {o.dsn.c_str(), options.c_str(), nullptr};
    PGconn* c = PQconnectStartParams(keys, values, 1);
    if (!c) throw error(error_kind::connect_failed, "out of memory");
    if (PQstatus(c) == CONNECTION_BAD) connect_failed(c, conn_message(c));
    finish_connect(c, connect_bound(dsn_value(info.get(), "connect_timeout"), until));
    if (!PQenterPipelineMode(c) || PQsetnonblocking(c, 1) != 0) connect_failed(c, conn_message(c));
    return new session(c, o.deadline);
}

session* open_session(const pool_options& o, std::string_view settings) {
    return open_session(o, clock::now() + o.acquire_timeout, settings);
}

}  // namespace pg::detail
