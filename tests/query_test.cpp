#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "pg/pg.hpp"

using namespace std::chrono;

namespace {

std::string dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    return d;
}

// Fresh pool and an empty table query_items(id int primary key, name text, score float8).
void reset() {
    pg::connect(dsn(), 8);
    pg::conn c;
    c.exec("DROP TABLE IF EXISTS query_items");
    c.exec("CREATE TABLE query_items (id int PRIMARY KEY, name text, score float8)");
}

void insert_three(pg::conn& c) {
    c.exec("INSERT INTO query_items VALUES ($1, $2, $3)", 1, "one", 1.5);
    c.exec("INSERT INTO query_items VALUES ($1, $2, $3)", 2, std::string("two"), 2.5);
    c.exec("INSERT INTO query_items VALUES ($1, $2, $3)", 3, std::string_view("three"), 3.5);
}

}  // namespace

TEST_CASE("exec returns the number of affected rows") {
    reset();
    pg::conn c;
    REQUIRE(c.exec("INSERT INTO query_items VALUES (1, 'a', 0), (2, 'b', 0), (3, 'c', 0)") == 3);
    REQUIRE(c.exec("UPDATE query_items SET score = $1 WHERE id >= $2", 4.0, 2) == 2);
    REQUIRE(c.exec("DELETE FROM query_items WHERE id = $1", 99) == 0);
}

TEST_CASE("query decodes rows as tuples") {
    reset();
    pg::conn c;
    insert_three(c);
    auto rows = c.query<std::int32_t, std::string, double>("SELECT id, name, score FROM query_items ORDER BY id");
    static_assert(std::is_same_v<decltype(rows), std::vector<std::tuple<std::int32_t, std::string, double>>>);
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0] == std::make_tuple(1, std::string("one"), 1.5));
    REQUIRE(rows[2] == std::make_tuple(3, std::string("three"), 3.5));
}

TEST_CASE("query with a single type returns that type, not a tuple") {
    reset();
    pg::conn c;
    insert_three(c);
    auto ids = c.query<std::int32_t>("SELECT id FROM query_items WHERE id > $1 ORDER BY id", 1);
    static_assert(std::is_same_v<decltype(ids), std::vector<std::int32_t>>);
    REQUIRE(ids == std::vector<std::int32_t>{2, 3});
}

TEST_CASE("query_one returns nullopt, one row, or throws") {
    reset();
    pg::conn c;
    insert_three(c);
    REQUIRE(c.query_one<std::string>("SELECT name FROM query_items WHERE id = $1", 2) == std::optional<std::string>{"two"});
    REQUIRE(c.query_one<std::string>("SELECT name FROM query_items WHERE id = $1", 9) == std::nullopt);
    auto pair = c.query_one<std::int32_t, std::string>("SELECT id, name FROM query_items WHERE id = 3");
    REQUIRE(pair == std::optional{std::make_tuple(3, std::string("three"))});
    REQUIRE_THROWS_AS(c.query_one<std::int32_t>("SELECT id FROM query_items"), pg::error);
}

TEST_CASE("NULL maps to optional and is rejected elsewhere") {
    reset();
    pg::conn c;
    c.exec("INSERT INTO query_items VALUES ($1, $2, $3)", 1, std::optional<std::string>{}, 1.0);
    REQUIRE(c.query_one<std::optional<std::string>>("SELECT name FROM query_items") ==
            std::optional<std::optional<std::string>>{std::optional<std::string>{}});
    REQUIRE_THROWS_AS(c.query<std::string>("SELECT name FROM query_items"), pg::error);
    c.exec("UPDATE query_items SET name = $1", std::optional<std::string>{"x"});
    REQUIRE(c.query<std::optional<std::string>>("SELECT name FROM query_items") ==
            std::vector<std::optional<std::string>>{"x"});
}

TEST_CASE("a column count mismatch throws") {
    reset();
    pg::conn c;
    insert_three(c);
    REQUIRE_THROWS_AS((c.query<std::int32_t, std::string>("SELECT id FROM query_items")), pg::error);
    REQUIRE_THROWS_AS(c.query<std::int32_t>("SELECT id, name FROM query_items"), pg::error);
}

