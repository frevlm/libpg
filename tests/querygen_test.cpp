// Runs the functions pg_tablegen generated from tests/queries/fixture.sql
// (tests/generated/fixture_queries.hpp) against a database built from
// tests/fixture_schema.sql, and inserts through a generated write shape.
#include <catch2/catch_test_macros.hpp>

#include <libpq-fe.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "fixture_queries.hpp"
#include "fixture_tables.hpp"
#include "pg/pg.hpp"

static_assert(pg::read_shape<db::users::row>);
static_assert(pg::write_shape<db::users::write>);
static_assert(pg::read_shape<db::group_members_row>);
static_assert(db::users::row::pg_columns == 21);
template <class T, class Member>
constexpr bool member_is = std::is_same_v<T, Member>;
static_assert(member_is<decltype(db::rollup_totals_row::group_id), std::optional<std::int32_t>>);
static_assert(member_is<decltype(db::left_join_false_row::group_id), std::optional<std::int16_t>>);
static_assert(member_is<decltype(db::left_join_false_row::user_id), std::int32_t>);
static_assert(member_is<decltype(db::left_join_pruned_row::measurement_id), std::optional<std::int32_t>>);
static_assert(member_is<decltype(db::add_group_row::id), std::int16_t>);
static_assert(member_is<decltype(db::add_group_row::title), std::string>);
static_assert(db::users::write::pg_names.size() == 19);

namespace {

const std::string database = "libpg_querygen";

std::string test_dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    return d;
}

// The test DSN pointing at `name` instead of its own database.
std::string dsn_for(const std::string& name) {
    auto dsn = test_dsn();
    return dsn.substr(0, dsn.rfind('/') + 1) + name;
}

void run_script(const std::string& dsn, const std::string& script) {
    std::unique_ptr<PGconn, decltype(&PQfinish)> c(PQconnectdb(dsn.c_str()), PQfinish);
    if (PQstatus(c.get()) != CONNECTION_OK) throw std::runtime_error(PQerrorMessage(c.get()));
    std::unique_ptr<PGresult, decltype(&PQclear)> r(PQexec(c.get(), script.c_str()), PQclear);
    auto st = PQresultStatus(r.get());
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) throw std::runtime_error(PQresultErrorMessage(r.get()));
}

std::string read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A fresh database holding fixture_schema.sql, and a pool on it.
pg::pool& test_pool() {
    static pg::pool p([] {
        run_script(test_dsn(), "DROP DATABASE IF EXISTS " + database + " WITH (FORCE)");
        run_script(test_dsn(), "CREATE DATABASE " + database);
        run_script(dsn_for(database), read_file(LIBPG_FIXTURE_SCHEMA));
        pg::pool_options o;
        o.dsn = dsn_for(database);
        return o;
    }());
    return p;
}

// Users ann (profile {"vip": true}) and bob (profile {"team": "x"}); group 1
// "admins" with ann as admin and bob as member; ann also in group 2, which has
// no groups row. Returns ann's id.
std::int32_t seed(pg::conn& c) {
    c.exec(pg::sql{"TRUNCATE users, memberships, groups"});
    db::users::write ann;
    ann.username = "ann";
    ann.balance = pg::decimal<2>::parse("10.50");
    ann.profile = pg::jsonb{R"({"vip": true})"};
    ann.mood = "calm";
    REQUIRE(pg::insert(c, ann) == 1);
    db::users::write bob;
    bob.username = "bob";
    bob.profile = pg::jsonb{R"({"team": "x"})"};
    REQUIRE(pg::insert(c, bob) == 1);
    c.exec(pg::sql{"INSERT INTO groups VALUES (1, 'admins')"});
    c.exec(pg::sql{"INSERT INTO memberships (user_id, group_id, role) "
                   "SELECT id, 1, CASE username WHEN 'ann' THEN 'admin' ELSE 'member' END FROM users"});
    c.exec(pg::sql{"INSERT INTO memberships (user_id, group_id, role) SELECT id, 2, 'guest' FROM users WHERE username = 'ann'"});
    return db::user_by_name(c, "ann").value().id;
}

}  // namespace

