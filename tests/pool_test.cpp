#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <latch>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "pg/pg.hpp"

using namespace std::chrono;
using pg::sql;

namespace {

std::string dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    return d;
}

pg::pool_options options(int max_conns, milliseconds acquire = 30s) {
    pg::pool_options o;
    o.dsn = dsn();
    o.max_conns = max_conns;
    o.acquire_timeout = acquire;
    return o;
}

// Runs fn(i) on `n` threads and joins them.
template <class F>
void on_threads(int n, F fn) {
    std::vector<std::thread> ts;
    for (int i = 0; i < n; ++i) ts.emplace_back([&fn, i] { fn(i); });
    for (auto& t : ts) t.join();
}

// Runs fn on a thread of its own: a second conn of a pool this thread has one of.
template <class F>
void elsewhere(F fn) {
    std::thread(fn).join();
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

std::int32_t backend_pid(pg::conn& c) { return c.query_one<std::int32_t>(sql{"SELECT pg_backend_pid()"}); }

void terminate_backend(std::int32_t pid) {
    pg::pool admin(options(1));
    pg::conn c(admin);
    c.exec(sql{"SELECT pg_terminate_backend({})", pid});
    for (int i = 0; i < 200; ++i) {
        if (c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pg_stat_activity WHERE pid = {}", pid}) == 0) return;
        std::this_thread::sleep_for(10ms);
    }
}

}  // namespace

TEST_CASE("many threads share a small pool") {
    pg::pool p(options(4));
    constexpr int threads = 32, rounds = 100;
    std::atomic<int> wrong{0};
    on_threads(threads, [&](int i) {
        for (int k = 0; k < rounds; ++k) {
            pg::conn c(p);
            if (c.query_one<std::int32_t>(sql{"SELECT {}::int + 1", i * rounds + k}) != i * rounds + k + 1) ++wrong;
        }
    });
    REQUIRE(wrong == 0);
    const pg::pool_stats s = p.stats();
    REQUIRE(s.open <= 4);
    REQUIRE(s.idle == s.open);
    REQUIRE(s.waiting == 0);
    REQUIRE(s.borrows == static_cast<std::uint64_t>(threads * rounds));
}

TEST_CASE("the pool never opens more than max_conns connections") {
    pg::pool p(options(2));
    std::mutex mu;
    std::set<std::int32_t> pids;
    on_threads(8, [&](int) {
        for (int k = 0; k < 5; ++k) {
            pg::conn c(p);
            const auto pid = c.query_one<std::int32_t>(sql{"SELECT pg_backend_pid() FROM pg_sleep(0.005)"});
            std::lock_guard lk(mu);
            pids.insert(pid);
        }
    });
    REQUIRE(pids.size() == 2);
    REQUIRE(p.stats().open == 2);
    REQUIRE(p.stats().idle == 2);
}

TEST_CASE("outside a transaction a conn holds no connection between statements") {
    pg::pool p(options(1, 2s));
    pg::conn a(p);
    REQUIRE(a.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
    REQUIRE(p.stats().idle == 1);
    std::int32_t got = 0;
    elsewhere([&] {
        pg::conn b(p);
        got = b.query_one<std::int32_t>(sql{"SELECT 2"});
    });
    REQUIRE(got == 2);
    REQUIRE(a.query_one<std::int32_t>(sql{"SELECT 3"}) == 3);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a statement waits for the connection a transaction holds") {
    pg::pool p(options(1));
    pg::conn holder(p);
    std::optional<pg::transaction> tx(std::in_place, holder);
    holder.exec(sql{"SELECT 1"});
    std::atomic<bool> got{false};
    std::thread waiter([&] {
        pg::conn second(p);
        second.exec(sql{"SELECT 1"});
        got = true;
    });
    for (int i = 0; i < 200 && p.stats().waiting == 0; ++i) std::this_thread::sleep_for(5ms);
    REQUIRE(p.stats().waiting == 1);
    REQUIRE(got.load() == false);
    tx->commit();
    waiter.join();
    REQUIRE(got.load() == true);
}

TEST_CASE("an exhausted pool throws after acquire_timeout") {
    pg::pool p(options(1, 200ms));
    pg::conn holder(p);
    pg::transaction tx(holder);
    holder.exec(sql{"SELECT 1"});
    pg::error e(pg::error_kind::broken, "nothing was thrown");
    milliseconds waited{};
    elsewhere([&] {
        pg::conn c(p);
        const auto t0 = steady_clock::now();
        try {
            c.exec(sql{"SELECT 1"});
        } catch (const pg::error& thrown) {
            e = thrown;
        }
        waited = duration_cast<milliseconds>(steady_clock::now() - t0);
    });
    REQUIRE(e.kind == pg::error_kind::other);
    REQUIRE(std::string(e.what()) == "no connection free within acquire_timeout (200 ms)");
    REQUIRE(waited >= 200ms);
    REQUIRE(waited < 2s);
    REQUIRE(p.stats().waiting == 0);
}

TEST_CASE("a nested borrow from one pool throws nested_borrow") {
    pg::pool p(options(4));
    pg::pool other(options(1));
    pg::conn c(p);
    REQUIRE_THROWS_AS(pg::conn(p), pg::nested_borrow);
    REQUIRE_THROWS_AS(p.transact([](pg::conn&) {}), pg::nested_borrow);
    pg::conn d(other);  // another pool is fine
    REQUIRE(d.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
}

TEST_CASE("a nested borrow inside transact throws nested_borrow and runs once") {
    pg::pool p(options(4));
    int calls = 0;
    REQUIRE_THROWS_AS(p.transact([&](pg::conn& c) {
        ++calls;
        c.exec(sql{"SELECT 1"});
        pg::conn again(p);
    }),
                      pg::nested_borrow);
    REQUIRE(calls == 1);
    pg::conn c(p);  // the thread holds nothing any more
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a failed connect throws connect_failed and frees its slot") {
    pg::pool_options o = options(1);
    o.dsn = "postgresql://postgres@127.0.0.1:1/none";
    pg::pool p(o);
    std::atomic<int> failed{0};
    on_threads(8, [&](int) {
        pg::conn c(p);
        try {
            c.exec(sql{"SELECT 1"});
        } catch (const pg::error& e) {
            if (e.kind == pg::error_kind::connect_failed) ++failed;
        }
    });
    REQUIRE(failed == 8);
    REQUIRE(p.stats().open == 0);
}

TEST_CASE("a wire broken by pg_terminate_backend is closed and the next statement works") {
    pg::pool p(options(1));
    pg::conn c(p);
    const std::int32_t victim = backend_pid(c);
    terminate_backend(victim);
    REQUIRE(error_of([&] { c.exec(sql{"SELECT 1"}); }).kind == pg::error_kind::broken);
    REQUIRE(p.stats().open == 0);
    REQUIRE(backend_pid(c) != victim);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a connection lost in a transaction fails the rest of it") {
    pg::pool p(options(2));
    pg::conn c(p);
    {
        pg::transaction tx(c);
        const std::int32_t victim = backend_pid(c);
        terminate_backend(victim);
        REQUIRE(error_of([&] { c.exec(sql{"SELECT 1"}); }).kind == pg::error_kind::broken);
        REQUIRE(error_of([&] { c.exec(sql{"SELECT 2"}); }).kind == pg::error_kind::broken);
        REQUIRE(error_of([&] { tx.commit(); }).kind == pg::error_kind::broken);
    }
    REQUIRE(p.stats().open == 0);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 3"}) == 3);
}

TEST_CASE("stats count connections, borrows and waiters") {
    pg::pool p(options(3));
    REQUIRE(p.stats().open == 0);
    pg::conn a(p);
    {
        pg::transaction tx(a);
        a.exec(sql{"SELECT 1"});
        elsewhere([&] {
            pg::conn b(p);
            b.exec(sql{"SELECT 1"});
        });
        const pg::pool_stats held = p.stats();
        REQUIRE(held.open == 2);
        REQUIRE(held.idle == 1);
        tx.commit();
    }
    const pg::pool_stats s = p.stats();
    REQUIRE(s.open == 2);
    REQUIRE(s.idle == 2);
    REQUIRE(s.waiting == 0);
    REQUIRE(s.borrows == 2);
}

TEST_CASE("session settings travel in the DSN") {
    pg::pool_options o = options(1);
    o.dsn += (o.dsn.find('?') == std::string::npos ? "?" : "&") +
             std::string("options=-c%20application_name%3Dfrom_dsn%20-c%20statement_timeout%3D5s");
    pg::pool p(o);
    pg::conn c(p);
    REQUIRE(c.query_one<std::string>(sql{"SHOW application_name"}) == "from_dsn");
    REQUIRE(c.query_one<std::string>(sql{"SHOW statement_timeout"}) == "5s");
}

TEST_CASE("sent statements travel together and each gets its own result") {
    pg::pool p(options(2));
    pg::conn c(p);
    auto a = c.send<std::int32_t>(sql{"SELECT 1"});
    auto b = c.send<std::int32_t>(sql{"SELECT {}", 2});
    auto d = c.send<std::int32_t>(sql{"SELECT generate_series(1, 3)"});
    auto n = c.send(sql{"SELECT generate_series(1, 4)"});
    REQUIRE(d.all() == std::vector<std::int32_t>{1, 2, 3});
    REQUIRE(b.one() == 2);
    REQUIRE(a.opt() == std::optional<std::int32_t>{1});
    REQUIRE(n.affected() == 4);
}

TEST_CASE("sent statements outside a transaction hold the connection until read") {
    pg::pool p(options(1, 200ms));
    pg::conn c(p);
    auto a = c.send<std::int32_t>(sql{"SELECT 1"});
    std::optional<pg::error_kind> refused;
    elsewhere([&] {
        pg::conn d(p);
        try {
            d.exec(sql{"SELECT 1"});
        } catch (const pg::error& e) {
            refused = e.kind;
        }
    });
    REQUIRE(refused == pg::error_kind::other);
    REQUIRE(a.one() == 1);
    std::int32_t got = 0;
    elsewhere([&] {
        pg::conn d(p);
        got = d.query_one<std::int32_t>(sql{"SELECT 2"});
    });
    REQUIRE(got == 2);
}

TEST_CASE("a sent statement nobody reads still runs before the conn ends") {
    pg::pool p(options(1));
    {
        pg::conn c(p);
        c.exec(sql{"CREATE TABLE IF NOT EXISTS sent_unread_items(id int)"});
        c.exec(sql{"TRUNCATE sent_unread_items"});
        (void)c.send(sql{"INSERT INTO sent_unread_items VALUES (1)"});
        (void)c.send(sql{"SELECT 1/0"});
    }
    pg::conn c(p);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM sent_unread_items"}) == 1);
}

TEST_CASE("a pending is read after its conn ended") {
    pg::pool p(options(1));
    std::optional<pg::pending<std::int32_t>> good, bad;
    {
        pg::conn c(p);
        good.emplace(c.send<std::int32_t>(sql{"SELECT 42"}));
        bad.emplace(c.send<std::int32_t>(sql{"SELECT 1/0"}));
    }
    REQUIRE(p.stats().idle == 1);
    REQUIRE(good->one() == 42);
    REQUIRE(error_of([&] { bad->one(); }).kind == pg::error_kind::data);
}

TEST_CASE("a pending result is read once") {
    pg::pool p(options(1));
    pg::conn c(p);
    auto q = c.send<std::int32_t>(sql{"SELECT 1"});
    REQUIRE(q.one() == 1);
    REQUIRE_THROWS_AS(q.one(), pg::error);
}

TEST_CASE("a sent statement's error is thrown by its reader; send checks columns at once") {
    pg::pool p(options(1));
    pg::conn c(p);
    auto failing = c.send<std::int32_t>(sql{"SELECT 1/0"});
    auto fine = c.send<std::int32_t>(sql{"SELECT 7"});
    REQUIRE(fine.one() == 7);
    REQUIRE(error_of([&] { failing.one(); }).sqlstate == "22012");
    REQUIRE_THROWS_AS(c.send<std::int64_t>(sql{"SELECT 1"}), pg::error);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
}

TEST_CASE("raw() hands out the connection in blocking mode and closes it with the conn") {
    pg::pool p(options(1));
    {
        pg::conn c(p);
        auto sent = c.send<std::int32_t>(sql{"SELECT 5"});
        PGconn* r = c.raw();
        REQUIRE(sent.one() == 5);
        REQUIRE(PQpipelineStatus(r) == PQ_PIPELINE_OFF);
        PGresult* res = PQexec(r, "SET application_name = 'raw'; SELECT pg_backend_pid()");
        REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
        const int pid = std::atoi(PQgetvalue(res, 0, 0));
        PQclear(res);
        REQUIRE(backend_pid(c) == pid);
        REQUIRE(c.query_one<std::string>(sql{"SHOW application_name"}) == "raw");
    }
    REQUIRE(p.stats().open == 0);
    pg::conn c(p);
    REQUIRE(c.query_one<std::string>(sql{"SHOW application_name"}).empty());
}

TEST_CASE("statements of many threads on one connection get their own results") {
    pg::pool p(options(1));
    constexpr int threads = 16, rounds = 300;
    std::atomic<int> wrong{0};
    on_threads(threads, [&](int i) {
        for (int k = 0; k < rounds; ++k) {
            const std::int64_t n = static_cast<std::int64_t>(i) * rounds + k;
            pg::conn c(p);
            if (k % 7 == 0) {
                try {
                    c.exec(sql{"SELECT 1/0"});
                    ++wrong;
                } catch (const pg::error& e) {
                    if (e.kind != pg::error_kind::data) ++wrong;
                }
            } else if (c.query_one<std::int64_t>(sql{"SELECT {}::int8 * 2", n}) != n * 2) {
                ++wrong;
            }
        }
    });
    REQUIRE(wrong.load() == 0);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("transactions interleave with plain statements") {
    pg::pool p(options(2));
    {
        pg::conn c(p);
        c.exec(sql{"DROP TABLE IF EXISTS pool_tx_counter"});
        c.exec(sql{"CREATE TABLE pool_tx_counter(id int primary key, n bigint)"});
        c.exec(sql{"INSERT INTO pool_tx_counter VALUES (1, 0)"});
    }
    constexpr int tx_threads = 4, tx_rounds = 200, plain_threads = 4, plain_rounds = 500;
    std::atomic<int> wrong{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < tx_threads; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < tx_rounds; ++k)
                p.transact([](pg::conn& c) { c.exec(sql{"UPDATE pool_tx_counter SET n = n + 1 WHERE id = 1"}); });
        });
    for (int i = 0; i < plain_threads; ++i)
        ts.emplace_back([&, i] {
            for (int k = 0; k < plain_rounds; ++k) {
                const std::int64_t n = static_cast<std::int64_t>(i) * plain_rounds + k;
                pg::conn c(p);
                if (c.query_one<std::int64_t>(sql{"SELECT {}::int8", n}) != n) ++wrong;
            }
        });
    for (auto& t : ts) t.join();
    REQUIRE(wrong.load() == 0);
    pg::conn c(p);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT n FROM pool_tx_counter WHERE id = 1"}) == tx_threads * tx_rounds);
}

TEST_CASE("a burst of borrowers is served") {
    pg::pool p(options(2));
    constexpr int threads = 64, rounds = 300;
    std::latch go(threads);
    std::atomic<int> wrong{0};
    on_threads(threads, [&](int) {
        go.arrive_and_wait();
        for (int k = 0; k < rounds; ++k) {
            pg::conn c(p);
            if (c.query_one<std::int32_t>(sql{"SELECT 1"}) != 1) ++wrong;
        }
    });
    REQUIRE(wrong.load() == 0);
    const pg::pool_stats s = p.stats();
    REQUIRE(s.waiting == 0);
    REQUIRE(s.open <= 2);
}

TEST_CASE("point reads and transactions of 32 threads on 4 connections all finish, none starved") {
    pg::pool p(options(4));
    constexpr int threads = 32, rounds = 50;
    {
        pg::conn c(p);
        c.exec(sql{"DROP TABLE IF EXISTS pool_mixed"});
        c.exec(sql{"CREATE TABLE pool_mixed(id int primary key, n bigint)"});
        c.exec(sql{"INSERT INTO pool_mixed SELECT i, 0 FROM generate_series(0, {}) i", threads - 1});
    }
    std::atomic<int> done{0}, wrong{0};
    std::atomic<std::int64_t> max_tx_us{0};
    on_threads(threads, [&](int i) {
        for (int k = 0; k < rounds; ++k) {
            if (i % 2 == 0) {
                pg::conn c(p);
                if (c.query_one<std::int32_t>(sql{"SELECT id FROM pool_mixed WHERE id = {}", i}) != i) ++wrong;
            } else {
                const auto t0 = steady_clock::now();
                p.transact([&](pg::conn& c) {
                    c.exec(sql{"UPDATE pool_mixed SET n = n + 1 WHERE id = {}", i});
                    c.query_one<std::int64_t>(sql{"SELECT n FROM pool_mixed WHERE id = {}", i});
                });
                const std::int64_t us = duration_cast<microseconds>(steady_clock::now() - t0).count();
                for (std::int64_t seen = max_tx_us; us > seen && !max_tx_us.compare_exchange_weak(seen, us);) {
                }
            }
            ++done;
        }
    });
    REQUIRE(wrong == 0);
    REQUIRE(done == threads * rounds);
    REQUIRE(max_tx_us.load() < 2'000'000);
    pg::conn c(p);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT sum(n)::int8 FROM pool_mixed"}) == threads / 2 * rounds);
    REQUIRE(p.stats().open <= 4);
    REQUIRE(p.stats().waiting == 0);
}

TEST_CASE("threads sharing one connection each read back their own value") {
    pg::pool p(options(1));
    constexpr int threads = 24, rounds = 200;
    std::atomic<int> wrong{0};
    std::latch go(threads);
    on_threads(threads, [&](int i) {
        go.arrive_and_wait();
        for (int k = 0; k < rounds; ++k) {
            pg::conn c(p);
            const auto [id, text] = c.query_one<std::int32_t, std::string>(sql{"SELECT {}::int4, {}::text", i, std::to_string(k)});
            if (id != i || text != std::to_string(k)) ++wrong;
        }
    });
    REQUIRE(wrong == 0);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a statement error on a shared connection affects that statement only") {
    pg::pool p(options(1));
    std::latch go(2);
    std::optional<pg::error_kind> failed;
    std::int32_t fine = 0;
    std::thread a([&] {
        pg::conn c(p);
        go.arrive_and_wait();
        try {
            c.exec(sql{"SELECT 1/0 FROM pg_sleep(0.05)"});
        } catch (const pg::error& e) {
            failed = e.kind;
        }
    });
    std::thread b([&] {
        pg::conn c(p);
        go.arrive_and_wait();
        fine = c.query_one<std::int32_t>(sql{"SELECT 2 FROM pg_sleep(0.05)"});
    });
    a.join();
    b.join();
    REQUIRE(failed == pg::error_kind::data);
    REQUIRE(fine == 2);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a shared connection terminated mid-flight fails its statements only, then statements work") {
    pg::pool p(options(1));
    std::int32_t victim = 0;
    {
        pg::conn c(p);
        victim = backend_pid(c);
    }
    constexpr int threads = 4;
    std::atomic<int> broken{0}, after{0};
    std::thread killer([&] {
        std::this_thread::sleep_for(300ms);
        terminate_backend(victim);
    });
    on_threads(threads, [&](int) {
        pg::conn c(p);
        try {
            c.exec(sql{"SELECT 1 FROM pg_sleep(3)"});
        } catch (const pg::error& e) {
            if (e.kind == pg::error_kind::broken) ++broken;
        }
        if (c.query_one<std::int32_t>(sql{"SELECT 2"}) == 2) ++after;
    });
    killer.join();
    REQUIRE(broken == threads);
    REQUIRE(after == threads);
    pg::conn c(p);
    REQUIRE(backend_pid(c) != victim);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a statement with more than 65535 parameters fails alone, before it borrows a connection") {
    pg::pool p(options(1));
    const std::vector<sql> many(70000, sql{"{}", 1});
    const sql big{"SELECT 1 WHERE 1 IN ({})", pg::join(many, sql{", "})};
    pg::conn c(p);
    REQUIRE(error_of([&] { c.exec(big); }).kind == pg::error_kind::other);
    REQUIRE(p.stats().borrows == 0);
    std::int32_t a = 0, b = 0;
    std::thread ta([&] {
        pg::conn d(p);
        a = d.query_one<std::int32_t>(sql{"SELECT 1 FROM pg_sleep(0.3)"});
    });
    std::this_thread::sleep_for(100ms);
    std::thread tb([&] {
        pg::conn d(p);
        b = d.query_one<std::int32_t>(sql{"SELECT 2"});
    });
    std::this_thread::sleep_for(100ms);
    REQUIRE(error_of([&] { c.exec(big); }).kind == pg::error_kind::other);
    ta.join();
    tb.join();
    REQUIRE(a == 1);
    REQUIRE(b == 2);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a shared statement that opens a transaction throws, and the statements queued after it fail unstored") {
    pg::pool p(options(1));
    pg::conn c(p);
    c.exec(sql{"DROP TABLE IF EXISTS pool_begin"});
    c.exec(sql{"CREATE TABLE pool_begin (id int)"});
    c.exec(sql{"INSERT INTO pool_begin VALUES ({})", 0});
    c.exec(sql{"SELECT pg_sleep({})", 0.0});
    std::optional<pg::error> begin, insert;
    // The sleeps keep the BEGIN unread until the insert is queued behind it on the one connection.
    std::thread z([&] {
        pg::conn d(p);
        d.exec(sql{"SELECT pg_sleep({})", 0.3});
    });
    std::this_thread::sleep_for(50ms);
    std::thread x([&] {
        pg::conn d(p);
        try {
            d.exec(sql{"BEGIN"});
        } catch (const pg::error& e) {
            begin = e;
        }
    });
    std::this_thread::sleep_for(50ms);
    std::thread z2([&] {
        pg::conn d(p);
        d.exec(sql{"SELECT pg_sleep({})", 0.6});
    });
    std::this_thread::sleep_for(400ms);
    std::thread y([&] {
        pg::conn d(p);
        try {
            d.exec(sql{"INSERT INTO pool_begin VALUES ({})", 1});
        } catch (const pg::error& e) {
            insert = e;
        }
    });
    for (auto* t : {&z, &x, &z2, &y}) t->join();
    REQUIRE(begin);
    REQUIRE(begin->kind == pg::error_kind::other);
    REQUIRE(std::string(begin->what()).find("pg::transaction") != std::string::npos);
    REQUIRE(insert);
    REQUIRE(insert->kind == pg::error_kind::broken);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pool_begin WHERE id = 1"}) == 0);
}

TEST_CASE("a waiter that becomes the oldest when a connection is handed out takes a free slot") {
    // h2 closes its wire (waking the exclusive waiter x) and h1 hands its wire to x;
    // when the hand-off lands first, s must still be woken to open a connection in the
    // freed slot instead of waiting for x. The rounds sweep h1's delay over that race.
    pg::pool p(options(2, 5s));
    int slow = 0;
    for (int round = 0; round < 40; ++round) {
        std::atomic<bool> go{false};
        std::latch held(2);
        std::thread h1([&] {
            pg::conn c(p);
            auto one = c.send<std::int32_t>(sql{"SELECT 1"});
            held.count_down();
            while (!go) std::this_thread::yield();
            for (const auto until = steady_clock::now() + microseconds(round % 10 * 5); steady_clock::now() < until;) {
            }
            (void)one.one();
        });
        std::thread h2([&] {
            pg::conn c(p);
            (void)c.raw();
            held.count_down();
            while (!go) std::this_thread::yield();
        });
        held.wait();
        std::this_thread::sleep_for(20ms);
        std::thread x([&] {
            pg::conn c(p);
            (void)c.send(sql{"SELECT pg_sleep(0.1)"});
        });
        while (p.stats().waiting < 1) std::this_thread::yield();
        steady_clock::time_point done;
        std::thread s([&] {
            pg::conn c(p);
            c.exec(sql{"SELECT 1"});
            done = steady_clock::now();
        });
        while (p.stats().waiting < 2) std::this_thread::yield();
        const auto start = steady_clock::now();
        go = true;
        for (auto* t : {&h1, &h2, &s, &x}) t->join();
        if (done - start > 60ms) ++slow;
    }
    REQUIRE(slow == 0);
}
