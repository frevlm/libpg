// Write-back scenarios (pg::writeback): counters in memory, postings written in batches.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

#include "harness.hpp"
#include "pg/pg.hpp"
#include "pg/writeback.hpp"
#include "raw.hpp"
#include "tags.hpp"

namespace bench {
namespace {

constexpr std::int64_t kHotRows = 16;
constexpr int kWbPool = 16;

// Seconds measured per write-back point: submits outrun the writer, so a point
// longer than this backs up more postings than the backlog limit holds.
double wb_seconds(const Cfg& cfg) { return std::min(cfg.duration, 1.0); }

pg::pool_options pool_options_of(const Cfg& cfg) {
    pg::pool_options o;
    o.dsn = cfg.dsn;
    o.max_conns = kWbPool;
    return o;
}

// A pool plus a writeback that writes only on flush().
struct Wb {
    pg::pool pool;
    std::unique_ptr<pg::writeback> wb;

    Wb(const Cfg& cfg, std::chrono::milliseconds interval) : pool(pool_options_of(cfg)) {
        pg::writeback_options o;
        o.interval = interval;
        o.backlog_limit = 50'000'000;
        wb = std::make_unique<pg::writeback>(pool, o);
    }
    // Loads keys 1..n into memory, 8 loaders each taking a stride.
    void load(std::int64_t n) {
        note("writeback: loading " + std::to_string(n) + " keys");
        run_point(8, 0.0, [&](Ctx& x) {
            for (std::int64_t i = 1 + x.tid; i <= n; i += 8) x.sink += static_cast<std::uint64_t>(wb->get<tag::balance>(i).units & 0xff);
        });
    }
};

const money kOne = money::parse("1");

// get or +1 on a random key among `rows`, 50/50.
void get_or_add(Ctx& x, pg::writeback& wb, std::int64_t rows) {
    const std::int64_t id = x.rng.key(rows);
    if (x.rng.next() & 1) x.sink += static_cast<std::uint64_t>(wb.get<tag::balance>(id).units & 0xff);
    else wb.submit(pg::posting{}.add<tag::balance>(kOne, id));
}

void get_add(const Cfg& cfg, const char* name, std::int64_t rows) {
    Wb w(cfg, std::chrono::hours(1));
    w.load(rows);
    for (int t : cfg.threads) {
        note(std::string(name) + " threads=" + std::to_string(t));
        emit(name, t, 0, run_point(t, wb_seconds(cfg), [&](Ctx& x) { x.loop([&] { get_or_add(x, *w.wb, rows); }); }));
        w.wb->flush();  // the backlog never fills
    }
}

void run_wb_get_add(const Cfg& cfg) { get_add(cfg, "wb_get_add", kRows); }
void run_wb_get_add_hot(const Cfg& cfg) { get_add(cfg, "wb_get_add_hot", kHotRows); }

// --- wb_posting -------------------------------------------------------------------

std::int64_t db_scalar(pg::pool& p, const pg::sql& q) {
    pg::conn c(p);
    return c.query_one<std::int64_t>(q);
}

void run_wb_posting(const Cfg& cfg) {
    Wb w(cfg, std::chrono::milliseconds(200));
    w.load(kRows);
    const pg::sql sum{"SELECT (sum(balance) * 1000000)::bigint FROM accounts"};
    for (int t : cfg.threads) {
        note("wb_posting threads=" + std::to_string(t));
        raw::run_once(cfg.dsn, "TRUNCATE events");
        w.wb->flush();
        const std::int64_t before = db_scalar(w.pool, sum);
        Result r = run_point(t, wb_seconds(cfg), [&](Ctx& x) {
            x.loop([&] {
                const std::int64_t id = x.rng.key();
                tag::event_write e;
                e.account_id = id;
                e.amount = kOne;
                e.note = "bench";
                w.wb->submit(pg::posting{}.insert(e).add<tag::balance>(kOne, id));
            });
        });
        const std::uint64_t t0 = now_ns();
        w.wb->flush();
        const double ms = static_cast<double>(now_ns() - t0) / 1e6;
        const std::int64_t n = db_scalar(w.pool, pg::sql{"SELECT count(*) FROM events"});
        const std::int64_t delta = db_scalar(w.pool, sum) - before;
        char buf[96];
        std::snprintf(buf, sizeof buf, "postings=%lld;flush_ms=%.1f", static_cast<long long>(n), ms);
        r.extra = buf;
        emit("wb_posting", t, 0, r);
        if (delta != n * 1'000'000) {
            std::fprintf(stderr, "wb_posting threads=%d: balance delta %lld != postings %lld\n", t,
                         static_cast<long long>(delta / 1'000'000), static_cast<long long>(n));
            throw std::runtime_error("wb_posting: balance sum mismatch");
        }
    }
}

// --- wb_flush ---------------------------------------------------------------------

// Dirty N distinct keys, then time flush().
void run_wb_flush(const Cfg& cfg) {
    Wb w(cfg, std::chrono::hours(1));
    w.load(kRows);
    // An untimed flush of the whole keyspace warms the writer, its statements and the
    // server's buffers.
    for (std::int64_t i = 1; i <= kRows; ++i) w.wb->submit(pg::posting{}.add<tag::balance>(kOne, i));
    w.wb->flush();
    for (std::int64_t n : {1'000, 10'000, 100'000}) {
        note("wb_flush n=" + std::to_string(n));
        for (std::int64_t i = 1; i <= n; ++i) w.wb->submit(pg::posting{}.add<tag::balance>(kOne, i));
        const std::uint64_t t0 = now_ns();
        w.wb->flush();
        const double secs = static_cast<double>(now_ns() - t0) / 1e9;
        Result r;
        r.ops_per_sec = static_cast<double>(n) / secs;
        char buf[64];
        std::snprintf(buf, sizeof buf, "postings=%lld;flush_ms=%.1f", static_cast<long long>(n), secs * 1e3);
        r.extra = buf;
        emit("wb_flush", 1, 0, r);
    }
}

// revlm's write path: each posting inserts one request row and charges one of 1000
// users; a loop submits kBatchRows postings and flushes them.
void run_wb_requests(const Cfg& cfg) {
    raw::run_once(cfg.dsn, "TRUNCATE bench_requests");
    std::vector<tag::request_row> rows = batch_rows();
    Wb w(cfg, std::chrono::hours(1));
    w.load(1000);
    for (int t : cfg.threads) {
        note("wb_requests threads=" + std::to_string(t));
        emit("wb_requests", t, 0, run_point(t, cfg.duration, [&](Ctx& x) {
                 x.loop([&] {
                     for (const tag::request_row& r : rows)
                         w.wb->submit(pg::posting{}.insert(r).add<tag::balance>(kOne, x.rng.key(1000)));
                     w.wb->flush();
                     return std::uint64_t{kBatchRows};
                 });
             }));
    }
}

}  // namespace

void register_wb(std::vector<Scenario>& v) {
    v.push_back({"wb_get_add", run_wb_get_add});
    v.push_back({"wb_get_add_hot", run_wb_get_add_hot});
    v.push_back({"wb_posting", run_wb_posting});
    v.push_back({"wb_flush", run_wb_flush});
    v.push_back({"wb_requests", run_wb_requests});
}

}  // namespace bench
