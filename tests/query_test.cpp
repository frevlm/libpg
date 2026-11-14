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

pg::pool_options test_options(int max_conns = 8) {
    pg::pool_options o;
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    o.dsn = d;
    o.max_conns = max_conns;
    return o;
}

pg::pool& db() {
    static pg::pool p(test_options());
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

// The error a call throws; fails the test when it throws none.
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

// A read shape over query_items.
struct item_row {
    std::int32_t id = 0;
    std::optional<std::string> name;
    std::optional<double> score;

    static constexpr std::size_t pg_columns = 3;
    static constexpr std::array<std::string_view, 3> pg_names{"id", "name", "score"};
    template <class F>
    void pg_each(F&& f) {
        f(id);
        f(name);
        f(score);
    }
};

// Members and pg_each but no pg_names: not a read shape.
struct unnamed_row {
    std::int32_t id = 0;
    static constexpr std::size_t pg_columns = 1;
    template <class F>
    void pg_each(F&& f) {
        f(id);
    }
};

struct shape_items_ {
    static constexpr std::string_view name = "shape_items";
};

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

struct insert_kinds_ {
    static constexpr std::string_view name = "insert_all_kinds";
};

struct kinds_write {
    using pg_table = insert_kinds_;
    static constexpr std::array<std::string_view, 15> pg_names{"i2", "i4", "i8", "f8", "num", "flag", "t", "v",
                                                               "jb", "ts", "d", "u", "b", "maybe", "nt"};
    std::int16_t i2 = 0;
    std::int32_t i4 = 0;
    std::int64_t i8 = 0;
    double f8 = 0;
    pg::decimal<6> num{};
    bool flag = false;
    std::string t;
    std::string v;
    pg::jsonb jb;
    pg::timestamptz ts{};
    pg::date d{};
    pg::uuid u{};
    pg::bytea b;
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
        f(ts);
        f(d);
        f(u);
        f(b);
        f(maybe);
        f(nt);
    }
    auto tie() const { return std::tie(i2, i4, i8, f8, num, flag, t, v, jb, ts, d, u, b, maybe, nt); }
};

struct insert_def_ {
    static constexpr std::string_view name = "insert_all_def";
};

struct def_write {
    using pg_table = insert_def_;
    static constexpr std::array<std::string_view, 3> pg_names{"id", "label", "n"};
    pg::field<std::int32_t> id;
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
    void pg_each(F&& f) const {
        f(id);
    }
};

struct insert_wide_ {
    static constexpr std::string_view name = "insert_all_wide";
};

struct insert_domain_ {
    static constexpr std::string_view name = "insert_all_domain";
};

// One int4 member: matches an int or a domain over int, not a bigint.
template <class Table>
struct typed_write {
    using pg_table = Table;
    static constexpr std::array<std::string_view, 1> pg_names{"id"};
    std::int32_t id = 0;

    template <class F>
    void pg_each(F&& f) const {
        f(id);
    }
};

struct no_cols_table {
    static constexpr std::string_view name = "query_no_cols";
};

struct no_cols_write {
    using pg_table = no_cols_table;
    static constexpr std::array<std::string_view, 0> pg_names{};
    template <class F>
    void pg_each(F&&) const {}
};

void reset_atomic(pg::conn& c) {
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_atomic"});
    c.exec(sql{"CREATE TABLE insert_all_atomic (id int PRIMARY KEY)"});
}

std::int64_t count_atomic(pg::conn& c) {
    return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_atomic"});
}

// The value the server sends back for `SELECT $1::<type>`.
template <class T>
T echo(pg::conn& c, const T& v, std::string_view type) {
    return c.query_one<T>(pg::sql{"SELECT {}::", v} + pg::unsafe_sql(std::string(type)));
}

}  // namespace

static_assert(pg::read_shape<item_row>);
static_assert(!pg::read_shape<unnamed_row>);
static_assert(pg::write_shape<shape_write>);
static_assert(pg::write_shape<kinds_write>);
static_assert(pg::write_shape<no_cols_write>);

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
    REQUIRE(c.query<std::int32_t>(sql{"SELECT id FROM query_items WHERE id > {} ORDER BY id", 1}) ==
            std::vector<std::int32_t>{2, 3});
}

