#include <catch2/catch_test_macros.hpp>

#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "../src/writeback.hpp"
#include "pg/pg.hpp"

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
    using table = balances;
    using type = dec;
    static constexpr std::string_view name = "amount";
};
struct points {
    using table = balances;
    using type = std::int64_t;
    static constexpr std::string_view name = "points";
};
struct stock {
    static constexpr std::string_view name = "wb_stock";
    using key = std::tuple<std::int32_t, std::string>;
    static constexpr std::array<std::string_view, 2> key_columns{"warehouse", "sku"};
};
struct qty {
    using table = stock;
    using type = std::int64_t;
    static constexpr std::string_view name = "qty";
};
struct ledger {
    static constexpr std::string_view name = "wb_ledger";
};
struct ledger_write {
    using table = ledger;
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
CREATE TABLE wb_balances (id int8 PRIMARY KEY, amount numeric(18,6) NOT NULL DEFAULT 0, points int8 NOT NULL DEFAULT 0);
CREATE TABLE wb_stock (warehouse int4, sku text, qty int8 NOT NULL, PRIMARY KEY (warehouse, sku));
CREATE TABLE wb_ledger (id bigserial PRIMARY KEY, account int8 NOT NULL, amount numeric(18,6) NOT NULL CHECK (amount > 0),
                        memo text, meta jsonb);
INSERT INTO wb_balances VALUES (1, 10, 0), (2, 20, 0), (3, 30, 0), (4, 40, 0);
INSERT INTO wb_stock VALUES (1, 'a b', 5), (2, 'é"x', 6);
)SQL";

// A fresh schema made by pg::migrate, with the three counters watched.
void reset() {
    run(sql{"DROP SCHEMA IF EXISTS libpg CASCADE"});
    run(sql{"DROP TABLE IF EXISTS wb_balances, wb_stock, wb_ledger"});
    pg::migrate_options o;
    o.migrations = {{"001_schema", schema}};
    o.watched = {pg::watch<amount>(), pg::watch<points>(), pg::watch<qty>()};
    pg::migrate(db(), o);
}

dec d(std::string_view text) { return dec::parse(text); }

dec db_amount(std::int64_t id) { return scalar<dec>(sql{"SELECT amount FROM wb_balances WHERE id = {}", id}); }
std::int64_t db_points(std::int64_t id) { return scalar<std::int64_t>(sql{"SELECT points FROM wb_balances WHERE id = {}", id}); }

// A hundred ledger postings, enough for one COPY group; `bad_row` gets a
// negative amount that its CHECK rejects.
void submit_ledger(pg::writeback& wb, int bad_row = -1) {
    for (int i = 0; i < 100; ++i) {
        ledger_write r{};
        r.account = 1 + i % 4;
        r.amount = i == bad_row ? d("-1") : d("1");
        if (i % 3) r.memo = "m" + std::to_string(i);
        if (i % 2) r.meta = pg::jsonb{"{\"i\": " + std::to_string(i) + "}"};
        wb.submit(pg::posting{}.insert(r).add<amount>(d("1"), 1 + i % 4));
    }
}

// db_amount of each balance after the hundred postings added 25 to it.
void check_balances_plus_25() {
    const std::array<dec, 4> initial{d("10"), d("20"), d("30"), d("40")};
    for (std::int64_t id = 1; id <= 4; ++id) CHECK(db_amount(id) == initial[id - 1] + d("25"));
}

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

TEST_CASE("notification payloads parse, escapes included", "[writeback]") {
    const auto n = pg::detail::parse_notice(R"({"t":"wb_stock","c":"qty","k":[["1","a\"b\\cé😀"],["2","x"]]})");
    REQUIRE(n.has_value());
    CHECK(n->table == "wb_stock");
    CHECK(n->column == "qty");
    REQUIRE(n->keys.size() == 2);
    CHECK(n->keys[0] == std::vector<std::string>{"1", "a\"b\\c\xc3\xa9\xf0\x9f\x98\x80"});
    CHECK(n->keys[1] == std::vector<std::string>{"2", "x"});
    CHECK(pg::detail::parse_notice(R"({"t":"x","c":"y","k":[["1"]})") == std::nullopt);
    const auto all = pg::detail::parse_notice(R"({"t":"wb_stock","c":"qty","all":true})");
    REQUIRE(all.has_value());
    CHECK(all->all);
}