TEST_CASE("server errors carry the SQLSTATE") {
    reset();
    pg::conn c;
    c.exec("INSERT INTO query_items VALUES (1, 'a', 0)");
    try {
        c.exec("INSERT INTO query_items VALUES (1, 'b', 0)");
        FAIL("duplicate key was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.sqlstate == "23505");
    }
    try {
        c.query<std::int32_t>("SELECT 1 / $1", 0);
        FAIL("division by zero was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.sqlstate == "22012");
    }
}

TEST_CASE("the connection stays usable after an error") {
    reset();
    pg::conn c;
    REQUIRE_THROWS_AS(c.exec("SELEKT"), pg::error);
    REQUIRE(c.query_one<std::int32_t>("SELECT 5") == std::optional<std::int32_t>{5});
}

TEST_CASE("values round-trip through the server") {
    reset();
    pg::conn c;
    const pg::timestamptz ts = sys_days{year{2024} / March / 5} + hours{7} + microseconds{120000};
    auto row = c.query_one<bool, std::int16_t, std::int64_t, double, pg::numeric, pg::jsonb, pg::timestamptz>(
        "SELECT $1::bool, $2::int2, $3::int8, $4::float8, $5::numeric, $6::jsonb, $7::timestamptz", true,
        std::int16_t{-7}, std::int64_t{9223372036854775807}, 0.1, pg::numeric{"12.3400"}, pg::jsonb{"{\"a\": [1]}"}, ts);
    REQUIRE(row.has_value());
    REQUIRE(*row == std::make_tuple(true, std::int16_t{-7}, std::int64_t{9223372036854775807}, 0.1,
                                    pg::numeric{"12.3400"}, pg::jsonb{"{\"a\": [1]}"}, ts));
}

TEST_CASE("the session time zone is UTC") {
    reset();
    pg::conn c;
    REQUIRE(c.query_one<std::string>("SHOW TimeZone") == std::optional<std::string>{"UTC"});
    REQUIRE(c.query_one<std::string>("SELECT '2024-03-05 07:08:09+00'::timestamptz::text") ==
            std::optional<std::string>{"2024-03-05 07:08:09+00"});
}

TEST_CASE("special doubles come back from the server") {
    reset();
    pg::conn c;
    auto nan = c.query_one<double>("SELECT 'NaN'::float8");
    REQUIRE(nan.has_value());
    REQUIRE(std::isnan(*nan));
    REQUIRE(c.query_one<double>("SELECT '-Infinity'::float8") == std::optional<double>{-HUGE_VAL});
}

TEST_CASE("a transaction commits and rolls back") {
    reset();
    pg::conn c;
    {
        pg::transaction tx(c);
        c.exec("INSERT INTO query_items VALUES (1, 'kept', 0)");
        tx.commit();
    }
    {
        pg::transaction tx(c);
        c.exec("INSERT INTO query_items VALUES (2, 'dropped', 0)");
    }
    REQUIRE(c.query<std::int32_t>("SELECT id FROM query_items ORDER BY id") == std::vector<std::int32_t>{1});
}

TEST_CASE("a failed statement rolls the transaction back") {
    reset();
    pg::conn c;
    try {
        pg::transaction tx(c);
        c.exec("INSERT INTO query_items VALUES (1, 'a', 0)");
        c.exec("INSERT INTO query_items VALUES (1, 'b', 0)");
        tx.commit();
        FAIL("duplicate key was accepted");
    } catch (const pg::error&) {
    }
    REQUIRE(c.query_one<std::int64_t>("SELECT count(*) FROM query_items") == std::optional<std::int64_t>{0});
}

TEST_CASE("the pool never opens more than max_conns connections") {
    pg::connect(dsn(), 2);
    std::mutex mu;
    std::set<std::int32_t> pids;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            try {
                for (int k = 0; k < 5; ++k) {
                    pg::conn c;
                    auto pid = c.query_one<std::int32_t>("SELECT pg_backend_pid() FROM pg_sleep(0.005)");
                    std::lock_guard lk(mu);
                    pids.insert(*pid);
                }
            } catch (...) {
                ++failures;
            }
        });
    for (auto& t : threads) t.join();
    REQUIRE(failures == 0);
    REQUIRE(pids.size() <= 2);
    pg::connect(dsn(), 8);
}

TEST_CASE("a lease waits when the pool is exhausted") {
    pg::connect(dsn(), 1);
    std::optional<pg::conn> first(std::in_place);
    std::atomic<bool> got{false};
    std::thread waiter([&] {
        pg::conn second;
        got = true;
    });
    std::this_thread::sleep_for(milliseconds{100});
    REQUIRE_FALSE(got.load());
    first.reset();
    waiter.join();
    REQUIRE(got.load());
    pg::connect(dsn(), 8);
}

