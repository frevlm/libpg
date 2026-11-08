// v2 scenarios: the write-back cache (pg::writeback) and the closure transaction (pool::transact).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#include "harness.hpp"
#include "pg/pg.hpp"
#include "raw.hpp"

namespace bench {
namespace {

using money = pg::decimal<6>;

// Tags and a write shape in the form pg_tablegen writes them.
namespace tag {
struct accounts_ {
    static constexpr std::string_view name = "accounts";
    using key = std::tuple<std::int64_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct balance {
    using pg_table = accounts_;
    using pg_type = money;
    static constexpr std::string_view pg_name = "balance";
};
struct events_ {
    static constexpr std::string_view name = "events";
};
struct event_write {
    using pg_table = events_;
    std::int64_t account_id{};
    money amount{};
    std::string note{};
    static constexpr std::array<std::string_view, 3> pg_names{"account_id", "amount", "note"};
    template <class F>
    void pg_each(F&& f) const {
        f(account_id);
        f(amount);
        f(note);
    }
};
static_assert(pg::write_shape<event_write>);

struct bench_requests_ {
    static constexpr std::string_view name = "bench_requests";
};

// revlm's requests row without its identity column.
struct request_row {
    using pg_table = bench_requests_;
    static constexpr std::array<std::string_view, 34> pg_names{
        "time",          "request_id",      "response_id",       "endpoint",      "method",
        "user_id",       "token_id",        "input_tokens",      "output_tokens", "cache_read_tokens",
        "cache_creation_1h_tokens", "cache_creation_5m_tokens", "tier_multiplier", "service_tier",
        "channel_multiplier", "channel_id", "status_code",       "latency_ms",    "first_token_latency_ms",
        "error_class",   "error_message",   "is_stream",         "model",         "usd",
        "outcome",       "attempts",        "body_read_us",      "prep_us",       "upstream_head_us",
        "upstream_wait_us", "relay_us",     "client_write_us",   "request_bytes", "response_bytes"};
    pg::timestamptz time{};
    std::optional<std::string> request_id, response_id, endpoint, method;
    std::int64_t user_id = 0;
    std::int64_t token_id = 0;
    std::int32_t input_tokens = 0;
    std::int32_t output_tokens = 0;
    std::int32_t cache_read_tokens = 0;
    std::int32_t cache_creation_1h_tokens = 0;
    std::int32_t cache_creation_5m_tokens = 0;
    double tier_multiplier = 1.0;
    std::optional<std::string> service_tier;
    double channel_multiplier = 1.0;
    std::int64_t channel_id = 0;
    std::int32_t status_code = 0;
    std::int32_t latency_ms = 0;
    std::int32_t first_token_latency_ms = 0;
    std::optional<std::string> error_class, error_message;
    bool is_stream = false;
    std::optional<std::string> model;
    pg::decimal<6> usd{};
    std::optional<std::string> outcome;
    std::int32_t attempts = 0;
    std::int64_t body_read_us = 0;
    std::int64_t prep_us = 0;
    std::int64_t upstream_head_us = 0;
    std::int64_t upstream_wait_us = 0;
    std::int64_t relay_us = 0;
    std::int64_t client_write_us = 0;
    std::int64_t request_bytes = 0;
    std::int64_t response_bytes = 0;

