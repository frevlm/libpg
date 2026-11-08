#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "pg/pg.hpp"

using namespace std::chrono;
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

// An empty table query_items(id int primary key, name text, score float8).
void reset(pg::conn& c) {
    c.exec(sql{"DROP TABLE IF EXISTS query_items"});
    c.exec(sql{"CREATE TABLE query_items (id int PRIMARY KEY, name text, score float8)"});
}

void insert_three(pg::conn& c) {
    c.exec(sql{"INSERT INTO query_items VALUES ({}, {}, {})", 1, "one", 1.5});
    c.exec(sql{"INSERT INTO query_items VALUES ({}, {}, {})", 2, std::string("two"), 2.5});
    c.exec(sql{"INSERT INTO query_items VALUES ({}, {}, {})", 3, std::string_view("three"), 3.5});
}

std::int64_t count_items(pg::conn& c) { return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM query_items"}); }

// A read shape without pg_names: it reads positionally.
struct pair_row {
    std::string first;
    std::string second;

    static constexpr std::size_t pg_columns = 2;
    template <class F>
    void pg_each(F&& f) {
        f(first);
        f(second);
    }
};

// A read shape over query_items.
struct item_row {
    std::int32_t id = 0;
    std::optional<std::string> name;
    std::optional<double> score;

    static constexpr std::size_t pg_columns = 3;
    template <class F>
    void pg_each(F&& f) {
        f(id);
        f(name);
        f(score);
    }
};

struct shape_items_ {
    static constexpr std::string_view name = "shape_items";
};

// A write shape over shape_items.
struct shape_write {
    using pg_table = shape_items_;
    static constexpr std::array<std::string_view, 4> pg_names{"id", "label", "note", "n"};
    std::int32_t id = 0;
    pg::field<std::string> label;
    std::optional<std::string> note;
    pg::field<std::optional<std::int32_t>> n;

    template <class F>
    void pg_each(F&& f) const {
        f(id);
        f(label);
        f(note);
        f(n);
    }
};

// The value the server sends back for `SELECT $1::<type>`.
template <class T>
T echo(pg::conn& c, const T& v, std::string_view type) {
    return c.query_one<T>(pg::sql{"SELECT {}::", v} + pg::unsafe_sql(std::string(type)));
}

struct insert_kinds_ {
    static constexpr std::string_view name = "insert_all_kinds";
};

struct kinds_write {
    using pg_table = insert_kinds_;
    static constexpr std::array<std::string_view, 17> pg_names{
        "i2", "i4", "i8", "f8", "num", "flag", "t", "v", "jb", "j", "ts", "d", "u", "b", "e", "maybe", "nt"};
    std::int16_t i2 = 0;
    std::int32_t i4 = 0;
    std::int64_t i8 = 0;
    double f8 = 0;
    pg::decimal<6> num{};
    bool flag = false;
    std::string t;
    std::string v;
    pg::jsonb jb;
    pg::jsonb j;
    pg::timestamptz ts{};
    pg::date d{};
    pg::uuid u{};
    pg::bytea b;
    std::string e;
    std::optional<std::int32_t> maybe;
    std::optional<std::string> nt;

    template <class F>
    void pg_each(F&& f) const {
        f(i2);
        f(i4);
        f(i8);
        f(f8);
        f(num);
        f(flag);
        f(t);
        f(v);
        f(jb);
        f(j);
        f(ts);
        f(d);
        f(u);
        f(b);
        f(e);
        f(maybe);
        f(nt);
    }
};

struct insert_inet_ {
    static constexpr std::string_view name = "insert_all_inet";
};

struct inet_write {
    using pg_table = insert_inet_;
    static constexpr std::array<std::string_view, 2> pg_names{"id", "addr"};
    std::int32_t id = 0;
    std::string addr;

    template <class F>
    void pg_each(F&& f) const {
        f(id);
        f(addr);
    }
};

struct insert_def_ {
    static constexpr std::string_view name = "insert_all_def";
};

struct def_write {
    using pg_table = insert_def_;
    static constexpr std::array<std::string_view, 3> pg_names{"id", "label", "n"};
    std::int32_t id = 0;
    pg::field<std::string> label;
    pg::field<std::optional<std::int32_t>> n;

    template <class F>
    void pg_each(F&& f) const {
        f(id);
        f(label);
        f(n);
    }
};

struct insert_atomic_ {
    static constexpr std::string_view name = "insert_all_atomic";
};

struct atomic_write {
    using pg_table = insert_atomic_;
    static constexpr std::array<std::string_view, 1> pg_names{"id"};
    std::int32_t id = 0;

    template <class F>
    void pg_each(F&& f) const { f(id); }
};

}  // namespace

