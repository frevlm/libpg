// Library scenarios: pooled queries, single-cell updates, transactions, and the
// pool itself.
#include <latch>
#include <thread>
#include <tuple>
#include <vector>

#include "harness.hpp"
#include "pg/pg.hpp"
#include "raw.hpp"

namespace bench {
namespace {

using money = pg::decimal<6>;

constexpr std::int64_t kHotRows = 16;

pg::sql select_account(std::int64_t id) { return pg::sql{"SELECT name, balance FROM accounts WHERE id = {}", id}; }

pg::pool_options pool_of(const Cfg& cfg, int size) {
    pg::pool_options o;
    o.dsn = cfg.dsn;
    o.max_conns = size;
    return o;
}

// Opens every connection of the pool up front so that connection setup stays
// out of the measurement: one borrower per connection, all at once.
void warm(pg::pool& p, int size) {
    std::latch all(size);
    std::vector<std::thread> ts;
    for (int i = 0; i < size; ++i)
        ts.emplace_back([&] {
            pg::conn c(p);
            all.arrive_and_wait();
        });
    for (auto& t : ts) t.join();
}

std::uint64_t touch(const std::tuple<std::string, money>& row) {
    return std::get<0>(row).size() + static_cast<std::uint64_t>(std::get<1>(row).units & 0xff);
}

// For every pool size and thread count: open the pool, run `step` on all threads.
template <class Step>
void sweep_pool(const Cfg& cfg, const char* name, Step step) {
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note(std::string(name) + " pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool p(pool_of(cfg, size));
            warm(p, size);
            emit(name, t, size, run_point(t, cfg.duration, [&](Ctx& x) { x.loop([&] { step(p, x); }); }));
        }
}

// --- v1_select --------------------------------------------------------------------

void run_select(const Cfg& cfg) {
    sweep_pool(cfg, "v1_select", [](pg::pool& p, Ctx& x) {
        pg::conn c(p);
        x.sink += touch(c.query_one<std::string, money>(select_account(x.rng.key())));
    });
}

void run_select_hold(const Cfg& cfg) {
    for (int t : cfg.threads) {
        note("v1_select_hold threads=" + std::to_string(t));
        pg::pool p(pool_of(cfg, t));
        warm(p, t);
        emit("v1_select_hold", t, t, run_point(t, cfg.duration, [&](Ctx& x) {
                 pg::conn c(p);
                 x.loop([&] { x.sink += touch(c.query_one<std::string, money>(select_account(x.rng.key()))); });
             }));
    }
}

// --- v1_update_cell ---------------------------------------------------------------

void update_cells(const Cfg& cfg, const char* name, std::int64_t rows) {
    sweep_pool(cfg, name, [rows](pg::pool& p, Ctx& x) {
        pg::conn c(p);
        c.exec(pg::sql{"UPDATE accounts SET balance = balance + {} WHERE id = {}", money::parse("1"), x.rng.key(rows)});
    });
}

void run_update_cell(const Cfg& cfg) { update_cells(cfg, "v1_update_cell", kRows); }
void run_update_cell_hot(const Cfg& cfg) { update_cells(cfg, "v1_update_cell_hot", kHotRows); }

// --- v1_tx3 -----------------------------------------------------------------------

void run_tx3(const Cfg& cfg) {
    raw::run_once(cfg.dsn, "TRUNCATE events");
    sweep_pool(cfg, "v1_tx3", [](pg::pool& p, Ctx& x) {
        const std::int64_t id = x.rng.key();
        const money one = money::parse("1");
        pg::conn c(p);
        pg::transaction tx(c);
        x.sink += touch(c.query_one<std::string, money>(select_account(id)));
        c.exec(pg::sql{"UPDATE accounts SET balance = balance + {} WHERE id = {}", one, id});
        c.exec(pg::sql{"INSERT INTO events(account_id, amount, note) VALUES ({}, {}, {})", id, one, "bench"});
        tx.commit();
    });
}

// --- pool_contention --------------------------------------------------------------

void run_pool_contention(const Cfg& cfg) {
    for (int size : cfg.pc_pools)
        for (int t : cfg.pc_threads) {
            note("pool_contention pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool p(pool_of(cfg, size));
            warm(p, size);
            emit("pool_contention", t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         const std::uint64_t t0 = now_ns();
                         pg::conn c(p);
                         x.add_aux(now_ns() - t0);
                         x.sink += static_cast<std::uint64_t>(c.query_one<std::int32_t>(pg::sql{"SELECT 1"}));
                     });
                 }));
        }
}

}  // namespace

void register_pool(std::vector<Scenario>& v) {
    v.push_back({"v1_select", run_select});
    v.push_back({"v1_select_hold", run_select_hold});
    v.push_back({"v1_update_cell", run_update_cell});
    v.push_back({"v1_update_cell_hot", run_update_cell_hot});
    v.push_back({"v1_tx3", run_tx3});
    v.push_back({"pool_contention", run_pool_contention});
}

}  // namespace bench