TEST_CASE("query_one wants exactly one row") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    REQUIRE(c.query_one<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 2}) == "two");
    REQUIRE(c.query_one<std::int32_t, std::string>(sql{"SELECT id, name FROM query_items WHERE id = 3"}) ==
            std::make_tuple(3, std::string("three")));
    REQUIRE_THROWS_AS(c.query_one<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 9}), pg::not_found);
    const pg::error e = error_of([&] { c.query_one<std::int32_t>(sql{"SELECT id FROM query_items"}); });
    REQUIRE(e.kind == pg::error_kind::other);
    REQUIRE(e.sql == "SELECT id FROM query_items");
}

TEST_CASE("query_opt returns zero or one row") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    REQUIRE(c.query_opt<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 2}) == std::optional<std::string>{"two"});
    REQUIRE(c.query_opt<std::string>(sql{"SELECT name FROM query_items WHERE id = {}", 9}) == std::nullopt);
    REQUIRE_THROWS_AS(c.query_opt<std::int32_t>(sql{"SELECT id FROM query_items"}), pg::error);
}

TEST_CASE("NULL reads into an optional; into anything else it throws") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES ({}, {}, {})", 1, std::optional<std::string>{}, 1.0});
    REQUIRE(c.query_one<std::optional<std::string>>(sql{"SELECT name FROM query_items"}) == std::nullopt);
    const pg::error e = error_of([&] { c.query<std::string>(sql{"SELECT name FROM query_items"}); });
    REQUIRE(e.kind == pg::error_kind::other);
    REQUIRE(std::string(e.what()) == "column 1 \"name\" is NULL; read it as std::optional");
}

TEST_CASE("a column count mismatch throws") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    REQUIRE_THROWS_AS((c.query<std::int32_t, std::string>(sql{"SELECT id FROM query_items"})), pg::error);
    REQUIRE_THROWS_AS(c.query<std::int32_t>(sql{"SELECT id, name FROM query_items"}), pg::error);
}

TEST_CASE("an int4 column read as int64_t throws before the statement executes") {
    pg::conn c(db());
    reset(c);
    const sql insert{"INSERT INTO query_items VALUES (7, 'x', 0) RETURNING id"};
    for (int round = 0; round < 2; ++round) {  // a fresh and a cached statement
        const pg::error e = error_of([&] { c.query<std::int64_t>(insert); });
        REQUIRE(e.kind == pg::error_kind::other);
        REQUIRE(e.sqlstate.empty());
        REQUIRE(e.sql == insert.render().text);
        REQUIRE(std::string(e.what()).starts_with("column 1 \"id\" has type OID 23, which "));
        REQUIRE(count_items(c) == 0);
    }
    REQUIRE(c.query<std::int32_t>(insert) == std::vector<std::int32_t>{7});
    REQUIRE_THROWS_AS(c.query<std::int16_t>(sql{"SELECT id FROM query_items"}), pg::error);
    REQUIRE_THROWS_AS(c.query<std::int32_t>(sql{"SELECT count(*) FROM query_items"}), pg::error);
    REQUIRE_THROWS_AS(c.query<double>(sql{"SELECT 1::float4"}), pg::error);
    REQUIRE_THROWS_AS(c.query<std::string>(sql{"SELECT id FROM query_items"}), pg::error);
}

TEST_CASE("a std::string parameter compared with an int column fails at prepare") {
    pg::conn c(db());
    reset(c);
    insert_three(c);
    const pg::error e = error_of([&] { c.query<std::int32_t>(sql{"SELECT id FROM query_items WHERE id = {}", std::string("1")}); });
    REQUIRE(e.sqlstate == "42883");  // operator does not exist: integer = text
    REQUIRE(e.kind == pg::error_kind::other);
    const pg::error insert = error_of([&] { c.exec(sql{"INSERT INTO query_items VALUES ({}, 'x', 0)", std::string("9")}); });
    REQUIRE(insert.sqlstate == "42804");  // column "id" is of type integer but expression is of type text
    REQUIRE(count_items(c) == 3);
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
    REQUIRE(c.query_one<item_row>(sql{"SELECT * FROM query_items WHERE id = {}", 3}).name == "three");
    REQUIRE_THROWS_AS(c.query<item_row>(sql{"SELECT id, name FROM query_items"}), pg::error);
}