static_assert(pg::read_shape<item_row>);
static_assert(pg::write_shape<shape_write>);
static_assert(pg::write_shape<kinds_write>);
static_assert(pg::write_shape<inet_write>);
static_assert(pg::write_shape<def_write>);
static_assert(pg::write_shape<atomic_write>);

TEST_CASE("exec returns the number of affected rows") {
    pg::conn c(db());
    reset(c);
    REQUIRE(c.exec(sql{"INSERT INTO query_items VALUES (1, 'a', 0), (2, 'b', 0), (3, 'c', 0)"}) == 3);
    REQUIRE(c.exec(sql{"UPDATE query_items SET score = {} WHERE id >= {}", 4.0, 2}) == 2);
    REQUIRE(c.exec(sql{"DELETE FROM query_items WHERE id = {}", 99}) == 0);
}

TEST_CASE("query decodes rows as tuples, one type as itself") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    auto rows = c.query<std::int32_t, std::string, double>(sql{"SELECT id, name, score FROM query_items ORDER BY id"});
    static_assert(std::is_same_v<decltype(rows), std::vector<std::tuple<std::int32_t, std::string, double>>>);
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0] == std::make_tuple(1, std::string("one"), 1.5));
    REQUIRE(rows[2] == std::make_tuple(3, std::string("three"), 3.5));
    auto ids = c.query<std::int32_t>(sql{"SELECT id FROM query_items WHERE id > {} ORDER BY id", 1});
    REQUIRE(ids == std::vector<std::int32_t>{2, 3});
}

TEST_CASE("query_one wants exactly one row") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    REQUIRE(c.query_one<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 2}) == "two");
    REQUIRE(c.query_one<std::int32_t, std::string>(sql{"SELECT id, name FROM query_items WHERE id = 3"}) ==
            std::make_tuple(3, std::string("three")));
    REQUIRE_THROWS_AS(c.query_one<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 9}), pg::not_found);
    try {
        c.query_one<std::int32_t>(sql{"SELECT id FROM query_items"});
        FAIL("three rows were accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::other);
        REQUIRE(e.sql == "SELECT id FROM query_items");
    }
}

TEST_CASE("query_opt returns zero or one row") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    REQUIRE(c.query_opt<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 2}) == std::optional<std::string>{"two"});
    REQUIRE(c.query_opt<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 9}) == std::nullopt);
    REQUIRE_THROWS_AS(c.query_opt<std::int32_t>(sql{"SELECT id FROM query_items"}), pg::error);
}

TEST_CASE("NULL maps to optional and is rejected elsewhere") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES ({}, {}, {})", 1, std::optional<std::string>{}, 1.0});
    REQUIRE(c.query_one<std::optional<std::string>>(sql{"SELECT name FROM query_items"}) == std::nullopt);
    REQUIRE_THROWS_AS(c.query<std::string>(sql{"SELECT name FROM query_items"}), pg::error);
    c.exec(sql{"UPDATE query_items SET name = {}", std::optional<std::string>{"x"}});
    REQUIRE(c.query<std::optional<std::string>>(sql{"SELECT name FROM query_items"}) ==
            std::vector<std::optional<std::string>>{"x"});
}

TEST_CASE("a column count mismatch throws") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    REQUIRE_THROWS_AS((c.query<std::int32_t, std::string>(sql{"SELECT id FROM query_items"})), pg::error);
    REQUIRE_THROWS_AS(c.query<std::int32_t>(sql{"SELECT id, name FROM query_items"}), pg::error);
}

TEST_CASE("a column type mismatch throws before the statement executes") {
    pg::conn c(db());
    reset(c);
    const sql insert{"INSERT INTO query_items VALUES (7, 'x', 0) RETURNING name"};
    for (int round = 0; round < 2; ++round) {  // a fresh and a cached statement
        try {
            c.query<std::int32_t>(insert);
            FAIL("text was read as int32");
        } catch (const pg::error& e) {
            REQUIRE(e.kind == pg::error_kind::other);
            REQUIRE(e.sqlstate.empty());
            REQUIRE(e.sql == insert.render().text);
        }
        REQUIRE(count_items(c) == 0);
    }
    REQUIRE(c.query<std::string>(insert) == std::vector<std::string>{"x"});
}

TEST_CASE("a type without a codec is rejected; a ::text cast reads it") {
    pg::conn c(db());
    REQUIRE_THROWS_AS(c.query<std::string>(sql{"SELECT '10.0.0.1'::inet"}), pg::error);
    REQUIRE(c.query_one<std::string>(sql{"SELECT '10.0.0.1'::inet::text"}) == "10.0.0.1/32");
}

