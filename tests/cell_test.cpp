#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

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

// Tags as pg_tablegen writes them, for
//   cell_items(a int, b int, qty int, price numeric(18,6), note text, code text unique, PRIMARY KEY (a, b)).
struct cell_items {
    static constexpr std::string_view name = "cell_items";
    using key = std::tuple<std::int32_t, std::int32_t>;
    static constexpr std::array<std::string_view, 2> key_columns{"a", "b"};
};
struct by_code {
    using table = cell_items;
    using key = std::tuple<std::string>;
    static constexpr std::array<std::string_view, 1> key_columns{"code"};
};
struct qty_col {
    using table = cell_items;
    using type = std::int32_t;
    static constexpr std::string_view name = "qty";
};
struct price_col {
    using table = cell_items;
    using type = pg::decimal<6>;
    static constexpr std::string_view name = "price";
};
struct note_col {
    using table = cell_items;
    using type = std::optional<std::string>;
    static constexpr std::string_view name = "note";
};

void reset(pg::conn& c) {
    c.exec(sql{"DROP TABLE IF EXISTS cell_items"});
    c.exec(sql{"CREATE TABLE cell_items (a int, b int, qty int NOT NULL, price numeric(18,6) NOT NULL, "
               "note text, code text UNIQUE, PRIMARY KEY (a, b))"});
    c.exec(sql{"INSERT INTO cell_items VALUES (1, 2, 10, 1.5, 'hi', 'x1')"});
}

}  // namespace

TEST_CASE("cell reads and writes an int", "[cell]") {
    pg::conn c(db());
    reset(c);
    pg::cell<qty_col> q(c, 1, 2);
    CHECK(q.get() == 10);
    q = 25;
    CHECK(static_cast<std::int32_t>(q) == 25);
    q += 5;
    CHECK(q.get() == 30);
    q -= 10;
    CHECK(q.get() == 20);
    ++q;
    --q;
    --q;
    CHECK(q.get() == 19);
}

TEST_CASE("cell on a decimal", "[cell]") {
    pg::conn c(db());
    reset(c);
    pg::cell<price_col> p(c, 1, 2);
    CHECK(p.get() == pg::decimal<6>::parse("1.5"));
    p = pg::decimal<6>::parse("2.25");
    p += pg::decimal<6>::parse("0.000001");
    CHECK(p.get().to_string() == "2.250001");
    p -= pg::decimal<6>::parse("1");
    CHECK(p.get().to_string() == "1.250001");
    ++p;
    CHECK(p.get().to_string() == "2.250001");
}

TEST_CASE("cell located by a unique key", "[cell]") {
    pg::conn c(db());
    reset(c);
    pg::cell<qty_col> q(c, by_code{}, std::string("x1"));
    CHECK(q.get() == 10);
    q += 1;
    CHECK(pg::cell<qty_col>(c, 1, 2).get() == 11);
}

TEST_CASE("cell copies a value on cell assignment", "[cell]") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO cell_items VALUES (3, 4, 77, 1, NULL, 'x2')"});
    pg::cell<qty_col> from(c, 3, 4), to(c, 1, 2);
    to = from;
    CHECK(to.get() == 77);
}

TEST_CASE("cell on a missing row throws not_found", "[cell]") {
    pg::conn c(db());
    reset(c);
    pg::cell<qty_col> q(c, 9, 9);
    CHECK_THROWS_AS(q.get(), pg::not_found);
    CHECK_THROWS_AS(q = 1, pg::not_found);
    CHECK_THROWS_AS(q += 1, pg::not_found);
    CHECK(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM cell_items"}) == 1);
}

TEST_CASE("cell takes part in the connection's transaction", "[cell]") {
    pg::conn c(db());
    reset(c);
    pg::cell<qty_col> q(c, 1, 2);
    {
        pg::transaction tx(c);
        q += 100;
        CHECK(q.get() == 110);
    }
    CHECK(q.get() == 10);
    {
        pg::transaction tx(c);
        q = 5;
        tx.commit();
    }
    CHECK(q.get() == 5);
}

TEST_CASE("cell on an optional column writes NULL", "[cell]") {
    pg::conn c(db());
    reset(c);
    pg::cell<note_col> n(c, 1, 2);
    CHECK(n.get() == std::optional<std::string>("hi"));
    n = std::nullopt;
    CHECK(!n.get().has_value());
    n = std::optional<std::string>("back");
    CHECK(n.get() == std::optional<std::string>("back"));
}
