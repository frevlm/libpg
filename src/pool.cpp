#include <poll.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <random>
#include <thread>
#include <unordered_map>

#include "pg/pool.hpp"

#if !defined(LIBPQ_HAS_PIPELINING)
#error "libpg needs libpq 14 or newer (pipeline mode)"
#endif

namespace pg {
namespace detail {

// A server-side prepared statement and its result columns.
struct prepared {
    std::string name;
    std::vector<oid> types;
    std::vector<std::string> names;
    check_fn checked = nullptr;  // the last check that passed
};

// One connection, nonblocking and in pipeline mode, with the statements prepared
// on it. Several threads may queue tickets on it at once; `mu` guards the
// connection and everything from `reading` to `formats`, the pool mutex guards
// `users` and `reserved`.
struct wire {
    explicit wire(PGconn* conn) : c(conn) {}
    ~wire() { PQfinish(c); }

    PGconn* c;
    std::mutex mu;
    std::condition_variable cv;  // a ticket was filled or the reader left poll()
    bool reading = false;        // a thread waits in poll() on the socket
    int nulls = 0;               // NULL results in a row
    std::uint64_t names = 0;     // prepared statements are s1, s2, ...
    std::uint64_t clears = 0;    // times the cache was cleared
    std::unordered_map<std::string, prepared> cache;  // by ticket key
    std::deque<std::shared_ptr<ticket>> queue;  // round trips sent and not yet read
    std::vector<const char*> values;            // bind buffers, reused
    std::vector<int> lengths, formats;
    std::atomic<bool> broken{false};  // lost or out of step: never used again
    bool raw = false;                 // handed out by conn::raw()
    bool shared = false;              // the queued tickets are of conns sharing it, one statement each
    int users = 0;                    // threads sharing it for a statement each; -1 while one conn holds it
    bool reserved = false;            // shared by nobody new: it goes to a waiter once its users are done
};

// A borrower waiting for a wire.
struct waiter {
    bool exclusive;
    wire* got = nullptr;
    std::condition_variable cv;
};

namespace {

// Runs f when the scope ends.
template <class F>
struct on_exit {
    F f;
    ~on_exit() { f(); }
};

// The statement text of a ticket key: what precedes its NUL.
std::string text_of(const std::string& key) { return key.c_str(); }

// The statement text, then NUL and the parameter type OIDs.
std::string key_of(std::string text, std::span<const param> params) {
    if (text.find('\0') != std::string::npos) throw error(error_kind::other, "statement text holds a NUL byte");
    text.push_back('\0');
    for (const auto& p : params) put_be(p.type, 4, text);
    return text;
}

std::string trimmed(std::string m) {
    while (!m.empty() && (m.back() == '\n' || m.back() == ' ')) m.pop_back();
    return m;
}

std::string message(PGconn* c) { return trimmed(PQerrorMessage(c)); }

int command(PGconn* c, const std::string& text) {
    return PQsendQueryParams(c, text.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 1);
}

error server_error(const PGresult* r, const std::string& key) {
    const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    const char* primary = PQresultErrorField(r, PG_DIAG_MESSAGE_PRIMARY);
    const std::string s = state ? state : "", m = primary ? primary : trimmed(PQresultErrorMessage(r));
    return error(classify(s, m), m, s, text_of(key));
}

// The wire cannot be used any more: every ticket on it is done, failing with
// `why` unless it holds its own error.
bool lose(wire& w, error_kind kind, const std::string& why) {
    w.broken = true;
    for (const auto& t : w.queue) {
        if (!t->err) t->err.emplace(kind, why, std::string(), text_of(t->key));
        t->done = true;
    }
    w.queue.clear();
    w.cv.notify_all();
    return false;
}

// Reads what has arrived without blocking and fills tickets in queue order, each
// up to its Sync; then pushes pending output. Returns whether output is left. A
// FATAL or PANIC error ends the session; a shared statement that opens a
// transaction would put the others' statements into it, so both lose the wire.
bool pump(wire& w) {
    if (!PQconsumeInput(w.c)) return lose(w, error_kind::broken, message(w.c));
    while (!w.queue.empty() && !PQisBusy(w.c)) {
        ticket& t = *w.queue.front();
        PGresult* r = PQgetResult(w.c);
        if (!r) {  // between two commands; twice in a row means nothing more is coming
            if (PQstatus(w.c) != CONNECTION_OK || ++w.nulls > 1) return lose(w, error_kind::broken, message(w.c));
            continue;
        }
        w.nulls = 0;
        const ExecStatusType st = PQresultStatus(r);
        if (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK || st == PGRES_EMPTY_QUERY) {
            t.res.reset(r);
            continue;
        }
        const char* sev = PQresultErrorField(r, PG_DIAG_SEVERITY_NONLOCALIZED);
        const std::string fatal = sev && std::string_view(sev) != "ERROR" ? trimmed(PQresultErrorMessage(r)) : "";
        if (st == PGRES_FATAL_ERROR && !t.err && fatal.empty()) t.err = server_error(r, t.key);
        PQclear(r);
        if (!fatal.empty() || PQstatus(w.c) != CONNECTION_OK)
            return lose(w, error_kind::broken, fatal.empty() ? message(w.c) : fatal);
        if (st != PGRES_PIPELINE_SYNC && st != PGRES_FATAL_ERROR && st != PGRES_PIPELINE_ABORTED)
            return lose(w, error_kind::other, std::string("unexpected result status ") + PQresStatus(st));
        if (st != PGRES_PIPELINE_SYNC) continue;
        const std::string_view tag = t.res ? PQcmdStatus(t.res.get()) : "";
        if (w.shared && (tag == "BEGIN" || tag == "START TRANSACTION")) {
            t.err.emplace(error_kind::other, "a statement opened a transaction outside pg::transaction", std::string(),
                          text_of(t.key));
            return lose(w, error_kind::broken, "the connection was closed: a statement sharing it opened a transaction");
        }
        t.done = true;
        if (t.err && t.err->kind == error_kind::stale_statement) w.cache.erase(t.key);
        w.queue.pop_front();
    }
    const int left = PQflush(w.c);
    return left < 0 ? lose(w, error_kind::broken, message(w.c)) : left > 0;
}

// Waits until t is filled. One thread at a time polls the socket and fills
// whichever tickets arrive; the others wait for it to fill theirs or to leave.
void drive(wire& w, ticket& t) {
    std::unique_lock lk(w.mu);
    w.cv.wait(lk, [&] { return t.done || !w.reading; });
    if (t.done) return;
    for (;;) {  // the poller pumps and notifies every time it leaves poll()
        const bool out = pump(w);
        w.cv.notify_all();
        if (t.done) return;
        pollfd fd{PQsocket(w.c), static_cast<short>(POLLIN | (out ? POLLOUT : 0)), 0};
        w.reading = true;
        lk.unlock();
        poll(&fd, 1, -1);
        lk.lock();
        w.reading = false;
    }
}

// Reads every ticket queued on a held wire; returns the first error among them.
std::optional<error> drain(wire& w) {
    std::unique_lock lk(w.mu);
    const auto unread = w.queue;
    lk.unlock();
    if (!unread.empty()) drive(w, *unread.back());
    for (const auto& t : unread)
        if (t->err) return t->err;
    return std::nullopt;
}

// The pools this thread has a live conn of.
thread_local std::vector<const pool*> live_pools;

}  // namespace

const PGresult* await(conn* c, ticket& t) {
    if (!t.done) {
        drive(*c->w_, t);
        c->settle();
    }
    if (t.err) throw *t.err;
    return t.res.get();
}

std::string count_error(std::size_t got, std::size_t want) {
    return "query returned " + std::to_string(got) + " columns, expected " + std::to_string(want);
}

std::string column_error(std::size_t i, const std::string& name, oid type, std::string_view cxx_type) {
    return "column " + std::to_string(i + 1) + " \"" + name + "\" has type OID " + std::to_string(type) +
           ", which " + std::string(cxx_type) + " does not read";
}

void row_count_error(int rows, const std::string& key) {
    if (rows == 0) {
        not_found e("query returned no row");
        e.sql = text_of(key);
        throw e;
    }
    throw error(error_kind::other, "query returned " + std::to_string(rows) + " rows, expected one",
                std::string(), text_of(key));
}

std::int64_t affected_of(const PGresult* r) {
    return std::strtoll(PQcmdTuples(const_cast<PGresult*>(r)), nullptr, 10);
}

void backoff(int attempt) {
    thread_local std::minstd_rand rng(std::random_device{}());
    const int full_us = 5'000 << std::min(attempt - 1, 10);
    std::this_thread::sleep_for(
        std::chrono::microseconds(std::uniform_int_distribution<int>(full_us / 2, full_us)(rng)));
}

std::int64_t exec_on(conn& c, const sql& q) { return c.exec(q); }

std::int64_t copy_on(conn& c, std::string_view table, std::span<const copy_column> cols, const std::string& data) {
    if (cols.empty()) throw error(error_kind::other, "insert_all: no column to insert, every field is unset");
    const on_exit s{[&c] { c.settle(); }};
    sql names;
    for (const auto& col : cols) names += (names.empty() ? sql{} : sql{", "}) + ident(col.name);
    const std::string key = key_of(sql{"SELECT {} FROM {} LIMIT 0", names, ident(table)}.render().text, {});
    // A cached description that does not fit may be stale: describe the columns again once.
    for (bool fresh = false;; fresh = true) {
        wire& w = c.hold();
        std::unique_lock lk(w.mu);
        if (fresh) w.cache.erase(key);
        const std::vector<oid>& types = c.lookup(w, lk, key, {}, nullptr).types;
        std::size_t k = 0;
        while (k < cols.size() && cols[k].accepts(types[k])) ++k;
        if (k == cols.size()) break;
        if (fresh) throw error(error_kind::other, "insert_all: " + column_error(k, std::string(cols[k].name), types[k], cols[k].type));
    }
    const std::string text = sql{"COPY {} ({}) FROM STDIN (FORMAT binary)", ident(table), names}.render().text;
    wire& w = c.quiet();
    std::unique_ptr<PGresult, result_deleter> r(PQexec(w.c, text.c_str()));
    if (PQresultStatus(r.get()) == PGRES_COPY_IN) {
        bool ok = true;
        for (std::size_t at = 0, chunk = 1 << 20; ok && at < data.size(); at += chunk)
            ok = PQputCopyData(w.c, data.data() + at, static_cast<int>(std::min(chunk, data.size() - at))) == 1;
        if (ok) PQputCopyEnd(w.c, nullptr);
        r.reset(PQgetResult(w.c));
        while (PGresult* more = PQgetResult(w.c)) PQclear(more);
    }
    w.broken = PQstatus(w.c) != CONNECTION_OK;  // nothing else is queued on it
    if (w.broken) throw error(error_kind::broken, message(w.c), std::string(), text);
    if (PQresultStatus(r.get()) == PGRES_COMMAND_OK) return affected_of(r.get());
    w.cache.erase(key);  // the description may be stale: the next call describes the columns again
    throw server_error(r.get(), text);
}

}  // namespace detail

namespace detail {

// A pool's wires and the borrowers waiting for one, under one mutex.
struct lot {
    const pool_options& o;
    std::mutex mu{};
    std::vector<std::unique_ptr<wire>> wires{};
    std::deque<waiter*> waiters{};  // oldest first
    int open = 0;                 // wires and the ones being opened
    std::uint64_t borrows = 0;