TEST_CASE(":one decodes a row; NOT NULL columns are plain, nullable ones optional") {
    pg::conn c(test_pool());
    auto ann = seed(c);
    db::user_by_id_row r = db::user_by_id(c, ann);
    CHECK(r.id == ann);
    CHECK(r.username == "ann");
    CHECK(r.email == std::nullopt);
    CHECK(r.balance.to_string() == "10.50");
    CHECK(r.stake.to_string() == "0.000000");
    CHECK(r.profile.text == R"({"vip": true})");
    CHECK(r.mood == std::optional<std::string>("calm"));
    REQUIRE_THROWS_AS(db::user_by_id(c, ann + 1000), pg::not_found);
}

TEST_CASE(":opt returns the row or nothing; a ? alias makes the member optional") {
    pg::conn c(test_pool());
    seed(c);
    auto r = db::user_by_name(c, "bob");
    REQUIRE(r.has_value());
    CHECK(r->login == std::optional<std::string>("bob"));
    CHECK(db::user_by_name(c, "nobody").has_value() == false);
}

TEST_CASE(":many with a hole: empty hole, and a hole whose parameter follows $1") {
    pg::conn c(test_pool());
    auto ann = seed(c);
    auto all = db::group_members(c, 1, pg::sql{});
    REQUIRE(all.size() == 2);
    CHECK(all[0].title == std::optional<std::string>("admins"));
    auto admins = db::group_members(c, 1, pg::sql{"AND m.role = {}", "admin"});
    REQUIRE(admins.size() == 1);
    CHECK(admins[0].user_id == ann);
    CHECK(admins[0].role == "admin");
    CHECK(admins[0].username == std::optional<std::string>("ann"));
}

TEST_CASE("a NOT NULL column on the nullable side of a LEFT JOIN reads NULL") {
    pg::conn c(test_pool());
    seed(c);
    auto rows = db::group_members(c, 2, pg::sql{});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].title == std::nullopt);
}

TEST_CASE("a ! alias makes an aggregate plain") {
    pg::conn c(test_pool());
    seed(c);
    db::user_stats_row s = db::user_stats(c);
    CHECK(s.total == 2);
    CHECK(s.top == std::optional<std::int64_t>(0));
}

TEST_CASE("jsonb @> in the query body is sent untouched") {
    pg::conn c(test_pool());
    seed(c);
    CHECK(db::users_with_profile(c, pg::jsonb{R"({"team": "x"})"}).size() == 2);
    CHECK(db::users_with_profile(c, pg::jsonb{R"({"team": "y"})"}).size() == 1);
}

TEST_CASE(":exec returns affected rows; parameters bind by $n, not by position in the body") {
    pg::conn c(test_pool());
    auto ann = seed(c);
    CHECK(db::set_active(c, ann, false) == 1);
    CHECK(c.query_one<bool>(pg::sql{"SELECT active FROM users WHERE id = {}", ann}) == false);
    CHECK(db::set_active(c, ann + 1000, true) == 0);
}

TEST_CASE("RETURNING old and new values are optional, the plain column is not") {
    pg::conn c(test_pool());
    auto ann = seed(c);
    db::add_points_row r = db::add_points(c, ann, 5);
    CHECK(r.before == std::optional<std::int64_t>(0));
    CHECK(r.after == std::optional<std::int64_t>(5));
    CHECK(r.points == 5);
}

TEST_CASE("a table's read shape decodes SELECT *") {
    pg::conn c(test_pool());
    auto ann = seed(c);
    auto r = c.query_one<db::memberships::row>(pg::sql{"SELECT * FROM memberships WHERE group_id = 2"});
    CHECK(r.user_id == ann);
    CHECK(r.group_id == 2);
    CHECK(r.role == "guest");
    CHECK(r.since == std::nullopt);
}

TEST_CASE("columns with no source row read as NULL") {
    pg::conn c(test_pool());
    seed(c);
    auto totals = db::rollup_totals(c);
    REQUIRE(totals.size() == 3);
    CHECK(totals.back().group_id == std::nullopt);
    for (auto& r : db::left_join_false(c)) CHECK(r.group_id == std::nullopt);
    for (auto& r : db::left_join_pruned(c)) CHECK(r.measurement_id == std::nullopt);
    CHECK(db::add_group(c, 9, "ops").id == 9);
}
