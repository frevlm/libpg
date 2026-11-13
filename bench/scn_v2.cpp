// Closure transactions (pool::transact), pipelined sends and batch inserts.
#include <chrono>
#include <latch>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include "harness.hpp"
#include "pg/pg.hpp"
#include "raw.hpp"
#include "tags.hpp"

namespace bench {
namespace {

const money kOne = money::parse("1");

pg::pool_options pool_of(const Cfg& cfg, int size) {
    pg::pool_options o;
    o.dsn = cfg.dsn;
    o.max_conns = size;
    return o;
}

// Opens every connection of the pool up front: one borrower per connection, all at once.
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

// --- v2_transact_tx3 --------------------------------------------------------------

void run_transact_tx3(const Cfg& cfg) {
    raw::run_once(cfg.dsn, "TRUNCATE events");
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note("v2_transact_tx3 pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool p(pool_of(cfg, size));
            warm(p, size);
            emit("v2_transact_tx3", t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         const std::int64_t id = x.rng.key();
                         x.sink += p.transact([&](pg::conn& c) -> std::uint64_t {
                             const auto row = c.query_one<std::string, money>(
                                 pg::sql{"SELECT name, balance FROM accounts WHERE id = {}", id});
                             c.exec(pg::sql{"UPDATE accounts SET balance = balance + {} WHERE id = {}", kOne, id});
                             c.exec(pg::sql{"INSERT INTO events(account_id, amount, note) VALUES ({}, {}, {})", id, kOne,
                                            "bench"});
                             return std::get<0>(row).size();
                         });
                     });
                 }));
        }
}

// --- v2_list_page -----------------------------------------------------------------

void run_list_page(const Cfg& cfg) {
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note("v2_list_page pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool p(pool_of(cfg, size));
            warm(p, size);
            emit("v2_list_page", t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         const std::int64_t k = x.rng.key(1000);
                         pg::conn c(p);
                         const std::int64_t n = c.query_one<std::int64_t>(
                             pg::sql{"SELECT count(*) FROM submissions WHERE user_id = {}", k});
                         const auto rows = c.query<std::int64_t, std::string>(
                             pg::sql{"SELECT id, title FROM submissions WHERE user_id = {} ORDER BY id DESC LIMIT 15", k});
                         x.sink += static_cast<std::uint64_t>(n) + rows.size();
                     });
                 }));
        }
}

void run_list_page_sent(const Cfg& cfg) {
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note("v2_list_page_sent pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool p(pool_of(cfg, size));
            warm(p, size);
            emit("v2_list_page_sent", t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         const std::int64_t k = x.rng.key(1000);
                         pg::conn c(p);
                         auto a = c.send<std::int64_t>(
                             pg::sql{"SELECT count(*) FROM submissions WHERE user_id = {}", k});
                         auto b = c.send<std::int64_t, std::string>(
                             pg::sql{"SELECT id, title FROM submissions WHERE user_id = {} ORDER BY id DESC LIMIT 15", k});
                         x.sink += static_cast<std::uint64_t>(a.one()) + b.all().size();
                     });
                 }));
        }
}

// --- v2_tx_hot --------------------------------------------------------------------

void run_tx_hot(const Cfg& cfg) {
    raw::run_once(cfg.dsn, "TRUNCATE events");
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note("v2_tx_hot pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool p(pool_of(cfg, size));
            warm(p, size);
            emit("v2_tx_hot", t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         const std::int64_t k = x.rng.key(16) + 1;
                         x.sink += p.transact([&](pg::conn& c) -> std::uint64_t {
                             const money bal = c.query_one<money>(
                                 pg::sql{"SELECT balance FROM accounts WHERE id = {} FOR UPDATE", k});
                             c.exec(pg::sql{"INSERT INTO events(account_id, amount, note) VALUES ({}, {}, {})", k, kOne,
                                            "bench"});
                             c.exec(pg::sql{"INSERT INTO events(account_id, amount, note) VALUES ({}, {}, {})", k, kOne,
                                            "bench"});
                             return static_cast<std::uint64_t>(bal.units & 0xff);
                         });
                     });
                 }));
        }
}