    template <class F>
    void pg_each(F&& f) const {
        f(time);
        f(request_id);
        f(response_id);
        f(endpoint);
        f(method);
        f(user_id);
        f(token_id);
        f(input_tokens);
        f(output_tokens);
        f(cache_read_tokens);
        f(cache_creation_1h_tokens);
        f(cache_creation_5m_tokens);
        f(tier_multiplier);
        f(service_tier);
        f(channel_multiplier);
        f(channel_id);
        f(status_code);
        f(latency_ms);
        f(first_token_latency_ms);
        f(error_class);
        f(error_message);
        f(is_stream);
        f(model);
        f(usd);
        f(outcome);
        f(attempts);
        f(body_read_us);
        f(prep_us);
        f(upstream_head_us);
        f(upstream_wait_us);
        f(relay_us);
        f(client_write_us);
        f(request_bytes);
        f(response_bytes);
    }
};
static_assert(pg::write_shape<request_row>);
}  // namespace tag

constexpr std::int64_t kHotRows = 16;
constexpr int kWbPool = 16;

// Seconds measured per write-back point: submits outrun the writer, so a point
// longer than this backs up more postings than the backlog limit holds.
double wb_seconds(const Cfg& cfg) { return std::min(cfg.duration, 1.0); }

pg::pool_options pool_of(const Cfg& cfg, int size) {
    pg::pool_options o;
    o.dsn = cfg.dsn;
    o.pooler = cfg.pooler;
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

// A pool plus a writeback that writes only on flush(). The libpg schema is created by an
// empty migration run.
struct Wb {
    pg::pool pool;
    std::unique_ptr<pg::writeback> wb;

    Wb(const Cfg& cfg, std::chrono::milliseconds interval) : pool(pool_of(cfg, kWbPool)) {
        pg::migrate(pool, {});
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
    // server's buffers, as v1_cached_flush's first flush does.
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

constexpr std::size_t kBatchRows = 200;

// A batch of requests rows; error fields stay NULL.
std::vector<tag::request_row> batch_rows() {
    std::vector<tag::request_row> rows(kBatchRows);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        tag::request_row& r = rows[i];
        r.time = pg::timestamptz(std::chrono::microseconds{1'700'000'000'000'000});
        r.request_id = "req_" + std::to_string(i);
        r.response_id = "resp_" + std::to_string(i);
        r.endpoint = "/v1/messages";
        r.method = "POST";
        r.user_id = static_cast<std::int64_t>(i) + 1;
        r.token_id = static_cast<std::int64_t>(i) + 1;
        r.input_tokens = 1024;
        r.output_tokens = 256;
        r.cache_read_tokens = 128;
        r.cache_creation_1h_tokens = 0;
        r.cache_creation_5m_tokens = 64;
        r.tier_multiplier = 1.0;
        r.service_tier = "standard";
        r.channel_multiplier = 1.0;
        r.channel_id = static_cast<std::int64_t>(i % 7);
        r.status_code = 200;
        r.latency_ms = 850;
        r.first_token_latency_ms = 300;
        r.is_stream = true;
        r.model = "claude-sonnet-4";
        r.usd = pg::decimal<6>::from_units(1234);
        r.outcome = "ok";
        r.attempts = 1;
        r.body_read_us = 1200;
        r.prep_us = 30;
        r.upstream_head_us = 410'000;
        r.upstream_wait_us = 390'000;
        r.relay_us = 20'000;
        r.client_write_us = 900;
        r.request_bytes = 48'000;
        r.response_bytes = 12'000;
    }
    return rows;
}

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

void run_batch_insert_rows(const Cfg& cfg) {
    run_batch_insert(cfg, "v2_batch_insert_rows", [](pg::conn& c, const std::vector<tag::request_row>& rows) {
        for (const tag::request_row& row : rows) (void)pg::insert(c, row);
    });
}

void run_batch_insert_sent(const Cfg& cfg) {
    run_batch_insert(cfg, "v2_batch_insert_sent", [](pg::conn& c, const std::vector<tag::request_row>& rows) {
        for (const tag::request_row& row : rows) (void)c.send(pg::detail::insert_statement(row));
    });
}

void run_batch_insert_copy(const Cfg& cfg) {
    run_batch_insert(cfg, "v2_batch_insert_copy", [](pg::conn& c, const std::vector<tag::request_row>& rows) {
        (void)pg::insert_all(c, rows);
    });
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

void register_v2(std::vector<Scenario>& v) {
    v.push_back({"wb_get_add", run_wb_get_add});
    v.push_back({"wb_get_add_hot", run_wb_get_add_hot});
    v.push_back({"wb_posting", run_wb_posting});
    v.push_back({"wb_flush", run_wb_flush});
    v.push_back({"v2_transact_tx3", run_transact_tx3});
    v.push_back({"v2_list_page", run_list_page});
    v.push_back({"v2_list_page_sent", run_list_page_sent});
    v.push_back({"v2_tx_hot", run_tx_hot});
    v.push_back({"v2_tx_hot_sent", run_tx_hot_sent});
    v.push_back({"v2_batch_insert_rows", run_batch_insert_rows});
    v.push_back({"v2_batch_insert_sent", run_batch_insert_sent});
    v.push_back({"v2_batch_insert_copy", run_batch_insert_copy});
    v.push_back({"wb_requests", run_wb_requests});
}

}  // namespace bench
