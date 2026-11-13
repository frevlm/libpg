#include <catch2/catch_test_macros.hpp>

#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "pg/pg.hpp"
#include "pg/writeback.hpp"

using namespace std::chrono_literals;
using pg::sql;
using dec = pg::decimal<6>;

namespace {

// Tags and a write shape as pg_tablegen writes them.
struct balances {
    static constexpr std::string_view name = "wb_balances";
    using key = std::tuple<std::int64_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct amount {
    using pg_table = balances;
    using pg_type = dec;
    static constexpr std::string_view pg_name = "amount";
};
struct points {
    using pg_table = balances;
    using pg_type = std::int64_t;
    static constexpr std::string_view pg_name = "points";
};
struct hits {
    using pg_table = balances;
    using pg_type = std::int32_t;
    static constexpr std::string_view pg_name = "hits";
};
struct tiny {
    using pg_table = balances;
    using pg_type = pg::decimal<18>;
    static constexpr std::string_view pg_name = "tiny";
};
// Two write shapes of one table that give `points` different C++ types.
template <class T>
struct points_write {
    using pg_table = balances;
    std::int64_t id{};
    T points{};
    static constexpr std::array<std::string_view, 2> pg_names{"id", "points"};
    template <class F>
    void pg_each(F&& f) const {
        f(id);
        f(points);
    }
};
static_assert(pg::write_shape<points_write<std::int32_t>>);
struct stock {
    static constexpr std::string_view name = "wb_stock";
    using key = std::tuple<std::int32_t, std::string>;
    static constexpr std::array<std::string_view, 2> key_columns{"warehouse", "sku"};
};
struct qty {
    using pg_table = stock;
    using pg_type = std::int64_t;
    static constexpr std::string_view pg_name = "qty";
};
struct ledger {
    static constexpr std::string_view name = "wb_ledger";
};
struct ledger_write {
    using pg_table = ledger;
    pg::field<std::int64_t> id{};
    std::int64_t account{};
    dec amount{};
    std::optional<std::string> memo{};
    std::optional<pg::jsonb> meta{};
    static constexpr std::array<std::string_view, 5> pg_names{"id", "account", "amount", "memo", "meta"};
    template <class F>
    void pg_each(F&& f) const {
        f(id);
        f(account);
        f(amount);
        f(memo);
        f(meta);
    }
};
static_assert(pg::write_shape<ledger_write>);

const std::string database = "libpg_writeback";

std::string dsn_for(const std::string& name) {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    const std::string dsn = d;
    return dsn.substr(0, dsn.rfind('/') + 1) + name;
}

void run_admin(const std::string& script) {
    std::unique_ptr<PGconn, decltype(&PQfinish)> c(PQconnectdb(dsn_for("libpg_test").c_str()), PQfinish);
    if (PQstatus(c.get()) != CONNECTION_OK) throw std::runtime_error(PQerrorMessage(c.get()));
    std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(c.get(), script.c_str()), PQclear);
    if (PQresultStatus(r.get()) != PGRES_COMMAND_OK) throw std::runtime_error(PQresultErrorMessage(r.get()));
}

// A database of its own: other test files reset the libpg schema of libpg_test.
pg::pool& db() {
    static pg::pool p([] {
        run_admin("DROP DATABASE IF EXISTS " + database + " WITH (FORCE)");
        run_admin("CREATE DATABASE " + database);
        pg::pool_options o;
        o.dsn = dsn_for(database);
        o.max_conns = 12;
        return o;
    }());
    return p;
}

void run(const sql& q) {
    pg::conn c(db());
    c.exec(q);
}

template <class T>
T scalar(const sql& q) {
    pg::conn c(db());
    return c.query_one<T>(q);
}

constexpr std::string_view schema = R"SQL(
CREATE TABLE wb_balances (id int8 PRIMARY KEY, amount numeric(18,6) NOT NULL DEFAULT 0, points int8 NOT NULL DEFAULT 0,
                          hits int4 NOT NULL DEFAULT 0, tiny numeric(19,18) NOT NULL DEFAULT 0);
CREATE TABLE wb_stock (warehouse int4, sku text, qty int8 NOT NULL, PRIMARY KEY (warehouse, sku));
CREATE TABLE wb_ledger (id bigserial PRIMARY KEY, account int8 NOT NULL, amount numeric(18,6) NOT NULL CHECK (amount > 0),
                        memo text, meta jsonb);
INSERT INTO wb_balances (id, amount) VALUES (1, 10), (2, 20), (3, 30), (4, 40);
INSERT INTO wb_stock VALUES (1, 'a b', 5), (2, 'é"x', 6);
)SQL";

