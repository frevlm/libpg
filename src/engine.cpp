#include "engine.hpp"

#include <poll.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace pg::detail {

const std::string* stmt_cache::find(std::string_view sql) {
    const auto it = index_.find(sql);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return &it->second->name;
}

std::optional<std::string> stmt_cache::add(std::string sql, std::string name) {
    lru_.push_front({std::move(sql), std::move(name)});
    index_.emplace(lru_.front().sql, lru_.begin());
    if (lru_.size() <= capacity) return std::nullopt;
    return erase(lru_.back().sql);
}

std::optional<std::string> stmt_cache::erase(std::string_view sql) {
    const auto it = index_.find(sql);
    if (it == index_.end()) return std::nullopt;
    std::string name = std::move(it->second->name);
    const auto node = it->second;
    index_.erase(it);
    lru_.erase(node);
    return name;
}

std::string conn_message(PGconn* c) {
    std::string s = PQerrorMessage(c);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s.empty() ? "connection lost" : s;
}

bool idle(const session& s) { return !s.begin_pending && PQtransactionStatus(s.c) == PQTRANS_IDLE; }

std::optional<std::string> unsendable(const params& p) {
    if (p.size() > 65535) return "more than 65535 parameters";
    for (const auto& v : p) {
        if (v && v->find('\0') != std::string::npos) return "parameter contains a NUL character";
    }
    return std::nullopt;
}