TEST_CASE("a read shape whose names differ from the result columns throws before decoding") {
    pg::conn c(db());
    reset(c);
    const sql renamed{"INSERT INTO query_items VALUES (1, 'x', 0) RETURNING id, name AS label, score"};
    const pg::error e = error_of([&] { c.query<item_row>(renamed); });
    REQUIRE(std::string(e.what()) == "column 2 is \"label\", the row shape expects \"name\"");
    REQUIRE(count_items(c) == 0);
    const pg::error swapped = error_of([&] { c.query<item_row>(sql{"SELECT id, 'x'::text AS score, name FROM query_items"}); });
    REQUIRE(std::string(swapped.what()) == "column 2 is \"score\", the row shape expects \"name\"");
}

TEST_CASE("types without a codec are rejected; a ::text cast reads them") {
    pg::conn c(db());
    REQUIRE_THROWS_AS(c.query<std::string>(sql{"SELECT '10.0.0.1'::inet"}), pg::error);
    REQUIRE_THROWS_AS(c.query<std::string>(sql{"SELECT '{{}}'::json"}), pg::error);
    REQUIRE_THROWS_AS(c.query<pg::jsonb>(sql{"SELECT '{{}}'::json"}), pg::error);
    REQUIRE_THROWS_AS(c.query<std::string>(sql{"SELECT 1::int4"}), pg::error);
    REQUIRE(c.query_one<std::string>(sql{"SELECT '10.0.0.1'::inet::text"}) == "10.0.0.1/32");
}

TEST_CASE("an enum reads only through ::text, a domain reads as its base type") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS string_kinds"});
    c.exec(sql{"DROP TYPE IF EXISTS query_mood"});
    c.exec(sql{"DROP DOMAIN IF EXISTS short_text"});
    c.exec(sql{"CREATE TYPE query_mood AS ENUM ('sad', 'happy')"});
    c.exec(sql{"CREATE DOMAIN short_text AS text CHECK (length(VALUE) < 10)"});
    c.exec(sql{"CREATE TABLE string_kinds (m query_mood, d short_text)"});
    const pg::error e = error_of([&] { c.exec(sql{"INSERT INTO string_kinds VALUES ({}, {})", "happy", "dom"}); });
    REQUIRE(e.sqlstate == "42804");  // text is not assignable to an enum
    c.exec(sql{"INSERT INTO string_kinds VALUES ({}::query_mood, {})", "happy", "dom"});
    REQUIRE_THROWS_AS(c.query<std::string>(sql{"SELECT m FROM string_kinds"}), pg::error);
    REQUIRE(c.query_one<std::string, std::string>(sql{"SELECT m::text, d FROM string_kinds"}) ==
            std::make_tuple(std::string("happy"), std::string("dom")));
}

TEST_CASE("server errors carry kind and SQLSTATE; the SQL text is apart") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES (1, 'a', 0)"});
    const sql dup{"INSERT INTO query_items VALUES (1, 'secret', 0)"};
    const pg::error e = error_of([&] { c.exec(dup); });
    REQUIRE(e.sqlstate == "23505");
    REQUIRE(e.kind == pg::error_kind::data);
    REQUIRE(e.sql == dup.render().text);
    REQUIRE(std::string(e.what()).starts_with("23505: "));
    REQUIRE(std::string(e.what()).find("secret") == std::string::npos);
    const pg::error syntax = error_of([&] { c.exec(sql{"SELEKT secret"}); });
    REQUIRE(syntax.sqlstate == "42601");
    REQUIRE(syntax.kind == pg::error_kind::other);
    REQUIRE(std::string(syntax.what()).find("secret") == std::string::npos);
    REQUIRE(syntax.sql == "SELEKT secret");
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 5"}) == 5);
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
    REQUIRE(c.query<std::int32_t, std::string, std::optional<std::string>, std::optional<std::int32_t>>(
                sql{"SELECT id, label, note, n FROM shape_items ORDER BY id"}) ==
            std::vector<row>{{1, "none", std::nullopt, 7}, {2, "set", "noted", std::nullopt}});
}

TEST_CASE("a write shape without columns inserts DEFAULT VALUES") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS query_no_cols"});
    c.exec(sql{"CREATE TABLE query_no_cols (id int GENERATED ALWAYS AS IDENTITY)"});
    REQUIRE(pg::insert(c, no_cols_write{}) == 1);
    REQUIRE(pg::insert(c, no_cols_write{}) == 1);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM query_no_cols"}) == 2);
}

