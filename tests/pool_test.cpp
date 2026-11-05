#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

// Runs fn(i) on `n` threads and joins them.
template <class F>
void on_threads(int n, F fn) {
    std::vector<std::thread> ts;
    for (int i = 0; i < n; ++i) ts.emplace_back([&fn, i] { fn(i); });
    for (auto& t : ts) t.join();
}

}  // namespace

TEST_CASE("many threads share a small pool") {
    pg::connect(dsn(), 4);
    constexpr int threads = 32, rounds = 100;
    std::atomic<int> done{0};
    std::atomic<int> wrong{0};
    on_threads(threads, [&](int i) {
        for (int k = 0; k < rounds; ++k) {
            pg::conn c;
            if (c.query_one<std::int32_t>("SELECT $1::int + 1", i * rounds + k) != i * rounds + k + 1) ++wrong;
            ++done;
        }
    });
    REQUIRE(done == threads * rounds);
    REQUIRE(wrong == 0);
}

TEST_CASE("waiters are served in turn, none starves") {
    pg::connect(dsn(), 4);
    constexpr int threads = 32;
    std::vector<std::int64_t> ops(threads, 0);
    std::atomic<bool> stop{false};
    std::thread timer([&] {
        std::this_thread::sleep_for(1500ms);
        stop = true;
    });
    on_threads(threads, [&](int i) {
        while (!stop) {
            pg::conn c;
            c.query_one<std::int32_t>("SELECT 1");
            ++ops[static_cast<std::size_t>(i)];
        }
    });
    timer.join();
    const auto [lo, hi] = std::minmax_element(ops.begin(), ops.end());
    INFO("min " << *lo << " max " << *hi);
    REQUIRE(*lo > 0);
    REQUIRE(*lo * 4 >= *hi);
}

TEST_CASE("a failed connect passes its slot on: every waiter gets an answer") {
    pg::connect("postgresql://postgres@127.0.0.1:1/none", 1);
    std::atomic<int> failed{0};
    on_threads(8, [&](int) {
        try {
            pg::conn c;
        } catch (const pg::error&) {
            ++failed;
        }
    });
    REQUIRE(failed == 8);
    pg::connect(dsn(), 1);
    pg::conn c;
    REQUIRE(c.query_one<std::int32_t>("SELECT 1") == 1);
}

TEST_CASE("connect releases waiters of the old generation") {
    pg::connect(dsn(), 1);
    std::atomic<int> done{0};
    std::vector<std::thread> ts;
    {
        pg::conn held;
        for (int i = 0; i < 4; ++i)
            ts.emplace_back([&] {
                pg::conn c;
                c.query_one<std::int32_t>("SELECT 1");
                ++done;
            });
        std::this_thread::sleep_for(100ms);
        pg::connect(dsn(), 1);  // the waiters now open connections of the new generation
        for (int i = 0; i < 100 && done < 4; ++i) std::this_thread::sleep_for(50ms);
        REQUIRE(done == 4);
    }
    for (auto& t : ts) t.join();
}