TEST_CASE("add, get and flush", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    CHECK(wb.get<amount>(1) == d("10"));
    wb.submit(pg::posting{}.add<amount>(d("2.5"), 1).add<qty>(3, 1, "a b").add<qty>(-1, 2, "é\"x"));
    CHECK(wb.get<amount>(1) == d("12.5"));
    CHECK(db_amount(1) == d("10"));
    CHECK(wb.stats().backlog == 1);
    wb.flush();
    CHECK(db_amount(1) == d("12.5"));
    CHECK(scalar<std::int64_t>(sql{"SELECT qty FROM wb_stock WHERE warehouse = 1"}) == 8);
    CHECK(scalar<std::int64_t>(sql{"SELECT qty FROM wb_stock WHERE warehouse = 2"}) == 5);
    CHECK(wb.get<qty>(2, "é\"x") == 5);
    CHECK(wb.stats().backlog == 0);
    CHECK(wb.stats().committed == 1);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.flush_receipts"}) == 1);
    REQUIRE_THROWS_AS(wb.get<amount>(99), pg::not_found);
}

TEST_CASE("cached reads and adds like a variable", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());

    pg::cached<amount> b(wb, std::int64_t{1});
    CHECK(b.get() == wb.get<amount>(1));
    CHECK(b.get() == d("10"));

    b += d("2.5");
    b -= d("0.5");
    CHECK(b.get() == d("12"));
    CHECK(wb.get<amount>(1) == d("12"));

    wb.flush();
    CHECK(db_amount(1) == d("12"));

    pg::cached<qty> q(wb, 1, std::string("a b"));
    CHECK(q.get() == wb.get<qty>(1, "a b"));
    CHECK(q.get() == 5);
    q += 3;
    q -= 1;
    CHECK(q.get() == 7);
    wb.flush();
    CHECK(scalar<std::int64_t>(sql{"SELECT qty FROM wb_stock WHERE warehouse = 1 AND sku = 'a b'"}) == 7);
}

TEST_CASE("a posting commits whole or is lost whole", "[writeback]") {
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
    CHECK(l[0].kind == pg::error_kind::data);
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

TEST_CASE("a batch whose result was lost after commit is replayed and counted once", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    wb.submit(pg::posting{}.add<points>(5, 3));
    pg::detail::inject_fault(wb, pg::detail::wb_fault::lost_after_commit);
    wb.flush();
    CHECK(db_points(3) == 5);
    CHECK(wb.get<points>(3) == 5);
    CHECK(wb.stats().replays == 1);
    CHECK(wb.stats().committed == 1);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.flush_receipts"}) == 1);
}

TEST_CASE("a large batch inserts its rows by COPY", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    submit_ledger(wb);
    pg::detail::inject_fault(wb, pg::detail::wb_fault::lost_after_copy);
    wb.flush();
    CHECK(wb.stats().replays == 1);  // the COPY path ran and was lost; the batch was replayed
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM wb_ledger"}) == 100);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(memo) FROM wb_ledger"}) == 66);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(meta) FROM wb_ledger"}) == 50);
    CHECK(scalar<dec>(sql{"SELECT sum(amount) FROM wb_ledger"}) == d("100"));
    CHECK(scalar<std::int64_t>(sql{"SELECT min((meta->>'i')::int8) FROM wb_ledger"}) == 1);
    check_balances_plus_25();
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.flush_receipts"}) == 1);
    CHECK(wb.stats().committed == 100);
}

TEST_CASE("a large batch lost after commit is replayed and counted once", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    submit_ledger(wb);
    pg::detail::inject_fault(wb, pg::detail::wb_fault::lost_after_commit);
    wb.flush();
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM wb_ledger"}) == 100);
    CHECK(wb.stats().replays == 1);
    check_balances_plus_25();
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.flush_receipts"}) == 1);
}