TEST_CASE("server errors carry kind and SQLSTATE; the SQL text is apart") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES (1, 'a', 0)"});
    const sql dup{"INSERT INTO query_items VALUES (1, 'secret', 0)"};
    try {
        c.exec(dup);
        FAIL("duplicate key was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.sqlstate == "23505");
        REQUIRE(e.kind == pg::error_kind::data);
        REQUIRE(e.sql == dup.render().text);
        REQUIRE(std::string(e.what()).starts_with("23505: "));
        REQUIRE(std::string(e.what()).find("secret") == std::string::npos);
    }
    try {
        c.exec(sql{"SELEKT secret"});
        FAIL("syntax error was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.sqlstate == "42601");
        REQUIRE(e.kind == pg::error_kind::other);
        REQUIRE(std::string(e.what()).find("secret") == std::string::npos);
        REQUIRE(e.sql == "SELEKT secret");
    }
}

TEST_CASE("the connection stays usable after an error") {
    pg::conn c(db());
    REQUIRE_THROWS_AS(c.exec(sql{"SELEKT"}), pg::error);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 5"}) == 5);
}

TEST_CASE("a read shape is decoded column by column") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    c.exec(sql{"INSERT INTO query_items VALUES (4, NULL, NULL)"});
    const auto rows = c.query<item_row>(sql{"SELECT * FROM query_items ORDER BY id"});
    REQUIRE(rows.size() == 4);
    REQUIRE(rows[1].id == 2);
    REQUIRE(rows[1].name == "two");
    REQUIRE(rows[1].score == 2.5);
    REQUIRE(rows[3].name == std::nullopt);
    const item_row one = c.query_one<item_row>(sql{"SELECT * FROM query_items WHERE id = {}", 3});
    REQUIRE(one.name == "three");
    REQUIRE_THROWS_AS(c.query<item_row>(sql{"SELECT id, name FROM query_items"}), pg::error);
    REQUIRE_THROWS_AS(c.query<item_row>(sql{"SELECT name, id, score FROM query_items"}), pg::error);
}

TEST_CASE("a read shape without pg_names reads positionally") {
    pg::conn c(db());
    const auto r = c.query_one<pair_row>(sql{"SELECT 'x' AS alpha, 'y' AS beta"});
    REQUIRE(r.first == "x");
    REQUIRE(r.second == "y");
}

TEST_CASE("insert writes DEFAULT for unset fields and NULL for empty optionals") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS shape_items"});
    c.exec(sql{"CREATE TABLE shape_items (id int PRIMARY KEY, label text NOT NULL DEFAULT 'none', note text, n int DEFAULT 7)"});
    shape_write a;
    a.id = 1;
    REQUIRE(pg::insert(c, a) == 1);
    shape_write b;
    b.id = 2;
    b.label = "set";
    b.note = "noted";
    b.n = std::optional<std::int32_t>{};
    REQUIRE(pg::insert(c, b) == 1);
    using row = std::tuple<std::int32_t, std::string, std::optional<std::string>, std::optional<std::int32_t>>;
    const auto rows = c.query<std::int32_t, std::string, std::optional<std::string>, std::optional<std::int32_t>>(
        sql{"SELECT id, label, note, n FROM shape_items ORDER BY id"});
    REQUIRE(rows == std::vector<row>{{1, "none", std::nullopt, 7}, {2, "set", "noted", std::nullopt}});
}