TEST_CASE("a broken connection is replaced on the next lease") {
    pg::connect(dsn(), 1);
    std::int32_t victim = 0;
    {
        pg::conn c;
        victim = *c.query_one<std::int32_t>("SELECT pg_backend_pid()");
        PGconn* killer = PQconnectdb(dsn().c_str());
        REQUIRE(PQstatus(killer) == CONNECTION_OK);
        const std::string kill = "SELECT pg_terminate_backend(" + std::to_string(victim) + ")";
        PQclear(PQexec(killer, kill.c_str()));
        PQfinish(killer);
        bool dead = false;
        for (int i = 0; i < 200 && !dead; ++i) {
            try {
                c.exec("SELECT 1");
                std::this_thread::sleep_for(milliseconds{10});
            } catch (const pg::error&) {
                dead = true;
            }
        }
        REQUIRE(dead);
    }
    pg::conn fresh;
    auto pid = fresh.query_one<std::int32_t>("SELECT pg_backend_pid()");
    REQUIRE(pid.has_value());
    REQUIRE(*pid != victim);
    pg::connect(dsn(), 8);
}

TEST_CASE("connect can be called again") {
    pg::connect(dsn(), 3);
    { pg::conn c; REQUIRE(c.query_one<std::int32_t>("SELECT 1") == std::optional<std::int32_t>{1}); }
    pg::connect(dsn(), 5);
    { pg::conn c; REQUIRE(c.query_one<std::int32_t>("SELECT 2") == std::optional<std::int32_t>{2}); }
}

TEST_CASE("an unreachable server is a pg::error without SQLSTATE") {
    pg::connect("postgresql://postgres@127.0.0.1:1/none?connect_timeout=2", 2);
    for (int i = 0; i < 3; ++i) {
        try {
            pg::conn c;
            FAIL("connected to a closed port");
        } catch (const pg::error& e) {
            REQUIRE(e.sqlstate.empty());
        }
    }
    pg::connect(dsn(), 8);
}

TEST_CASE("commit after a failed statement throws and nothing is stored") {
    reset();
    {
        pg::conn c;
        pg::transaction tx(c);
        c.exec("INSERT INTO query_items VALUES (1, 'a', 0)");
        REQUIRE_THROWS_AS(c.exec("INSERT INTO query_items VALUES (1, 'dup', 0)"), pg::error);
        REQUIRE_THROWS_AS(tx.commit(), pg::error);
    }
    pg::conn c;
    REQUIRE(c.query<std::int64_t>("SELECT count(*) FROM query_items").front() == 0);
}

TEST_CASE("a transaction cannot be nested") {
    reset();
    pg::conn c;
    pg::transaction outer(c);
    REQUIRE_THROWS_AS(pg::transaction(c), pg::error);
    c.exec("INSERT INTO query_items VALUES (1, 'a', 0)");
    outer.commit();
    REQUIRE(c.query<std::int64_t>("SELECT count(*) FROM query_items").front() == 1);
}

TEST_CASE("a string parameter with NUL throws") {
    reset();
    pg::conn c;
    const std::string bad("a\0b", 3);
    REQUIRE_THROWS_AS(c.exec("INSERT INTO query_items VALUES (1, $1, 0)", bad), pg::error);
    REQUIRE(c.query<std::int64_t>("SELECT count(*) FROM query_items").front() == 0);
}

TEST_CASE("session settings ignore PGDATESTYLE") {
    ::setenv("PGDATESTYLE", "German", 1);
    pg::connect(dsn(), 2);
    pg::conn c;
    ::unsetenv("PGDATESTYLE");
    REQUIRE(c.query<std::string>("SHOW DateStyle").front() == "ISO, MDY");
    REQUIRE(c.query<std::string>("SHOW TimeZone").front() == "UTC");
    REQUIRE(c.query<std::string>("SHOW IntervalStyle").front() == "postgres");
    REQUIRE(c.query<std::string>("SHOW extra_float_digits").front() == "3");
    const auto t = c.query<pg::timestamptz>("SELECT '2024-03-05 07:08:09.25+00'::timestamptz").front();
    REQUIRE(t == sys_days{year{2024} / March / 5} + hours{7} + minutes{8} + seconds{9} + microseconds{250000});
}