void reset() {
    run(sql{"DROP SCHEMA IF EXISTS libpg CASCADE"});
    run(sql{"DROP TABLE IF EXISTS wb_balances, wb_stock, wb_ledger"});
    pg::conn c(db());
    PQexec(c.raw(), std::string(schema).c_str());
}

dec d(std::string_view text) { return dec::parse(text); }

dec db_amount(std::int64_t id) { return scalar<dec>(sql{"SELECT amount FROM wb_balances WHERE id = {}", id}); }
std::int64_t db_points(std::int64_t id) { return scalar<std::int64_t>(sql{"SELECT points FROM wb_balances WHERE id = {}", id}); }
std::int64_t receipts() { return scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.flush_receipts"}); }

// Options for a writeback that only writes on flush().
pg::writeback_options manual() {
    pg::writeback_options o;
    o.interval = 1h;
    return o;
}

// Collects lost postings.
struct lost_log {
    std::mutex m;
    std::vector<pg::lost_posting> v;
    std::function<void(const pg::lost_posting&)> sink() {
        return [this](const pg::lost_posting& l) {
            std::lock_guard lk(m);
            v.push_back(l);
        };
    }
    std::vector<pg::lost_posting> get() {
        std::lock_guard lk(m);
        return v;
    }
};

template <class F>
bool eventually(F f, std::chrono::milliseconds limit = 10s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (f()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return f();
}

}  // namespace

TEST_CASE("submit, get and flush with integer, decimal and composite-key counters", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    CHECK(wb.get<amount>(1) == d("10"));
    wb.submit(pg::posting{}.add<amount>(d("2.5"), 1).add<qty>(3, 1, "a b").add<qty>(-1, 2, "é\"x").add<hits>(4, 1).add<hits>(-1, 1));
    CHECK(wb.get<amount>(1) == d("12.5"));
    CHECK(wb.get<hits>(1) == 3);
    CHECK(db_amount(1) == d("10"));
    CHECK(wb.stats().backlog == 1);
    wb.flush();
    CHECK(db_amount(1) == d("12.5"));
    CHECK(scalar<std::int32_t>(sql{"SELECT hits FROM wb_balances WHERE id = 1"}) == 3);
    CHECK(scalar<std::int64_t>(sql{"SELECT qty FROM wb_stock WHERE warehouse = 1"}) == 8);
    CHECK(scalar<std::int64_t>(sql{"SELECT qty FROM wb_stock WHERE warehouse = 2"}) == 5);
    CHECK(wb.get<qty>(2, "é\"x") == 5);
    CHECK(wb.stats().backlog == 0);
    CHECK(wb.stats().committed == 1);
    CHECK(receipts() == 1);
    REQUIRE_THROWS_AS(wb.get<amount>(99), pg::not_found);
}

TEST_CASE("a posting commits whole or is lost whole while the rest of its batch commits", "[writeback]") {
    reset();
    lost_log lost;
    auto o = manual();
    o.on_lost = lost.sink();
    pg::writeback wb(db(), o);
    CHECK(wb.get<amount>(1) == d("10"));
    CHECK(wb.get<points>(2) == 0);

    ledger_write good{.account = 1, .amount = d("5"), .memo = "ok", .meta = pg::jsonb{R"({"src": "test"})"}};
    ledger_write bad{.account = 1, .amount = d("-1")};
    wb.submit(pg::posting{}.insert(good).add<amount>(d("5"), 1));
    wb.submit(pg::posting{}.insert(bad).add<amount>(d("-1"), 1).add<points>(7, 2));
    wb.submit(pg::posting{}.add<points>(1, 2));
    CHECK(wb.get<points>(2) == 8);
    wb.flush();

    const auto l = lost.get();
    REQUIRE(l.size() == 1);
    INFO(l[0].reason); CHECK(l[0].kind == pg::error_kind::data);
    CHECK(l[0].reason.find("23514") != std::string::npos);
    CHECK(l[0].description.size() == 3);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM wb_ledger"}) == 1);
    CHECK(scalar<std::string>(sql{"SELECT meta->>'src' FROM wb_ledger"}) == "test");
    CHECK(db_amount(1) == d("15"));
    CHECK(db_points(2) == 1);
    CHECK(wb.get<amount>(1) == d("15"));
    CHECK(wb.get<points>(2) == 1);
    const auto st = wb.stats();
    CHECK(st.committed == 2);
    CHECK(st.lost == 1);
    CHECK(st.backlog == 0);
}

TEST_CASE("a delta to a row that does not exist is lost and reverted", "[writeback]") {
    reset();
    lost_log lost;
    auto o = manual();
    o.on_lost = lost.sink();
    pg::writeback wb(db(), o);
    wb.submit(pg::posting{}.add<points>(1, 77));
    wb.submit(pg::posting{}.add<points>(2, 1));
    wb.flush();
    REQUIRE(lost.get().size() == 1);
    CHECK(lost.get()[0].kind == pg::error_kind::data);
    CHECK(db_points(1) == 2);
    REQUIRE_THROWS_AS(wb.get<points>(77), pg::not_found);
}

TEST_CASE("inserted rows with different column sets are written by their own statements", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    ledger_write a{.account = 1, .amount = d("1")};
    ledger_write b{.id = 500, .account = 2, .amount = d("2"), .memo = "m"};
    ledger_write c{.account = 3, .amount = d("3"), .meta = pg::jsonb{"{}"}};
    wb.submit(pg::posting{}.insert(a));
    wb.submit(pg::posting{}.insert(b).insert(c));
    wb.flush();
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM wb_ledger"}) == 3);
    CHECK(scalar<std::string>(sql{"SELECT memo FROM wb_ledger WHERE id = 500"}) == "m");
    CHECK(scalar<std::int64_t>(sql{"SELECT count(meta) FROM wb_ledger"}) == 1);
}

TEST_CASE("many threads submit to hot and spread keys and the database ends exact", "[writeback]") {
    reset();
    run(sql{"INSERT INTO wb_balances (id) SELECT g FROM generate_series(100, 163) g"});
    pg::writeback_options o;
    o.interval = 20ms;
    pg::writeback wb(db(), o);
    constexpr int threads = 8, rounds = 400;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            for (int i = 0; i < rounds; ++i) {
                wb.submit(pg::posting{}.add<points>(1, 1).add<amount>(d("0.5"), 100 + (t * 7 + i) % 64).add<hits>(1, 2));
                if (i % 50 == 0) (void)wb.get<points>(100 + i % 64);
            }
        });
    for (auto& t : ts) t.join();
    wb.flush();
    CHECK(db_points(1) == threads * rounds);
    CHECK(scalar<std::int32_t>(sql{"SELECT hits FROM wb_balances WHERE id = 2"}) == threads * rounds);
    CHECK(scalar<dec>(sql{"SELECT sum(amount) FROM wb_balances WHERE id >= 100"}) == d("1600"));
    CHECK(wb.get<points>(1) == threads * rounds);
    CHECK(wb.stats().committed == threads * rounds);
    CHECK(wb.stats().backlog == 0);
}

