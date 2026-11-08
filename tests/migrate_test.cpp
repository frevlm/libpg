#include <catch2/catch_test_macros.hpp>
#include <poll.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../src/sha256.hpp"
#include "pg/pg.hpp"

using namespace std::chrono;
using pg::sql;

namespace {

std::string dsn() {
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    return d;
}

pg::pool& db() {
    static pg::pool p([] {
        pg::pool_options o;
        o.dsn = dsn();
        return o;
    }());
    return p;
}

// The helpers borrow a connection only for one call: migrate() borrows its own.
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
    run(sql{"DROP TABLE IF EXISTS mig_a, mig_b, mig_c, schema_migrations, lockseen, lt, watch_t, watch_c CASCADE"});
}

bool exists(std::string_view table) {
    return scalar<std::optional<std::string>>(sql{"SELECT to_regclass({})::text", table}).has_value();
}

// A blocking libpq connection outside the pool.
struct raw_conn {
    PGconn* c;
    raw_conn() : c(PQconnectdb(dsn().c_str())) { REQUIRE(PQstatus(c) == CONNECTION_OK); }
    ~raw_conn() { PQfinish(c); }
    raw_conn(const raw_conn&) = delete;
    raw_conn& operator=(const raw_conn&) = delete;

    void exec(const char* text) {
        PGresult* r = PQexec(c, text);
        const auto st = PQresultStatus(r);
        PQclear(r);
        REQUIRE((st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK));
    }

    // Notification payloads that arrive within `wait`.
    std::vector<std::string> notifications(milliseconds wait) {
        std::vector<std::string> out;
        const auto until = steady_clock::now() + wait;
        for (;;) {
            const auto left = duration_cast<milliseconds>(until - steady_clock::now()).count();
            if (left <= 0) break;
            pollfd fd{PQsocket(c), POLLIN, 0};
            if (poll(&fd, 1, static_cast<int>(left)) <= 0) break;
            PQconsumeInput(c);
            while (PGnotify* n = PQnotifies(c)) {
                out.emplace_back(n->extra);
                PQfreemem(n);
            }
        }
        return out;
    }
};

std::string hex_of_repeated(char ch, std::size_t n) { return pg::detail::sha256_hex(std::string(n, ch)); }

}  // namespace

TEST_CASE("sha256 matches the FIPS 180-4 vectors") {
    using pg::detail::sha256_hex;
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    CHECK(hex_of_repeated('a', 1'000'000) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("the migration checksum ignores CRLF and trailing whitespace") {
    using pg::detail::migration_checksum;
    CHECK(migration_checksum("a  \r\nb\t\n") == migration_checksum("a\nb\n"));
    CHECK(migration_checksum("a\nb\n") != migration_checksum("a\nc\n"));
    CHECK(migration_checksum(" a\n") != migration_checksum("a\n"));
}

TEST_CASE("pending migrations run in order in one transaction") {
    reset();
    const std::string_view first = "CREATE TABLE mig_a (id int);\nINSERT INTO mig_a VALUES (1);";
    const std::string_view second_bad = "INSERT INTO mig_a VALUES (2);\nSELECT 1/0;";
    pg::migrate_options o;
    o.migrations = {{"001_a", first}, {"002_b", second_bad}};
    REQUIRE_THROWS_AS(pg::migrate(db(), o), pg::error);
    CHECK_FALSE(exists("mig_a"));
    CHECK_FALSE(exists("libpg.migrations"));

    const std::string_view second = "INSERT INTO mig_a VALUES (2); CREATE TABLE mig_b AS SELECT max(id) AS m FROM mig_a;";
    o.migrations = {{"001_a", first}, {"002_b", second}};
    pg::migrate(db(), o);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.migrations"}) == 2);
    CHECK(scalar<std::int32_t>(sql{"SELECT m FROM mig_b"}) == 2);

    pg::migrate(db(), o);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM mig_a"}) == 2);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.migrations"}) == 2);
}

TEST_CASE("a failing migration names itself and keeps the error kind") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_bad", "SELECT * FROM no_such_table_here"}};
    try {
        pg::migrate(db(), o);
        FAIL("expected a throw");
    } catch (const pg::error& e) {
        CHECK(e.sqlstate == "42P01");
        CHECK(std::string(e.what()).find("001_bad") != std::string::npos);
    }
}