TEST_CASE("insert_all copies rows of every column type") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_kinds"});
    c.exec(sql{"DROP TYPE IF EXISTS insert_mood"});
    c.exec(sql{"CREATE TYPE insert_mood AS ENUM ('red', 'green')"});
    c.exec(sql{
        "CREATE TABLE insert_all_kinds (i2 smallint, i4 int, i8 bigint, f8 float8, num numeric(18,6), flag boolean, "
        "t text, v varchar, jb jsonb, j json, ts timestamptz, d date, u uuid, b bytea, e insert_mood, maybe int, nt text)"});

    std::vector<kinds_write> rows(3);
    rows[0].i2 = -7;
    rows[0].i4 = 1;
    rows[0].i8 = std::int64_t{1} << 40;
    rows[0].f8 = 1.5;
    rows[0].num = pg::decimal<6>::parse("123.456789");
    rows[0].flag = true;
    rows[0].t = "héllo";
    rows[0].v = "v";
    rows[0].jb = pg::jsonb{"{\"a\": [1, 2]}"};
    rows[0].j = pg::jsonb{"{\"k\":2}"};
    rows[0].ts = sys_days{year{2024} / March / 5} + hours{7} + microseconds{120000};
    rows[0].d = sys_days{year{2024} / March / 5};
    rows[0].u = pg::uuid::parse("a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
    rows[0].b = pg::bytea{std::byte{0}, std::byte{0xff}, std::byte{'a'}};
    rows[0].e = "green";
    rows[0].maybe = 5;
    rows[0].nt = "note";

    rows[1].i2 = 32767;
    rows[1].i4 = 2;
    rows[1].i8 = -9'000'000'000'000;
    rows[1].f8 = -0.25;
    rows[1].num = pg::decimal<6>::parse("-0.000001");
    rows[1].flag = false;
    rows[1].t = "two";
    rows[1].v = "vv";
    rows[1].jb = pg::jsonb{"null"};
    rows[1].j = pg::jsonb{"[]"};
    rows[1].ts = sys_days{year{1969} / December / 31} + microseconds{123450};
    rows[1].d = sys_days{year{1969} / December / 31};
    rows[1].u = pg::uuid::parse("00000000-0000-0000-0000-000000000123");
    rows[1].b = pg::bytea{};
    rows[1].e = "red";
    rows[1].maybe = std::nullopt;
    rows[1].nt = std::nullopt;

    rows[2].i2 = 0;
    rows[2].i4 = 3;
    rows[2].i8 = 0;
    rows[2].f8 = 0;
    rows[2].num = pg::decimal<6>::parse("0");
    rows[2].flag = true;
    rows[2].t = "";
    rows[2].v = "";
    rows[2].jb = pg::jsonb{"{\"z\": 9}"};
    rows[2].j = pg::jsonb{"{\"y\": true}"};
    rows[2].ts = sys_days{year{2000} / January / 1};
    rows[2].d = sys_days{year{2000} / January / 1};
    rows[2].u = pg::uuid::parse("ffffffff-ffff-ffff-ffff-ffffffffffff");
    rows[2].b = pg::bytea{std::byte{'z'}, std::byte{0}};
    rows[2].e = "green";
    rows[2].maybe = -1;
    rows[2].nt = "";

    REQUIRE(pg::insert_all(c, rows) == 3);
    using kinds_row =
        std::tuple<std::int16_t, std::int32_t, std::int64_t, double, pg::decimal<6>, bool, std::string, std::string,
                   pg::jsonb, pg::jsonb, pg::timestamptz, pg::date, pg::uuid, pg::bytea, std::string,
                   std::optional<std::int32_t>, std::optional<std::string>>;
    const auto got = c.query<std::int16_t, std::int32_t, std::int64_t, double, pg::decimal<6>, bool, std::string,
                              std::string, pg::jsonb, pg::jsonb, pg::timestamptz, pg::date, pg::uuid, pg::bytea,
                              std::string, std::optional<std::int32_t>, std::optional<std::string>>(
        sql{"SELECT i2, i4, i8, f8, num, flag, t, v, jb, j, ts, d, u, b, e, maybe, nt FROM insert_all_kinds "
            "ORDER BY i4"});
    const std::vector<kinds_row> want{
        {rows[0].i2, rows[0].i4, rows[0].i8, rows[0].f8, rows[0].num, rows[0].flag, rows[0].t, rows[0].v,
         rows[0].jb, rows[0].j, rows[0].ts, rows[0].d, rows[0].u, rows[0].b, rows[0].e, rows[0].maybe, rows[0].nt},
        {rows[1].i2, rows[1].i4, rows[1].i8, rows[1].f8, rows[1].num, rows[1].flag, rows[1].t, rows[1].v,
         rows[1].jb, rows[1].j, rows[1].ts, rows[1].d, rows[1].u, rows[1].b, rows[1].e, rows[1].maybe, rows[1].nt},
        {rows[2].i2, rows[2].i4, rows[2].i8, rows[2].f8, rows[2].num, rows[2].flag, rows[2].t, rows[2].v,
         rows[2].jb, rows[2].j, rows[2].ts, rows[2].d, rows[2].u, rows[2].b, rows[2].e, rows[2].maybe, rows[2].nt},
    };
    REQUIRE(got == want);
}

TEST_CASE("insert_all falls back to INSERTs for a column it cannot copy") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_inet"});
    c.exec(sql{"CREATE TABLE insert_all_inet (id int PRIMARY KEY, addr inet)"});
    std::vector<inet_write> rows(2);
    rows[0].id = 1;
    rows[0].addr = "10.0.0.1";
    rows[1].id = 2;
    rows[1].addr = "192.168.1.2";
    REQUIRE(pg::insert_all(c, rows) == 2);
    const auto addrs = c.query<std::string>(sql{"SELECT host(addr) FROM insert_all_inet ORDER BY id"});
    REQUIRE(addrs == std::vector<std::string>{"10.0.0.1", "192.168.1.2"});
}