TEST_CASE("insert_all writes rows of every column type in one statement") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_kinds"});
    c.exec(sql{"CREATE TABLE insert_all_kinds (i2 smallint, i4 int, i8 bigint, f8 float8, num numeric(18,6), "
               "flag boolean, t text, v varchar, jb jsonb, ts timestamptz, d date, u uuid, b bytea, maybe int, nt text)"});
    std::vector<kinds_write> rows(3);
    rows[0] = {-7, 1, std::int64_t{1} << 40, 1.5, pg::decimal<6>::parse("123.456789"), true, "héllo", "v",
               pg::jsonb{"{\"a\": [1, 2]}"}, sys_days{year{2024} / March / 5} + hours{7} + microseconds{120000},
               sys_days{year{2024} / March / 5}, pg::uuid::parse("a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11"),
               pg::bytea{std::byte{0}, std::byte{0xff}, std::byte{'a'}}, 5, "note"};
    rows[1] = {32767, 2, -9'000'000'000'000, -0.25, pg::decimal<6>::parse("-0.000001"), false, "two", "vv",
               pg::jsonb{"null"}, sys_days{year{1969} / December / 31} + microseconds{123450},
               sys_days{year{1969} / December / 31}, pg::uuid::parse("00000000-0000-0000-0000-000000000123"),
               pg::bytea{}, std::nullopt, std::nullopt};
    rows[2] = {0, 3, 0, 0, pg::decimal<6>{}, true, "", "", pg::jsonb{"{\"z\": 9}"}, pg::timestamptz::max(),
               pg::date::min(), pg::uuid::parse("ffffffff-ffff-ffff-ffff-ffffffffffff"), pg::bytea{std::byte{'z'}},
               -1, ""};
    REQUIRE(pg::insert_all(c, rows) == 3);
    const auto got = c.query<std::int16_t, std::int32_t, std::int64_t, double, pg::decimal<6>, bool, std::string,
                             std::string, pg::jsonb, pg::timestamptz, pg::date, pg::uuid, pg::bytea,
                             std::optional<std::int32_t>, std::optional<std::string>>(
        sql{"SELECT i2, i4, i8, f8, num, flag, t, v, jb, ts, d, u, b, maybe, nt FROM insert_all_kinds ORDER BY i4"});
    REQUIRE(got.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) REQUIRE(got[i] == rows[i].tie());
}

TEST_CASE("insert_all takes DEFAULT for a field unset in the first row and every other") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_def"});
    c.exec(sql{"CREATE TABLE insert_all_def (id int GENERATED BY DEFAULT AS IDENTITY, label text NOT NULL DEFAULT 'none', n int DEFAULT 7)"});
    std::vector<def_write> rows(2);
    rows[0].label = "a";
    rows[1].label = "b";
    REQUIRE(pg::insert_all(c, rows) == 2);
    using row = std::tuple<std::int32_t, std::string, std::optional<std::int32_t>>;
    REQUIRE(c.query<std::int32_t, std::string, std::optional<std::int32_t>>(
                sql{"SELECT id, label, n FROM insert_all_def ORDER BY id"}) == std::vector<row>{{1, "a", 7}, {2, "b", 7}});
}

TEST_CASE("insert_all rows that disagree on set fields throw before anything is sent") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_def"});
    c.exec(sql{"CREATE TABLE insert_all_def (id int GENERATED BY DEFAULT AS IDENTITY, label text NOT NULL DEFAULT 'none', n int DEFAULT 7)"});
    std::vector<def_write> mixed(2);
    mixed[0].label = "set";
    const pg::error e = error_of([&] { pg::insert_all(c, mixed); });
    REQUIRE(e.kind == pg::error_kind::other);
    REQUIRE(std::string(e.what()) == "insert_all: row 1 sets field label unlike the first row");
    std::vector<def_write> none(2);
    REQUIRE_THROWS_AS(pg::insert_all(c, none), pg::error);
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_def"}) == 0);
}

TEST_CASE("insert_all is atomic") {
    pg::conn c(db());
    reset_atomic(c);
    std::vector<atomic_write> rows{{1}, {2}, {1}};
    const pg::error e = error_of([&] { pg::insert_all(c, rows); });
    REQUIRE(e.kind == pg::error_kind::data);
    REQUIRE(e.sqlstate == "23505");
    REQUIRE(count_atomic(c) == 0);
}

