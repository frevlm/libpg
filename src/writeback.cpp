#include "pg/writeback.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <random>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace pg {
namespace detail {

using clock = std::chrono::steady_clock;
using units_rows = std::vector<row_t<std::int64_t, std::int64_t>>;
constexpr std::string_view kReceipt = "INSERT INTO libpg.flush_receipts";
constexpr std::size_t kBatchMax = 10'000;  // postings in one transaction
constexpr int kSplitAfter = 3;             // lock or conflict failures before a batch is halved

std::string pack_param(std::string out, const param& p) {
    put_be(p.type, 4, out);
    put_be(p.bytes->size(), 4, out);
    return out + *p.bytes;
}

struct slot {
    const counter* def = nullptr;
    std::int64_t committed = 0, pending = 0;  // database value as last read; unsettled deltas
    int unsettled = 0;                        // deltas recorded and not settled
    bool loaded = false, read = false;        // committed is a database value; read since it was loaded
    clock::time_point read_at, loaded_at;
};

struct shard { std::mutex mu; std::unordered_map<std::string, slot> map; };

void add_param(array_builder& b, const param& p) {
    b.element = p.type;
    ++b.count;
    b.nulls |= !p.bytes;
    put_be(p.bytes ? p.bytes->size() : 0xffffffff, 4, b.body);
    if (p.bytes) b.body += *p.bytes;
}

// The array parameters of columns, then the extra ones.
sql arrays_sql(const std::vector<array_builder>& bs, const std::vector<array_builder>& extra = {}) {
    std::vector<sql> parts;
    for (const auto* v : {&bs, &extra})
        for (const array_builder& b : *v) parts.push_back(sql{"{}", param{array_of(b.element), b.finish()}});
    return join(parts, sql{", "});
}

// One typed array per key column from packed keys.
std::vector<array_builder> key_arrays(const counter& d, std::span<const std::string> keys) {
    std::vector<array_builder> out(d.keys.size());
    for (const std::string& k : keys) {
        std::string_view v = std::string_view(k).substr(sizeof(counter*));
        for (array_builder& b : out) {
            const std::size_t n = get_be(v.substr(4, 4), 4);
            b.element = static_cast<oid>(get_be(v.substr(0, 4), 4));
            put_be(n, 4, b.body);
            b.body.append(v.substr(8, n));
            ++b.count;
            v.remove_prefix(8 + n);
        }
    }
    return out;
}

// "d(k0, k1, <last>)" and "t.<key> = d.k0 AND ..." for a counter's key columns.
sql alias_sql(const counter& d, std::string last) {
    std::string s;
    for (std::size_t i = 0; i < d.keys.size(); ++i) s += "k" + std::to_string(i) + ", ";
    return unsafe_sql("d(" + s + last + ")");
}
sql match_sql(const counter& d) {
    std::vector<sql> parts;
    for (std::size_t i = 0; i < d.keys.size(); ++i) parts.push_back(sql{"t.{} = d.{}", ident(d.keys[i]), unsafe_sql("k" + std::to_string(i))});
    return join(parts, sql{" AND "});
}

struct wb_state {
    pool& p;
    writeback_options o;
    shard shards[64];
    std::shared_mutex db_mu;  // shared: loads; exclusive: the writer's refresh, and a batch from its commit to its settle
    clock::time_point pruned{};  // when the writer last deleted old receipts
    std::mutex mu;            // guards what follows
    std::condition_variable wake, done;
    std::vector<posting> queue;
    std::uint64_t reserved = 0, submitted = 0, settled = 0, committed = 0, lost = 0, replays = 0;
    bool kick = false;
    std::atomic<bool> stop{false};
    std::atomic<int> fault{0};
    std::thread writer;

    wb_state(pool& pl, writeback_options opts) : p(pl), o(std::move(opts)) {
        p.transact([](conn& c) {
            c.exec(sql{"SELECT pg_advisory_xact_lock(0x6c69627067)"});
            c.exec(sql{"CREATE SCHEMA IF NOT EXISTS libpg"});
            c.exec(sql{"CREATE TABLE IF NOT EXISTS libpg.flush_receipts (id uuid PRIMARY KEY, created_at timestamptz NOT NULL DEFAULT now())"});
        });
        writer = std::thread([this] { run(); });
    }