TEST_CASE("a large batch with a bad row loses only that posting", "[writeback]") {
    reset();
    lost_log lost;
    auto o = manual();
    o.on_lost = lost.sink();
    pg::writeback wb(db(), o);
    submit_ledger(wb, 37);
    wb.flush();
    const auto l = lost.get();
    REQUIRE(l.size() == 1);
    CHECK(l[0].kind == pg::error_kind::data);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM wb_ledger"}) == 99);
    CHECK(wb.stats().committed == 99);
    const std::array<dec, 4> initial{d("10"), d("20"), d("30"), d("40")};
    for (std::int64_t id = 1; id <= 4; ++id) CHECK(db_amount(id) == initial[id - 1] + d(id == 2 ? "24" : "25"));
}

TEST_CASE("a batch lost before it was sent is replayed", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    wb.submit(pg::posting{}.add<points>(4, 3));
    pg::detail::inject_fault(wb, pg::detail::wb_fault::lost_before_send);
    wb.flush();
    CHECK(db_points(3) == 4);
    CHECK(wb.stats().replays == 1);
    CHECK(wb.stats().committed == 1);
}

TEST_CASE("a delta to a deleted row loses its posting", "[writeback]") {
    reset();
    lost_log lost;
    auto o = manual();
    o.on_lost = lost.sink();
    pg::writeback wb(db(), o);
    CHECK(wb.get<points>(2) == 0);
    run(sql{"DELETE FROM wb_balances WHERE id = 2"});
    wb.submit(pg::posting{}.add<points>(1, 2).add<points>(1, 1));
    wb.submit(pg::posting{}.add<points>(3, 1));
    CHECK(wb.get<points>(2) == 1);
    wb.flush();
    const auto l = lost.get();
    REQUIRE(l.size() == 1);
    CHECK(l[0].reason == "wb_balances.points: the row of a delta does not exist");
    CHECK(db_points(1) == 3);
    CHECK(wb.get<points>(1) == 3);
    CHECK(wb.get<points>(2) == 0);
}

TEST_CASE("over the backlog limit submit throws and records nothing", "[writeback]") {
    reset();
    auto o = manual();
    o.backlog_limit = 2;
    pg::writeback wb(db(), o);
    wb.submit(pg::posting{}.add<points>(1, 1));
    wb.submit(pg::posting{}.add<points>(1, 1));
    REQUIRE_THROWS_AS(wb.submit(pg::posting{}.add<points>(1, 1)), pg::backlog_full);
    CHECK(wb.stats().backlog == 2);
    CHECK(wb.get<points>(1) == 2);
    wb.flush();
    CHECK(wb.stats().backlog == 0);
    CHECK(db_points(1) == 2);
}

TEST_CASE("the destructor flushes", "[writeback]") {
    reset();
    {
        pg::writeback wb(db(), manual());
        wb.submit(pg::posting{}.add<amount>(d("0.000001"), 4));
    }
    CHECK(db_amount(4) == d("40.000001"));
}

TEST_CASE("reconcile() reads every loaded slot again", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    CHECK(wb.get<points>(1) == 0);
    wb.submit(pg::posting{}.add<points>(2, 1));
    run(sql{"UPDATE wb_balances SET points = 100 WHERE id = 1"});
    wb.reconcile();
    CHECK(wb.get<points>(1) == 102);
    CHECK(wb.stats().full_reconciles == 1);
}

