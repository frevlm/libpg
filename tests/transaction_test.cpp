#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "pg/pg.hpp"

using namespace std::chrono;
using pg::sql;

namespace {

pg::pool_options options(int max_conns = 8) {
    pg::pool_options o;
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    o.dsn = d;
    o.max_conns = max_conns;
    return o;
}

pg::pool& db() {
    static pg::pool p(options());
    return p;
}

template <class F>
pg::error error_of(F&& f) {
    try {
        f();
    } catch (const pg::error& e) {
        return e;
    }
    FAIL("no pg::error was thrown");
    throw;
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
    c.exec(pg::unsafe_sql(std::string("DO $$ BEGIN RAISE EXCEPTION 'raised' USING ERRCODE = '") + sqlstate + "'; END $$"));
}

std::int32_t backend_pid(pg::conn& c) { return c.query_one<std::int32_t>(sql{"SELECT pg_backend_pid()"}); }

void terminate_backend(std::int32_t pid) {
    pg::pool admin(options(1));
    pg::conn c(admin);
    c.exec(sql{"SELECT pg_terminate_backend({})", pid});
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
    REQUIRE(c.in_transaction() == false);
    {
        pg::transaction tx(c);
        add(c, 2);
    }
    REQUIRE(ids(c) == std::vector<std::int32_t>{1});
}

TEST_CASE("the first statement of a guard runs inside the transaction") {
    pg::conn c(db());
    pg::transaction tx(c);
    c.exec(sql{"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE"});
    REQUIRE(c.query_one<std::string>(sql{"SHOW transaction_isolation"}) == "serializable");
    tx.commit();
}

TEST_CASE("statements in a transaction see each other on one session") {
    reset();
    pg::conn c(db());
    pg::transaction tx(c);
    const std::int32_t pid = backend_pid(c);
    add(c, 1);
    add(c, 2);
    REQUIRE(ids(c) == std::vector<std::int32_t>{1, 2});
    REQUIRE(backend_pid(c) == pid);
    tx.commit();
}

TEST_CASE("a guard that runs nothing sends nothing") {
    pg::pool p(options(1));
    pg::conn c(p);
    {
        pg::transaction tx(c);
    }
    {
        pg::transaction outer(c);
        pg::transaction inner(c);
        inner.commit();
        outer.commit();
    }
    REQUIRE(p.stats().borrows == 0);
}

TEST_CASE("work is invisible to other connections until commit") {
    reset();
    pg::conn c(db());
    pg::transaction tx(c);
    add(c, 1);
    pg::pool other(options(1));
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
        REQUIRE(error_of([&] { add(c, 1); }).kind == pg::error_kind::data);
        REQUIRE(error_of([&] { tx.commit(); }).kind == pg::error_kind::other);
        REQUIRE(c.in_transaction() == false);
        REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
    }
    REQUIRE(ids().empty());
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

TEST_CASE("a nested commit after a failure rolls back to the savepoint and throws") {
    reset();
    {
        pg::conn c(db());
        pg::transaction outer(c);
        add(c, 1);
        {
            pg::transaction inner(c);
            REQUIRE_THROWS_AS(add(c, 1), pg::error);
            REQUIRE_THROWS_AS(inner.commit(), pg::error);
        }
        add(c, 2);
        outer.commit();
    }
    REQUIRE(ids() == std::vector<std::int32_t>{1, 2});
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
    REQUIRE_THROWS_AS(outer.commit(), pg::error);
}

TEST_CASE("sent writes in a transaction commit with it") {
    reset();
    {
        pg::conn c(db());
        pg::transaction tx(c);
        (void)c.send(sql{"INSERT INTO tx_items VALUES (1)"});
        (void)c.send(sql{"INSERT INTO tx_items VALUES (2)"});
        auto third = c.send(sql{"INSERT INTO tx_items VALUES (3)"});
        tx.commit();
        REQUIRE(third.affected() == 1);
    }
    REQUIRE(ids() == std::vector<std::int32_t>{1, 2, 3});
}

TEST_CASE("commit throws the error of an unread sent statement and stores nothing") {
    reset();
    {
        pg::conn c(db());
        add(c, 1);
    }
    {
        pg::conn c(db());
        pg::transaction tx(c);
        // A send that is not prepared yet prepares first, which fails in a transaction an
        // earlier send aborted; the failing send goes last so that every send queues.
        (void)c.send(sql{"INSERT INTO tx_items VALUES (2)"});
        (void)c.send(sql{"INSERT INTO tx_items VALUES (1)"});
        const pg::error e = error_of([&] { tx.commit(); });
        REQUIRE(e.kind == pg::error_kind::data);
        REQUIRE(e.sqlstate == "23505");
        REQUIRE(e.sql == "INSERT INTO tx_items VALUES (1)");
    }
    REQUIRE(ids() == std::vector<std::int32_t>{1});
}

TEST_CASE("a statement after a failed sent write throws that write's error when it must be prepared") {
    reset();
    pg::conn c(db());
    pg::transaction tx(c);
    auto first = c.send(sql{"INSERT INTO tx_items VALUES (1)"});
    auto dup = c.send(sql{"INSERT INTO tx_items VALUES (1)"});
    const auto fresh = "SELECT " + std::to_string(steady_clock::now().time_since_epoch().count());
    REQUIRE(error_of([&] { c.query_one<std::int64_t>(pg::unsafe_sql(fresh)); }).sqlstate == "23505");
    REQUIRE(first.affected() == 1);
    REQUIRE(error_of([&] { dup.affected(); }).sqlstate == "23505");
}

TEST_CASE("a rolled back transaction reads its sent writes first") {
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

TEST_CASE("a failing COMMIT throws its error") {
    {
        pg::conn c(db());
        c.exec(sql{"DROP TABLE IF EXISTS tx_deferred"});
        c.exec(sql{"CREATE TABLE tx_deferred (id int, u int, CONSTRAINT tx_deferred_u UNIQUE (u) DEFERRABLE INITIALLY DEFERRED)"});
    }
    {
        pg::conn c(db());
        pg::transaction tx(c);
        c.exec(sql{"INSERT INTO tx_deferred VALUES (1, 1)"});
        c.exec(sql{"INSERT INTO tx_deferred VALUES (2, 1)"});
        const pg::error e = error_of([&] { tx.commit(); });
        REQUIRE(e.kind == pg::error_kind::data);
        REQUIRE(e.sqlstate == "23505");
    }
    pg::conn other(db());
    REQUIRE(other.query_one<std::int64_t>(sql{"SELECT count(*) FROM tx_deferred"}) == 0);
}

TEST_CASE("losing the connection while COMMIT is in flight throws commit_unknown") {
    {
        pg::conn c(db());
        c.exec(sql{"DROP TABLE IF EXISTS tx_slow"});
        c.exec(sql{"CREATE TABLE tx_slow (id int)"});
        c.exec(sql{"CREATE OR REPLACE FUNCTION tx_slow_wait() RETURNS trigger LANGUAGE plpgsql AS "
                   "$$ BEGIN PERFORM pg_sleep(5); RETURN NULL; END $$"});
        c.exec(sql{"CREATE CONSTRAINT TRIGGER tx_slow_t AFTER INSERT ON tx_slow DEFERRABLE INITIALLY DEFERRED "
                   "FOR EACH ROW EXECUTE FUNCTION tx_slow_wait()"});
    }
    pg::pool p(options(1));
    pg::conn c(p);
    pg::transaction tx(c);
    const std::int32_t pid = backend_pid(c);
    c.exec(sql{"INSERT INTO tx_slow VALUES (1)"});
    std::thread killer([pid] {
        std::this_thread::sleep_for(300ms);
        terminate_backend(pid);
    });
    const auto t0 = steady_clock::now();
    const pg::error e = error_of([&] { tx.commit(); });
    killer.join();
    REQUIRE(e.kind == pg::error_kind::commit_unknown);
    REQUIRE(steady_clock::now() - t0 < 4s);
    REQUIRE(p.stats().open == 0);
}

TEST_CASE("a stale statement inside a transaction throws stale_statement") {
    pg::pool p(options(1));
    pg::conn c(p);
    c.exec(sql{"DROP TABLE IF EXISTS tx_shape"});
    c.exec(sql{"CREATE TABLE tx_shape (a int)"});
    REQUIRE(c.query<std::int32_t>(sql{"SELECT * FROM tx_shape"}).empty());
    c.exec(sql{"ALTER TABLE tx_shape ALTER COLUMN a TYPE bigint"});
    {
        pg::transaction tx(c);
        c.exec(sql{"SELECT 1"});
        const pg::error e = error_of([&] { c.query<std::int32_t>(sql{"SELECT * FROM tx_shape"}); });
        REQUIRE(e.sqlstate == "0A000");
        REQUIRE(e.kind == pg::error_kind::stale_statement);
    }
    REQUIRE(c.query<std::int64_t>(sql{"SELECT * FROM tx_shape"}).empty());
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
    const pg::error e = error_of([&] {
        db().transact([&](pg::conn& c) {
            ++attempts;
            add(c, attempts);
            raise(c, "40P01");
        });
    });
    REQUIRE(e.kind == pg::error_kind::conflict);
    REQUIRE(attempts == db().options().transact_attempts);
    REQUIRE(ids().empty());
}

TEST_CASE("transact does not retry a data error") {
    reset();
    int attempts = 0;
    const pg::error e = error_of([&] {
        db().transact([&](pg::conn& c) {
            ++attempts;
            add(c, 1);
            add(c, 1);
        });
    });
    REQUIRE(e.kind == pg::error_kind::data);
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
    pg::pool rival(options(1));
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
    const int got = db().transact(f);
    stored = 6;
    REQUIRE(got == 5);
}

TEST_CASE("transactions on one connection run one after another") {
    {
        pg::conn c(db());
        c.exec(sql{"DROP TABLE IF EXISTS tx_counter"});
        c.exec(sql{"CREATE TABLE tx_counter (id int PRIMARY KEY, n int)"});
        c.exec(sql{"INSERT INTO tx_counter VALUES (1, 0)"});
    }
    pg::pool p(options(1));
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                p.transact([](pg::conn& c) { c.exec(sql{"UPDATE tx_counter SET n = n + 1 WHERE id = 1"}); });
                p.transact([&](pg::conn& c) { c.send(sql{"SELECT {}::int", i}); });
            }
        });
    for (auto& t : threads) t.join();
    pg::conn c(db());
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT n FROM tx_counter"}) == 400);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a statement that must be prepared throws the first error of the sequence") {
    {
        pg::conn c(db());
        c.exec(sql{"DROP TABLE IF EXISTS tx_checked"});
        c.exec(sql{"CREATE TABLE tx_checked (n int CHECK (n > 0))"});
    }
    pg::conn c(db());
    pg::transaction tx(c);
    auto first = c.send(sql{"INSERT INTO tx_checked VALUES ({})", -1});
    const auto fresh = "SELECT " + std::to_string(steady_clock::now().time_since_epoch().count());
    REQUIRE(error_of([&] { c.send(pg::unsafe_sql(fresh)); }).sqlstate == "23514");
    REQUIRE(error_of([&] { first.affected(); }).sqlstate == "23514");
}