TEST_CASE("insert_all takes DEFAULT for a field unset in every row and INSERTs mixed rows") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_def"});
    c.exec(sql{
        "CREATE TABLE insert_all_def (id int PRIMARY KEY, label text NOT NULL DEFAULT 'none', n int DEFAULT 7)"});
    std::vector<def_write> defaults(2);
    defaults[0].id = 1;
    defaults[1].id = 2;
    REQUIRE(pg::insert_all(c, defaults) == 2);
    using row = std::tuple<std::int32_t, std::string, std::optional<std::int32_t>>;
    REQUIRE(c.query<std::int32_t, std::string, std::optional<std::int32_t>>(
                sql{"SELECT id, label, n FROM insert_all_def ORDER BY id"}) ==
            std::vector<row>{{1, "none", 7}, {2, "none", 7}});

    std::vector<def_write> mixed(2);
    mixed[0].id = 3;
    mixed[0].label = "set";
    mixed[1].id = 4;
    REQUIRE(pg::insert_all(c, mixed) == 2);
    REQUIRE(c.query<std::int32_t, std::string, std::optional<std::int32_t>>(
                sql{"SELECT id, label, n FROM insert_all_def WHERE id > 2 ORDER BY id"}) ==
            std::vector<row>{{3, "set", 7}, {4, "none", 7}});
}

TEST_CASE("insert_all is atomic") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_atomic"});
    c.exec(sql{"CREATE TABLE insert_all_atomic (id int PRIMARY KEY)"});
    std::vector<atomic_write> rows(3);
    rows[0].id = 1;
    rows[1].id = 2;
    rows[2].id = 1;
    try {
        pg::insert_all(c, rows);
        FAIL("a duplicate key was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::data);
        REQUIRE(e.sqlstate == "23505");
    }
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_atomic"}) == 0);
}

TEST_CASE("insert_all joins the caller's transaction") {
    pg::pool_options po;
    po.dsn = std::getenv("LIBPG_TEST_DSN");
    pg::pool other(po);
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_atomic"});
    c.exec(sql{"CREATE TABLE insert_all_atomic (id int PRIMARY KEY)"});
    std::vector<atomic_write> rows(3);
    rows[0].id = 1;
    rows[1].id = 2;
    rows[2].id = 3;
    {
        pg::transaction tx(c);
        REQUIRE(pg::insert_all(c, rows) == 3);
        pg::conn o(other);
        REQUIRE(o.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_atomic"}) == 0);
        tx.commit();
    }
    {
        pg::conn o(other);
        REQUIRE(o.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_atomic"}) == 3);
    }
    {
        pg::transaction tx(c);
        std::vector<atomic_write> more(1);
        more[0].id = 4;
        REQUIRE(pg::insert_all(c, more) == 1);
    }
    {
        pg::conn o(other);
        REQUIRE(o.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_atomic"}) == 3);
    }
}

TEST_CASE("insert_all of nothing does nothing") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_atomic"});
    c.exec(sql{"CREATE TABLE insert_all_atomic (id int PRIMARY KEY)"});
    REQUIRE(pg::insert_all(c, std::vector<atomic_write>{}) == 0);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_atomic"}) == 0);
}

TEST_CASE("insert_all inside a transaction rethrows a failed COPY instead of retrying") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_atomic"});
    c.exec(sql{"CREATE TABLE insert_all_atomic (id int PRIMARY KEY)"});
    std::vector<atomic_write> rows(2);
    rows[0].id = 1;
    rows[1].id = 1;
    {
        pg::transaction tx(c);
        try {
            pg::insert_all(c, rows);
            FAIL("a duplicate key was accepted");
        } catch (const pg::error& e) {
            REQUIRE(e.sqlstate == "23505");
        }
        // No fallback ran: the failed COPY left the caller's transaction aborted.
        REQUIRE_THROWS_AS(c.exec(sql{"SELECT 1"}), pg::error);
    }
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_atomic"}) == 0);
}

TEST_CASE("every codec type round-trips through the server in binary") {
    pg::conn c(db());
    REQUIRE(echo(c, true, "bool") == true);
    REQUIRE(echo(c, std::int16_t{-32768}, "int2") == -32768);
    REQUIRE(echo(c, std::int32_t{2147483647}, "int4") == 2147483647);
    REQUIRE(echo(c, std::int64_t{-9223372036854775807 - 1}, "int8") == -9223372036854775807 - 1);
    REQUIRE(echo(c, 0.1f, "float4") == 0.1f);
    REQUIRE(echo(c, 0.1, "float8") == 0.1);
    REQUIRE(std::isinf(echo(c, -HUGE_VAL, "float8")));
    REQUIRE(std::isnan(c.query_one<double>(sql{"SELECT 'NaN'::float8"})));
    REQUIRE(echo(c, std::string("héllo"), "text") == "héllo");
    REQUIRE(echo(c, std::string("v"), "varchar(4)") == "v");
    REQUIRE(echo(c, std::string("ab"), "char(3)") == "ab ");
}

TEST_CASE("both 64-bit signed integer types bind and decode as int8") {
    pg::conn c(db());
    const long long big = -9000000000000000000LL;
    REQUIRE(c.query_one<long long>(sql{"SELECT {}::int8", big}) == big);
    const long other = 1234567890123L;
    REQUIRE(c.query_one<long>(sql{"SELECT {}::int8", other}) == other);
    REQUIRE(c.query_one<std::optional<long long>>(sql{"SELECT {}::int8", std::optional<long long>{big}}) == big);
    REQUIRE(c.query_one<std::optional<long>>(sql{"SELECT NULL::int8"}) == std::nullopt);
}

TEST_CASE("a vector of a binary type binds as an array parameter") {
    pg::conn c(db());
    REQUIRE(c.query<std::int64_t>(sql{"SELECT x FROM unnest({}) AS x ORDER BY x",
                                      std::vector<std::int64_t>{3, 1, 2}}) ==
            std::vector<std::int64_t>{1, 2, 3});
    REQUIRE(c.query<long long>(sql{"SELECT x FROM unnest({}) AS x ORDER BY x",
                                   std::vector<long long>{3, 1, 2}}) == std::vector<long long>{1, 2, 3});
    REQUIRE(c.query<std::int32_t>(sql{"SELECT x FROM unnest({}) AS x ORDER BY x",
                                      std::vector<std::int32_t>{3, 1, 2}}) ==
            std::vector<std::int32_t>{1, 2, 3});
    const auto dec = c.query<pg::decimal<6>>(
        sql{"SELECT x FROM unnest({}) AS x ORDER BY x",
            std::vector<pg::decimal<6>>{pg::decimal<6>::from_units(2500000), pg::decimal<6>::from_units(-1000000),
                                        pg::decimal<6>::from_units(0)}});
    REQUIRE(dec == std::vector<pg::decimal<6>>{pg::decimal<6>::from_units(-1000000), pg::decimal<6>::from_units(0),
                                               pg::decimal<6>::from_units(2500000)});
    REQUIRE(c.query<bool>(sql{"SELECT x FROM unnest({}) AS x ORDER BY x",
                              std::vector<bool>{true, false, true}}) == std::vector<bool>{false, true, true});
}

TEST_CASE("an empty array parameter matches no rows") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS array_items"});
    c.exec(sql{"CREATE TABLE array_items (id int PRIMARY KEY)"});
    c.exec(sql{"INSERT INTO array_items VALUES (1), (2), (3)"});
    REQUIRE(c.query<std::int32_t>(sql{"SELECT id FROM array_items WHERE id = ANY({}) ORDER BY id",
                                      std::vector<std::int32_t>{1, 3}}) == std::vector<std::int32_t>{1, 3});
    REQUIRE(c.query<std::int32_t>(sql{"SELECT id FROM array_items WHERE id = ANY({})",
                                      std::vector<std::int32_t>{}}).empty());
}

