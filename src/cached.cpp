#include "pg/cached.hpp"
#include "pg/batch.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <optional>
#include <shared_mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pg {
namespace {

using slot_ptr = std::shared_ptr<detail::slot_base>;
using errors = std::vector<std::exception_ptr>;

// Process-wide cache state. Never destroyed (like the connection pool), so
// threads still running at exit can use it. See cached.hpp for the lock order.
struct state {
    std::mutex shutdown_mu;  // serializes shutdown(); held while waiting for the flusher
    std::mutex flush_mu;     // serializes flushes; held across a flush's database IO

    std::mutex mu;  // everything below
    std::condition_variable cv;
    std::vector<slot_ptr> dirty;
    cache_options opts;
    std::uint64_t config_generation = 0;
    std::uint64_t stop_generation = 0;  // a flusher runs while this equals the value it started with
    std::atomic<bool> flusher_alive{false};  // written under mu
    bool draining = false;  // a shutdown is in progress: no flusher is started
};

state& st() {
    static state* s = new state;
    return *s;
}

thread_local bool in_flusher = false;

std::atomic<detail::commit_fault> next_commit_fault{detail::commit_fault::none};

// The slot registry: shard_count shards, each its own reader-writer lock and hash
// table, so that finding an existing slot takes a shared lock on one shard and
// builds no string. An entry keeps its identity (table, column, encoded key) to
// resolve hash collisions.
struct entry {
    std::string table, column;
    detail::params key;
    slot_ptr slot;
};

struct shard {
    std::shared_mutex mu;
    std::unordered_multimap<std::uint64_t, entry> map;
};

constexpr unsigned shard_bits = 6;
constexpr std::size_t shard_count = std::size_t{1} << shard_bits;

// Never destroyed, like state.
shard* shards() {
    static shard* s = new shard[shard_count];
    return s;
}

std::uint64_t slot_hash(std::string_view table, std::string_view column, const detail::params& key) {
    constexpr std::hash<std::string_view> hash;
    std::uint64_t h = hash(table);
    const auto add = [&h](std::uint64_t x) { h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    add(hash(column));
    for (const auto& v : key) add(v ? hash(*v) : 0x6e756c6cull);
    h ^= h >> 33;  // the shard index comes from the top bits: mix them well
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

shard& shard_of(std::uint64_t h) { return shards()[h >> (64 - shard_bits)]; }

// The caller holds the shard lock.
slot_ptr find_in(const shard& sh, std::uint64_t h, std::string_view table, std::string_view column,
                 const detail::params& key) {
    const auto [first, last] = sh.map.equal_range(h);
    for (auto it = first; it != last; ++it) {
        const entry& e = it->second;
        if (e.table == table && e.column == column && e.key == key) return e.slot;
    }
    return nullptr;
}

// Hands one error to the handler, or to stderr. No libpg lock may be held.
void deliver(const std::function<void(const error&)>& handler, std::exception_ptr ep) {
    try {
        std::rethrow_exception(ep);
    } catch (const error& e) {
        try {
            if (handler) handler(e);
            else std::fprintf(stderr, "libpg: cache write-back failed: %s\n", e.what());
        } catch (...) {
            // A throwing handler must not take the flusher down.
        }
    } catch (...) {
    }
}

void report_all(const errors& errs) {
    if (errs.empty()) return;
    std::function<void(const error&)> handler;
    {
        std::lock_guard lk(st().mu);
        handler = st().opts.on_error;
    }
    for (const auto& ep : errs) deliver(handler, ep);
}

struct item {
    slot_ptr slot;
    detail::op o;
    bool dropped = false;  // permanently failed: discarded, not retried
};

// Puts slots whose pending write was not taken back on the dirty list.
void requeue(const std::vector<slot_ptr>& slots) {
    for (const auto& s : slots) detail::schedule(s);
}

// Swaps the dirty list out and takes every slot's pending write. A slot that
// cannot produce its write goes back on the list.
std::vector<item> take_all(errors& errs) {
    std::vector<slot_ptr> batch;
    {
        std::lock_guard lk(st().mu);
        batch.swap(st().dirty);
    }
    std::vector<item> items;
    try {
        items.reserve(batch.size());
    } catch (...) {
        requeue(batch);
        throw;
    }
    std::vector<slot_ptr> untaken;
    for (auto& s : batch) {
        item it{s, {}};
        try {
            if (s->take(it.o)) items.push_back(std::move(it));
        } catch (const std::exception& e) {
            untaken.push_back(s);
            errs.push_back(std::make_exception_ptr(error("cannot write " + s->label() + ": " + e.what())));
        }
    }
    requeue(untaken);
    return items;
}

// A failure that says nothing about the statement itself: the connection, the
// server's resources or a retryable conflict.
bool transient_state(const std::string& state) {
    if (state.empty()) return false;
    const std::string cls = state.substr(0, 2);
    return cls == "08" || cls == "53" || cls == "57" || state == "40001" || state == "40P01";
}

// A client-side error on a live connection (empty SQLSTATE) is the statement's fault.
bool transient(conn& c, const error& e) {
    return transient_state(e.sqlstate) || PQstatus(c.raw()) == CONNECTION_BAD;
}

// A failed COMMIT may still have taken effect when the connection was lost.
bool commit_outcome_unknown(const std::string& state) {
    return state.empty() || state.compare(0, 2, "08") == 0 || state.compare(0, 2, "57") == 0;
}

// Discards the op of `it`: it failed for good.
void drop(item& it, std::exception_ptr why, errors& errs) {
    it.dropped = true;
    errs.push_back(std::move(why));
}

void drop_missing(item& it, errors& errs) {
    drop(it, std::make_exception_ptr(not_found("no row for " + it.slot->label())), errs);
}

// SAVEPOINT w, the UPDATE and RELEASE in one round trip; returns the updated
// row count. When the UPDATE fails, savepoint w stays for the caller to roll back to.
std::int64_t update_in_savepoint(conn& c, const detail::op& o) {
    batch b(c);
    b.exec("SAVEPOINT w");
    const pending<std::int64_t> rows = b.exec_params(o.statement(), o.p);
    b.exec("RELEASE SAVEPOINT w");
    b.run();
    return rows.get();
}

// Runs one UPDATE inside its own savepoint. A permanent failure drops only this
// op; a transient one propagates and ends the batch. An op whose parameters
// cannot be sent is dropped before anything is sent.
void run_op(conn& c, item& it, errors& errs) {
    if (const auto why = detail::unsendable(it.o.p)) {
        drop(it, std::make_exception_ptr(error(*why)), errs);
        return;
    }
    std::int64_t rows = 0;
    try {
        rows = update_in_savepoint(c, it.o);
    } catch (const error& e) {
        if (transient(c, e)) throw;
        c.exec("ROLLBACK TO SAVEPOINT w");
        drop(it, std::make_exception_ptr(e), errs);
        return;
    }
    if (rows == 0) drop_missing(it, errs);  // nothing was updated, so nothing to roll back
}

// --- Bulk write-back ---------------------------------------------------------------
//
// Ops of one statement kind (same column, same set-or-add) are written by one
// UPDATE ... FROM unnest(...): the keys and values travel as text arrays and
// the server matches rows by key. Each chunk runs in its own savepoint; when it
// fails for good, the chunk is written op by op, which finds the culprit.

// Ops per bulk statement.
constexpr std::size_t chunk_size = 1000;

using op_group = std::vector<item*>;

// The bulk statements of one cell kind.
struct bulk_sql {
    std::string set, add;
};

// Built once per cell kind; only touched with flush_mu held.
std::unordered_map<const detail::cell_sql*, bulk_sql>& bulk_cache() {
    static auto* cache = new std::unordered_map<const detail::cell_sql*, bulk_sql>;
    return *cache;
}

// The type of a column, as SQL text. Only the type, not its modifier: the
// assignment to the column then checks length and precision as a parameter would.
std::string sql_type(conn& c, const std::string& table, const std::string& column) {
    const auto type = c.query_one<std::string>(
        "SELECT a.atttypid::regtype::text FROM pg_attribute a WHERE a.attrelid = $1::regclass AND a.attname = $2"
        " AND a.attnum > 0 AND NOT a.attisdropped",
        detail::quote_ident(table), column);
    if (!type) throw error("no column " + table + "." + column);
    return *type;
}

// Parameters are the key arrays, then the value array; the result is the
// 1-based position in the arrays of every updated row.
bulk_sql build_bulk_sql(conn& c, const detail::cell_sql& cell) {
    const std::string t = detail::quote_ident(cell.table);
    std::string arrays, names, match;
    for (std::size_t i = 0; i < cell.key.size(); ++i) {
        const std::string k = "k" + std::to_string(i);
        arrays += "$" + std::to_string(i + 1) + "::text[], ";
        names += k + ", ";
        match += (i ? " AND " : "") + t + "." + detail::quote_ident(cell.key[i]) + " = _pgv." + k + "::" +
                 sql_type(c, cell.table, cell.key[i]);
    }
    arrays += "$" + std::to_string(cell.key.size() + 1) + "::text[]";
    names += "val, ord";
    const std::string col = detail::quote_ident(cell.column);
    const std::string value = "_pgv.val::" + sql_type(c, cell.table, cell.column);
    const std::string rest = " FROM unnest(" + arrays + ") WITH ORDINALITY AS _pgv(" + names + ") WHERE " + match +
                             " RETURNING _pgv.ord";
    return {"UPDATE " + t + " SET " + col + " = " + value + rest,
            "UPDATE " + t + " SET " + col + " = " + t + "." + col + " + " + value + rest};
}

const bulk_sql& bulk_sql_of(conn& c, const detail::cell_sql& cell) {
    auto& cache = bulk_cache();
    const auto it = cache.find(&cell);
    if (it != cache.end()) return it->second;
    return cache.emplace(&cell, build_bulk_sql(c, cell)).first->second;
}

void append_element(std::string& out, const detail::param& v) {
    if (!v) {
        out += "NULL";
        return;
    }
    out += '"';
    for (const char ch : *v) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
    }
    out += '"';
}

// One text array literal per parameter position of the ops, the value array last.
detail::params array_params(std::span<item* const> ops) {
    const std::size_t width = ops.front()->o.p.size();
    std::vector<std::string> columns(width, "{");
    for (const item* it : ops) {
        for (std::size_t j = 0; j < width; ++j) {
            if (columns[j].size() > 1) columns[j] += ',';
            append_element(columns[j], it->o.p[j]);
        }
    }
    detail::params out;
    for (std::size_t j = 1; j < width; ++j) out.push_back(columns[j] + '}');
    out.push_back(columns[0] + '}');
    return out;
}

// Ops whose key matched no row are dropped as not_found; the others are done.
void settle_matches(std::span<item* const> ops, const std::vector<std::int64_t>& updated, errors& errs) {
    std::vector<char> hit(ops.size(), 0);
    for (const std::int64_t pos : updated) {
        if (pos >= 1 && static_cast<std::size_t>(pos) <= ops.size()) hit[static_cast<std::size_t>(pos) - 1] = 1;
    }
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (!hit[i]) drop_missing(*ops[i], errs);
    }
}

// Ends the chunk's savepoint after a permanent failure, in one round trip.
void abandon_chunk(conn& c, const detail::cell_sql& cell) {
    batch b(c);
    b.exec("ROLLBACK TO SAVEPOINT g");
    b.exec("RELEASE SAVEPOINT g");
    b.run();
    bulk_cache().erase(&cell);  // the table may have changed under us: look the types up again
}

// SAVEPOINT g, the bulk UPDATE and RELEASE in one round trip; returns the
// positions of the updated rows. When the statement text is not built yet,
// SAVEPOINT g goes first on its own so that the type lookups run inside it.
// When the UPDATE fails, savepoint g stays for abandon_chunk.
std::vector<std::int64_t> bulk_update(conn& c, const detail::op& first, detail::params p) {
    batch b(c);
    if (bulk_cache().contains(first.cell)) b.exec("SAVEPOINT g");
    else c.exec("SAVEPOINT g");
    const bulk_sql& sql = bulk_sql_of(c, *first.cell);
    const auto updated = b.query_params<std::int64_t>(first.delta ? sql.add : sql.set, std::move(p));
    b.exec("RELEASE SAVEPOINT g");
    b.run();
    return updated.get();
}

// Writes the ops, all of one kind, with one statement. False when it failed for
// good and nothing was written, or when a value cannot be sent (run_op then
// finds it); a transient failure propagates.
bool run_bulk(conn& c, std::span<item* const> ops, errors& errs) {
    const detail::op& first = ops.front()->o;
    detail::params p = array_params(ops);
    if (detail::unsendable(p)) return false;
    std::vector<std::int64_t> updated;
    try {
        updated = bulk_update(c, first, std::move(p));
    } catch (const error& e) {
        if (transient(c, e)) throw;
        abandon_chunk(c, *first.cell);
        return false;
    }
    settle_matches(ops, updated, errs);
    return true;
}

void run_chunk(conn& c, std::span<item* const> ops, errors& errs) {
    if (ops.size() > 1 && run_bulk(c, ops, errs)) return;
    for (item* it : ops) run_op(c, *it, errs);
}

// The items by statement kind. Each cell appears once, so a group has no duplicate keys.
std::map<std::pair<const detail::cell_sql*, bool>, op_group> group_items(std::vector<item>& items) {
    std::map<std::pair<const detail::cell_sql*, bool>, op_group> groups;
    for (auto& it : items) groups[{it.o.cell, it.o.delta}].push_back(&it);
    return groups;
}

void run_items(conn& c, std::vector<item>& items, errors& errs) {
    for (auto& [kind, group] : group_items(items)) {
        for (std::size_t at = 0; at < group.size(); at += chunk_size) {
            const std::size_t n = std::min(chunk_size, group.size() - at);
            run_chunk(c, std::span<item* const>(group.data() + at, n), errs);
        }
    }
}

void commit(transaction& tx) {
    const auto fault = next_commit_fault.exchange(detail::commit_fault::none);
    if (fault == detail::commit_fault::lost_before_commit) throw error("simulated connection loss before COMMIT");
    tx.commit();
    if (fault == detail::commit_fault::lost_after_commit) throw error("simulated connection loss after COMMIT");
}

enum class outcome {
    committed,
    rolled_back,  // nothing of the batch is in the database
    unknown,      // COMMIT was sent and the connection died: maybe committed
};

struct batch_result {
    outcome out = outcome::committed;
    std::exception_ptr failure;
    std::string xid;  // the transaction id, for resolving `unknown`
};

// BEGIN (pending in tx), SET CONSTRAINTS ALL IMMEDIATE and the transaction id
// in one round trip.
std::string begin_flush(conn& c) {
    batch b(c);
    b.exec("SET CONSTRAINTS ALL IMMEDIATE");
    const auto xid = b.query<std::string>("SELECT pg_current_xact_id()::text");
    b.run();
    const auto rows = xid.get();
    if (rows.size() != 1) throw error("pg_current_xact_id returned " + std::to_string(rows.size()) + " rows");
    return rows.front();
}

// BEGIN, every op (grouped, each group in its savepoint), COMMIT. Never throws.
batch_result run_batch(std::vector<item>& items, errors& errs) {
    batch_result r;
    bool committing = false;
    try {
        conn c;
        transaction tx(c);
        r.xid = begin_flush(c);
        run_items(c, items, errs);
        committing = true;
        commit(tx);
    } catch (const error& e) {
        r.failure = std::make_exception_ptr(e);
        r.out = committing && commit_outcome_unknown(e.sqlstate) ? outcome::unknown : outcome::rolled_back;
    } catch (const std::exception& e) {
        r.failure = std::make_exception_ptr(error(e.what()));
        r.out = outcome::rolled_back;
    }
    return r;
}

// Asks the server, on a fresh connection, what became of a transaction.
outcome resolve_unknown(const std::string& xid) {
    try {
        conn c;
        const auto row = c.query_one<std::optional<std::string>>("SELECT pg_xact_status($1::xid8)", xid);
        const std::string status = row && *row ? **row : "";
        if (status == "committed") return outcome::committed;
        if (status == "aborted") return outcome::rolled_back;
    } catch (...) {
    }
    return outcome::unknown;
}

// Tells every slot what happened to its write.
void settle(std::vector<item>& items, outcome out) {
    for (auto& it : items) {
        if (it.dropped) it.slot->drop();
        else if (out == outcome::rolled_back) it.slot->restore(true);
        else if (out == outcome::unknown) it.slot->restore(false);
    }
}

// Writes what is dirty. Failures are appended to `errs`; what can be retried is
// queued again, what failed for good is dropped.
void flush_batch(errors& errs) {
    std::vector<item> items = take_all(errs);
    if (items.empty()) return;
    batch_result r = run_batch(items, errs);
    if (r.out == outcome::unknown) r.out = resolve_unknown(r.xid);
    if (r.out != outcome::committed) errs.push_back(r.failure);
    settle(items, r.out);
}

errors flush_collect() {
    errors errs;
    std::lock_guard lk(st().flush_mu);
    flush_batch(errs);
    return errs;
}

// The flusher thread: one flush per interval, until its stop generation passes.
void run_flusher(std::uint64_t my_generation) {
    state& g = st();
    in_flusher = true;
    std::unique_lock lk(g.mu);
    while (g.stop_generation == my_generation) {
        const auto config = g.config_generation;
        const auto interval = std::max(g.opts.interval, std::chrono::milliseconds(1));
        const bool woken =
            g.cv.wait_for(lk, interval, [&] { return g.stop_generation != my_generation || g.config_generation != config; });
        if (woken) continue;
        lk.unlock();
        try {
            report_all(flush_collect());
        } catch (...) {
        }
        lk.lock();
    }
    g.flusher_alive = false;
    g.cv.notify_all();
}

// Starts the flusher unless one runs or a shutdown is draining. Caller holds g.mu.
void start_flusher_locked() {
    state& g = st();
    if (g.flusher_alive || g.draining) return;
    std::thread(run_flusher, g.stop_generation).detach();
    g.flusher_alive = true;
}

// Stops the flusher and waits until it has left (unless called from it).
void stop_flusher() {
    state& g = st();
    std::unique_lock lk(g.mu);
    g.draining = true;
    ++g.stop_generation;
    g.cv.notify_all();
    g.cv.wait(lk, [&] { return !g.flusher_alive; });
}

// Lets the flusher start again when a shutdown ends, however it ends.
struct drain_end {
    ~drain_end() {
        std::lock_guard lk(st().mu);
        st().draining = false;
    }
};

// At process exit, says how many cells never reached the database. Does no IO
// and takes no part in any flush: static destruction is too late for that.
struct exit_notice {
    ~exit_notice() {
        std::vector<slot_ptr> pending;
        {
            std::lock_guard lk(st().mu);
            pending = st().dirty;
        }
        const auto lost = std::count_if(pending.begin(), pending.end(), [](const slot_ptr& s) { return s->has_pending(); });
        if (lost > 0)
            std::fprintf(stderr, "libpg: %ld cached cells were not written back; call pg::shutdown() before exit\n",
                         static_cast<long>(lost));
    }
};
const exit_notice notice;

}  // namespace