TEST_CASE("over the backlog limit submit throws and records nothing", "[writeback]") {
    reset();
    auto o = manual();
    o.backlog_limit = 3;
    pg::writeback wb(db(), o);
    CHECK(wb.get<points>(1) == 0);
    for (int i = 0; i < 3; ++i) wb.submit(pg::posting{}.add<points>(1, 1));
    REQUIRE_THROWS_AS(wb.submit(pg::posting{}.add<points>(100, 1)), pg::backlog_full);
    CHECK(wb.get<points>(1) == 3);
    CHECK(wb.stats().backlog == 3);
    wb.flush();
    CHECK(db_points(1) == 3);
    wb.submit(pg::posting{}.add<points>(1, 1));
    wb.flush();
    CHECK(db_points(1) == 4);
}

TEST_CASE("an empty posting is not recorded", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    wb.submit(pg::posting{});
    wb.flush();
    CHECK(wb.stats().backlog == 0);
    CHECK(receipts() == 0);
}

TEST_CASE("a batch lost before it was sent is replayed", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    wb.submit(pg::posting{}.add<points>(5, 3));
    pg::detail::inject_fault(wb, 1);
    wb.flush();
    CHECK(db_points(3) == 5);
    CHECK(wb.stats().replays == 1);
    CHECK(wb.stats().committed == 1);
    CHECK(receipts() == 1);
}

