#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include "pg/pg.hpp"

using namespace std::chrono;

// Hand-written tags in the shape pg_tablegen generates.
namespace db::sql_cells {
struct table_ {
    static constexpr std::string_view name = "sql_cells";
    using key = std::tuple<std::int32_t, std::string>;
    static constexpr std::array<std::string_view, 2> key_columns{"a", "b"};
};
struct n {
    using table = table_;
    using type = std::int64_t;
    static constexpr std::string_view name = "n";
};
struct small {
    using table = table_;
    using type = std::int16_t;
    static constexpr std::string_view name = "small";
};
struct ratio {
    using table = table_;
    using type = double;
    static constexpr std::string_view name = "ratio";
};
struct label {
    using table = table_;
    using type = std::optional<std::string>;
    static constexpr std::string_view name = "label";
};
struct price {
    using table = table_;
    using type = pg::numeric;
    static constexpr std::string_view name = "price";
};
struct doc {
    using table = table_;
    using type = pg::jsonb;
    static constexpr std::string_view name = "doc";
};
struct at {
    using table = table_;
    using type = pg::timestamptz;
    static constexpr std::string_view name = "at";
};
struct flag {
    using table = table_;
    using type = bool;
    static constexpr std::string_view name = "flag";
};
struct by_code_t {
    using table = table_;
    using key = std::tuple<std::string>;
    static constexpr std::array<std::string_view, 1> key_columns{"code"};
};
inline constexpr by_code_t by_code{};
}  // namespace db::sql_cells

namespace db::sql_single {
struct table_ {
    static constexpr std::string_view name = "sql_single";
    using key = std::tuple<std::int32_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct hits {
    using table = table_;
    using type = std::int64_t;
    static constexpr std::string_view name = "hits";
};
}  // namespace db::sql_single

namespace {

namespace cells = db::sql_cells;

std::string dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    return d;
}

// Recreates the tables with row (7, 'k') coded 'seven' and row (8, 'k') coded 'eight', plus single row 1.
void reset() {
    pg::connect(dsn(), 8);
    pg::conn c;
    c.exec("DROP TABLE IF EXISTS sql_cells");
    c.exec("DROP TABLE IF EXISTS sql_single");
    c.exec(R"sql(CREATE TABLE sql_cells (
        a int NOT NULL, b text NOT NULL, code text NOT NULL UNIQUE,
        n bigint NOT NULL DEFAULT 0, small smallint NOT NULL DEFAULT 0, ratio float8 NOT NULL DEFAULT 0,
        label text, price numeric NOT NULL DEFAULT 0, doc jsonb NOT NULL DEFAULT '{}',
        at timestamptz NOT NULL DEFAULT 'epoch', flag boolean NOT NULL DEFAULT false,
        PRIMARY KEY (a, b)
    ))sql");
    c.exec("CREATE TABLE sql_single (id int PRIMARY KEY, hits bigint NOT NULL DEFAULT 0)");
    c.exec("INSERT INTO sql_cells (a, b, code, n) VALUES (7, 'k', 'seven', 70), (8, 'k', 'eight', 80)");
    c.exec("INSERT INTO sql_single (id) VALUES (1)");
}

}  // namespace

template <class S>
concept has_arithmetic = requires(S s, typename S::value_type v) {
    s += v;
    s -= v;
    ++s;
    --s;
};

static_assert(has_arithmetic<pg::sql<cells::n>>);
static_assert(has_arithmetic<pg::sql<cells::ratio>>);
static_assert(!has_arithmetic<pg::sql<cells::label>>);
static_assert(!has_arithmetic<pg::sql<cells::price>>);

TEST_CASE("get reads a cell by composite primary key") {
    reset();
    pg::sql<cells::n> x{7, "k"};
    REQUIRE(x.get() == 70);
    std::int64_t as_value = pg::sql<cells::n>{8, "k"};
    REQUIRE(as_value == 80);
}

TEST_CASE("a unique locator addresses the same cell") {
    reset();
    pg::sql<cells::n> by_pk{8, "k"};
    pg::sql<cells::n> by_code{cells::by_code, "eight"};
    REQUIRE(by_code.get() == 80);
    by_code = 81;
    REQUIRE(by_pk.get() == 81);
}

TEST_CASE("a single-column primary key takes one argument") {
    reset();
    pg::sql<db::sql_single::hits> h{1};
    h += 3;
    REQUIRE(h.get() == 3);
}