void run_tx_hot_sent(const Cfg& cfg) {
    raw::run_once(cfg.dsn, "TRUNCATE events");
    for (int size : cfg.pools)
        for (int t : cfg.threads) {
            note("v2_tx_hot_sent pool=" + std::to_string(size) + " threads=" + std::to_string(t));
            pg::pool p(pool_of(cfg, size));
            warm(p, size);
            emit("v2_tx_hot_sent", t, size, run_point(t, cfg.duration, [&](Ctx& x) {
                     x.loop([&] {
                         const std::int64_t k = x.rng.key(16) + 1;
                         x.sink += p.transact([&](pg::conn& c) -> std::uint64_t {
                             const money bal = c.query_one<money>(
                                 pg::sql{"SELECT balance FROM accounts WHERE id = {} FOR UPDATE", k});
                             (void)c.send(pg::sql{"INSERT INTO events(account_id, amount, note) VALUES ({}, {}, {})", k,
                                                  kOne, "bench"});
                             (void)c.send(pg::sql{"INSERT INTO events(account_id, amount, note) VALUES ({}, {}, {})", k,
                                                  kOne, "bench"});
                             return static_cast<std::uint64_t>(bal.units & 0xff);
                         });
                     });
                 }));
        }
}

// --- v2_batch_insert_* ------------------------------------------------------------

// Runs one batch of kBatchRows rows in a transaction per loop, `insert_batch` doing the writes.
template <class F>
void run_batch_insert(const Cfg& cfg, const char* name, F&& insert_batch) {
    raw::run_once(cfg.dsn, "TRUNCATE bench_requests");
    const std::vector<tag::request_row> rows = batch_rows();
    for (int t : cfg.threads) {
        note(std::string(name) + " threads=" + std::to_string(t));
        pg::pool p(pool_of(cfg, t));
        warm(p, t);
        emit(name, t, t, run_point(t, cfg.duration, [&](Ctx& x) {
                 x.loop([&] {
                     p.transact([&](pg::conn& c) { insert_batch(c, rows); });
                     return std::uint64_t{kBatchRows};
                 });
             }));
    }
}

// The single-row INSERT that pg::insert runs, as a statement to send.
pg::sql insert_statement(const tag::request_row& row) {
    pg::sql cols, values;
    std::size_t i = 0;
    row.pg_each([&](const auto& m) {
        const pg::sql sep = i ? pg::sql{", "} : pg::sql{};
        cols += sep + pg::ident(tag::request_row::pg_names[i++]);
        values += sep + pg::sql{"{}", m};
    });
    return pg::sql{"INSERT INTO {} ({}) VALUES ({})", pg::ident(tag::request_row::pg_table::name), cols, values};
}

void run_batch_insert_rows(const Cfg& cfg) {
    run_batch_insert(cfg, "v2_batch_insert_rows", [](pg::conn& c, const std::vector<tag::request_row>& rows) {
        for (const tag::request_row& row : rows) (void)pg::insert(c, row);
    });
}

void run_batch_insert_sent(const Cfg& cfg) {
    run_batch_insert(cfg, "v2_batch_insert_sent", [](pg::conn& c, const std::vector<tag::request_row>& rows) {
        for (const tag::request_row& row : rows) (void)c.send(insert_statement(row));
    });
}

void run_batch_insert_copy(const Cfg& cfg) {
    run_batch_insert(cfg, "v2_batch_insert_copy", [](pg::conn& c, const std::vector<tag::request_row>& rows) {
        (void)pg::insert_all(c, rows);
    });
}

}  // namespace

void register_v2(std::vector<Scenario>& v) {
    v.push_back({"v2_transact_tx3", run_transact_tx3});
    v.push_back({"v2_list_page", run_list_page});
    v.push_back({"v2_list_page_sent", run_list_page_sent});
    v.push_back({"v2_tx_hot", run_tx_hot});
    v.push_back({"v2_tx_hot_sent", run_tx_hot_sent});
    v.push_back({"v2_batch_insert_rows", run_batch_insert_rows});
    v.push_back({"v2_batch_insert_sent", run_batch_insert_sent});
    v.push_back({"v2_batch_insert_copy", run_batch_insert_copy});
}

}  // namespace bench