namespace {

enum class step_kind { close, begin, prepare, execute };

// One protocol message of an operation that produces one result.
struct step {
    step_kind kind;
    std::size_t stmt = 0;  // the statement it belongs to; 0 for close and begin
    std::string name;      // the server side statement of a close or prepare
};

// What an operation queued, in order. `misses` maps the text of each statement
// prepared by the operation to its new name; `syncs` counts the Sync groups.
struct plan {
    std::vector<step> steps;
    std::unordered_map<std::string_view, std::string> misses;
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
    throw error(conn_message(s.c));
}

// Marks the session broken unless the operation read everything up to its Sync.
struct pipeline_guard {
    session& s;
    bool drained = false;
    ~pipeline_guard() {
        if (!drained) s.broken = true;
    }
};

void send_close(session& s, plan& pl, std::string name) {
    if (!send_close_message(s.c, name)) lost(s);
    pl.steps.push_back({step_kind::close, 0, std::move(name)});
}

void send_sync(session& s, plan& pl) {
    if (!send_sync_message(s.c)) lost(s);
    ++pl.syncs;
}

void send_begin(session& s, plan& pl) {
    if (!PQsendQueryParams(s.c, "BEGIN", 0, nullptr, nullptr, nullptr, nullptr, 0)) lost(s);
    s.begin_pending = false;
    pl.steps.push_back({step_kind::begin, 0, {}});
}

// The name statement `i` executes under: cached, prepared earlier in this
// operation, or prepared now.
const std::string& statement_name(session& s, plan& pl, std::size_t i, const statement& st) {
    if (const std::string* name = s.cache.find(st.sql)) return *name;
    const auto [it, fresh] = pl.misses.try_emplace(st.sql);
    if (!fresh) return it->second;
    it->second = "s" + std::to_string(++s.next_name);
    const std::string sql(st.sql);
    const int n = static_cast<int>(st.p->size());
    if (!PQsendPrepare(s.c, it->second.c_str(), sql.c_str(), n, nullptr)) lost(s);
    pl.steps.push_back({step_kind::prepare, i, it->second});
    return it->second;
}

void send_execute(session& s, plan& pl, std::size_t i, const statement& st, std::vector<const char*>& values) {
    const std::string& name = statement_name(s, pl, i, st);
    values.clear();
    for (const auto& v : *st.p) values.push_back(v ? v->c_str() : nullptr);
    const int n = static_cast<int>(values.size());
    if (!PQsendQueryPrepared(s.c, name.c_str(), n, values.data(), nullptr, nullptr, 0)) lost(s);
    pl.steps.push_back({step_kind::execute, i, {}});
}

// Queues the closes in a Sync group of their own, so that a failing close
// (DEALLOCATE of a missing name) cannot abort the statements; then the pending
// BEGIN, every statement and their Sync. Both groups travel in one round trip.
plan queue(session& s, std::span<const statement> stmts) {
    plan pl;
    pl.steps.reserve(s.closes.size() + 1 + stmts.size());
    for (std::string& name : std::exchange(s.closes, {})) send_close(s, pl, std::move(name));
    if (!pl.steps.empty()) send_sync(s, pl);
    if (s.begin_pending) send_begin(s, pl);
    std::vector<const char*> values;
    for (std::size_t i = 0; i < stmts.size(); ++i) send_execute(s, pl, i, stmts[i], values);
    send_sync(s, pl);
    return pl;
}

// Waits until the socket is readable, or writable while output is pending.
void wait_socket(session& s, bool output_pending) {
    pollfd p{PQsocket(s.c), static_cast<short>(POLLIN | (output_pending ? POLLOUT : 0)), 0};
    while (poll(&p, 1, -1) < 0) {
        if (errno != EINTR) lost(s);
    }
}

// Moves every result that is ready into `got`; true once the last Sync arrived.
// A NULL ends one step's results; two in a row mean nothing is queued.
bool take_ready(session& s, std::vector<result>& got, int& syncs) {
    int nulls = 0;
    while (!PQisBusy(s.c)) {
        result r{PQgetResult(s.c)};
        if (!r) {
            if (PQstatus(s.c) == CONNECTION_BAD || ++nulls > 1) lost(s);
            continue;
        }
        nulls = 0;
        if (PQresultStatus(r.get()) != PGRES_PIPELINE_SYNC) got.push_back(std::move(r));
        else if (--syncs == 0) return true;
    }
    return false;
}

// Reads the result of every step up to the last Sync, flushing queued output
// meanwhile: the server never waits for us to read while we wait for it to read.
std::vector<result> drain(session& s, std::size_t steps, int syncs) {
    std::vector<result> got;
    got.reserve(steps);
    for (;;) {
        const int unsent = PQflush(s.c);
        if (unsent < 0) lost(s);
        if (take_ready(s, got, syncs)) break;
        wait_socket(s, unsent == 1);
        if (!PQconsumeInput(s.c)) lost(s);
    }
    if (got.size() != steps) {
        s.broken = true;
        throw error("pipeline out of step: " + std::to_string(got.size()) + " results for " +
                    std::to_string(steps) + " messages");
    }
    return got;
}

bool succeeded(ExecStatusType status) { return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK; }

bool has_state(const PGresult* r, const char* want) {
    const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    return state && std::strcmp(state, want) == 0;
}

error error_of(const PGresult* r) {
    std::string msg = PQresultErrorMessage(r);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    if (msg.empty()) msg = PQresStatus(PQresultStatus(r));
    const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    return error(msg, state ? state : "");
}

// The server no longer has the statement (26000) or its result shape changed (0A000).
bool stale(const PGresult* r) { return has_state(r, "26000") || has_state(r, "0A000"); }

void remember(session& s, std::string_view sql, const std::string& name) {
    if (auto evicted = s.cache.add(std::string(sql), name)) s.closes.push_back(std::move(*evicted));
}

// Drops a stale statement from the cache; it is closed later unless the server
// already lost it (26000).
void forget(session& s, std::string_view sql, const PGresult* why) {
    auto name = s.cache.erase(sql);
    if (name && !has_state(why, "26000")) s.closes.push_back(std::move(*name));
}

// A close is done once it succeeded or the statement does not exist; otherwise
// (aborted, or refused inside an aborted transaction) it is queued again.
void settle_close(session& s, const step& st, const PGresult* r) {
    if (!succeeded(PQresultStatus(r)) && !has_state(r, "26000")) s.closes.push_back(st.name);
}

// Records the first failure of the operation.
void fail_step(session& s, const step& st, std::span<const statement> stmts, const PGresult* r, outcome& o) {
    o.failure = error_of(r);
    o.failed = st.stmt;
    o.stale = st.kind == step_kind::execute && stale(r);
    if (o.stale) forget(s, stmts[st.stmt].sql, r);
}

// Applies one step's result to the session and the outcome.
void apply(session& s, const step& st, std::span<const statement> stmts, result r, outcome& o) {
    if (st.kind == step_kind::close) return settle_close(s, st, r.get());
    const ExecStatusType status = PQresultStatus(r.get());
    if (!succeeded(status)) {
        if (!o.failure) fail_step(s, st, stmts, r.get(), o);
        return;
    }
    if (st.kind == step_kind::prepare) remember(s, stmts[st.stmt].sql, st.name);
    if (st.kind == step_kind::execute) o.results[st.stmt] = std::move(r);
}

outcome run_once(session& s, std::span<const statement> stmts) {
    pipeline_guard guard{s};
    const plan pl = queue(s, stmts);
    std::vector<result> got = drain(s, pl.steps.size(), pl.syncs);
    guard.drained = true;
    outcome o;
    o.results.resize(stmts.size());
    o.failed = stmts.size();
    for (std::size_t k = 0; k < got.size(); ++k) apply(s, pl.steps[k], stmts, std::move(got[k]), o);
    return o;
}

}  // namespace

outcome execute(session& s, std::span<const statement> stmts) {
    for (const statement& st : stmts) {
        if (auto why = unsendable(*st.p)) throw error(*why);
    }
    if (s.broken) throw error("connection is unusable: an earlier operation failed mid-pipeline");
    if (stmts.empty()) return {};
    const bool was_idle = idle(s);
    outcome o = run_once(s, stmts);
    if (!o.stale || !was_idle) return o;
    return run_once(s, stmts);
}

}  // namespace pg::detail