TEST_CASE("string-like types read as std::string") {
    pg::conn c(db());
    c.exec(sql{"CREATE EXTENSION IF NOT EXISTS citext"});
    c.exec(sql{"DROP TABLE IF EXISTS string_kinds"});
    c.exec(sql{"DROP TYPE IF EXISTS mood"});
    c.exec(sql{"DROP DOMAIN IF EXISTS short_text"});
    c.exec(sql{"CREATE TYPE mood AS ENUM ('sad', 'happy')"});
    c.exec(sql{"CREATE DOMAIN short_text AS text CHECK (length(VALUE) < 10)"});
    c.exec(sql{"CREATE TABLE string_kinds (m mood, ci citext, d short_text)"});
    c.exec(sql{"INSERT INTO string_kinds VALUES ({}, {}, {})", "happy", "MiXed", "dom"});
    REQUIRE(c.query_one<std::string, std::string, std::string>(sql{"SELECT m, ci, d FROM string_kinds"}) ==
            std::make_tuple(std::string("happy"), std::string("MiXed"), std::string("dom")));
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM string_kinds WHERE ci = {}", "mixed"}) == 1);
    REQUIRE_THROWS_AS(c.query<std::int32_t>(sql{"SELECT m FROM string_kinds"}), pg::error);
}

