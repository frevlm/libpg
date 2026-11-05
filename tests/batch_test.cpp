#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
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

// Fresh pool and an empty table batch_items(id int primary key, name text).
void reset() {
    pg::connect(dsn(), 8);
    pg::conn c;
    c.exec("DROP TABLE IF EXISTS batch_items");
    c.exec("CREATE TABLE batch_items (id int PRIMARY KEY, name text)");
}

std::vector<std::int32_t> ids(pg::conn& c) { return c.query<std::int32_t>("SELECT id FROM batch_items ORDER BY id"); }

std::int64_t prepared_count(pg::conn& c) {
    return c.query_one<std::int64_t>("SELECT count(*) FROM pg_prepared_statements").value();
}

// A text the server has never seen on this connection.
std::string distinct_select(int i) { return "SELECT " + std::to_string(i) + " + $1::int"; }

}  // namespace

TEST_CASE("a batch returns the result of every statement") {
    reset();
    pg::conn c;
    pg::batch b(c);
    const auto inserted = b.exec("INSERT INTO batch_items VALUES ($1, $2), ($3, $4)", 1, "one", 2, "two");
    const auto updated = b.exec("UPDATE batch_items SET name = upper(name) WHERE id = $1", 2);
    const auto rows = b.query<std::int32_t, std::string>("SELECT id, name FROM batch_items ORDER BY id");
    const auto one = b.query<std::int32_t>("SELECT $1::int * 2", 21);
    b.run();
    REQUIRE(inserted.get() == 2);
    REQUIRE(updated.get() == 1);
    REQUIRE(rows.get() == std::vector<std::tuple<std::int32_t, std::string>>{{1, "one"}, {2, "TWO"}});
    REQUIRE(one.get() == std::vector<std::int32_t>{42});
}