namespace detail {

std::string slot_label(std::string_view table, std::string_view column, const params& key) {
    std::string out = std::string(table) + "." + std::string(column) + "[";
    for (std::size_t i = 0; i < key.size(); ++i) out += (i ? "," : "") + key[i].value_or("NULL");
    return out + "]";
}

std::shared_ptr<slot_base> find_or_insert(std::string_view table, std::string_view column, const params& key,
                                          const std::function<std::shared_ptr<slot_base>()>& make) {
    const std::uint64_t h = slot_hash(table, column, key);
    shard& sh = shard_of(h);
    {
        std::shared_lock lk(sh.mu);
        if (slot_ptr s = find_in(sh, h, table, column, key)) return s;
    }
    std::unique_lock lk(sh.mu);
    if (slot_ptr s = find_in(sh, h, table, column, key)) return s;  // inserted meanwhile
    slot_ptr s = make();
    sh.map.emplace(h, entry{std::string(table), std::string(column), key, s});
    return s;
}

void schedule(std::shared_ptr<slot_base> s) {
    state& g = st();
    std::lock_guard lk(g.mu);
    g.dirty.push_back(std::move(s));
    start_flusher_locked();
}

void ensure_flusher() {
    state& g = st();
    if (g.flusher_alive) return;
    std::lock_guard lk(g.mu);
    start_flusher_locked();
}

std::mutex& flush_mutex() { return st().flush_mu; }

void inject_commit_fault(commit_fault f) { next_commit_fault = f; }

}  // namespace detail

void configure_cache(cache_options o) {
    state& g = st();
    {
        std::lock_guard lk(g.mu);
        g.opts = std::move(o);
        ++g.config_generation;
    }
    g.cv.notify_all();
}

void flush() {
    errors errs = flush_collect();
    if (errs.empty()) return;
    const std::exception_ptr first = errs.front();
    errs.erase(errs.begin());
    report_all(errs);
    std::rethrow_exception(first);
}

void shutdown() {
    state& g = st();
    if (in_flusher) {  // the flusher cannot wait for itself
        report_all(flush_collect());
        return;
    }
    errors errs;
    {
        std::lock_guard sl(g.shutdown_mu);
        const drain_end reopen;
        stop_flusher();
        errs = flush_collect();
    }
    report_all(errs);
}

}  // namespace pg