TEST_CASE("numeric, decimal, json, uuid and bytea round-trip") {
    pg::conn c(db());
    const pg::numeric n = pg::numeric::parse("-12345678901234567890.0012300");
    REQUIRE(echo(c, n, "numeric") == n);
    REQUIRE(echo(c, pg::numeric::parse("NaN"), "numeric").kind == pg::numeric::special::nan);
    const auto d = pg::decimal<6>::parse("-123456789012.345678");
    REQUIRE(echo(c, d, "numeric(18,6)") == d);
    c.exec(sql{"DROP TABLE IF EXISTS money_items"});
    c.exec(sql{"CREATE TABLE money_items (amount numeric(18,6) NOT NULL)"});
    c.exec(sql{"INSERT INTO money_items VALUES ({})", pg::decimal<6>::parse("0.000001")});
    c.exec(sql{"UPDATE money_items SET amount = amount + {}", pg::decimal<6>::parse("2.5")});
    REQUIRE(c.query_one<pg::decimal<6>>(sql{"SELECT amount FROM money_items"}).to_string() == "2.500001");
    REQUIRE(echo(c, pg::jsonb{"{\"a\": [1, 2]}"}, "jsonb") == pg::jsonb{"{\"a\": [1, 2]}"});
    REQUIRE(echo(c, pg::jsonb{"{\"b\":1}"}, "json") == pg::jsonb{"{\"b\":1}"});
    const pg::uuid u = pg::uuid::parse("a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
    REQUIRE(echo(c, u, "uuid") == u);
    const pg::bytea bytes{std::byte{0}, std::byte{0xff}, std::byte{'a'}, std::byte{0}};
    REQUIRE(echo(c, bytes, "bytea") == bytes);
}

TEST_CASE("timestamptz and date round-trip, infinities included") {
    pg::conn c(db());
    const pg::timestamptz values[] = {
        sys_days{year{2024} / March / 5} + hours{7} + microseconds{120000},
        sys_days{year{1969} / December / 31} + microseconds{123450},
        sys_days{year{-43} / March / 15},
        pg::timestamptz::max(),
        pg::timestamptz::min(),
    };
    for (const auto& v : values) REQUIRE(echo(c, v, "timestamptz") == v);
    REQUIRE(c.query_one<pg::timestamptz>(sql{"SELECT 'infinity'::timestamptz"}) == pg::timestamptz::max());
    REQUIRE(c.query_one<pg::timestamptz>(sql{"SELECT '-infinity'::timestamptz"}) == pg::timestamptz::min());
    REQUIRE(c.query_one<pg::timestamptz>(sql{"SELECT '2024-03-05 07:08:09.25+02'::timestamptz"}) ==
            sys_days{year{2024} / March / 5} + hours{5} + minutes{8} + seconds{9} + microseconds{250000});
    const pg::date day = sys_days{year{2026} / November / 5};
    REQUIRE(echo(c, day, "date") == day);
    REQUIRE(c.query_one<pg::date>(sql{"SELECT 'infinity'::date"}) == pg::date::max());
}

TEST_CASE("a text parameter with NUL throws before anything is sent") {
    pg::conn c(db());
    reset(c);
    const std::string bad("a\0b", 3);
    REQUIRE_THROWS_AS(c.exec(sql{"INSERT INTO query_items VALUES (1, {}, 0)", bad}), pg::error);
    REQUIRE(count_items(c) == 0);
}

TEST_CASE("one statement text with different parameter types") {
    pg::conn c(db());
    REQUIRE(c.query_one<std::string>(sql{"SELECT ({} + 1)::text", std::int32_t{1}}) == "2");
    REQUIRE(c.query_one<std::string>(sql{"SELECT ({} + 1)::text", std::int64_t{1} << 40}) == "1099511627777");
}

TEST_CASE("a statement text can run many times on one connection") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES (1, 'a', 0)"});
    for (int i = 0; i < 500; ++i) {
        REQUIRE(c.exec(sql{"UPDATE query_items SET score = {} WHERE id = {}", static_cast<double>(i), 1}) == 1);
        REQUIRE(c.query_one<double>(sql{"SELECT score FROM query_items WHERE id = {}", 1}) == static_cast<double>(i));
    }
}

TEST_CASE("a stale statement after ALTER TABLE is retried once outside a transaction") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS query_shape"});
    c.exec(sql{"CREATE TABLE query_shape (a int)"});
    c.exec(sql{"INSERT INTO query_shape VALUES (7)"});
    REQUIRE(c.query<std::int32_t>(sql{"SELECT * FROM query_shape"}).size() == 1);
    c.exec(sql{"ALTER TABLE query_shape ADD COLUMN b text DEFAULT 'x'"});
    auto rows = c.query<std::int32_t, std::string>(sql{"SELECT * FROM query_shape"});
    REQUIRE(rows == std::vector<std::tuple<std::int32_t, std::string>>{{7, "x"}});
    c.exec(sql{"ALTER TABLE query_shape ALTER COLUMN a TYPE text"});
    REQUIRE(c.query<std::string, std::string>(sql{"SELECT * FROM query_shape"}).size() == 1);
}

TEST_CASE("a stale statement inside a transaction throws stale_statement") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS query_shape"});
    c.exec(sql{"CREATE TABLE query_shape (a int)"});
    REQUIRE(c.query<std::int32_t>(sql{"SELECT * FROM query_shape"}).empty());
    c.exec(sql{"ALTER TABLE query_shape ADD COLUMN b text"});
    {
        pg::transaction tx(c);
        c.exec(sql{"SELECT 1"});
        try {
            c.query<std::int32_t>(sql{"SELECT * FROM query_shape"});
            FAIL("expected pg::error");
        } catch (const pg::error& e) {
            REQUIRE(e.sqlstate == "0A000");
            REQUIRE(e.kind == pg::error_kind::stale_statement);
        }
    }
    REQUIRE((c.query<std::int32_t, std::optional<std::string>>(sql{"SELECT * FROM query_shape"}).empty()));
}