TEST_CASE("applied migrations are verified by normalized checksum") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_a", "CREATE TABLE mig_a (id int);\nINSERT INTO mig_a VALUES (1);\n"}};
    pg::migrate(db(), o);

    o.migrations = {{"001_a", "CREATE TABLE mig_a (id int);  \r\nINSERT INTO mig_a VALUES (1);\t\r\n"}};
    pg::migrate(db(), o);

    o.migrations = {{"001_a", "CREATE TABLE mig_a (id int);\nINSERT INTO mig_a VALUES (2);\n"}};
    try {
        pg::migrate(db(), o);
        FAIL("expected a throw");
    } catch (const pg::error& e) {
        CHECK(e.kind == pg::error_kind::other);
        CHECK(std::string(e.what()).find("001_a") != std::string::npos);
    }
}

TEST_CASE("a legacy table is imported by name and by version, and only read") {
    reset();
    run(sql{"CREATE TABLE schema_migrations (version int PRIMARY KEY, name text NOT NULL)"});
    run(sql{"INSERT INTO schema_migrations VALUES (1, 'alpha'), (2, 'renamed_long_ago')"});
    pg::migrate_options o;
    o.legacy = pg::legacy_import{};
    // 0001 matches by bare name, 0002 by version; 0003 is new.
    o.migrations = {{"0001_alpha.sql", "SELECT 1/0"}, {"0002_beta.sql", "SELECT 1/0"}, {"0003_c.sql", "CREATE TABLE mig_c (id int)"}};
    pg::migrate(db(), o);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM libpg.migrations"}) == 3);
    CHECK(exists("mig_c"));
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM schema_migrations"}) == 2);
    CHECK(scalar<std::string>(sql{"SELECT name FROM schema_migrations WHERE version = 1"}) == "alpha");
}

TEST_CASE("migrate takes the libpg lock and the legacy lock") {
    reset();
    pg::migrate_options o;
    o.legacy_lock = 20261018;
    o.migrations = {{"001_locks",
                     "CREATE TABLE lockseen AS SELECT classid::int8 AS classid, objid::int8 AS objid FROM pg_locks "
                     "WHERE locktype = 'advisory' AND pid = pg_backend_pid()"}};
    pg::migrate(db(), o);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM lockseen WHERE classid = 0 AND objid = 20261018"}) == 1);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM lockseen WHERE classid = 108 AND objid = 1768058983"}) == 1);
}

TEST_CASE("a lock timeout rolls back and the run is retried") {
    reset();
    run(sql{"CREATE TABLE lt (id int)"});
    raw_conn holder;
    holder.exec("BEGIN");
    holder.exec("LOCK TABLE lt IN ACCESS SHARE MODE");
    std::thread release([&] {
        std::this_thread::sleep_for(1s);
        holder.exec("COMMIT");
    });

    pg::migrate_options o;
    o.lock_timeout = 200ms;
    o.attempts = 30;
    o.migrations = {{"001_alter", "ALTER TABLE lt ADD COLUMN x int"}};
    const auto start = steady_clock::now();
    pg::migrate(db(), o);
    const auto took = steady_clock::now() - start;
    release.join();
    CHECK(took >= 900ms);
    CHECK(scalar<std::int64_t>(sql{"SELECT count(*) FROM information_schema.columns WHERE table_name = 'lt' AND column_name = 'x'"}) == 1);
}

TEST_CASE("running out of attempts throws the lock timeout") {
    reset();
    run(sql{"CREATE TABLE lt (id int)"});
    raw_conn holder;
    holder.exec("BEGIN");
    holder.exec("LOCK TABLE lt IN ACCESS SHARE MODE");
    pg::migrate_options o;
    o.lock_timeout = 100ms;
    o.attempts = 2;
    o.migrations = {{"001_alter", "ALTER TABLE lt ADD COLUMN x int"}};
    try {
        pg::migrate(db(), o);
        FAIL("expected a throw");
    } catch (const pg::error& e) {
        CHECK(e.sqlstate == "55P03");
        CHECK(e.kind == pg::error_kind::lock_or_cancel);
    }
    holder.exec("ROLLBACK");
}

