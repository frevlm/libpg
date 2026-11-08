// Runs through a transaction pooler (PgBouncer) when LIBPG_TEST_POOLER is set to
// its URI without the database, e.g. postgresql://postgres@bouncer:6432; the
// direct server is LIBPG_TEST_DSN's. Without it every case is skipped.
#include <catch2/catch_test_macros.hpp>

#include <libpq-fe.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "pg/pg.hpp"

using namespace std::chrono_literals;
using pg::sql;

namespace {

struct balances {
    static constexpr std::string_view name = "pooler_balances";
    using key = std::tuple<std::int64_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct points {
    using pg_table = balances;
    using pg_type = std::int64_t;
    static constexpr std::string_view pg_name = "points";
};

const std::string database = "libpg_pooler";

std::string direct_dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    const std::string dsn = d;
    return dsn.substr(0, dsn.rfind('/') + 1) + database;
}

std::string pooler_dsn() { return std::string(std::getenv("LIBPG_TEST_POOLER")) + "/" + database; }

void run_admin(const std::string& script) {
    const std::string d = direct_dsn();
    const std::string admin = d.substr(0, d.rfind('/') + 1) + "postgres";
    std::unique_ptr<PGconn, decltype(&PQfinish)> c(PQconnectdb(admin.c_str()), PQfinish);
    if (PQstatus(c.get()) != CONNECTION_OK) throw std::runtime_error(PQerrorMessage(c.get()));
    std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(c.get(), script.c_str()), PQclear);
    if (PQresultStatus(r.get()) != PGRES_COMMAND_OK) throw std::runtime_error(PQresultErrorMessage(r.get()));
}

// A fresh database migrated over a direct connection, with points watched.
void reset() {
    run_admin("DROP DATABASE IF EXISTS " + database + " WITH (FORCE)");
    run_admin("CREATE DATABASE " + database);
    pg::pool_options o;
    o.dsn = direct_dsn();
    pg::pool direct(o);
    pg::migrate_options m;
    m.migrations = {{"001_schema",
                     "CREATE TABLE pooler_balances (id int8 PRIMARY KEY, points int8 NOT NULL DEFAULT 0);"
                     "INSERT INTO pooler_balances SELECT g, 0 FROM generate_series(1, 100) g;"}};
    m.watched = {pg::watch<points>()};
    pg::migrate(direct, m);
}

pg::pool_options through_pooler(int max_conns) {
    pg::pool_options o;
    o.dsn = pooler_dsn();
    o.max_conns = max_conns;
    o.pooler = true;
    return o;
}

template <class F>
bool eventually(F f, std::chrono::milliseconds limit = 10s) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        if (f()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return f();
}

bool enabled() { return std::getenv("LIBPG_TEST_POOLER") != nullptr; }

}  // namespace

TEST_CASE("many threads share a few pooled connections", "[pooler]") {
    if (!enabled()) SKIP("LIBPG_TEST_POOLER is not set");
    reset();
    pg::pool p(through_pooler(4));
    std::atomic<int> wrong{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 32; ++t)
        ts.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) {
                pg::conn c(p);
                const std::int64_t v = std::int64_t{t} * 1000 + i;
                auto a = c.send<std::int64_t>(sql{"SELECT {}::int8", v});
                auto b = c.send<std::int64_t>(sql{"SELECT {}::int8 + 1", v});
                if (a.one() != v || b.one() != v + 1) ++wrong;
            }
        });
    for (auto& t : ts) t.join();
    CHECK(wrong == 0);
    CHECK(p.stats().open <= 4);
}

TEST_CASE("transactions and cells work through the pooler", "[pooler]") {
    if (!enabled()) SKIP("LIBPG_TEST_POOLER is not set");
    reset();
    pg::pool p(through_pooler(4));
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 100; ++i)
                p.transact([&](pg::conn& c) {
                    c.send(sql{"UPDATE pooler_balances SET points = points + 1 WHERE id = 1"});
                    c.exec(sql{"UPDATE pooler_balances SET points = points + 1 WHERE id = 2"});
                });
        });
    for (auto& t : ts) t.join();
    pg::conn c(p);
    CHECK(c.query_one<std::int64_t>(sql{"SELECT points FROM pooler_balances WHERE id = 1"}) == 800);
    pg::cell<points> cell(c, std::int64_t{2});
    cell += 5;
    CHECK(cell.get() == 805);
    CHECK(p.ready());
}

TEST_CASE("writebacks behind a pooler see each other through the direct connection", "[pooler]") {
    if (!enabled()) SKIP("LIBPG_TEST_POOLER is not set");
    reset();
    pg::pool p(through_pooler(4));
    pg::writeback_options o;
    o.interval = 1h;
    o.direct_dsn = direct_dsn();
    pg::writeback a(p, o), b(p, o);
    a.watch<points>();
    b.watch<points>();
    REQUIRE(eventually([&] { return a.stats().full_reconciles >= 1 && b.stats().full_reconciles >= 1; }));
    CHECK(b.get<points>(3) == 0);
    a.submit(pg::posting{}.add<points>(7, 3));
    a.flush();
    REQUIRE(eventually([&] { return b.stats().reconciled >= 1; }));
    CHECK(b.get<points>(3) == 7);
    REQUIRE(eventually([&] { return a.stats().ignored >= 1; }));
    CHECK(a.stats().reconciled == 0);
}