TEST_CASE("a batch lost after commit is replayed and counted once", "[writeback]") {
    reset();
    ledger_write r{.account = 1, .amount = d("1")};
    pg::writeback wb(db(), manual());
    wb.submit(pg::posting{}.insert(r).add<points>(5, 3).add<amount>(d("1.5"), 3));
    pg::detail::inject_fault(wb, 2);
    wb.flush();
    CHECK(db_points(3) == 5);
    CHECK(db_amount(3) == d("31.5"));
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM wb_ledger"}) == 1);
    CHECK(wb.get<points>(3) == 5);
    CHECK(wb.stats().replays == 1);
    CHECK(wb.stats().committed == 1);
    CHECK(receipts() == 1);
}

TEST_CASE("a slot read since it was loaded is re-read after refresh", "[writeback]") {
    reset();
    pg::writeback_options o;
    o.interval = 20ms;
    o.refresh = 100ms;
    pg::writeback wb(db(), o);
    CHECK(wb.get<points>(1) == 0);
    run(sql{"UPDATE wb_balances SET points = 40 WHERE id = 1"});
    CHECK(eventually([&] { return wb.get<points>(1) == 40; }, o.refresh + o.interval + 2s));
    wb.submit(pg::posting{}.add<points>(2, 1));
    wb.flush();
    run(sql{"UPDATE wb_balances SET points = 100 WHERE id = 1"});
    CHECK(eventually([&] { return wb.get<points>(1) == 100; }, 3s));
}

TEST_CASE("a slot nobody reads is evicted and loaded again", "[writeback]") {
    reset();
    pg::writeback_options o;
    o.interval = 20ms;
    o.refresh = 100ms;
    pg::writeback wb(db(), o);
    CHECK(wb.get<points>(1) == 0);
    std::this_thread::sleep_for(600ms);
    run(sql{"UPDATE wb_balances SET points = 9 WHERE id = 1"});
    CHECK(wb.get<points>(1) == 9);
}

TEST_CASE("the destructor flushes", "[writeback]") {
    reset();
    {
        pg::writeback wb(db(), manual());
        wb.submit(pg::posting{}.add<points>(6, 4).add<qty>(2, 1, "a b"));
    }
    CHECK(db_points(4) == 6);
    CHECK(scalar<std::int64_t>(sql{"SELECT qty FROM wb_stock WHERE warehouse = 1"}) == 7);
}


TEST_CASE("a failed refresh does not drop the batch", "[writeback]") {
    reset();
    auto o = manual();
    o.interval = 50ms;
    o.refresh = 100ms;
    pg::writeback wb(db(), o);
    CHECK(wb.get<qty>(1, "a b") == 5);
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop) {
            (void)wb.get<qty>(1, "a b");
            std::this_thread::sleep_for(10ms);
        }
    });
    std::this_thread::sleep_for(300ms);
    run(sql{"ALTER TABLE wb_stock RENAME COLUMN qty TO qty2"});
    wb.submit(pg::posting{}.add<points>(7, 1));
    wb.flush();
    run(sql{"ALTER TABLE wb_stock RENAME COLUMN qty2 TO qty"});
    stop = true;
    reader.join();
    CHECK(db_points(1) == 7);
    CHECK(wb.stats().committed == 1);
    CHECK(wb.stats().lost == 0);
}

TEST_CASE("an acquire timeout replays the batch instead of losing it", "[writeback]") {
    reset();
    pg::pool_options po;
    po.dsn = dsn_for(database);
    po.max_conns = 1;
    po.acquire_timeout = 200ms;
    pg::pool p(po);
    lost_log lost;
    auto o = manual();
    o.on_lost = lost.sink();
    pg::writeback wb(p, o);
    {
        pg::conn c(p);
        pg::transaction tx(c);
        c.exec(sql{"SELECT 1"});
        wb.submit(pg::posting{}.add<points>(3, 1));
        auto f = std::async(std::launch::async, [&] { wb.flush(); });
        std::this_thread::sleep_for(600ms);
        tx.commit();
        f.get();
    }
    CHECK(lost.get().empty());
    CHECK(db_points(1) == 3);
    CHECK(wb.stats().replays > 0);
}