TEST_CASE("insert_all joins the caller's transaction") {
    pg::pool other(test_options(1));
    pg::conn c(db());
    reset_atomic(c);
    {
        pg::transaction tx(c);
        REQUIRE(pg::insert_all(c, std::vector<atomic_write>{{1}, {2}, {3}}) == 3);
        pg::conn o(other);
        REQUIRE(count_atomic(o) == 0);
        tx.commit();
    }
    {
        pg::transaction tx(c);
        REQUIRE(pg::insert_all(c, std::vector<atomic_write>{{4}}) == 1);
    }
    pg::conn o(other);
    REQUIRE(count_atomic(o) == 3);
}

TEST_CASE("insert_all with a member type unlike its column throws before inserting anything") {
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_wide"});
    c.exec(sql{"CREATE TABLE insert_all_wide (id bigint)"});
    const pg::error e = error_of([&] { pg::insert_all(c, std::vector<typed_write<insert_wide_>>{{1}, {2}}); });
    REQUIRE(e.kind == pg::error_kind::data);
    REQUIRE(std::string(e.what()).starts_with("insert_all: column 1 \"id\" has type OID 20"));
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_wide"}) == 0);
}

TEST_CASE("insert_all writes a domain column as its base type") {
    using domain_write = typed_write<insert_domain_>;
    pg::conn c(db());
    c.exec(sql{"DROP TABLE IF EXISTS insert_all_domain"});
    c.exec(sql{"DROP DOMAIN IF EXISTS insert_all_posint"});
    c.exec(sql{"CREATE DOMAIN insert_all_posint AS int CHECK (VALUE > 0)"});
    c.exec(sql{"CREATE TABLE insert_all_domain (id insert_all_posint)"});
    REQUIRE(pg::insert_all(c, std::vector<domain_write>{{1}, {2}, {3}}) == 3);
    REQUIRE(c.query<std::int32_t>(sql{"SELECT id FROM insert_all_domain ORDER BY id"}) == std::vector<std::int32_t>{1, 2, 3});
    REQUIRE(error_of([&] { pg::insert_all(c, std::vector<domain_write>{{4}, {0}}); }).sqlstate == "23514");
    REQUIRE(c.query_one<std::int64_t>(sql{"SELECT count(*) FROM insert_all_domain"}) == 3);
}

TEST_CASE("insert_all of nothing sends nothing") {
    pg::pool p(test_options(1));
    pg::conn c(p);
    REQUIRE(pg::insert_all(c, std::vector<atomic_write>{}) == 0);
    REQUIRE(p.stats().borrows == 0);
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
    REQUIRE(c.query_one<std::string>(sql{"SELECT 'pg_class'::name"}) == "pg_class");
    const auto d = pg::decimal<6>::parse("-123456789012.345678");
    REQUIRE(echo(c, d, "numeric(18,6)") == d);
    REQUIRE(echo(c, pg::jsonb{"{\"a\": [1, 2]}"}, "jsonb") == pg::jsonb{"{\"a\": [1, 2]}"});
    const pg::uuid u = pg::uuid::parse("a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11");
    REQUIRE(echo(c, u, "uuid") == u);
    const pg::bytea bytes{std::byte{0}, std::byte{0xff}, std::byte{'a'}, std::byte{0}};
    REQUIRE(echo(c, bytes, "bytea") == bytes);
}

TEST_CASE("both 64-bit signed integer types bind and decode as int8") {
    pg::conn c(db());
    const long long big = -9000000000000000000LL;
    REQUIRE(c.query_one<long long>(sql{"SELECT {}::int8", big}) == big);
    const long other = 1234567890123L;
    REQUIRE(c.query_one<long>(sql{"SELECT {}::int8", other}) == other);
    REQUIRE(c.query_one<std::optional<long>>(sql{"SELECT NULL::int8"}) == std::nullopt);
}

TEST_CASE("decimal reads a value exactly or throws") {
    pg::conn c(db());
    REQUIRE(c.query_one<pg::decimal<2>>(sql{"SELECT 1.230::numeric(10,3)"}).units == 123);
    const pg::error e = error_of([&] { c.query_one<pg::decimal<2>>(sql{"SELECT 1.234::numeric"}); });
    REQUIRE(e.kind == pg::error_kind::data);
    REQUIRE_THROWS_AS(c.query_one<pg::decimal<2>>(sql{"SELECT 'NaN'::numeric"}), pg::error);
    c.exec(sql{"DROP TABLE IF EXISTS money_items"});
    c.exec(sql{"CREATE TABLE money_items (amount numeric(18,6) NOT NULL)"});
    c.exec(sql{"INSERT INTO money_items VALUES ({})", pg::decimal<6>::parse("0.000001")});
    c.exec(sql{"UPDATE money_items SET amount = amount + {}", pg::decimal<6>::parse("2.5")});
    REQUIRE(c.query_one<pg::decimal<6>>(sql{"SELECT amount FROM money_items"}).to_string() == "2.500001");
}