TEST_CASE("timestamptz round-trips through the database") {
    reset();
    pg::conn c;
    const pg::timestamptz values[] = {
        sys_days{year{0} / January / 1},
        sys_days{year{-43} / March / 15},
        sys_days{year{12345} / January / 1},
        pg::timestamptz::max(),
        pg::timestamptz::min(),
        sys_days{year{2024} / March / 5} + hours{7} + microseconds{1},
        sys_days{year{2024} / March / 5} + hours{7} + microseconds{999999},
        sys_days{year{1969} / December / 31} + microseconds{123450},
    };
    for (const auto& v : values) {
        REQUIRE(c.query<pg::timestamptz>("SELECT $1::timestamptz", v).front() == v);
    }
    REQUIRE(c.query<std::string>("SELECT $1::timestamptz::text", values[0]).front() == "0001-01-01 00:00:00+00 BC");
}

TEST_CASE("a statement text can run many times on one connection") {
    reset();
    pg::conn c;
    c.exec("INSERT INTO query_items VALUES (1, 'a', 0)");
    for (int i = 0; i < 500; ++i) {
        REQUIRE(c.exec("UPDATE query_items SET score = $1 WHERE id = $2", static_cast<double>(i), 1) == 1);
        REQUIRE(c.query_one<double>("SELECT score FROM query_items WHERE id = $1", 1) == static_cast<double>(i));
    }
}

TEST_CASE("a result shape change outside a transaction is retried") {
    reset();
    pg::conn c;
    c.exec("INSERT INTO query_items VALUES (1, 'a', 2.5)");
    REQUIRE(c.query<std::int32_t, std::string>("SELECT id, name FROM query_items").size() == 1);
    c.exec("DROP TABLE IF EXISTS query_shape");
    c.exec("CREATE TABLE query_shape (a int)");
    c.exec("INSERT INTO query_shape VALUES (7)");
    REQUIRE(c.query<std::int32_t>("SELECT * FROM query_shape").size() == 1);
    c.exec("ALTER TABLE query_shape ADD COLUMN b text DEFAULT 'x'");
    auto rows = c.query<std::int32_t, std::string>("SELECT * FROM query_shape");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0] == std::make_tuple(7, std::string("x")));
    c.exec("DROP TABLE query_shape");
}

TEST_CASE("a result shape change inside a transaction throws, the next call works") {
    reset();
    pg::conn c;
    c.exec("DROP TABLE IF EXISTS query_shape");
    c.exec("CREATE TABLE query_shape (a int)");
    REQUIRE(c.query<std::int32_t>("SELECT * FROM query_shape").empty());
    c.exec("ALTER TABLE query_shape ADD COLUMN b text");
    {
        pg::transaction tx(c);
        try {
            c.query<std::int32_t, std::optional<std::string>>("SELECT * FROM query_shape");
            FAIL("expected pg::error");
        } catch (const pg::error& e) {
            REQUIRE(e.sqlstate == "0A000");
        }
    }
    REQUIRE((c.query<std::int32_t, std::optional<std::string>>("SELECT * FROM query_shape").empty()));
    c.exec("DROP TABLE query_shape");
}

TEST_CASE("statements the server forgot are prepared again") {
    reset();
    pg::conn c;
    REQUIRE(c.query_one<std::int32_t>("SELECT $1::int", 5) == 5);
    c.exec("DEALLOCATE ALL");
    REQUIRE(c.query_one<std::int32_t>("SELECT $1::int", 6) == 6);
}

TEST_CASE("more distinct statements than the cache holds") {
    reset();
    pg::conn c;
    for (int i = 0; i < 600; ++i)
        REQUIRE(c.query_one<std::int32_t>("SELECT " + std::to_string(i) + " + $1::int", 1) == i + 1);
    // The earliest ones were evicted and prepare again.
    for (int i = 0; i < 600; i += 50)
        REQUIRE(c.query_one<std::int32_t>("SELECT " + std::to_string(i) + " + $1::int", 2) == i + 2);
    REQUIRE(c.query_one<std::int64_t>("SELECT count(*) FROM pg_prepared_statements") <= 257);
}

TEST_CASE("a syntax error in a transaction aborts it as before") {
    reset();
    pg::conn c;
    pg::transaction tx(c);
    REQUIRE_THROWS_AS(c.exec("SELEC 1"), pg::error);
    REQUIRE_THROWS_AS(tx.commit(), pg::error);
}