namespace tags {
struct watch_t_ {
    static constexpr std::string_view name = "watch_t";
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct bal {
    using table = watch_t_;
    static constexpr std::string_view name = "bal";
};
struct watch_c_ {
    static constexpr std::string_view name = "watch_c";
    static constexpr std::array<std::string_view, 2> key_columns{"a", "b"};
};
struct v {
    using table = watch_c_;
    static constexpr std::string_view name = "v";
};
}  // namespace tags

namespace {

pg::migrate_options watch_options() {
    pg::migrate_options o;
    o.migrations = {{"001_tables",
                     "CREATE TABLE watch_t (id int PRIMARY KEY, bal bigint NOT NULL, other text);"
                     "CREATE TABLE watch_c (a int, b text, v int, PRIMARY KEY (a, b));"}};
    o.watched = {pg::watch<tags::bal>(), pg::watch<tags::v>()};
    return o;
}

std::string triggers() {
    return scalar<std::string>(sql{
        "SELECT coalesce(string_agg(tgname, ',' ORDER BY tgname), '') FROM pg_trigger WHERE tgfoid = 'libpg.watch_notify'::regproc"});
}

std::string trigger_suffixes() {
    return scalar<std::string>(sql{
        "SELECT coalesce(string_agg(right(tgname, 4), ',' ORDER BY tgname), '') FROM pg_trigger WHERE tgfoid = 'libpg.watch_notify'::regproc"});
}

std::int64_t trigger_count() {
    return scalar<std::int64_t>(sql{"SELECT count(*) FROM pg_trigger WHERE tgfoid = 'libpg.watch_notify'::regproc AND tgname LIKE 'libpg\\_w\\_%'"});
}

std::string trigger_oids() {
    return scalar<std::string>(sql{
        "SELECT coalesce(string_agg(oid::text, ',' ORDER BY tgname), '') FROM pg_trigger WHERE tgfoid = 'libpg.watch_notify'::regproc"});
}

}  // namespace

TEST_CASE("watch<Col> reads the column tag") {
    const pg::watched_column w = pg::watch<tags::v>();
    CHECK(w.table == "watch_c");
    CHECK(w.column == "v");
    CHECK(w.key_columns == std::vector<std::string>{"a", "b"});
}

TEST_CASE("watched columns notify the changed keys") {
    reset();
    pg::migrate(db(), watch_options());
    CHECK(trigger_count() == 8);
    CHECK(trigger_suffixes().size() == 8 * 4 + 7);

    raw_conn listener;
    listener.exec("LISTEN libpg_watch");
    using strings = std::vector<std::string>;

    run(sql{"INSERT INTO watch_t VALUES (1, 10, 'a'), (2, 20, 'b')"});
    CHECK(listener.notifications(500ms) == strings{R"({"t":"watch_t","c":"bal","k":[["1"],["2"]]})"});

    run(sql{"UPDATE watch_t SET bal = 11 WHERE id = 1"});
    CHECK(listener.notifications(500ms) == strings{R"({"t":"watch_t","c":"bal","k":[["1"]]})"});

    run(sql{"UPDATE watch_t SET other = 'z'"});
    run(sql{"UPDATE watch_t SET bal = bal"});
    CHECK(listener.notifications(300ms).empty());

    run(sql{"UPDATE watch_t SET bal = bal + 1"});
    // The order of keys within a payload is the join's, so either is accepted.
    const strings both = listener.notifications(500ms);
    REQUIRE(both.size() == 1);
    CHECK((both[0] == R"({"t":"watch_t","c":"bal","k":[["1"],["2"]]})" ||
           both[0] == R"({"t":"watch_t","c":"bal","k":[["2"],["1"]]})"));

    run(sql{"DELETE FROM watch_t WHERE id = 2"});
    CHECK(listener.notifications(500ms) == strings{R"({"t":"watch_t","c":"bal","k":[["2"]]})"});

    run(sql{"INSERT INTO watch_c VALUES (7, 'x y', 5)"});
    CHECK(listener.notifications(500ms) == strings{R"({"t":"watch_c","c":"v","k":[["7","x y"]]})"});
}

TEST_CASE("a notification larger than 7000 bytes is split") {
    reset();
    pg::migrate(db(), watch_options());
    raw_conn listener;
    listener.exec("LISTEN libpg_watch");
    run(sql{"INSERT INTO watch_t SELECT 100000 + g, g, NULL FROM generate_series(1, 2000) g"});
    const auto got = listener.notifications(1s);
    REQUIRE(got.size() >= 2);
    std::size_t keys = 0;
    for (const std::string& payload : got) {
        CHECK(payload.size() <= 7000);
        CHECK(payload.starts_with(R"({"t":"watch_t","c":"bal","k":[[")"));
        CHECK(payload.ends_with(R"("]]})"));
        for (std::size_t at = payload.find("[\""); at != std::string::npos; at = payload.find("[\"", at + 1)) ++keys;
    }
    CHECK(keys == 2000);
}