TEST_CASE("timestamptz and date round-trip, infinities included") {
    pg::conn c(db());
    for (const pg::timestamptz v : {pg::timestamptz{sys_days{year{2024} / March / 5} + hours{7} + microseconds{120000}},
                                    pg::timestamptz{sys_days{year{1969} / December / 31} + microseconds{123450}},
                                    pg::timestamptz{sys_days{year{-43} / March / 15}}, pg::timestamptz::max(),
                                    pg::timestamptz::min()})
        REQUIRE(echo(c, v, "timestamptz") == v);
    REQUIRE(c.query_one<pg::timestamptz>(sql{"SELECT '-infinity'::timestamptz"}) == pg::timestamptz::min());
    REQUIRE(c.query_one<pg::timestamptz>(sql{"SELECT '2024-03-05 07:08:09.25+02'::timestamptz"}) ==
            sys_days{year{2024} / March / 5} + hours{5} + minutes{8} + seconds{9} + microseconds{250000});
    const pg::date day = sys_days{year{2026} / November / 5};
    REQUIRE(echo(c, day, "date") == day);
    REQUIRE(c.query_one<pg::date>(sql{"SELECT 'infinity'::date"}) == pg::date::max());
    REQUIRE_THROWS_AS(c.query<pg::timestamptz>(sql{"SELECT now()::timestamp"}), pg::error);
}

TEST_CASE("a vector binds as one typed array parameter") {
    pg::conn c(db());
    const auto sorted = [&](const auto& v) {
        using T = typename std::remove_cvref_t<decltype(v)>::value_type;
        return c.query<T>(sql{"SELECT x FROM unnest({}) AS x ORDER BY x", v});
    };
    REQUIRE(sorted(std::vector<std::int64_t>{3, 1, 2}) == std::vector<std::int64_t>{1, 2, 3});
    REQUIRE(sorted(std::vector<std::int32_t>{3, 1, 2}) == std::vector<std::int32_t>{1, 2, 3});
    REQUIRE(sorted(std::vector<std::string>{"b", "a"}) == std::vector<std::string>{"a", "b"});
    REQUIRE(sorted(std::vector<bool>{true, false, true}) == std::vector<bool>{false, true, true});
    REQUIRE(sorted(std::vector<pg::decimal<2>>{pg::decimal<2>{250}, pg::decimal<2>{-100}}) ==
            std::vector<pg::decimal<2>>{pg::decimal<2>{-100}, pg::decimal<2>{250}});
    REQUIRE(c.query<std::optional<std::int32_t>>(sql{"SELECT x FROM unnest({}) AS x",
                                                     std::vector<std::optional<std::int32_t>>{7, std::nullopt}}) ==
            std::vector<std::optional<std::int32_t>>{7, std::nullopt});
    reset(c);
    insert_three(c);
    REQUIRE(c.query<std::int32_t>(sql{"SELECT id FROM query_items WHERE id = ANY({}) ORDER BY id",
                                      std::vector<std::int32_t>{1, 3}}) == std::vector<std::int32_t>{1, 3});
    REQUIRE(c.query<std::int32_t>(sql{"SELECT id FROM query_items WHERE id = ANY({})", std::vector<std::int32_t>{}}).empty());
}

TEST_CASE("one statement text with different parameter types is prepared once per type") {
    pg::conn c(db());
    for (int i = 0; i < 2; ++i) {
        REQUIRE(c.query_one<std::string>(sql{"SELECT ({} + 1)::text", std::int32_t{1}}) == "2");
        REQUIRE(c.query_one<std::string>(sql{"SELECT ({} + 1)::text", std::int64_t{1} << 40}) == "1099511627777");
    }
}