TEST_CASE("decimal deltas of 18 digits reach the database exactly", "[writeback]") {
    reset();
    using dec18 = pg::decimal<18>;
    {
        pg::writeback wb(db(), manual());
        wb.submit(pg::posting{}.add<tiny>(dec18::parse("1.234567890123456789"), 1));
        wb.submit(pg::posting{}.add<tiny>(dec18::parse("7.000000000000000001"), 1));
        wb.flush();
    }
    CHECK(scalar<std::string>(sql{"SELECT tiny::text FROM wb_balances WHERE id = 1"}) == "8.234567890123456790");
    pg::writeback wb(db(), manual());
    CHECK(wb.get<tiny>(1) == dec18::parse("8.23456789012345679"));
}

TEST_CASE("a batch holds at most 10000 postings", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    for (int i = 0; i < 25'000; ++i) wb.submit(pg::posting{}.add<points>(1, 1));
    wb.flush();
    CHECK(db_points(1) == 25'000);
    CHECK(receipts() == 3);
    CHECK(wb.stats().committed == 25'000);
}

TEST_CASE("a batch that keeps hitting a lock is halved and loads are not blocked meanwhile", "[writeback]") {
    reset();
    pg::pool_options po;
    po.dsn = dsn_for(database) + "?options=-c%20lock_timeout%3D100ms";
    po.max_conns = 4;
    pg::pool p(po);
    pg::writeback wb(p, manual());
    std::thread locker([] {
        pg::conn c(db());
        pg::transaction tx(c);
        c.exec(sql{"SELECT 1 FROM wb_balances WHERE id = 1 FOR UPDATE"});
        std::this_thread::sleep_for(3s);
        tx.commit();
    });
    std::this_thread::sleep_for(200ms);
    wb.submit(pg::posting{}.add<points>(1, 2));
    wb.submit(pg::posting{}.add<points>(1, 1));
    auto f = std::async(std::launch::async, [&] { wb.flush(); });
    CHECK(eventually([] { return db_points(2) == 1; }, 2s));
    CHECK(db_points(1) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(wb.get<points>(3) == 0);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    locker.join();
    f.get();
    CHECK(db_points(1) == 1);
    CHECK(wb.stats().lost == 0);
}

TEST_CASE("a commit of unknown outcome that meets the shutdown is settled by its receipt", "[writeback]") {
    reset();
    lost_log lost;
    std::thread locker;
    {
        auto o = manual();
        o.shutdown_timeout = 50ms;
        o.on_lost = lost.sink();
        pg::writeback wb(db(), o);
        locker = std::thread([] {
            pg::conn c(db());
            pg::transaction tx(c);
            c.exec(sql{"SELECT 1 FROM wb_balances WHERE id = 1 FOR UPDATE"});
            std::this_thread::sleep_for(400ms);
            tx.commit();
        });
        std::this_thread::sleep_for(100ms);
        wb.submit(pg::posting{}.add<points>(5, 1));
        pg::detail::inject_fault(wb, 2);
    }
    locker.join();
    CHECK(lost.get().empty());
    CHECK(db_points(1) == 5);
}

TEST_CASE("get inside a transaction of the pool throws for hot and cold keys alike", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    CHECK(wb.get<points>(1) == 0);
    for (const std::int64_t id : {1, 2})
        REQUIRE_THROWS_AS(db().transact([&](pg::conn& c) { return c.exec(sql{"SELECT 1"}), wb.get<points>(id); }), pg::nested_borrow);
}

TEST_CASE("write shapes with different types for a column are inserted by their own statements", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    wb.submit(pg::posting{}.insert(points_write<std::int64_t>{.id = 10, .points = 1}).insert(points_write<std::int32_t>{.id = 11, .points = 2}));
    wb.flush();
    CHECK(db_points(10) == 1);
    CHECK(db_points(11) == 2);
    CHECK(receipts() == 1);
}
