#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "pg/pg.hpp"

using pg::sql;

namespace {

pg::pool& db() {
    static pg::pool p([] {
        pg::pool_options o;
        const char* d = std::getenv("LIBPG_TEST_DSN");
        REQUIRE(d != nullptr);
        o.dsn = d;
        return o;
    }());
    return p;
}

// An empty table tx_items(id int primary key).
void reset() {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS tx_items"});
    c.exec(sql{"CREATE TABLE tx_items (id int PRIMARY KEY)"});
}

std::vector<std::int32_t> ids(pg::conn& c) { return c.query<std::int32_t>(sql{"SELECT id FROM tx_items ORDER BY id"}); }

std::vector<std::int32_t> ids() {
    pg::conn c(db());
    return ids(c);
}

void add(pg::conn& c, int id) { c.exec(sql{"INSERT INTO tx_items VALUES ({})", id}); }

void raise(pg::conn& c, const char* sqlstate) {
    c.exec(pg::unsafe_sql(std::string("DO $$ BEGIN RAISE EXCEPTION 'raised' USING ERRCODE = '") + sqlstate +
                          "'; END $$"));
}

}  // namespace

TEST_CASE("a transaction commits, and rolls back in its destructor") {
    reset();
    pg::conn c(db());
    {
        pg::transaction tx(c);
        REQUIRE(c.in_transaction());
        add(c, 1);
        tx.commit();
    }
    REQUIRE_FALSE(c.in_transaction());
    {
        pg::transaction tx(c);
        add(c, 2);
    }
    REQUIRE(ids(c) == std::vector<std::int32_t>{1});
}

TEST_CASE("the transaction is READ COMMITTED") {
    pg::conn c(db());
    pg::transaction tx(c);
    REQUIRE(c.query_one<std::string>(sql{"SHOW transaction_isolation"}) == "read committed");
    tx.commit();
}

TEST_CASE("work is invisible to other connections until commit") {
    reset();
    pg::conn c(db());
    pg::transaction tx(c);
    add(c, 1);
    pg::pool other([] {
        pg::pool_options o = db().options();
        o.max_conns = 1;
        return o;
    }());
    pg::conn d(other);
    REQUIRE(ids(d).empty());
    tx.commit();
    REQUIRE(ids(d) == std::vector<std::int32_t>{1});
}

TEST_CASE("commit after a failed statement throws and nothing is stored") {
    reset();
    {
        pg::conn c(db());
        pg::transaction tx(c);
        add(c, 1);
        REQUIRE_THROWS_AS(add(c, 1), pg::error);
        REQUIRE_THROWS_AS(tx.commit(), pg::error);
        REQUIRE_FALSE(c.in_transaction());
        REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
    }
    REQUIRE(ids().empty());
}

TEST_CASE("a syntax error inside a transaction aborts it") {
    reset();
    pg::conn c(db());
    pg::transaction tx(c);
    REQUIRE_THROWS_AS(c.exec(sql{"SELEC 1"}), pg::error);
    REQUIRE_THROWS_AS(tx.commit(), pg::error);
}

TEST_CASE("a nested guard rolls back to its savepoint and keeps outer work") {
    reset();
    {
        pg::conn c(db());
        pg::transaction outer(c);
        add(c, 1);
        {
            pg::transaction inner(c);
            add(c, 2);
            REQUIRE_THROWS_AS(add(c, 1), pg::error);
        }
        {
            pg::transaction inner(c);
            add(c, 3);
        }
        add(c, 4);
        outer.commit();
    }
    REQUIRE(ids() == std::vector<std::int32_t>{1, 4});
}

TEST_CASE("a committed nested guard keeps its work in the outer transaction") {
    reset();
    {
        pg::conn c(db());
        pg::transaction outer(c);
        {
            pg::transaction inner(c);
            add(c, 1);
            {
                pg::transaction innermost(c);
                add(c, 2);
                innermost.commit();
            }
            inner.commit();
        }
        REQUIRE(c.in_transaction());
        outer.commit();
    }
    REQUIRE(ids() == std::vector<std::int32_t>{1, 2});
}

TEST_CASE("committing an outer guard while an inner one is open throws") {
    pg::conn c(db());
    pg::transaction outer(c);
    pg::transaction inner(c);
    REQUIRE_THROWS_AS(outer.commit(), pg::error);
    inner.commit();
    outer.commit();
}

