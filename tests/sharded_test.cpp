#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pg/pg.hpp"

namespace {

std::string dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    return d;
}

}  // namespace

TEST_CASE("shard_of is the same in every process and build") {
    // Pinned values: a change here moves every stored key to another shard.
    CHECK(pg::shard_of(16, std::int64_t{1}) == 2);
    CHECK(pg::shard_of(16, std::int64_t{2}) == 2);
    CHECK(pg::shard_of(16, std::int64_t{3}) == 11);
    CHECK(pg::shard_of(16, std::int64_t{42}) == 15);
    CHECK(pg::shard_of(16, std::int64_t{-7}) == 10);
    CHECK(pg::shard_of(16, std::string("alice")) == 12);
    CHECK(pg::shard_of(1000, std::int64_t{123456789}) == 481);
}

TEST_CASE("integer widths and text spellings of a key agree") {
    for (int k = -100; k < 100; ++k) {
        CHECK(pg::shard_of(7, k) == pg::shard_of(7, static_cast<std::int64_t>(k)));
        CHECK(pg::shard_of(7, static_cast<short>(k)) == pg::shard_of(7, static_cast<long long>(k)));
    }
    CHECK(pg::shard_of(16, "alice") == pg::shard_of(16, std::string_view("alice")));
}

TEST_CASE("composite keys keep their component boundaries") {
    CHECK(pg::shard_of(16, "ab", "c") == 6);
    CHECK(pg::shard_of(16, "a", "bc") == 4);
}

TEST_CASE("keys spread evenly over the shards") {
    std::vector<int> count(8);
    for (std::int64_t k = 0; k < 80'000; ++k) ++count[pg::shard_of(8, k)];
    for (int c : count) {
        CHECK(c > 9'000);
        CHECK(c < 11'000);
    }
}

TEST_CASE("adding a shard moves keys only onto the new shard") {
    int moved = 0;
    for (std::int64_t k = 0; k < 10'000; ++k) {
        const std::size_t before = pg::shard_of(4, k), after = pg::shard_of(5, k);
        if (before != after) {
            CHECK(after == 4);
            ++moved;
        }
    }
    CHECK(moved > 1'600);  // about a fifth
    CHECK(moved < 2'400);
}

TEST_CASE("sharded pools and writebacks route a key to the same index") {
    pg::pool_options o;
    o.dsn = dsn();
    o.max_conns = 2;
    pg::sharded<pg::pool> pools(std::vector<pg::pool_options>(3, o));
    pg::sharded<pg::writeback> wbs(pools.size(), [&](std::size_t i) { return std::make_unique<pg::writeback>(pools[i]); });
    REQUIRE(pools.size() == 3);
    for (std::int64_t id = 0; id < 50; ++id) {
        const std::size_t i = pg::shard_of(3, id);
        CHECK(&pools.of(id) == &pools[i]);
        CHECK(&wbs.of(id) == &wbs[i]);
    }
    pg::conn c(pools.of(std::int64_t{7}));
    CHECK(c.query_one<int>(pg::sql{"SELECT 1"}) == 1);
}

TEST_CASE("sharded needs at least one shard") {
    CHECK_THROWS_AS(pg::sharded<pg::pool>(std::vector<pg::pool_options>{}), pg::error);
}

TEST_CASE("ready is true for a reachable server and false for an unreachable one") {
    pg::pool_options o;
    o.dsn = dsn();
    pg::pool good(o);
    CHECK(good.ready());
    CHECK(good.stats().open >= 1);
    CHECK(good.ready());

    pg::pool_options bad;
    bad.dsn = "postgresql://nobody@127.0.0.1:1/none?connect_timeout=2";
    bad.acquire_timeout = std::chrono::milliseconds{2'000};
    pg::pool down(bad);
    CHECK_FALSE(down.ready());
}