TEST_CASE("assignment writes every column type") {
    reset();
    const pg::timestamptz ts = sys_days{year{2024} / March / 5} + hours{7} + microseconds{120000};
    pg::sql<cells::small>{7, "k"} = std::int16_t{-12};
    pg::sql<cells::ratio>{7, "k"} = 0.1;
    pg::sql<cells::price>{7, "k"} = pg::numeric{"12.3400"};
    pg::sql<cells::doc>{7, "k"} = pg::jsonb{"{\"a\": [1, 2]}"};
    pg::sql<cells::at>{7, "k"} = ts;
    pg::sql<cells::flag>{7, "k"} = true;
    REQUIRE(pg::sql<cells::small>{7, "k"}.get() == -12);
    REQUIRE(pg::sql<cells::ratio>{7, "k"}.get() == 0.1);
    REQUIRE(pg::sql<cells::price>{7, "k"}.get() == pg::numeric{"12.3400"});
    REQUIRE(pg::sql<cells::doc>{7, "k"}.get() == pg::jsonb{"{\"a\": [1, 2]}"});
    REQUIRE(pg::sql<cells::at>{7, "k"}.get() == ts);
    REQUIRE(pg::sql<cells::flag>{7, "k"}.get() == true);
}

TEST_CASE("a nullable column reads NULL as nullopt and writes it back") {
    reset();
    pg::sql<cells::label> label{7, "k"};
    REQUIRE(label.get() == std::nullopt);
    label = std::optional<std::string>{"hello"};
    REQUIRE(label.get() == std::optional<std::string>{"hello"});
    label = std::nullopt;
    REQUIRE(label.get() == std::nullopt);
}

TEST_CASE("identifiers and values with quotes are safe") {
    reset();
    pg::sql<cells::label> label{cells::by_code, "seven"};
    label = std::optional<std::string>{"it's \"quoted\""};
    REQUIRE(label.get() == std::optional<std::string>{"it's \"quoted\""});
}

TEST_CASE("arithmetic operators update in the database") {
    reset();
    pg::sql<cells::n> x{7, "k"};
    x += 5;
    REQUIRE(x.get() == 75);
    x -= 15;
    REQUIRE(x.get() == 60);
    ++x;
    REQUIRE(x.get() == 61);
    --x;
    --x;
    REQUIRE(x.get() == 59);
    pg::sql<cells::ratio> r{7, "k"};
    r += 0.5;
    r += 0.25;
    REQUIRE(r.get() == 0.75);
    pg::sql<cells::small> s{7, "k"};
    s += std::int16_t{300};
    REQUIRE(s.get() == 300);
}

TEST_CASE("a cell assigns by value from another cell") {
    reset();
    pg::sql<cells::n> a{7, "k"};
    pg::sql<cells::n> b{8, "k"};
    a = b;
    REQUIRE(pg::sql<cells::n>{7, "k"}.get() == 80);
    REQUIRE(pg::sql<cells::n>{8, "k"}.get() == 80);
}

TEST_CASE("a missing row is not_found for every operation") {
    reset();
    pg::sql<cells::n> x{99, "none"};
    REQUIRE_THROWS_AS(x.get(), pg::not_found);
    REQUIRE_THROWS_AS(x = 1, pg::not_found);
    REQUIRE_THROWS_AS(x += 1, pg::not_found);
    REQUIRE_THROWS_AS(x -= 1, pg::not_found);
    REQUIRE_THROWS_AS(++x, pg::not_found);
    pg::sql<cells::n> by_code{cells::by_code, "none"};
    REQUIRE_THROWS_AS(by_code.get(), pg::not_found);
}

TEST_CASE("a failed update leaves the other rows alone") {
    reset();
    pg::sql<cells::n> x{99, "none"};
    REQUIRE_THROWS_AS(x = 1, pg::not_found);
    REQUIRE(pg::sql<cells::n>{7, "k"}.get() == 70);
    REQUIRE(pg::sql<cells::n>{8, "k"}.get() == 80);
}

TEST_CASE("an out-of-range increment is a pg::error with SQLSTATE 22003") {
    reset();
    pg::sql<cells::small> s{7, "k"};
    try {
        s += std::int16_t{32767};
        s += std::int16_t{1};
        FAIL("smallint overflow was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.sqlstate == "22003");
    }
}

TEST_CASE("8 threads adding 1000 times each reach exactly 8000") {
    reset();
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&] {
            try {
                pg::sql<cells::n> x{cells::by_code, "seven"};
                for (int i = 0; i < 1000; ++i) x += 1;
            } catch (...) {
                ++failures;
            }
        });
    for (auto& t : threads) t.join();
    REQUIRE(failures == 0);
    REQUIRE(pg::sql<cells::n>{7, "k"}.get() == 70 + 8000);
}