TEST_CASE("statements the server forgot are prepared again") {
    pg::pool own([] {
        pg::pool_options o;
        o.dsn = std::getenv("LIBPG_TEST_DSN");
        return o;
    }());
    pg::conn c(own);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT {}::int", 5}) == 5);
    c.exec(sql{"DEALLOCATE ALL"});
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT {}::int", 6}) == 6);
}

TEST_CASE("more distinct statements than the cache holds") {
    pg::conn c(db());
    const auto nth = [](int i) { return pg::unsafe_sql("SELECT " + std::to_string(i) + " + ") + sql{"{}::int", 1}; };
    for (int i = 0; i < 600; ++i) REQUIRE(c.query_one<std::int32_t>(nth(i)) == i + 1);
    for (int i = 0; i < 600; i += 50) REQUIRE(c.query_one<std::int32_t>(nth(i)) == i + 1);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pg_prepared_statements"}) <= 257);
}

namespace {

struct no_cols_table {
    static constexpr std::string_view name = "query_no_cols";
};

// A write shape without columns.
struct no_cols_write {
    using pg_table = no_cols_table;
    static constexpr std::array<std::string_view, 0> pg_names{};
    template <class F>
    void pg_each(F&&) const {}
};

pg::pool& short_deadline_db() {
    static pg::pool p([] {
        pg::pool_options o;
        o.dsn = std::getenv("LIBPG_TEST_DSN");
        o.deadline = 3s;
        return o;
    }());
    return p;
}

}  // namespace

TEST_CASE("COPY TO STDOUT is rejected promptly and the connection is not reused") {
    pg::pool& p = short_deadline_db();
    std::int32_t victim = 0;
    {
        pg::conn c(p);
        reset(c);
        victim = c.query_one<std::int32_t>(sql{"SELECT pg_backend_pid()"});
        const auto t0 = steady_clock::now();
        try {
            c.exec(pg::unsafe_sql("COPY query_items TO STDOUT"));
            FAIL("COPY was accepted");
        } catch (const pg::error& e) {
            REQUIRE(e.kind == pg::error_kind::other);
            REQUIRE(std::string(e.what()) == "COPY is not supported");
        }
        REQUIRE(steady_clock::now() - t0 < 1s);
    }
    pg::conn c(p);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT pg_backend_pid()"}) != victim);
}

TEST_CASE("COPY FROM STDIN is rejected too") {
    pg::conn c(short_deadline_db());
    reset(c);
    const auto t0 = steady_clock::now();
    try {
        c.exec(pg::unsafe_sql("COPY query_items FROM STDIN"));
        FAIL("COPY was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::other);
        REQUIRE(std::string(e.what()) == "COPY is not supported");
    }
    REQUIRE(steady_clock::now() - t0 < 1s);
}

TEST_CASE("a feature_not_supported error is kind other and runs a closure once") {
    REQUIRE(pg::classify("0A000") == pg::error_kind::other);
    int runs = 0;
    try {
        db().transact([&](pg::conn& c) {
            ++runs;
            return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pg_class FOR UPDATE"});
        });
        FAIL("FOR UPDATE with an aggregate was accepted");
    } catch (const pg::error& e) {
        REQUIRE(e.kind == pg::error_kind::other);
        REQUIRE(e.sqlstate == "0A000");
    }
    REQUIRE(runs == 1);
}

TEST_CASE("a statement text with NUL throws before anything is sent") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES (1, 'a', 0)"});
    const std::string bad("UPDATE query_items SET score = 100 WHERE id = 1\0 AND false", 56);
    REQUIRE_THROWS_AS(c.exec(pg::unsafe_sql(bad)), pg::error);
    REQUIRE(c.query_one<double>(sql{"SELECT score FROM query_items WHERE id = 1"}) == 0.0);
}

TEST_CASE("an empty statement succeeds with no affected rows") {
    pg::conn c(db());
    REQUIRE(c.exec(sql{}) == 0);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
}

TEST_CASE("a write shape without columns inserts DEFAULT VALUES") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS query_no_cols"});
    c.exec(sql{"CREATE TABLE query_no_cols (id int GENERATED ALWAYS AS IDENTITY)"});
    REQUIRE(pg::insert(c, no_cols_write{}) == 1);
    REQUIRE(pg::insert(c, no_cols_write{}) == 1);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM query_no_cols"}) == 2);
}