TEST_CASE("a statement text runs many times") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES (1, 'a', 0)"});
    for (int i = 0; i < 300; ++i) {
        REQUIRE(c.exec(sql{"UPDATE query_items SET score = {} WHERE id = {}", static_cast<double>(i), 1}) == 1);
        REQUIRE(c.query_one<double>(sql{"SELECT score FROM query_items WHERE id = {}", 1}) == static_cast<double>(i));
    }
}

TEST_CASE("a cached statement whose result type changed throws stale_statement once") {
    pg::pool p(test_options(1));
    pg::conn c(p);
    c.exec(sql{"DROP TABLE IF EXISTS query_shape"});
    c.exec(sql{"CREATE TABLE query_shape (a int)"});
    c.exec(sql{"INSERT INTO query_shape VALUES (7)"});
    const sql star{"SELECT * FROM query_shape"};
    REQUIRE(c.query<std::int32_t>(star) == std::vector<std::int32_t>{7});
    c.exec(sql{"ALTER TABLE query_shape ADD COLUMN b text DEFAULT 'x'"});
    // The cached description fails the new row type: the statement is described again.
    REQUIRE((c.query<std::int32_t, std::string>(star)) == std::vector<std::tuple<std::int32_t, std::string>>{{7, "x"}});
    c.exec(sql{"ALTER TABLE query_shape DROP COLUMN b"});
    // The old row type passes the cached check; the server refuses the cached plan.
    const pg::error e = error_of([&] { c.query<std::int32_t, std::string>(star); });
    REQUIRE(e.kind == pg::error_kind::stale_statement);
    REQUIRE(e.sqlstate == "0A000");
    REQUIRE(c.query<std::int32_t>(star) == std::vector<std::int32_t>{7});
}

TEST_CASE("statements the server forgot throw stale_statement once, then are prepared again") {
    pg::pool p(test_options(1));
    pg::conn c(p);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT {}::int", 5}) == 5);
    c.exec(sql{"DEALLOCATE ALL"});
    const pg::error e = error_of([&] { c.query_one<std::int32_t>(sql{"SELECT {}::int", 6}); });
    REQUIRE(e.kind == pg::error_kind::stale_statement);
    REQUIRE(e.sqlstate == "26000");
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT {}::int", 6}) == 6);
}

TEST_CASE("the statement cache is cleared at 1000 entries") {
    pg::pool p(test_options(1));
    pg::conn c(p);
    const auto nth = [](int i) { return pg::unsafe_sql("SELECT " + std::to_string(i) + " + ") + sql{"{}::int", 1}; };
    for (int i = 0; i < 1100; ++i) REQUIRE(c.query_one<std::int32_t>(nth(i)) == i + 1);
    const auto held = c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pg_prepared_statements"});
    REQUIRE(held == 101);  // the 100 prepared after the clear, and this one
    for (int i = 0; i < 1100; i += 100) REQUIRE(c.query_one<std::int32_t>(nth(i)) == i + 1);
}

TEST_CASE("a statement text with NUL throws before anything is sent") {
    pg::conn c(db());
    reset(c);
    c.exec(sql{"INSERT INTO query_items VALUES (1, 'a', 0)"});
    const std::string bad("UPDATE query_items SET score = 100 WHERE id = 1\0 AND false", 56);
    REQUIRE_THROWS_AS(c.exec(pg::unsafe_sql(bad)), pg::error);
    REQUIRE(c.query_one<double>(sql{"SELECT score FROM query_items WHERE id = 1"}) == 0.0);
}

TEST_CASE("a text parameter with NUL is refused by the server") {
    pg::conn c(db());
    reset(c);
    const pg::error e = error_of([&] { c.exec(sql{"INSERT INTO query_items VALUES (1, {}, 0)", std::string("a\0b", 3)}); });
    REQUIRE(e.kind == pg::error_kind::data);
    REQUIRE(count_items(c) == 0);
}

TEST_CASE("an empty statement succeeds with no affected rows") {
    pg::conn c(db());
    REQUIRE(c.exec(sql{}) == 0);
    REQUIRE(c.query_one<std::int32_t>(sql{"SELECT 1"}) == 1);
}

TEST_CASE("a feature_not_supported error is kind other and runs a closure once") {
    int runs = 0;
    const pg::error e = error_of([&] {
        db().transact([&](pg::conn& c) {
            ++runs;
            return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pg_class FOR UPDATE"});
        });
    });
    REQUIRE(e.kind == pg::error_kind::other);
    REQUIRE(e.sqlstate == "0A000");
    REQUIRE(runs == 1);
}