    shard& shard_of(const std::string& k) { return shards[std::hash<std::string>{}(k) % 64]; }

    // The values of packed keys as units: (position in keys from 1, units) of the rows that exist.
    units_rows load(const counter& d, std::span<const std::string> keys) {
        conn c(p);
        return c.query<std::int64_t, std::int64_t>(sql{"SELECT d.n, (t.{} * 1{})::int8 FROM unnest({}) WITH ORDINALITY AS {} JOIN {} AS t ON {}",
                                                      ident(d.column), unsafe_sql(std::string(d.scale, '0')), arrays_sql(key_arrays(d, keys)),
                                                      alias_sql(d, "n"), ident(d.table), match_sql(d)});
    }

    std::int64_t value(const counter& d, const std::string& k) {
        { conn probe(p); }  // a thread holding a conn of the pool fails here, hot key or cold
        shard& sh = shard_of(k);
        const auto now = clock::now();
        {
            std::lock_guard lk(sh.mu);
            if (auto it = sh.map.find(k); it != sh.map.end() && it->second.loaded) {
                it->second.read = true, it->second.read_at = now;
                return it->second.committed + it->second.pending;
            }
        }
        std::shared_lock rd(db_mu);  // no batch settles between the SELECT and the slot
        const units_rows rows = load(d, std::span<const std::string>(&k, 1));
        if (rows.empty()) throw not_found("writeback: no row of " + std::string(d.table) + " for the key");
        std::lock_guard lk(sh.mu);
        slot& s = sh.map[k];
        s = {&d, std::get<1>(rows[0]), s.pending, s.unsettled, true, true, now, now};
        return s.committed + s.pending;
    }

    void submit(posting po) {
        {
            std::lock_guard lk(mu);
            if (reserved - settled >= o.backlog_limit) throw backlog_full("writeback: backlog limit reached");
            ++reserved;
        }
        const auto now = clock::now();
        for (const wb_delta& d : po.deltas_) {
            std::lock_guard lk(shard_of(d.key).mu);
            slot& s = shard_of(d.key).map.try_emplace(d.key, slot{d.def, 0, 0, 0, false, false, now, now}).first->second;
            s.pending += d.units, ++s.unsettled;
        }
        std::lock_guard lk(mu);
        queue.push_back(std::move(po));
        ++submitted;
    }

    // Waits until everything submitted before the call is settled, or the deadline.
    void flush(clock::time_point deadline) {
        std::unique_lock lk(mu);
        const std::uint64_t target = submitted;
        kick = true;
        wake.notify_all();
        done.wait_until(lk, deadline, [&] { return settled >= target; });
    }

    // Drops the slots nobody reads and nothing is unsettled on; re-reads, one
    // statement per counter, the loaded slots read since they were loaded and older than `refresh`.
    void refresh() {
        const auto now = clock::now();
        // A receipt only has to outlive the replays of its batch; once an hour, drop those older than a day.
        if (now - pruned > std::chrono::hours(1)) {
            pruned = now;
            conn(p).exec(sql{"DELETE FROM libpg.flush_receipts WHERE created_at < now() - interval '1 day'"});
        }
        std::map<const counter*, std::vector<std::string>> due;
        for (shard& sh : shards) {
            std::lock_guard lk(sh.mu);
            for (auto it = sh.map.begin(); it != sh.map.end();) {
                const slot& s = it->second;
                if (s.unsettled == 0 && now - s.read_at > o.refresh) it = sh.map.erase(it);
                else if (s.read && s.loaded && now - s.loaded_at > o.refresh) due[s.def].push_back((it++)->first);
                else ++it;
            }
        }
        for (const auto& [d, keys] : due)
            for (const auto& [n, units] : load(*d, keys)) {
                const std::string& k = keys[static_cast<std::size_t>(n - 1)];
                std::lock_guard lk(shard_of(k).mu);
                if (auto it = shard_of(k).map.find(k); it != shard_of(k).map.end()) it->second.committed = units, it->second.loaded_at = now, it->second.read = false;
            }
    }