    // The wire a new borrower may take: for a shared one the least used, for an
    // exclusive one an unused one; never one held, reserved or lost.
    wire* lightest(bool exclusive) const {
        wire* best = nullptr;
        for (const auto& w : wires)
            if (w->users >= 0 && !w->reserved && !w->broken && (!exclusive || w->users == 0) &&
                (!best || w->users < best->users))
                best = w.get();
        return best;
    }

    wire* take(wire* w, bool exclusive) {
        ++borrows;
        w->users = exclusive ? -1 : w->users + 1;
        return w;
    }

    // Lets the oldest waiter look again: a slot was freed or the queue moved.
    void wake() { if (!waiters.empty()) waiters.front()->cv.notify_one(); }

    // Hands w to the oldest waiters: to shared ones while they come first, to an
    // exclusive one when nobody uses w; otherwise reserves w for that one. Then
    // the waiter now oldest looks again.
    void serve(wire* w) {
        while (!waiters.empty() && w->users >= 0) {
            waiter* h = waiters.front();
            if (h->exclusive && w->users > 0) {
                w->reserved = true;
                break;
            }
            waiters.pop_front();
            h->got = take(w, h->exclusive);
            h->cv.notify_one();
        }
        wake();
    }

    // Opens a wire (its slot counted while the lock is released) and takes it.
    wire* connect(std::unique_lock<std::mutex>& lk, bool exclusive) {
        ++open;
        lk.unlock();
        auto w = std::make_unique<wire>(PQconnectdb(o.dsn.c_str()));
        const bool ok = PQstatus(w->c) == CONNECTION_OK && PQsetnonblocking(w->c, 1) == 0 && PQenterPipelineMode(w->c);
        lk.lock();
        if (!ok) {
            --open;
            wake();
            throw error(error_kind::connect_failed, message(w->c));
        }
        wires.push_back(std::move(w));
        wire* got = take(wires.back().get(), exclusive);
        serve(got);
        return got;
    }