TEST_CASE("replicas see each other's commits and admin writes", "[writeback]") {
    reset();
    pg::writeback a(db(), manual());
    pg::writeback b(db(), manual());
    a.watch<amount>();
    b.watch<amount>();
    REQUIRE(eventually([&] { return a.stats().full_reconciles >= 1 && b.stats().full_reconciles >= 1; }));
    CHECK(a.get<amount>(1) == d("10"));
    CHECK(b.get<amount>(1) == d("10"));

    b.submit(pg::posting{}.add<amount>(d("1"), 1));
    a.submit(pg::posting{}.add<amount>(d("100"), 1));
    a.flush();
    REQUIRE(eventually([&] { return b.stats().reconciled >= 1; }));
    CHECK(b.get<amount>(1) == d("111"));  // database 110 + b's unsettled 1
    CHECK(a.get<amount>(1) == d("110"));
    REQUIRE(eventually([&] { return a.stats().ignored >= 1; }));
    CHECK(a.stats().reconciled == 0);

    b.flush();
    REQUIRE(eventually([&] { return a.stats().reconciled >= 1; }));
    CHECK(a.get<amount>(1) == d("111"));

    run(sql{"UPDATE wb_balances SET amount = amount + 7 WHERE id = 1"});
    REQUIRE(eventually([&] { return a.stats().reconciled >= 2 && b.stats().reconciled >= 2; }));
    CHECK(a.get<amount>(1) == d("118"));
    CHECK(b.get<amount>(1) == d("118"));
}

TEST_CASE("a listener reconnect reconciles everything", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    wb.watch<points>();
    REQUIRE(eventually([&] { return wb.stats().full_reconciles >= 1; }));
    CHECK(wb.get<points>(1) == 0);
    {
        pg::conn c(db());
        pg::transaction tx(c);
        c.exec(sql{"SET LOCAL session_replication_role = replica"});  // no trigger, no notification
        c.exec(sql{"UPDATE wb_balances SET points = 50 WHERE id = 1"});
        tx.commit();
    }
    CHECK(wb.get<points>(1) == 0);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(pg_terminate_backend(pid)) FROM pg_stat_activity "
                                   "WHERE datname = current_database() AND query LIKE 'LISTEN%'"}) == 1);
    REQUIRE(eventually([&] { return wb.stats().full_reconciles >= 2; }));
    CHECK(wb.get<points>(1) == 50);
}

TEST_CASE("receipts older than a day are deleted", "[writeback]") {
    reset();
    run(sql{"INSERT INTO libpg.flush_receipts VALUES (gen_random_uuid(), now() - interval '2 days'), "
            "(gen_random_uuid(), now() - interval '1 hour')"});
    pg::writeback wb(db(), manual());
    wb.flush();
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.flush_receipts"}) == 1);
}

TEST_CASE("concurrent submits add up", "[writeback]") {
    reset();
    auto o = pg::writeback_options{};
    o.interval = 20ms;
    pg::writeback wb(db(), o);
    constexpr int threads = 8, per_thread = 10'000;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&wb, t] {
            for (int i = 0; i < per_thread; ++i) {
                const std::int64_t id = (t + i) % 4 + 1;
                wb.submit(pg::posting{}.add<points>(1, id).add<amount>(dec::from_units(1), id));
            }
        });
    }
    for (auto& t : ts) t.join();
    wb.flush();
    std::int64_t total = 0;
    for (std::int64_t id = 1; id <= 4; ++id) {
        CHECK(wb.get<points>(id) == db_points(id));
        CHECK(wb.get<amount>(id) == db_amount(id));
        total += db_points(id);
    }
    CHECK(total == threads * per_thread);
    CHECK(scalar<dec>(sql{"SELECT sum(amount) FROM wb_balances"}) == d("100.080000"));
    CHECK(wb.stats().committed == threads * per_thread);
}

TEST_CASE("a TRUNCATE reconciles every loaded slot of the counter", "[writeback]") {
    reset();
    pg::writeback wb(db(), manual());
    wb.watch<qty>();
    REQUIRE(eventually([&] { return wb.stats().full_reconciles >= 1; }));
    CHECK(wb.get<qty>(1, "a b") == 5);
    run(sql{"TRUNCATE wb_stock"});
    REQUIRE(eventually([&] { return wb.stats().reconciled >= 1; }));
    REQUIRE_THROWS_AS(wb.get<qty>(1, "a b"), pg::not_found);
}