    // One transaction: the receipt, one INSERT per (table, column set), one UPDATE per counter.
    void transact_batch(std::span<posting> ps, const uuid& receipt) {
        std::map<std::string, std::pair<const wb_row*, std::vector<array_builder>>> inserts;
        std::map<const counter*, std::map<std::string, std::int64_t>> sums;  // one delta per key
        for (const posting& po : ps) {
            for (const wb_row& r : po.rows_) {
                std::string id(r.table);
                for (std::size_t i = 0; i < r.columns.size(); ++i) id += '\0' + std::string(r.columns[i]) + ':' + std::to_string(r.values[i].type);
                auto& [first, arrays] = inserts[id];
                if (!first) first = &r, arrays.resize(r.columns.size());
                for (std::size_t i = 0; i < r.values.size(); ++i) add_param(arrays[i], r.values[i]);
            }
            for (const wb_delta& d : po.deltas_) sums[d.def][d.key] += d.units;
        }
        conn c(p);
        transaction tx(c);
        std::vector<pending<>> sent;
        std::vector<std::int64_t> expect;  // rows each statement must touch, -1: any
        auto send = [&](const sql& q, std::int64_t rows) { sent.push_back(c.send<>(q)), expect.push_back(rows); };
        send(sql{"INSERT INTO libpg.flush_receipts (id) VALUES ({})", receipt}, 1);
        for (auto& [id, g] : inserts) {
            std::vector<sql> cols;
            for (std::string_view name : g.first->columns) cols.push_back(ident(name));
            send(sql{"INSERT INTO {} ({}) SELECT * FROM unnest({})", ident(g.first->table), join(cols, sql{", "}), arrays_sql(g.second)}, -1);
        }
        for (const auto& [d, by_key] : sums) {
            std::vector<std::string> keys;
            array_builder deltas;
            for (const auto& [k, units] : by_key) keys.push_back(k), deltas.add(units);
            // units * 10^-S: a product keeps every digit, a quotient would not
            const std::string unit = d->scale ? "* 0." + std::string(d->scale - 1, '0') + "1" : "";
            send(sql{"UPDATE {} AS t SET {} = t.{} + d.v::numeric {} FROM unnest({}) AS {} WHERE {}", ident(d->table), ident(d->column),
                     ident(d->column), unsafe_sql(unit), arrays_sql(key_arrays(*d, keys), {deltas}), alias_sql(*d, "v"), match_sql(*d)},
                 static_cast<std::int64_t>(keys.size()));
        }
        for (std::size_t i = 0; i < sent.size(); ++i)
            if (const std::int64_t n = sent[i].affected(); expect[i] >= 0 && n != expect[i]) throw error(error_kind::data, "writeback: a counter's row does not exist");
        tx.commit();
    }

    // After a commit of unknown outcome and a stop, one look at the receipt decides.
    std::optional<error> resolve(std::span<posting> ps, const uuid& receipt) {
        try {
            if (p.transact([&](conn& c) { return c.query_opt<std::int32_t>(sql{"SELECT 1 FROM libpg.flush_receipts WHERE id = {}", receipt}); }))
                return settle(ps, nullptr), std::nullopt;
        } catch (const error&) {
        }
        return error(error_kind::commit_unknown, "writeback ended after a commit of unknown outcome: the postings may or may not be in the database");
    }

    // Sends the batch until it commits, and settles it; returns the error when these postings cannot commit as they are.
    std::optional<error> attempt(std::span<posting> ps) {
        thread_local std::mt19937_64 rng{std::random_device{}()};
        uuid receipt;
        for (auto& b : receipt.bytes) b = static_cast<std::uint8_t>(rng());
        std::unique_lock w(db_mu, std::defer_lock);  // held through a try, and between tries after a commit of unknown outcome
        bool unknown = false;
        int slow = 0;
        for (bool again = false;; again = true) {
            if (stop) return unknown ? resolve(ps, receipt) : error(error_kind::other, "writeback ended before the posting was written");
            if (again) {
                if (!unknown) w.unlock();
                std::this_thread::sleep_for(std::min(o.interval, std::chrono::milliseconds(200)));
                std::lock_guard lk(mu);
                ++replays;
            }
            if (!w.owns_lock()) w.lock();
            try {
                const int f = fault.exchange(0);
                if (f == 1) throw error(error_kind::broken, "injected: connection lost before the batch was sent");
                transact_batch(ps, receipt);
                if (f == 2) throw commit_unknown("injected: connection lost after the batch committed");
                return settle(ps, nullptr), std::nullopt;
            } catch (const error& e) {
                if (e.sqlstate == "23505" && e.sql.starts_with(kReceipt)) return settle(ps, nullptr), std::nullopt;  // it committed before
                unknown |= e.kind == error_kind::commit_unknown;
                if (e.kind == error_kind::data || (e.kind == error_kind::other && !e.sqlstate.empty())) return e;  // the server refuses the data
                const bool busy = e.kind == error_kind::lock_or_cancel || e.kind == error_kind::conflict;
                if (busy && ++slow >= kSplitAfter && ps.size() > 1) return e;
            }
        }
    }