    // A shared borrower takes an unused wire, else opens one while open <
    // max_conns, else shares the least used. An exclusive one takes an unused
    // wire, else opens one, else waits and reserves the least used; waiters are
    // served oldest first.
    wire* acquire(bool exclusive) {
        std::unique_lock lk(mu);
        waiter me{exclusive, nullptr, {}};
        const auto deadline = std::chrono::steady_clock::now() + o.acquire_timeout;
        for (bool queued = false; !me.got;) {
            if (!queued || waiters.front() == &me) {
                wire* w = lightest(exclusive);
                const bool room = open < o.max_conns;
                if (w && w->users > 0 && room) w = nullptr;  // a new wire before sharing a busy one
                if (w || room) {
                    if (queued) waiters.pop_front(), wake();
                    return w ? take(w, exclusive) : connect(lk, exclusive);
                }
            }
            if (!queued) {
                queued = true;
                waiters.push_back(&me);
                if (wire* busy = exclusive ? lightest(false) : nullptr) busy->reserved = true;
            }
            if (me.cv.wait_until(lk, deadline) == std::cv_status::timeout && !me.got) {
                std::erase(waiters, &me);
                wake();
                throw error(error_kind::other, "no connection free within acquire_timeout (" +
                                                   std::to_string(o.acquire_timeout.count()) + " ms)");
            }
        }
        return me.got;
    }

