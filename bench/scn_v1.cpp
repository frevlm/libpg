// v1 library scenarios: pooled queries, pg::sql<Col>, transactions, pg::cached<Col>,
// and the pool itself.
#include <array>
#include <memory>
#include <tuple>

#include "harness.hpp"
#include "pg/pg.hpp"
#include "raw.hpp"

namespace bench {
namespace {

// Column tags in the shape pg_tablegen writes. accounts.balance is numeric(18,6), which
// pg_tablegen maps to pg::numeric; that type has no +=, so the arithmetic scenarios
// read the column as double (the text codec round-trips it).
namespace tag {
struct accounts_ {
    static constexpr std::string_view name = "accounts";
    using key = std::tuple<std::int64_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct balance {
    using table = accounts_;
    using type = double;
    static constexpr std::string_view name = "balance";
};
}  // namespace tag

constexpr const char* kSelect = raw::kSelect;
constexpr std::int64_t kHotRows = 16;

// Opens P connections up front so that connection setup stays out of the measurement.
void open_pool(const Cfg& cfg, int pool) {
    pg::connect(cfg.dsn, pool);
    std::vector<std::unique_ptr<pg::conn>> held;
    for (int i = 0; i < pool; ++i) held.push_back(std::make_unique<pg::conn>());
}

std::uint64_t touch(const std::optional<std::tuple<std::string, pg::numeric>>& row) {
    if (!row) throw std::runtime_error("select: no row");
    return std::get<0>(*row).size() + std::get<1>(*row).text.size();
}

// For every pool size and thread count: open the pool, run `step` on all threads.
template <class MakeStep>
void sweep_pool(const Cfg& cfg, const char* name, MakeStep make_step) {
    for (int p : cfg.pools)
        for (int t : cfg.threads) {
            note(std::string(name) + " pool=" + std::to_string(p) + " threads=" + std::to_string(t));
            open_pool(cfg, p);
            emit(name, t, p, run_point(t, cfg.duration, [&](Ctx& x) { x.loop([&] { return make_step(x); }); }));
        }
}

// --- v1_select --------------------------------------------------------------------

void run_select(const Cfg& cfg) {
    sweep_pool(cfg, "v1_select", [](Ctx& x) {
        pg::conn c;
        x.sink += touch(c.query_one<std::string, pg::numeric>(kSelect, x.rng.key()));
    });
}

void run_select_hold(const Cfg& cfg) {
    for (int t : cfg.threads) {
        note("v1_select_hold threads=" + std::to_string(t));
        open_pool(cfg, t);
        emit("v1_select_hold", t, t, run_point(t, cfg.duration, [&](Ctx& x) {
                 pg::conn c;
                 x.loop([&] { x.sink += touch(c.query_one<std::string, pg::numeric>(kSelect, x.rng.key())); });
             }));
    }
}

// --- v1_update_cell ---------------------------------------------------------------

void update_cells(const Cfg& cfg, const char* name, std::int64_t rows) {
    sweep_pool(cfg, name, [rows](Ctx& x) {
        pg::sql<tag::balance> cell(x.rng.key(rows));
        cell += 1.0;
    });
}

void run_update_cell(const Cfg& cfg) { update_cells(cfg, "v1_update_cell", kRows); }
void run_update_cell_hot(const Cfg& cfg) { update_cells(cfg, "v1_update_cell_hot", kHotRows); }

// --- v1_tx3 -----------------------------------------------------------------------

void run_tx3(const Cfg& cfg) {
    raw::run_once(cfg.dsn, "TRUNCATE events");
    sweep_pool(cfg, "v1_tx3", [](Ctx& x) {
        const std::int64_t id = x.rng.key();
        pg::conn c;
        pg::transaction tx(c);
        x.sink += touch(c.query_one<std::string, pg::numeric>(kSelect, id));
        c.exec("UPDATE accounts SET balance = balance + $1 WHERE id = $2", pg::numeric{"1.000000"}, id);
        c.exec("INSERT INTO events(account_id, amount, note) VALUES ($1, $2, $3)", id, pg::numeric{"1.000000"},
               std::string_view("bench"));
        tx.commit();
    });
}

// The v1_tx3 work as one batch outside a transaction: the batch is one implicit
// transaction, so it stays atomic in one round trip (mirrors raw_tx3_pipelined).
void run_tx3_batch(const Cfg& cfg) {
    raw::run_once(cfg.dsn, "TRUNCATE events");
    sweep_pool(cfg, "v1_tx3_batch", [](Ctx& x) {
        const std::int64_t id = x.rng.key();
        pg::conn c;
        pg::batch b(c);
        const auto row = b.query<std::string, pg::numeric>(kSelect, id);
        b.exec("UPDATE accounts SET balance = balance + $1 WHERE id = $2", pg::numeric{"1.000000"}, id);
        b.exec("INSERT INTO events(account_id, amount, note) VALUES ($1, $2, $3)", id, pg::numeric{"1.000000"},
               std::string_view("bench"));
        b.run();
        const auto rows = row.get();
        x.sink += touch(rows.size() == 1 ? std::optional{rows.front()} : std::nullopt);
    });
}

// --- v1_batch16 -------------------------------------------------------------------

constexpr int kBatch = 16;

// 16 single-row selects in one batch: one round trip (mirrors raw_pipeline_16).
void run_batch16(const Cfg& cfg) {
    sweep_pool(cfg, "v1_batch16", [](Ctx& x) -> std::uint64_t {
        pg::conn c;
        pg::batch b(c);
        std::vector<pg::pending<std::vector<std::tuple<std::string, pg::numeric>>>> rows;
        rows.reserve(kBatch);
        for (int i = 0; i < kBatch; ++i) rows.push_back(b.query<std::string, pg::numeric>(kSelect, x.rng.key()));
        b.run();
        for (const auto& r : rows) {
            const auto got = r.get();
            x.sink += touch(got.size() == 1 ? std::optional{got.front()} : std::nullopt);
        }
        return kBatch;
    });
}

// --- v1_cached --------------------------------------------------------------------

using cell_t = pg::cached<tag::balance>;

// Every cell of the keyspace, loaded once per process. The flusher is parked for an hour
// so the hot path measures memory only; flushes are driven by hand.
struct CachedKeyspace {
    std::vector<cell_t> cells;
};

CachedKeyspace& keyspace(const Cfg& cfg) {
    static CachedKeyspace* ks = nullptr;
    if (ks) return *ks;
    pg::cache_options opt;
    opt.interval = std::chrono::hours(1);
    pg::configure_cache(opt);
    open_pool(cfg, 16);
    ks = new CachedKeyspace;
    ks->cells.reserve(kRows);
    for (std::int64_t id = 1; id <= kRows; ++id) ks->cells.emplace_back(id);
    note("v1_cached: loading " + std::to_string(kRows) + " cells");
    run_point(8, 0.0, [&](Ctx& x) {  // 8 loaders, each takes a stride of the keyspace
        for (std::int64_t i = x.tid; i < kRows; i += 8) x.sink += static_cast<std::uint64_t>(ks->cells[static_cast<std::size_t>(i)].get());
    });
    return *ks;
}

// get() or += with equal odds, on a random cell.
void cached_op(Ctx& x, cell_t& cell) {
    if (x.rng.next() & 1) x.sink += static_cast<std::uint64_t>(cell.get());
    else cell += 1.0;
}

void run_cached(const Cfg& cfg) {
    keyspace(cfg);
    for (int t : cfg.threads) {
        note("v1_cached threads=" + std::to_string(t));
        emit("v1_cached", t, 0, run_point(t, cfg.duration, [&](Ctx& x) {
                 x.loop([&] {
                     cell_t cell(x.rng.key());  // locates the slot through the registry on every access
                     cached_op(x, cell);
                 });
             }));
    }
}

void run_cached_held(const Cfg& cfg) {
    CachedKeyspace& ks = keyspace(cfg);
    for (int t : cfg.threads) {
        note("v1_cached_held threads=" + std::to_string(t));
        emit("v1_cached_held", t, 0, run_point(t, cfg.duration, [&](Ctx& x) {
                 x.loop([&] { cached_op(x, ks.cells[static_cast<std::size_t>(x.rng.key() - 1)]); });
             }));
    }
}

// Dirty N cells, then time pg::flush().
void run_cached_flush(const Cfg& cfg) {
    CachedKeyspace& ks = keyspace(cfg);
    pg::flush();  // what the hot path left behind
    for (std::int64_t n : {1'000, 10'000, 100'000}) {
        note("v1_cached_flush n=" + std::to_string(n));
        for (std::int64_t i = 0; i < n; ++i) ks.cells[static_cast<std::size_t>(i)] += 1.0;
        const std::uint64_t t0 = now_ns();
        pg::flush();
        const double secs = static_cast<double>(now_ns() - t0) / 1e9;
        Result r;
        r.ops_per_sec = static_cast<double>(n) / secs;
        char buf[64];
        std::snprintf(buf, sizeof buf, "cells=%lld;flush_ms=%.1f", static_cast<long long>(n), secs * 1e3);
        r.extra = buf;
        emit("v1_cached_flush", 1, 0, r);
    }
}

// --- pool_contention --------------------------------------------------------------

void run_pool_contention(const Cfg& cfg) {
    for (int p : cfg.pc_pools)
        for (int t : cfg.pc_threads) {
            note("pool_contention pool=" + std::to_string(p) + " threads=" + std::to_string(t));
            open_pool(cfg, p);
            emit("pool_contention", t, p, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         const std::uint64_t t0 = now_ns();
                         pg::conn c;
                         x.add_aux(now_ns() - t0);
                         x.sink += static_cast<std::uint64_t>(*c.query_one<std::int32_t>("SELECT 1"));
                     });
                 }));
        }
}

}  // namespace

void register_v1(std::vector<Scenario>& v) {
    v.push_back({"v1_select", run_select});
    v.push_back({"v1_select_hold", run_select_hold});
    v.push_back({"v1_update_cell", run_update_cell});
    v.push_back({"v1_update_cell_hot", run_update_cell_hot});
    v.push_back({"v1_tx3", run_tx3});
    v.push_back({"v1_tx3_batch", run_tx3_batch});
    v.push_back({"v1_batch16", run_batch16});
    v.push_back({"v1_cached", run_cached});
    v.push_back({"v1_cached_held", run_cached_held});
    v.push_back({"v1_cached_flush", run_cached_flush});
    v.push_back({"pool_contention", run_pool_contention});
}

}  // namespace bench
