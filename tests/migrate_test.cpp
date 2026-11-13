#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <string_view>

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

void run(const sql& q) {
    pg::conn c(db());
    c.exec(q);
}

template <class T>
T scalar(const sql& q) {
    pg::conn c(db());
    return c.query_one<T>(q);
}

// Drops everything the tests in this file create.
void reset() {
    run(sql{"DROP SCHEMA IF EXISTS libpg CASCADE"});
    run(sql{"DROP TABLE IF EXISTS mig_a, mig_b, lockseen CASCADE"});
}

bool exists(std::string_view table) {
    return scalar<std::optional<std::string>>(sql{"SELECT to_regclass({})::text", table}).has_value();
}

}  // namespace

TEST_CASE("pending migrations run in order in one transaction") {
    reset();
    const std::string_view first = "CREATE TABLE mig_a (id int);\nINSERT INTO mig_a VALUES (1);";
    pg::migrate_options o;
    o.migrations = {{"001_a", first}, {"002_b", "INSERT INTO mig_a VALUES (2);\nSELECT 1/0;"}};
    REQUIRE(error_of([&] { pg::migrate(db(), o); }).sqlstate == "22012");
    REQUIRE(exists("mig_a") == false);
    REQUIRE(exists("libpg.migrations") == false);

    o.migrations = {{"001_a", first}, {"002_b", "INSERT INTO mig_a VALUES (2); CREATE TABLE mig_b AS SELECT max(id) AS m FROM mig_a;"}};
    pg::migrate(db(), o);
    REQUIRE(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.migrations"}) == 2);
    REQUIRE(scalar<std::int32_t>(sql{"SELECT m FROM mig_b"}) == 2);

    pg::migrate(db(), o);
    REQUIRE(scalar<std::int64_t>(sql{"SELECT count(*) FROM mig_a"}) == 2);
    REQUIRE(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.migrations"}) == 2);
}

TEST_CASE("a failing migration names itself and keeps its SQLSTATE") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_bad", "SELECT * FROM no_such_table_here"}};
    const pg::error e = error_of([&] { pg::migrate(db(), o); });
    REQUIRE(e.sqlstate == "42P01");
    REQUIRE(std::string(e.what()).find("migration 001_bad failed") != std::string::npos);
}

TEST_CASE("the stored checksum is the one libpg 0.3.0 stored") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_a", "CREATE TABLE mig_a (id int);  \r\nINSERT INTO mig_a VALUES (1);\t\r\n"},
                    {"002_b", "SELECT 1 \t\r\n\r\n  -- trailing\v\f\n"}};
    pg::migrate(db(), o);
    // sha256 of the text with CRLF -> LF and trailing whitespace stripped per line, as 0.3.0 computed it.
    REQUIRE(scalar<std::string>(sql{"SELECT sha256 FROM libpg.migrations WHERE name = '001_a'"}) ==
            "5ed7affa4520d65a63cceede67e7acefb668b2834586a4bae88831601b17a862");
    REQUIRE(scalar<std::string>(sql{"SELECT sha256 FROM libpg.migrations WHERE name = '002_b'"}) ==
            "cedc497a718e876c2238c6b31acedcf0b326b221b6e1dd68fc03a87b0d6fe388");
}

TEST_CASE("an applied migration may change only in line endings and trailing whitespace") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_a", "CREATE TABLE mig_a (id int);\nINSERT INTO mig_a VALUES (1);\n"}};
    pg::migrate(db(), o);
    o.migrations = {{"001_a", "CREATE TABLE mig_a (id int);  \r\nINSERT INTO mig_a VALUES (1);\t\r\n"}};
    pg::migrate(db(), o);
    o.migrations = {{"001_a", "CREATE TABLE mig_a (id int);\nINSERT INTO mig_a VALUES (2);\n"},
                    {"002_b", "CREATE TABLE mig_b (id int)"}};
    const pg::error e = error_of([&] { pg::migrate(db(), o); });
    REQUIRE(e.kind == pg::error_kind::other);
    REQUIRE(std::string(e.what()) == "migration 001_a changed after it was applied");
    REQUIRE(exists("mig_b") == false);
    REQUIRE(scalar<std::int64_t>(sql{"SELECT count(*) FROM mig_a"}) == 1);
}

TEST_CASE("migrate holds the libpg advisory lock") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_locks",
                     "CREATE TABLE lockseen AS SELECT classid::int8 AS classid, objid::int8 AS objid FROM pg_locks "
                     "WHERE locktype = 'advisory' AND pid = pg_backend_pid()"}};
    pg::migrate(db(), o);
    REQUIRE(scalar<std::int64_t>(sql{"SELECT count(*) FROM lockseen WHERE classid = 108 AND objid = 1768058983"}) == 1);
}

TEST_CASE("a migration that ends the transaction fails") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_commit", "CREATE TABLE mig_a (id int); COMMIT;"}};
    const pg::error e = error_of([&] { pg::migrate(db(), o); });
    REQUIRE(e.kind == pg::error_kind::other);
    REQUIRE(std::string(e.what()) == "migration 001_commit must not end the transaction");
}

TEST_CASE("migrate runs on a connection of its own that is closed afterwards") {
    reset();
    pg::pool_options po = db().options();
    po.max_conns = 1;
    pg::pool p(po);
    pg::migrate_options o;
    o.migrations = {{"001_session", "SET search_path = pg_catalog, public; CREATE TABLE public.mig_a (id int);"}};
    pg::migrate(p, o);
    REQUIRE(p.stats().open == 0);
    pg::conn c(p);
    REQUIRE(c.query_one<std::string>(sql{"SHOW search_path"}) == "\"$user\", public");
}