    // Gives back a shared or held wire; one nobody uses goes to the waiters, or is
    // closed when it is lost, not idle, or was handed out by raw().
    void release(wire* w, bool exclusive) {
        std::unique_ptr<wire> dead;  // closed after the lock is released
        std::lock_guard lk(mu);
        w->users = exclusive ? 0 : w->users - 1;
        if (w->users > 0) return;
        w->reserved = false;
        if (!w->broken && !w->raw && PQstatus(w->c) == CONNECTION_OK && PQtransactionStatus(w->c) == PQTRANS_IDLE)
            return serve(w);
        const auto it = std::ranges::find_if(wires, [&](const auto& x) { return x.get() == w; });
        dead = std::move(*it);
        wires.erase(it);
        --open;
        wake();
    }
};

}  // namespace detail

pool::pool(pool_options o) : opts_(std::move(o)), lot_(new detail::lot{opts_}) {}
pool::~pool() = default;

pool_stats pool::stats() const {
    std::lock_guard lk(lot_->mu);
    const auto idle = std::ranges::count_if(lot_->wires, [](const auto& w) { return w->users == 0; });
    return {lot_->open, static_cast<int>(idle), static_cast<int>(lot_->waiters.size()), lot_->borrows};
}

conn::conn(pool& p) : p_(&p) {
    if (std::ranges::find(detail::live_pools, &p) != detail::live_pools.end())
        throw nested_borrow("this thread already holds a conn of this pool");
    detail::live_pools.push_back(&p);
}

conn::~conn() {
    if (w_) {
        detail::drain(*w_);
        p_->lot_->release(w_, true);
    }
    std::erase(detail::live_pools, p_);
}

detail::wire& conn::hold() {
    if (!w_) w_ = p_->lot_->acquire(true);
    return *w_;
}

void conn::settle() {
    const auto empty = [this] {
        std::lock_guard lk(w_->mu);
        return w_->queue.empty();
    };
    if (w_ && sent_ == 0 && !w_->raw && empty()) p_->lot_->release(std::exchange(w_, nullptr), true);
}

// Sends the guards' pending BEGIN / SAVEPOINTs, what `send` sends, and a Sync;
// the caller holds w.mu.
template <class F>
std::shared_ptr<detail::ticket> conn::queue(detail::wire& w, std::string key, F&& send) {
    if (w.broken) throw error(error_kind::broken, "the connection was lost", std::string(), detail::text_of(key));
    auto t = std::make_shared<detail::ticket>();
    t->key = std::move(key);
    w.shared = w_ != &w;
    bool ok = PQsetnonblocking(w.c, 1) == 0 && PQenterPipelineMode(w.c);
    for (; ok && sent_ < depth_; ++sent_)
        ok = detail::command(w.c, sent_ == 0 ? "BEGIN" : "SAVEPOINT libpg_" + std::to_string(sent_ + 1));
    ok = ok && send(w.c) && PQpipelineSync(w.c);
    if (ok) {
        w.queue.push_back(t);
        return t;
    }
    // libpq refused it: t fails alone. What libpq buffered may end in part of t, so
    // the wire takes no new ticket and is closed once the tickets before t are read.
    const bool lost = PQstatus(w.c) != CONNECTION_OK;
    t->err.emplace(lost ? error_kind::broken : error_kind::other, detail::message(w.c), std::string(),
                   detail::text_of(t->key));
    t->done = true;
    w.broken = true;
    if (lost) detail::lose(w, error_kind::broken, detail::message(w.c));
    return t;
}

// The checked prepared statement of `key` on w; on a miss it is prepared and
// described first (w.mu is released while that round trip is read).
detail::prepared& conn::lookup(detail::wire& w, std::unique_lock<std::mutex>& lk, const std::string& key,
                               std::span<const detail::param> params, detail::check_fn check) {
    for (bool fresh = false;; fresh = true) {
        if (const auto it = w.cache.find(key); it != w.cache.end()) {
            detail::prepared& p = it->second;
            const std::string bad = check && p.checked != check ? check(p.types, p.names) : std::string();
            if (bad.empty()) {
                if (check) p.checked = check;
                return p;
            }
            if (fresh) throw error(error_kind::other, bad, std::string(), detail::text_of(key));
            w.cache.erase(it);  // a cached description that fails the check may be stale: describe it again
        }
        if (w.cache.size() >= 1000) {
            w.cache.clear();
            ++w.clears;
            queue(w, "DEALLOCATE ALL", [](PGconn* c) { return detail::command(c, "DEALLOCATE ALL"); });
        }
        std::vector<oid> types;
        for (const auto& p : params) types.push_back(p.type);
        const std::string name = "s" + std::to_string(++w.names);
        const std::uint64_t clears = w.clears;
        const auto t = queue(w, key, [&](PGconn* c) {
            return PQsendPrepare(c, name.c_str(), key.c_str(), static_cast<int>(types.size()), types.data()) &&
                   PQsendDescribePrepared(c, name.c_str());
        });
        // On a held wire the tickets before t are this conn's: the earliest failure is the one thrown.
        const auto seq = w_ == &w ? w.queue : std::deque{t};
        lk.unlock();
        detail::drive(w, *t);
        lk.lock();
        if (t->err) {
            const auto first = std::ranges::find_if(seq, [](const auto& e) { return e->err.has_value(); });
            throw first == seq.end() ? *t->err : *(*first)->err;
        }
        if (w.clears != clears) continue;  // DEALLOCATE ALL went out after it: prepare again
        detail::prepared p{name, {}, {}, nullptr};
        for (int i = 0; i < PQnfields(t->res.get()); ++i) {
            p.types.push_back(PQftype(t->res.get(), i));
            p.names.emplace_back(PQfname(t->res.get(), i));
        }
        w.cache.insert_or_assign(key, std::move(p));
    }
}

// A statement read at once on a conn that holds no wire shares a wire for as
// long as it runs; anything else runs on the conn's held wire.
std::shared_ptr<detail::ticket> conn::start(const sql& q, detail::check_fn check, bool now) {
    const rendered r = q.render();
    if (r.params.size() > 65535)
        throw error(error_kind::other, "a statement takes at most 65535 parameters", std::string(), r.text);
    std::string key = detail::key_of(r.text, r.params);
    const bool shared = !w_ && depth_ == 0 && now;
    detail::wire& w = shared ? *p_->lot_->acquire(false) : hold();
    const detail::on_exit done{[&] { shared ? p_->lot_->release(&w, false) : settle(); }};
    std::unique_lock lk(w.mu);
    const detail::prepared& p = lookup(w, lk, key, r.params, check);
    w.values.clear();
    w.lengths.clear();
    w.formats.assign(r.params.size(), 1);
    for (const auto& prm : r.params) {
        w.values.push_back(prm.bytes ? prm.bytes->data() : nullptr);
        w.lengths.push_back(prm.bytes ? static_cast<int>(prm.bytes->size()) : 0);
    }
    const auto t = queue(w, std::move(key), [&](PGconn* c) {
        return PQsendQueryPrepared(c, p.name.c_str(), static_cast<int>(r.params.size()), w.values.data(),
                                   w.lengths.data(), w.formats.data(), 1);
    });
    lk.unlock();
    if (now) detail::drive(w, *t);
    return t;
}

// The held wire with every statement read and the guards' BEGIN / SAVEPOINT
// sent, out of pipeline mode and blocking.
detail::wire& conn::quiet() {
    detail::wire& w = hold();
    if (sent_ < depth_) {
        std::lock_guard lk(w.mu);
        queue(w, "", [](PGconn*) { return 1; });
    }
    detail::drain(w);
    if (w.broken) throw error(error_kind::broken, "the connection was lost");
    if (!PQexitPipelineMode(w.c) || PQsetnonblocking(w.c, 0) != 0) throw error(error_kind::other, detail::message(w.c));
    return w;
}

PGconn* conn::raw() {
    const detail::on_exit s{[this] { settle(); }};
    detail::wire& w = quiet();
    w.raw = true;
    return w.c;
}

std::optional<error> conn::control(const std::vector<std::string>& commands) {
    try {
        std::shared_ptr<detail::ticket> t;
        {
            std::lock_guard lk(w_->mu);
            t = queue(*w_, commands.front(), [&](PGconn* c) {
                return std::ranges::all_of(commands, [&](const std::string& s) { return detail::command(c, s) != 0; });
            });
        }
        detail::drive(*w_, *t);
        return t->err;
    } catch (const error& e) {
        return e;
    }
}

void conn::end_tx(int level, bool commit) {
    depth_ = level - 1;
    if (level > sent_) return settle();  // the guard sent nothing
    const std::string sp = "SAVEPOINT libpg_" + std::to_string(level);
    std::optional<error> failed;
    if (commit) {
        const std::optional<error> unread = detail::drain(*w_);
        if (!w_->broken && PQtransactionStatus(w_->c) == PQTRANS_INTRANS) {
            failed = control({level == 1 ? "COMMIT" : "RELEASE " + sp});
            if (!failed || level == 1) {  // a failed COMMIT ended the transaction
                sent_ = level - 1;
                settle();
                if (failed && failed->kind == error_kind::broken) throw commit_unknown(failed->what());
                if (failed) throw *failed;
                return;
            }
        } else {
            failed = unread ? *unread
                            : error(w_->broken ? error_kind::broken : error_kind::other,
                                    w_->broken ? "the connection was lost"
                                               : "the transaction is no longer open: an earlier statement failed or ended it");
        }
    }
    if (PQtransactionStatus(w_->c) != PQTRANS_IDLE) {
        if (level == 1) control({"ROLLBACK"});
        else control({"ROLLBACK TO " + sp, "RELEASE " + sp});
    }
    sent_ = level - 1;
    settle();
    if (failed) throw *failed;
}

void transaction::commit() {
    if (!open_ || level_ != c_.depth_)
        throw error(error_kind::other, "commit of a guard that is closed or has an inner guard open");
    open_ = false;
    c_.end_tx(level_, true);
}

transaction::~transaction() {
    if (open_ && level_ == c_.depth_) {
        try {
            c_.end_tx(level_, false);
        } catch (...) {
        }
    }
}

}  // namespace pg