TEST_CASE("triggers follow the watched list") {
    reset();
    auto o = watch_options();
    pg::migrate(db(), o);
    const std::string before = trigger_oids();
    pg::migrate(db(), o);
    CHECK(trigger_oids() == before);

    o.watched = {pg::watch<tags::bal>()};
    pg::migrate(db(), o);
    CHECK(trigger_count() == 4);

    o.watched.clear();
    pg::migrate(db(), o);
    CHECK(triggers().empty());
    raw_conn listener;
    listener.exec("LISTEN libpg_watch");
    run(sql{"INSERT INTO watch_t VALUES (1, 10, 'a')"});
    CHECK(listener.notifications(300ms).empty());
}

TEST_CASE("a long column name keeps four triggers across runs") {
    reset();
    const std::string col(60, 'c');
    pg::migrate_options o;
    const std::string ddl = "CREATE TABLE watch_t (id int PRIMARY KEY, " + col + " int)";
    o.migrations = {{"001_t", ddl}};
    o.watched = {pg::watched_column{"watch_t", col, {"id"}}};
    for (int i = 0; i < 3; ++i) {
        pg::migrate(db(), o);
        CHECK(trigger_count() == 4);
        CHECK(triggers().size() < 4 * 64);
    }
}

TEST_CASE("an update that changes a key reports the old and the new key") {
    reset();
    pg::migrate(db(), watch_options());
    run(sql{"INSERT INTO watch_t VALUES (1, 10, 'a'), (2, 20, 'b')"});
    raw_conn listener;
    listener.exec("LISTEN libpg_watch");
    run(sql{"UPDATE watch_t SET id = 5 WHERE id = 1"});
    const auto got = listener.notifications(500ms);
    REQUIRE(got.size() == 1);
    CHECK((got[0] == R"({"t":"watch_t","c":"bal","k":[["5"],["1"]]})" || got[0] == R"({"t":"watch_t","c":"bal","k":[["1"],["5"]]})"));
}

TEST_CASE("truncate announces every key") {
    reset();
    pg::migrate(db(), watch_options());
    run(sql{"INSERT INTO watch_t VALUES (1, 10, 'a')"});
    raw_conn listener;
    listener.exec("LISTEN libpg_watch");
    run(sql{"TRUNCATE watch_t"});
    CHECK(listener.notifications(500ms) == std::vector<std::string>{R"({"t":"watch_t","c":"bal","all":true})"});
}

TEST_CASE("migrations run without the pool's statement timeout") {
    reset();
    pg::pool_options po;
    po.dsn = dsn();
    po.statement_timeout = 1s;
    pg::pool p(po);
    pg::migrate_options o;
    o.migrations = {{"001_slow", "SELECT pg_sleep(1.5)"}};
    CHECK_NOTHROW(pg::migrate(p, o));
}

TEST_CASE("a migration that ends the transaction fails") {
    reset();
    pg::migrate_options o;
    o.migrations = {{"001_commit", "CREATE TABLE mig_a (id int); COMMIT;"}};
    try {
        pg::migrate(db(), o);
        FAIL("expected a throw");
    } catch (const pg::error& e) {
        CHECK(e.kind == pg::error_kind::other);
        CHECK(std::string(e.what()).find("001_commit") != std::string::npos);
    }
}
