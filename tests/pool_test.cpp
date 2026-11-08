#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

pg::pool_options options(int max_conns) {
    pg::pool_options o;
    o.dsn = dsn();
    o.max_conns = max_conns;
    return o;
}

// Runs fn(i) on `n` threads and joins them.
template <class F>
void on_threads(int n, F fn) {
    std::vector<std::thread> ts;
    for (int i = 0; i < n; ++i) ts.emplace_back([&fn, i] { fn(i); });
    for (auto& t : ts) t.join();
}

// Runs fn on a thread of its own: a second borrow from a pool this thread holds.
template <class F>
void elsewhere(F fn) {
    std::thread(fn).join();
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

pg::error_kind kind_of_borrow(pg::pool& p) {
    pg::error_kind k = pg::error_kind::other;
    bool threw = false;
    elsewhere([&] {
        try {
            pg::conn c(p);
        } catch (const pg::error& e) {
            k = e.kind;
            threw = true;
        }
    });
    REQUIRE(threw);
    return k;
}

}  // namespace

TEST_CASE("many threads share a small pool") {
    pg::pool p(options(4));
    constexpr int threads = 32, rounds = 100;
    std::atomic<int> done{0}, wrong{0};
    on_threads(threads, [&](int i) {
        for (int k = 0; k < rounds; ++k) {
            pg::conn c(p);
            if (c.query_one<std::int32_t>(sql{"SELECT {}::int + 1", i * rounds + k}) != i * rounds + k + 1) ++wrong;
            ++done;
        }
    });
    REQUIRE(done == threads * rounds);
    REQUIRE(wrong == 0);
    const pg::pool_stats s = p.stats();
    REQUIRE(s.open <= 4);
    REQUIRE(s.leased == 0);
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

TEST_CASE("waiters are served in turn, none starves") {
    pg::pool p(options(4));
    constexpr int threads = 32;
    std::vector<std::int64_t> ops(threads, 0);
    std::atomic<bool> stop{false};
    std::thread timer([&] {
        std::this_thread::sleep_for(1500ms);
        stop = true;
    });
    on_threads(threads, [&](int i) {
        while (!stop) {
            pg::conn c(p);
            c.query_one<std::int32_t>(sql{"SELECT 1"});
            ++ops[static_cast<std::size_t>(i)];
        }
    });
    timer.join();
    const auto [lo, hi] = std::minmax_element(ops.begin(), ops.end());
    INFO("min " << *lo << " max " << *hi);
    REQUIRE(*lo > 0);
    REQUIRE(*lo * 4 >= *hi);
}

TEST_CASE("a borrow waits for a returned connection") {
    pg::pool p(options(1));
    std::optional<pg::conn> first(std::in_place, p);
    std::atomic<bool> got{false};
    std::thread waiter([&] {
        pg::conn second(p);
        got = true;
    });
    std::this_thread::sleep_for(100ms);
    REQUIRE_FALSE(got.load());
    REQUIRE(p.stats().waiting == 1);
    first.reset();
    waiter.join();
    REQUIRE(got.load());
}

TEST_CASE("a borrow gives up after acquire_timeout") {
    pg::pool_options o = options(1);
    o.acquire_timeout = 200ms;
    pg::pool p(o);
    pg::conn held(p);
    const auto t0 = steady_clock::now();
    REQUIRE(kind_of_borrow(p) == pg::error_kind::connect_failed);
    const auto waited = steady_clock::now() - t0;
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

TEST_CASE("a nested borrow inside transact throws nested_borrow") {
    pg::pool p(options(4));
    int calls = 0;
    REQUIRE_THROWS_AS(p.transact([&](pg::conn&) {
        ++calls;
        pg::conn again(p);
    }),
                      pg::nested_borrow);
    REQUIRE(calls == 1);
    pg::conn c(p);  // the thread holds nothing any more
    REQUIRE(p.stats().leased == 0);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("a failed connect passes its slot on: every waiter gets an answer") {
    pg::pool_options o = options(1);
    o.dsn = "postgresql://postgres@127.0.0.1:1/none";
    o.breaker_failures = 1000;
    pg::pool p(o);
    std::atomic<int> failed{0};
    on_threads(8, [&](int) {
        try {
            pg::conn c(p);
        } catch (const pg::error& e) {
            if (e.kind == pg::error_kind::connect_failed) ++failed;
        }
    });
    REQUIRE(failed == 8);
    REQUIRE(p.stats().connect_failures == 8);
    REQUIRE(p.stats().open == 0);
}

TEST_CASE("the breaker opens after repeated connect failures and still lends idle connections") {
    {
        pg::pool admin(options(1));
        pg::conn c(admin);
        c.exec(sql{"DROP ROLE IF EXISTS breaker_user"});
        c.exec(sql{"CREATE ROLE breaker_user LOGIN CONNECTION LIMIT 1 PASSWORD 'breaker'"});
    }
    {
        pg::pool_options o = options(3);
        const std::string d = dsn();
        const auto user = d.find("://") + 3;
        o.dsn = d.substr(0, user) + "breaker_user:breaker@" + d.substr(d.find('@', user) + 1);
        o.breaker_failures = 3;
        o.breaker_cooldown = 60s;
        pg::pool p(o);
        std::optional<pg::conn> held(std::in_place, p);
        for (int i = 0; i < 3; ++i) REQUIRE(kind_of_borrow(p) == pg::error_kind::connect_failed);
        REQUIRE(p.stats().connect_failures == 3);
        REQUIRE(p.stats().breaker_open);

        const auto t0 = steady_clock::now();
        REQUIRE(kind_of_borrow(p) == pg::error_kind::connect_failed);
        REQUIRE(steady_clock::now() - t0 < 100ms);
        REQUIRE(p.stats().connect_failures == 3);

        held.reset();
        std::int32_t one = 0;
        elsewhere([&] {
            pg::conn c(p);
            one = c.query_one<std::int32_t>(sql{"SELECT 1"});
        });
        REQUIRE(one == 1);
        REQUIRE(p.stats().breaker_open);
    }
    pg::pool admin(options(1));
    pg::conn c(admin);
    c.exec(sql{"DROP ROLE breaker_user"});
}

TEST_CASE("a broken connection is replaced on the next borrow") {
    pg::pool p(options(1));
    std::int32_t victim = 0;
    {
        pg::conn c(p);
        victim = backend_pid(c);
        terminate_backend(victim);
        try {
            c.exec(sql{"SELECT 1"});
            FAIL("a terminated backend answered");
        } catch (const pg::error& e) {
            REQUIRE(e.kind == pg::error_kind::broken);
        }
    }
    REQUIRE(p.stats().open == 0);
    pg::conn fresh(p);
    REQUIRE(backend_pid(fresh) != victim);
}

TEST_CASE("an idle connection that fails its probe is replaced") {
    pg::pool_options o = options(1);
    o.probe_after = 0ms;
    pg::pool p(o);
    std::int32_t victim = 0;
    {
        pg::conn c(p);
        victim = backend_pid(c);
    }
    terminate_backend(victim);
    pg::conn c(p);
    const std::int32_t pid = backend_pid(c);
    REQUIRE(pid != victim);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("statement_timeout is 30 s unless configured") {
    {
        pg::pool p(options(1));
        pg::conn c(p);
        REQUIRE(c.query_one<std::string>(sql{"SHOW statement_timeout"}) == "30s");
    }
    pg::pool_options o = options(1);
    o.statement_timeout = 0ms;
    pg::pool p(o);
    pg::conn c(p);
    REQUIRE(c.query_one<std::string>(sql{"SHOW statement_timeout"}) == "0");
}

TEST_CASE("commits are asynchronous unless the pool asks for synchronous ones") {
    {
        pg::pool p(options(1));
        pg::conn c(p);
        REQUIRE(c.query_one<std::string>(sql{"SHOW synchronous_commit"}) == "off");
    }
    {
        pg::pool_options o = options(1);
        o.synchronous_commit = true;
        pg::pool p(o);
        pg::conn c(p);
        REQUIRE(c.query_one<std::string>(sql{"SHOW synchronous_commit"}) == "on");
    }
    {
        pg::pool p(options(1));
        {
            pg::conn c(p);
            c.exec(sql{"SET synchronous_commit = on"});
        }
        pg::conn c(p);
        REQUIRE(c.query_one<std::string>(sql{"SHOW synchronous_commit"}) == "off");
    }
}

TEST_CASE("a passed deadline cancels the query and discards the connection") {
    pg::pool_options o = options(1);
    o.deadline = 200ms;
    o.statement_timeout = 0ms;
    pg::pool p(o);
    std::int32_t victim = 0;
    {
        pg::conn c(p);
        victim = backend_pid(c);
        const auto t0 = steady_clock::now();
        try {
            c.exec(sql{"SELECT pg_sleep(5)"});
            FAIL("pg_sleep(5) finished within the deadline");
        } catch (const pg::error& e) {
            REQUIRE(e.kind == pg::error_kind::deadline);
            REQUIRE(e.sql == "SELECT pg_sleep(5)");
        }
        REQUIRE(steady_clock::now() - t0 < 1s);
    }
    REQUIRE(p.stats().open == 0);
    pg::conn c(p);
    REQUIRE(backend_pid(c) != victim);
}

TEST_CASE("stats count connections, borrows and waiters") {
    pg::pool p(options(3));
    REQUIRE(p.stats().open == 0);
    {
        pg::conn a(p);
        pg::pool_stats both;
        elsewhere([&] {
            pg::conn b(p);
            both = p.stats();
        });
        REQUIRE(both.open == 2);
        REQUIRE(both.leased == 2);
        REQUIRE(both.idle == 0);
        REQUIRE(p.stats().idle == 1);
    }
    const pg::pool_stats s = p.stats();
    REQUIRE(s.open == 2);
    REQUIRE(s.idle == 2);
    REQUIRE(s.leased == 0);
    REQUIRE(s.waiting == 0);
    REQUIRE(s.borrows == 2);
    REQUIRE(s.connect_failures == 0);
    REQUIRE_FALSE(s.breaker_open);
}

TEST_CASE("session settings of one borrower are gone for the next, statement_timeout stays") {
    pg::pool p(options(1));
    {
        pg::conn c(p);
        c.exec(sql{"SET statement_timeout = 1"});
        c.exec(sql{"SET search_path = pg_catalog"});
        c.exec(sql{"SET application_name = 'leaky'"});
    }
    {
        pg::conn untouched(p);
    }
    pg::conn c(p);
    REQUIRE(c.query_one<std::string>(sql{"SHOW statement_timeout"}) == "30s");
    REQUIRE(c.query_one<std::string>(sql{"SHOW search_path"}) != "pg_catalog");
    REQUIRE(c.query_one<std::string>(sql{"SHOW application_name"}) != "leaky");
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("the DSN's own options are kept next to statement_timeout") {
    pg::pool_options o = options(1);
    o.dsn += (o.dsn.find('?') == std::string::npos ? "?" : "&") + std::string("options=-c%20application_name%3Dfrom_dsn");
    pg::pool p(o);
    pg::conn c(p);
    REQUIRE(c.query_one<std::string>(sql{"SHOW application_name"}) == "from_dsn");
    REQUIRE(c.query_one<std::string>(sql{"SHOW statement_timeout"}) == "30s");
}

namespace {

pg::pool_options blackhole_options(milliseconds acquire) {
    pg::pool_options o;
    o.dsn = "postgresql://postgres@10.255.255.1:5432/none";
    o.max_conns = 8;
    o.acquire_timeout = acquire;
    return o;
}

// Borrows once, expecting connect_failed; returns how long it took.
milliseconds time_borrow(pg::pool& p) {
    const auto t0 = steady_clock::now();
    try {
        pg::conn c(p);
        FAIL("connected to a blackhole");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::connect_failed);
    }
    return duration_cast<milliseconds>(steady_clock::now() - t0);
}

// A network that refuses at once cannot show a connect that hangs.
bool blackhole_hangs() {
    pg::pool p(blackhole_options(400ms));
    return time_borrow(p) >= 300ms;
}

}  // namespace

TEST_CASE("a connect to an unreachable host gives up at acquire_timeout") {
    if (!blackhole_hangs()) {
        WARN("the network refuses 10.255.255.1 at once; nothing to time out");
        return;
    }
    pg::pool p(blackhole_options(500ms));
    const auto took = time_borrow(p);
    REQUIRE(took >= 400ms);
    REQUIRE(took < 1500ms);
    REQUIRE(p.stats().open == 0);
}

TEST_CASE("the breaker cooldown doubles only when the trial connect fails") {
    if (!blackhole_hangs()) {
        WARN("the network refuses 10.255.255.1 at once; nothing to time out");
        return;
    }
    pg::pool_options o = blackhole_options(400ms);
    o.breaker_failures = 3;
    o.breaker_cooldown = 100ms;
    pg::pool p(o);
    on_threads(6, [&](int) {
        try {
            pg::conn c(p);
        } catch (const pg::error&) {
        }
    });
    REQUIRE(p.stats().breaker_open);
    REQUIRE(p.stats().connect_failures == 6);

    // Six in-flight failures left the cooldown at 100 ms: a trial connect is allowed.
    std::this_thread::sleep_for(150ms);
    REQUIRE(time_borrow(p) >= 300ms);
    REQUIRE(p.stats().connect_failures == 7);

    // The failed trial doubled it: refused at once for the next 200 ms.
    REQUIRE(time_borrow(p) < 100ms);
    REQUIRE(p.stats().connect_failures == 7);
}

TEST_CASE("floating statements on one connection get their own results") {
    pg::pool p(options(1));
    constexpr int threads = 16, rounds = 300;
    std::atomic<int> wrong{0}, bad_kind{0};
    on_threads(threads, [&](int i) {
        for (int k = 0; k < rounds; ++k) {
            const std::int64_t n = static_cast<std::int64_t>(i) * rounds + k;
            pg::conn c(p);
            if (k % 7 == 0) {
                try {
                    c.exec(sql{"SELECT 1/0"});
                    ++bad_kind;
                } catch (const pg::error& e) {
                    if (e.kind == pg::error_kind::broken) ++bad_kind;
                }
            } else if (c.query_one<std::int64_t>(sql{"SELECT {}::int8 * 2", n}) != n * 2) {
                ++wrong;
            }
        }
    });
    REQUIRE(wrong.load() == 0);
    REQUIRE(bad_kind.load() == 0);
    REQUIRE(p.stats().open == 1);
}

TEST_CASE("transactions interleave with floating statements") {
    pg::pool p(options(2));
    {
        pg::conn c(p);
        c.exec(sql{"DROP TABLE IF EXISTS wb_tx_counter"});
        c.exec(sql{"CREATE TABLE wb_tx_counter(id int primary key, n bigint)"});
        c.exec(sql{"INSERT INTO wb_tx_counter VALUES (1, 0)"});
    }
    constexpr int tx_threads = 4, tx_rounds = 200, fl_threads = 4, fl_rounds = 500;
    std::atomic<int> wrong{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < tx_threads; ++i)
        ts.emplace_back([&] {
            for (int k = 0; k < tx_rounds; ++k)
                p.transact([](pg::conn& c) { c.exec(sql{"UPDATE wb_tx_counter SET n = n + 1 WHERE id = 1"}); });
        });
    for (int i = 0; i < fl_threads; ++i)
        ts.emplace_back([&, i] {
            for (int k = 0; k < fl_rounds; ++k) {
                const std::int64_t n = static_cast<std::int64_t>(i) * fl_rounds + k;
                pg::conn c(p);
                if (c.query_one<std::int64_t>(sql{"SELECT {}::int8", n}) != n) ++wrong;
            }
        });
    for (auto& t : ts) t.join();
    REQUIRE(wrong.load() == 0);
    pg::conn c(p);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT n FROM wb_tx_counter WHERE id = 1"}) == tx_threads * tx_rounds);
}

TEST_CASE("a session-state statement keeps the conn on its connection") {
    pg::pool p(options(4));
    pg::conn c(p);
    c.exec(pg::unsafe_sql("SET application_name = 'pinned'"));
    std::atomic<bool> stop{false};
    std::atomic<int> wrong{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            while (!stop.load()) {
                pg::conn f(p);
                f.query_one<std::int32_t>(sql{"SELECT 1"});
            }
        });
    for (int k = 0; k < 20; ++k)
        if (c.query_one<std::string>(sql{"SELECT current_setting('application_name')"}) != "pinned") ++wrong;
    stop = true;
    for (auto& t : ts) t.join();
    REQUIRE(wrong.load() == 0);
}

TEST_CASE("short statements do not queue behind a slow one") {
    pg::pool p(options(2));
    std::thread slow([&] {
        pg::conn c(p);
        c.exec(sql{"SELECT pg_sleep(1)"});
    });
    std::this_thread::sleep_for(100ms);
    std::atomic<std::int64_t> slowest{0};
    on_threads(4, [&](int) {
        for (int k = 0; k < 50; ++k) {
            const auto t0 = steady_clock::now();
            pg::conn c(p);
            c.query_one<std::int32_t>(sql{"SELECT 1"});
            const std::int64_t took = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
            std::int64_t prev = slowest.load();
            while (took > prev && !slowest.compare_exchange_weak(prev, took)) {
            }
        }
    });
    slow.join();
    INFO("slowest short statement took " << slowest.load() / 1'000'000 << " ms");
    REQUIRE(slowest.load() < 500'000'000);
}

TEST_CASE("a statement known to be slow gets no company") {
    pg::pool p(options(2));
    {
        pg::conn c(p);
        c.exec(sql{"SELECT pg_sleep(0.2)"});  // learn the duration
    }
    std::atomic<bool> started{false};
    std::thread slow([&] {
        started = true;
        for (int k = 0; k < 3; ++k) {
            pg::conn c(p);
            c.exec(sql{"SELECT pg_sleep(0.2)"});
        }
    });
    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);
    std::atomic<std::int64_t> slowest{0};
    on_threads(4, [&](int) {
        for (int k = 0; k < 100; ++k) {
            const auto t0 = steady_clock::now();
            pg::conn c(p);
            c.query_one<std::int32_t>(sql{"SELECT 1"});
            const std::int64_t took = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
            std::int64_t prev = slowest.load();
            while (took > prev && !slowest.compare_exchange_weak(prev, took)) {
            }
        }
    });
    slow.join();
    INFO("slowest short statement took " << slowest.load() / 1'000'000 << " ms");
    REQUIRE(slowest.load() < 150'000'000);
}

TEST_CASE("sent queries travel together and each gets its own result") {
    pg::pool p(options(2));
    pg::conn c(p);
    auto a = c.send<std::int32_t>(sql{"SELECT 1"});
    auto b = c.send<std::int32_t>(sql{"SELECT {}", 2});
    auto d = c.send<std::int32_t>(sql{"SELECT generate_series(1, 3)"});
    REQUIRE(d.all() == std::vector<std::int32_t>{1, 2, 3});
    REQUIRE(b.one() == 2);
    REQUIRE(a.one() == 1);
}

TEST_CASE("a sent statement nobody reads still runs before the conn ends") {
    pg::pool p(options(1));
    {
        pg::conn c(p);
        c.exec(sql{"CREATE TABLE IF NOT EXISTS sent_unread_items(id int)"});
        c.exec(sql{"TRUNCATE sent_unread_items"});
        (void)c.send(sql{"INSERT INTO sent_unread_items VALUES (1)"});
    }
    pg::conn c(p);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM sent_unread_items"}) == 1);
}

TEST_CASE("a pending result is read once") {
    pg::pool p(options(1));
    pg::conn c(p);
    auto q = c.send<std::int32_t>(sql{"SELECT 1"});
    REQUIRE(q.one() == 1);
    REQUIRE_THROWS_AS(q.one(), pg::error);
}

TEST_CASE("a sent statement's error is thrown by its reader") {
    pg::pool p(options(1));
    pg::conn c(p);
    REQUIRE_THROWS_AS(c.send<std::int32_t>(sql{"SELECT 1/0"}).one(), pg::error);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
}

TEST_CASE("a burst of borrowers does not lock the pool into a line") {
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
    REQUIRE(s.leased == 0);
    REQUIRE(s.open <= 2);
}