TEST_CASE("transact commits and returns what the closure returned") {
    reset();
    const int got = db().transact([](pg::conn& c) {
        add(c, 7);
        return 42;
    });
    REQUIRE(got == 42);
    REQUIRE(ids() == std::vector<std::int32_t>{7});
}

TEST_CASE("transact retries a serialization failure") {
    reset();
    int attempts = 0;
    db().transact([&](pg::conn& c) {
        ++attempts;
        add(c, attempts);
        if (attempts == 1) raise(c, "40001");
    });
    REQUIRE(attempts == 2);
    REQUIRE(ids() == std::vector<std::int32_t>{2});
}

TEST_CASE("transact gives up after transact_attempts") {
    reset();
    int attempts = 0;
    try {
        db().transact([&](pg::conn& c) {
            ++attempts;
            add(c, attempts);
            raise(c, "40P01");
        });
        FAIL("a deadlock every time was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::conflict);
    }
    REQUIRE(attempts == db().options().transact_attempts);
    REQUIRE(ids().empty());
}

TEST_CASE("transact does not retry a data error") {
    reset();
    int attempts = 0;
    try {
        db().transact([&](pg::conn& c) {
            ++attempts;
            add(c, 1);
            add(c, 1);
        });
        FAIL("a duplicate key was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::data);
    }
    REQUIRE(attempts == 1);
    REQUIRE(ids().empty());
}

TEST_CASE("transact rolls back and rethrows other exceptions") {
    reset();
    int attempts = 0;
    REQUIRE_THROWS_AS(db().transact([&](pg::conn& c) {
        ++attempts;
        add(c, 1);
        throw std::runtime_error("not a database error");
    }),
                      std::runtime_error);
    REQUIRE(attempts == 1);
    REQUIRE(ids().empty());
}

TEST_CASE("a real serialization conflict is retried") {
    reset();
    {
        pg::conn c(db());
        add(c, 1);
    }
    pg::pool rival([] {
        pg::pool_options o = db().options();
        o.max_conns = 1;
        return o;
    }());
    int attempts = 0;
    db().transact([&](pg::conn& c) {
        ++attempts;
        c.exec(sql{"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"});
        const auto n = c.query_one<std::int64_t>(sql{"SELECT count(*) FROM tx_items"});
        if (attempts == 1) {
            pg::conn r(rival);
            r.exec(sql{"UPDATE tx_items SET id = 10 WHERE id = 1"});
        }
        c.exec(sql{"UPDATE tx_items SET id = id + {}", n});
    });
    REQUIRE(attempts == 2);
    REQUIRE(ids() == std::vector<std::int32_t>{11});
}

TEST_CASE("transact returns a referenced result by value") {
    static int stored = 5;
    auto f = [](pg::conn&) -> int& { return stored; };
    static_assert(std::is_same_v<decltype(db().transact(f)), int>);
    int got = db().transact(f);
    stored = 6;
    REQUIRE(got == 5);
}

TEST_CASE("a deadline passing while COMMIT is in flight throws commit_unknown") {
    {
        pg::conn c(db());
        c.exec(sql{"DROP TABLE IF EXISTS tx_slow"});
        c.exec(sql{"CREATE TABLE tx_slow (id int)"});
        c.exec(sql{"CREATE OR REPLACE FUNCTION tx_slow_wait() RETURNS trigger LANGUAGE plpgsql AS "
                   "$$ BEGIN PERFORM pg_sleep(3); RETURN NULL; END $$"});
        c.exec(sql{"CREATE CONSTRAINT TRIGGER tx_slow_t AFTER INSERT ON tx_slow DEFERRABLE INITIALLY DEFERRED "
                   "FOR EACH ROW EXECUTE FUNCTION tx_slow_wait()"});
    }
    pg::pool_options o;
    o.dsn = std::getenv("LIBPG_TEST_DSN");
    o.deadline = std::chrono::milliseconds(300);
    o.statement_timeout = std::chrono::milliseconds(0);
    pg::pool p(o);
    pg::conn c(p);
    pg::transaction tx(c);
    c.exec(sql{"INSERT INTO tx_slow VALUES (1)"});
    try {
        tx.commit();
        FAIL("COMMIT finished within the deadline");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::commit_unknown);
    }
}

TEST_CASE("a committed transaction's connection is free before COMMIT's answer is read") {
    {
        pg::conn c(db());
        c.exec(sql{"DROP TABLE IF EXISTS tx_counter"});
        c.exec(sql{"CREATE TABLE tx_counter (id int PRIMARY KEY, n int)"});
        c.exec(sql{"INSERT INTO tx_counter VALUES (1, 0)"});
    }
    pg::pool p([] {
        pg::pool_options o = db().options();
        o.max_conns = 1;
        return o;
    }());
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i)
                p.transact([](pg::conn& c) {
                    c.exec(sql{"UPDATE tx_counter SET n = n + 1 WHERE id = 1"});
                });
        });
    }
    for (auto& t : threads) t.join();
    pg::conn c(db());
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT n FROM tx_counter"}) == 800);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a conn floats after commit") {
    reset();
    {
        pg::conn c(db());
        {
            pg::transaction tx(c);
            add(c, 1);
            tx.commit();
        }
        REQUIRE_FALSE(c.in_transaction());
        REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
        {
            pg::transaction tx(c);
            add(c, 2);
            tx.commit();
        }
    }
    pg::conn other(db());
    REQUIRE(ids(other) == std::vector<std::int32_t>{1, 2});
}