    void give_up(const posting& po, const std::string& reason, error_kind kind) {
        lost_posting l{reason, kind, {}};
        for (const wb_row& r : po.rows_) l.description.push_back("insert into " + std::string(r.table));
        for (const wb_delta& d : po.deltas_)
            l.description.push_back("add " + std::to_string(d.units) + " units to " + std::string(d.def->table) + "." + std::string(d.def->column));
        if (o.on_lost) return o.on_lost(l);
        std::fprintf(stderr, "libpg writeback: lost posting: %s\n", reason.c_str());
        for (const std::string& line : l.description) std::fprintf(stderr, "  %s\n", line.c_str());
    }

    // Moves the postings' deltas into the database value (committed) or out of memory (lost).
    void settle(std::span<posting> ps, const error* e) {
        for (const posting& po : ps) {
            for (const wb_delta& d : po.deltas_) {
                std::lock_guard lk(shard_of(d.key).mu);
                slot& s = shard_of(d.key).map[d.key];
                s.pending -= d.units, --s.unsettled;
                if (!e) s.committed += d.units;
            }
            if (e) give_up(po, e->what(), e->kind);
        }
        std::lock_guard lk(mu);
        (e ? lost : committed) += ps.size();
        settled += ps.size();
        done.notify_all();
    }

    // Commits the postings as one transaction; when that cannot be, each half alone.
    void write(std::span<posting> ps) {
        const auto err = attempt(ps);
        if (!err) return;
        if (ps.size() == 1 || err->kind == error_kind::commit_unknown) return settle(ps, &*err);
        write(ps.first(ps.size() / 2));
        write(ps.subspan(ps.size() / 2));
    }

    void run() {
        std::unique_lock lk(mu);
        while (!stop) {
            wake.wait_for(lk, o.interval, [&] { return stop || kick; });
            if (stop) return;
            const auto n = static_cast<std::ptrdiff_t>(std::min(queue.size(), kBatchMax));
            std::vector<posting> batch(std::make_move_iterator(queue.begin()), std::make_move_iterator(queue.begin() + n));
            queue.erase(queue.begin(), queue.begin() + n);
            kick = !queue.empty();
            lk.unlock();
            try {
                std::unique_lock w(db_mu);
                refresh();
            } catch (const error&) {  // a failed refresh is tried again next interval
            }
            if (!batch.empty()) write(batch);
            lk.lock();
        }
    }

    ~wb_state() {
        flush(clock::now() + o.shutdown_timeout);
        { std::lock_guard lk(mu); stop = true; wake.notify_all(); }
        writer.join();
        for (const posting& po : queue) give_up(po, "writeback ended before the posting was written", error_kind::other);
    }
};

void inject_fault(writeback& w, int fault) { w.st_->fault = fault; }

}  // namespace detail

writeback::writeback(pool& p, writeback_options o) : st_(std::make_unique<detail::wb_state>(p, std::move(o))) {}
writeback::~writeback() = default;

std::int64_t writeback::value(const detail::counter& d, const std::string& key) { return st_->value(d, key); }
void writeback::submit(posting p) { if (!p.empty()) st_->submit(std::move(p)); }
void writeback::flush() { st_->flush(detail::clock::now() + std::chrono::hours(24 * 365)); }

writeback_stats writeback::stats() const {
    std::lock_guard lk(st_->mu);
    return {static_cast<std::size_t>(st_->reserved - st_->settled), st_->committed, st_->lost, st_->replays};
}

}  // namespace pg