TEST_CASE("a batch outside a transaction is atomic") {
    reset();
    pg::conn c;
    pg::batch b(c);
    b.exec("INSERT INTO batch_items VALUES ($1, $2)", 1, "rolled back");
    const auto dup = b.exec("INSERT INTO batch_items VALUES ($1, $2)", 1, "duplicate");
    const auto after = b.exec("INSERT INTO batch_items VALUES ($1, $2)", 3, "not executed");
    try {
        b.run();
        FAIL("duplicate key was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.sqlstate == "23505");
    }
    REQUIRE_THROWS_AS(dup.get(), pg::error);
    REQUIRE_THROWS_AS(after.get(), pg::error);
    REQUIRE(ids(c) == std::vector<std::int32_t>{});
}

TEST_CASE("get throws for the failed statement and the ones after it") {
    reset();
    pg::conn c;
    pg::batch b(c);
    const auto ok = b.query<std::int32_t>("SELECT $1::int", 7);
    const auto bad = b.query<std::int32_t>("SELECT 1 / $1::int", 0);
    const auto skipped = b.query<std::int32_t>("SELECT $1::int", 8);
    REQUIRE_THROWS_AS(ok.get(), pg::error);  // before run()
    REQUIRE_THROWS_AS(b.run(), pg::error);
    REQUIRE(ok.get() == std::vector<std::int32_t>{7});
    try {
        bad.get();
        FAIL("the failed statement returned a result");
    } catch (const pg::error& e) {
        REQUIRE(e.sqlstate == "22012");
    }
    REQUIRE_THROWS_AS(skipped.get(), pg::error);
}

TEST_CASE("the connection is usable after a failed batch") {
    reset();
    pg::conn c;
    pg::batch b(c);
    b.exec("SELEKT");
    REQUIRE_THROWS_AS(b.run(), pg::error);
    REQUIRE(c.query_one<std::int32_t>("SELECT 5") == 5);
    const auto again = b.query<std::int32_t>("SELECT $1::int", 6);
    b.run();
    REQUIRE(again.get() == std::vector<std::int32_t>{6});
}

TEST_CASE("a batch inside a transaction joins it") {
    reset();
    pg::conn c;
    {
        pg::transaction tx(c);
        pg::batch b(c);
        b.exec("INSERT INTO batch_items VALUES ($1, $2)", 1, "a");
        b.exec("INSERT INTO batch_items VALUES ($1, $2)", 2, "b");
        b.run();
        c.exec("INSERT INTO batch_items VALUES ($1, $2)", 3, "c");
    }  // rolled back
    REQUIRE(ids(c) == std::vector<std::int32_t>{});
    {
        pg::transaction tx(c);
        pg::batch b(c);
        b.exec("INSERT INTO batch_items VALUES ($1, $2)", 4, "d");
        b.run();
        tx.commit();
    }
    REQUIRE(ids(c) == std::vector<std::int32_t>{4});
}

TEST_CASE("a failed batch inside a transaction makes commit throw") {
    reset();
    pg::conn c;
    {
        pg::transaction tx(c);
        pg::batch b(c);
        b.exec("INSERT INTO batch_items VALUES ($1, $2)", 1, "a");
        b.exec("INSERT INTO batch_items VALUES ($1, $2)", 1, "dup");
        REQUIRE_THROWS_AS(b.run(), pg::error);
        REQUIRE_THROWS_AS(tx.commit(), pg::error);
    }
    REQUIRE(ids(c) == std::vector<std::int32_t>{});
}

TEST_CASE("a parameter with NUL throws before anything is sent") {
    reset();
    pg::conn c;
    pg::batch b(c);
    b.exec("INSERT INTO batch_items VALUES ($1, $2)", 1, "fine");
    b.exec("INSERT INTO batch_items VALUES ($1, $2)", 2, std::string("a\0b", 3));
    REQUIRE_THROWS_AS(b.run(), pg::error);
    REQUIRE(ids(c) == std::vector<std::int32_t>{});
}

TEST_CASE("an empty transaction commits without error") {
    reset();
    pg::conn c;
    {
        pg::transaction tx(c);
        tx.commit();
    }
    { pg::transaction tx(c); }  // destroyed without commit
    pg::transaction tx(c);
    c.exec("INSERT INTO batch_items VALUES (1, 'a')");
    tx.commit();
    REQUIRE(ids(c) == std::vector<std::int32_t>{1});
}

TEST_CASE("BEGIN is sent with the first statement of the transaction") {
    reset();
    pg::conn c;
    pg::transaction tx(c);
    const auto t0 = c.query_one<pg::timestamptz>("SELECT now()").value();
    std::this_thread::sleep_for(milliseconds{20});
    const auto t1 = c.query_one<pg::timestamptz>("SELECT now()").value();
    REQUIRE(t0 == t1);  // now() is the transaction start
    REQUIRE(PQtransactionStatus(c.raw()) == PQTRANS_INTRANS);
    tx.commit();
    REQUIRE(PQtransactionStatus(c.raw()) == PQTRANS_IDLE);
}

TEST_CASE("a transaction cannot be nested while its BEGIN is pending") {
    reset();
    pg::conn c;
    pg::transaction outer(c);
    REQUIRE_THROWS_AS(pg::transaction(c), pg::error);
    c.exec("INSERT INTO batch_items VALUES (1, 'a')");
    REQUIRE_THROWS_AS(pg::transaction(c), pg::error);
    outer.commit();
    REQUIRE(ids(c) == std::vector<std::int32_t>{1});
}

TEST_CASE("a transaction whose only statement failed throws on commit") {
    reset();
    pg::conn c;
    pg::transaction tx(c);
    REQUIRE_THROWS_AS(c.exec("INSERT INTO batch_items VALUES (1, 'a', 'extra')"), pg::error);
    REQUIRE_THROWS_AS(tx.commit(), pg::error);
    REQUIRE(PQtransactionStatus(c.raw()) == PQTRANS_IDLE);
}

TEST_CASE("a lease returned with BEGIN pending comes back idle") {
    pg::connect(dsn(), 1);
    {
        pg::conn c;
        static_cast<void>(new pg::transaction(c));  // never destroyed: the lease ends with BEGIN pending
    }
    pg::conn c;
    pg::transaction tx(c);
    REQUIRE(c.query_one<std::int32_t>("SELECT 1") == 1);
    tx.commit();
    pg::connect(dsn(), 8);
}

TEST_CASE("a batch prepares misses once and re-prepares evicted statements") {
    reset();
    pg::conn c;
    REQUIRE(c.query_one<std::int32_t>(distinct_select(0), 1) == 1);
    for (int i = 1; i <= 300; ++i) c.query_one<std::int32_t>(distinct_select(i), 1);  // pushes statement 0 out
    pg::batch b(c);
    const auto evicted = b.query<std::int32_t>(distinct_select(0), 10);
    const auto fresh_a = b.query<std::int32_t>(distinct_select(1000), 1);
    const auto fresh_b = b.query<std::int32_t>(distinct_select(1000), 2);  // same text, prepared once
    const auto cached = b.query<std::int32_t>(distinct_select(300), 3);
    b.run();
    REQUIRE(evicted.get() == std::vector<std::int32_t>{10});
    REQUIRE(fresh_a.get() == std::vector<std::int32_t>{1001});
    REQUIRE(fresh_b.get() == std::vector<std::int32_t>{1002});
    REQUIRE(cached.get() == std::vector<std::int32_t>{303});
    REQUIRE(prepared_count(c) <= 257);
}

TEST_CASE("600 distinct statements in one batch leave at most 257 prepared") {
    reset();
    pg::conn c;
    pg::batch b(c);
    std::vector<pg::pending<std::vector<std::int32_t>>> results;
    for (int i = 0; i < 600; ++i) results.push_back(b.query<std::int32_t>(distinct_select(i), 1));
    b.run();
    for (int i = 0; i < 600; ++i) REQUIRE(results[static_cast<std::size_t>(i)].get() == std::vector<std::int32_t>{i + 1});
    REQUIRE(prepared_count(c) <= 257);
}

TEST_CASE("a statement the server forgot is prepared again inside a batch") {
    reset();
    pg::conn c;
    REQUIRE(c.query_one<std::int32_t>("SELECT $1::int", 5) == 5);
    c.exec("DEALLOCATE ALL");
    pg::batch b(c);
    const auto first = b.exec("INSERT INTO batch_items VALUES ($1, $2)", 1, "a");
    const auto forgot = b.query<std::int32_t>("SELECT $1::int", 6);
    b.run();
    REQUIRE(first.get() == 1);
    REQUIRE(forgot.get() == std::vector<std::int32_t>{6});
    REQUIRE(ids(c) == std::vector<std::int32_t>{1});  // the retry inserted once
}

TEST_CASE("a large batch with large parameters and results does not deadlock") {
    reset();
    pg::conn c;
    const std::string big(8192, 'x');
    pg::batch b(c);
    std::vector<pg::pending<std::vector<std::string>>> echoes;
    for (int i = 0; i < 5000; ++i) {
        if (i % 2) echoes.push_back(b.query<std::string>("SELECT $1::text || $2::text", big, i));
        else b.exec("INSERT INTO batch_items VALUES ($1, $2)", i, big);
    }
    b.run();
    for (std::size_t k = 0; k < echoes.size(); ++k)
        REQUIRE(echoes[k].get() == std::vector<std::string>{big + std::to_string(2 * k + 1)});
    REQUIRE(c.query_one<std::int64_t>("SELECT count(*) FROM batch_items WHERE name = $1", big) == 2500);
}

TEST_CASE("an empty batch sends nothing and leaves a pending BEGIN pending") {
    reset();
    pg::conn c;
    pg::transaction tx(c);
    pg::batch b(c);
    b.run();
    REQUIRE(PQtransactionStatus(c.raw()) == PQTRANS_IDLE);
    REQUIRE_THROWS_AS(pg::transaction(c), pg::error);
    tx.commit();
}