TEST_CASE("sent writes in a transaction commit with it") {
    reset();
    {
        pg::conn c(db());
        pg::transaction tx(c);
        (void)c.send(sql{"INSERT INTO tx_items VALUES (1)"});
        (void)c.send(sql{"INSERT INTO tx_items VALUES (2)"});
        (void)c.send(sql{"INSERT INTO tx_items VALUES (3)"});
        tx.commit();
    }
    REQUIRE(ids() == std::vector<std::int32_t>{1, 2, 3});
}

TEST_CASE("a failing sent write makes commit throw its error and stores nothing") {
    reset();
    {
        pg::conn c(db());
        add(c, 1);
    }
    {
        pg::conn c(db());
        pg::transaction tx(c);
        (void)c.send(sql{"INSERT INTO tx_items VALUES (2)"});
        (void)c.send(sql{"INSERT INTO tx_items VALUES (1)"});
        (void)c.send(sql{"INSERT INTO tx_items VALUES (3)"});
        try {
            tx.commit();
            FAIL("commit accepted a duplicate key sent statement");
        } catch (const pg::error& e) {
            REQUIRE(e.kind == pg::error_kind::data);
        }
    }
    REQUIRE(ids() == std::vector<std::int32_t>{1});
}

TEST_CASE("a later statement reports the failed sent write") {
    reset();
    pg::conn c(db());
    pg::transaction tx(c);
    (void)c.send(sql{"INSERT INTO tx_items VALUES (1)"});
    (void)c.send(sql{"INSERT INTO tx_items VALUES (1)"});
    try {
        c.query_one<std::int32_t>(sql{"SELECT 1"});
        FAIL("the aborted transaction answered");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::data);
    }
}

TEST_CASE("a rolled back transaction waits for its sent writes") {
    reset();
    {
        pg::conn c(db());
        {
            pg::transaction tx(c);
            (void)c.send(sql{"INSERT INTO tx_items VALUES (1)"});
        }
        REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
    }
    REQUIRE(ids().empty());
}

TEST_CASE("a failing COMMIT still throws") {
    {
        pg::conn c(db());
        c.exec(sql{"DROP TABLE IF EXISTS tx_deferred"});
        c.exec(sql{"CREATE TABLE tx_deferred (id int, u int, "
                   "CONSTRAINT tx_deferred_u UNIQUE (u) DEFERRABLE INITIALLY DEFERRED)"});
    }
    {
        pg::conn c(db());
        pg::transaction tx(c);
        c.exec(sql{"INSERT INTO tx_deferred VALUES (1, 1)"});
        c.exec(sql{"INSERT INTO tx_deferred VALUES (2, 1)"});
        try {
            tx.commit();
            FAIL("COMMIT accepted a deferred unique violation");
        } catch (const pg::error& e) {
            REQUIRE(e.kind != pg::error_kind::commit_unknown);
        }
    }
    pg::conn other(db());
    REQUIRE(other.query_one<std::int64_t>(sql{"SELECT count(*) FROM tx_deferred"}) == 0);
}


TEST_CASE("transactions with unread sent statements finish while others wait for the connection") {
    pg::pool_options o = db().options();
    o.max_conns = 1;
    o.acquire_timeout = std::chrono::seconds(10);
    pg::pool p(o);
    std::vector<std::thread> ts;
    std::atomic<int> done{0};
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                p.transact([&](pg::conn& c) { c.send(sql{"SELECT {}::int", i}); });
                ++done;
            }
        });
    for (auto& t : ts) t.join();
    CHECK(done == 200);
}
