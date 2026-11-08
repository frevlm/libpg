#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "pg/pg.hpp"

using pg::sql;

namespace {

pg::pool_options opts(bool prefetch) {
    pg::pool_options o;
    const char* d = std::getenv("LIBPG_TEST_DSN");
    REQUIRE(d != nullptr);
    o.dsn = d;
    o.max_conns = 4;
    o.prefetch = prefetch;
    return o;
}

// The tables the read shapes below use, filled once per run.
void seed() {
    static bool done = false;
    if (done) return;
    pg::pool p(opts(false));
    pg::conn c(p);
    for (const char* t : {"pf_sessions", "pf_users", "pf_msgs", "pf_groups", "pf_nicks", "pf_items", "pf_problems",
                          "pf_judgers", "pf_queue", "pf_div", "pf_div_keys", "pf_log", "pf_many", "pf_coll",
                          "pf_words"})
        c.exec(sql{"DROP TABLE IF EXISTS "} + pg::unsafe_sql(t) + sql{" CASCADE"});
    c.exec(sql{"DROP SEQUENCE IF EXISTS pf_seq"});
    c.exec(sql{"CREATE SEQUENCE pf_seq"});
    c.exec(sql{"CREATE TABLE pf_sessions (token text PRIMARY KEY, username text NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_users (username text PRIMARY KEY, id int NOT NULL, rating int NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_msgs (receiver text NOT NULL, body text)"});
    c.exec(sql{"CREATE TABLE pf_groups (username text NOT NULL, gname text NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_nicks (username text PRIMARY KEY, nick text NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_items (id int PRIMARY KEY, owner int NOT NULL, problem_id int NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_problems (id int PRIMARY KEY, title text NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_judgers (name text PRIMARY KEY, secret text NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_queue (id int PRIMARY KEY, status text NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_div (id int PRIMARY KEY, d int NOT NULL, label text NOT NULL)"});
    c.exec(sql{"CREATE TABLE pf_div_keys (k text PRIMARY KEY, id int NOT NULL)"});
    c.exec(sql{"INSERT INTO pf_sessions SELECT 'tok' || i, 'u' || i FROM generate_series(0, 9) i"});
    c.exec(sql{"INSERT INTO pf_users SELECT 'u' || i, i, i * 200 FROM generate_series(0, 9) i"});
    c.exec(sql{"INSERT INTO pf_msgs SELECT 'u' || i, 'm' FROM generate_series(1, 9) i, generate_series(1, i) j"});
    c.exec(sql{"INSERT INTO pf_groups SELECT 'u' || i, g FROM generate_series(0, 9) i, "
               "unnest(ARRAY['ga', 'gb']) g"});
    c.exec(sql{"INSERT INTO pf_groups VALUES ('u0', 'gc')"});
    c.exec(sql{"INSERT INTO pf_nicks SELECT 'u' || i, 'nick' || i FROM generate_series(0, 9, 2) i"});
    c.exec(sql{"INSERT INTO pf_problems SELECT i, 'title' || i FROM generate_series(1, 4) i"});
    c.exec(sql{"INSERT INTO pf_items SELECT i, 1 + i % 3, 1 + i % 4 FROM generate_series(1, 18) i"});
    c.exec(sql{"INSERT INTO pf_judgers SELECT 'j' || i, 's' || i FROM generate_series(0, 3) i"});
    c.exec(sql{"INSERT INTO pf_queue VALUES (7, 'Waiting')"});
    c.exec(sql{"INSERT INTO pf_div VALUES (1, 1, 'one'), (2, 2, 'two'), (3, 0, 'three'), (4, 4, 'four')"});
    c.exec(sql{"INSERT INTO pf_div_keys VALUES ('ka', 1), ('kb', 2), ('kc', 3), ('kd', 4)"});
    c.exec(sql{"CREATE TABLE pf_log (who text NOT NULL)"});
    c.exec(sql{"CREATE OR REPLACE FUNCTION pf_touch(u text) RETURNS bigint LANGUAGE sql AS "
               "$$ INSERT INTO pf_log VALUES (u); SELECT count(*) FROM pf_log WHERE who = u $$"});
    c.exec(sql{"CREATE OR REPLACE FUNCTION pf_flip(u text) RETURNS text LANGUAGE plpgsql AS "
               "$$ BEGIN PERFORM set_config('default_transaction_read_only', 'off', false); RETURN u; END $$"});
    c.exec(sql{"CREATE OR REPLACE FUNCTION pf_bump(u text) RETURNS int LANGUAGE sql AS "
               "$$ UPDATE pf_users SET rating = rating + 1 WHERE username = u RETURNING rating $$"});
    c.exec(sql{"CREATE TABLE pf_many (id int PRIMARY KEY, v int NOT NULL)"});
    c.exec(sql{"INSERT INTO pf_many SELECT i, i * 10 FROM generate_series(1, 1100) i"});
    c.exec(sql{"CREATE TABLE pf_coll (id int PRIMARY KEY, slug text COLLATE \"C\" NOT NULL)"});
    c.exec(sql{"INSERT INTO pf_coll VALUES (1, 'B'), (2, 'a'), (3, 'b')"});
    c.exec(sql{"CREATE TABLE pf_words (w text NOT NULL)"});
    c.exec(sql{"INSERT INTO pf_words VALUES ('A'), ('a'), ('B'), ('b'), ('c')"});
    done = true;
}

// GET /me: a session, then reads keyed by its username.
struct me_t {
    std::string name;
    std::tuple<std::int32_t, std::int32_t> user;
    std::int64_t msgs = 0;
    std::vector<std::string> groups;
    std::int64_t admins = 0;
    std::optional<std::string> nick;
    bool operator==(const me_t&) const = default;
};

std::string me_root(pg::conn& c, int user) {
    return c.query_one<std::string>(sql{"SELECT username FROM pf_sessions WHERE token = {}", "tok" + std::to_string(user)});
}

sql user_sql(const std::string& name) { return sql{"SELECT id, rating FROM pf_users WHERE username = {}", name}; }

me_t me(pg::conn& c, int user) {
    me_t m;
    m.name = me_root(c, user);
    m.user = c.query_one<std::int32_t, std::int32_t>(user_sql(m.name));
    m.msgs = c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_msgs WHERE receiver = {}", m.name});
    m.groups = c.query<std::string>(sql{"SELECT gname FROM pf_groups WHERE username = {} ORDER BY gname", m.name});
    m.admins = c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_users WHERE rating > 1000"});
    m.nick = c.query_opt<std::string>(sql{"SELECT nick FROM pf_nicks WHERE username = {}", m.name});
    return m;
}

// A list page: a count, a page by the same key, a title per row (N+1).
struct list_t {
    std::int64_t total = 0;
    std::vector<std::tuple<std::int32_t, std::int32_t>> rows;
    std::vector<std::string> titles;
    bool operator==(const list_t&) const = default;
};

list_t list(pg::conn& c, std::int32_t owner) {
    list_t l;
    l.total = c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_items WHERE owner = {}", owner});
    l.rows = c.query<std::int32_t, std::int32_t>(
        sql{"SELECT id, problem_id FROM pf_items WHERE owner = {} ORDER BY id DESC LIMIT 4", owner});
    for (const auto& [id, problem] : l.rows)
        l.titles.push_back(c.query_one<std::string>(sql{"SELECT title FROM pf_problems WHERE id = {}", problem}));
    return l;
}

// A judger poll: an auth read, then fixed probes.
struct poll_t {
    std::string secret;
    std::vector<std::optional<std::int32_t>> probes;
    bool operator==(const poll_t&) const = default;
};

poll_t poll(pg::conn& c, int judger) {
    poll_t p;
    p.secret = c.query_one<std::string>(sql{"SELECT secret FROM pf_judgers WHERE name = {}", "j" + std::to_string(judger)});
    p.probes.push_back(c.query_opt<std::int32_t>(sql{"SELECT id FROM pf_queue WHERE status = 'Waiting' ORDER BY id LIMIT 1"}));
    p.probes.push_back(c.query_opt<std::int32_t>(sql{"SELECT id FROM pf_queue WHERE status = 'Rejudge' ORDER BY id LIMIT 1"}));
    p.probes.push_back(c.query_opt<std::int32_t>(sql{"SELECT id FROM pf_queue WHERE status = 'Hack' ORDER BY id LIMIT 1"}));
    return p;
}

template <class F>
auto on(pg::pool& p, F&& f) {
    pg::conn c(p);
    return f(c);
}

// Runs each shape enough times for the pool to plan it.
void warm(pg::pool& p) {
    for (int i = 0; i < 12; ++i) {
        on(p, [&](pg::conn& c) { return me(c, 1 + i % 9); });
        on(p, [&](pg::conn& c) { return list(c, 1 + i % 3); });
        on(p, [&](pg::conn& c) { return poll(c, i % 4); });
    }
}

}  // namespace

TEST_CASE("prefetched reads return what the reads return without prefetch") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    warm(p);
    const pg::pool_stats warmed = p.stats();
    CHECK(warmed.prefetch_plans > 0);
    CHECK(warmed.prefetch_hits > 0);
    for (int u = 0; u < 10; ++u)
        CHECK(on(p, [&](pg::conn& c) { return me(c, u); }) == on(plain, [&](pg::conn& c) { return me(c, u); }));
    for (std::int32_t o = 1; o <= 3; ++o)
        CHECK(on(p, [&](pg::conn& c) { return list(c, o); }) == on(plain, [&](pg::conn& c) { return list(c, o); }));
    for (int j = 0; j < 4; ++j)
        CHECK(on(p, [&](pg::conn& c) { return poll(c, j); }) == on(plain, [&](pg::conn& c) { return poll(c, j); }));
    const pg::pool_stats s = p.stats();
    CHECK(s.prefetch_plans >= warmed.prefetch_plans + 17);
    CHECK(s.prefetch_hits > warmed.prefetch_hits);
}

TEST_CASE("a prefetched result answers one read; the same read again executes") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    warm(p);
    const pg::pool_stats before = p.stats();
    const auto [first, second] = on(p, [&](pg::conn& c) {
        const std::string name = me_root(c, 2);
        auto a = c.query_one<std::int32_t, std::int32_t>(user_sql(name));
        on(plain, [&](pg::conn& w) { return w.exec(sql{"UPDATE pf_users SET rating = 7 WHERE username = {}", name}); });
        auto b = c.query_one<std::int32_t, std::int32_t>(user_sql(name));
        return std::pair{a, b};
    });
    on(plain, [](pg::conn& w) { return w.exec(sql{"UPDATE pf_users SET rating = 400 WHERE username = 'u2'"}); });
    CHECK(first == std::tuple<std::int32_t, std::int32_t>{2, 400});
    CHECK(second == std::tuple<std::int32_t, std::int32_t>{2, 7});
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_hits > before.prefetch_hits);
    CHECK(after.prefetch_misses >= before.prefetch_misses + 1);
}

TEST_CASE("a nextval read returns a new value at every call") {
    seed();
    pg::pool p(opts(true));
    std::set<std::int64_t> seen;
    int calls = 0;
    for (int i = 0; i < 16; ++i)
        on(p, [&](pg::conn& c) {
            c.query_one<std::int32_t>(sql{"SELECT id FROM pf_div_keys WHERE k = {}", "ka"});
            for (int n = 0; n < 2; ++n, ++calls) seen.insert(c.query_one<std::int64_t>(sql{"SELECT nextval('pf_seq')"}));
            return 0;
        });
    CHECK(seen.size() == static_cast<std::size_t>(calls));
}

TEST_CASE("a prefetched read that writes never writes speculatively") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    on(plain, [](pg::conn& c) { return c.exec(sql{"TRUNCATE pf_log"}); });
    std::int64_t calls = 0;
    for (int i = 0; i < 16; ++i) {
        const std::int64_t n = on(p, [&](pg::conn& c) {
            const std::string name = me_root(c, 1);
            return c.query_one<std::int64_t>(sql{"SELECT pf_touch({})", name});
        });
        ++calls;
        CHECK(n == calls);
    }
    const auto logged = on(plain, [](pg::conn& c) { return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_log"}); });
    CHECK(logged == calls);
    CHECK(p.stats().prefetch_failures > 0);
}

TEST_CASE("an unpredicted read drops what the scope prefetched") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    warm(p);
    const pg::pool_stats before = p.stats();
    const auto [bumped, user] = on(p, [](pg::conn& c) {
        const std::string name = me_root(c, 6);
        const auto r = c.query_one<std::int32_t>(sql{"SELECT pf_bump({})", name});
        return std::pair{r, c.query_one<std::int32_t, std::int32_t>(user_sql(name))};
    });
    on(plain, [](pg::conn& w) { return w.exec(sql{"UPDATE pf_users SET rating = 1200 WHERE username = 'u6'"}); });
    CHECK(bumped == 1201);
    CHECK(user == std::tuple<std::int32_t, std::int32_t>{6, 1201});
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_plans == before.prefetch_plans + 1);
    CHECK(after.prefetch_misses == before.prefetch_misses + 2);
}

TEST_CASE("a read behind an unfinished send is not prefetched") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    warm(p);
    const pg::pool_stats before = p.stats();
    const auto user = on(p, [](pg::conn& c) {
        auto w = c.send(sql{"UPDATE pf_users SET rating = rating + 1 WHERE username = {}", "u8"});
        const std::string name = me_root(c, 8);
        w.affected();
        return c.query_one<std::int32_t, std::int32_t>(user_sql(name));
    });
    on(plain, [](pg::conn& w) { return w.exec(sql{"UPDATE pf_users SET rating = 1600 WHERE username = 'u8'"}); });
    CHECK(user == std::tuple<std::int32_t, std::int32_t>{8, 1601});
    CHECK(p.stats().prefetch_plans == before.prefetch_plans);
}

TEST_CASE("a root with more keys than a keyed statement slices stays correct") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    const auto page = [](pg::conn& c, std::int32_t upto) {
        const auto ids = c.query<std::int32_t>(sql{"SELECT id FROM pf_many WHERE id <= {} ORDER BY id", upto});
        std::vector<std::int32_t> vs;
        for (std::size_t i = 0; i < 3 && i < ids.size(); ++i)
            vs.push_back(c.query_one<std::int32_t>(sql{"SELECT v FROM pf_many WHERE id = {}", ids[i]}));
        return std::pair{ids.size(), vs};
    };
    for (int i = 0; i < 12; ++i) on(p, [&](pg::conn& c) { return page(c, 5); });
    const pg::pool_stats before = p.stats();
    CHECK(before.prefetch_plans > 0);
    const auto got = on(p, [&](pg::conn& c) { return page(c, 1100); });
    CHECK(got == on(plain, [&](pg::conn& c) { return page(c, 1100); }));
    CHECK(got.first == 1100);
    CHECK(p.stats().prefetch_plans == before.prefetch_plans + 1);
}

TEST_CASE("zero rows and several rows per key come back as executed") {
    seed();
    pg::pool p(opts(true));
    warm(p);
    const me_t none = on(p, [](pg::conn& c) { return me(c, 0); });
    CHECK(none.msgs == 0);
    CHECK(none.groups == std::vector<std::string>{"ga", "gb", "gc"});
    CHECK(none.nick == std::optional<std::string>("nick0"));
    const me_t odd = on(p, [](pg::conn& c) { return me(c, 3); });
    CHECK(odd.msgs == 3);
    CHECK(odd.groups == std::vector<std::string>{"ga", "gb"});
    CHECK(odd.nick == std::nullopt);
    const poll_t idle = on(p, [](pg::conn& c) { return poll(c, 2); });
    CHECK(idle.probes == std::vector<std::optional<std::int32_t>>{7, std::nullopt, std::nullopt});
}

TEST_CASE("members with several rows or none per parent row equal the reads without prefetch") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    const auto page = [](pg::conn& c, std::int32_t upto) {
        const auto names = c.query<std::string>(sql{"SELECT username FROM pf_users WHERE id < {} ORDER BY id", upto});
        std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> per;
        for (const std::string& n : names) {
            auto bodies = c.query<std::string>(sql{"SELECT body FROM pf_msgs WHERE receiver = {}", n});
            auto groups = c.query<std::string>(sql{"SELECT gname FROM pf_groups WHERE username = {} ORDER BY gname", n});
            per.emplace_back(std::move(bodies), std::move(groups));
        }
        return per;
    };
    for (int i = 0; i < 12; ++i) on(p, [&](pg::conn& c) { return page(c, 4 + i % 3); });
    const pg::pool_stats before = p.stats();
    CHECK(before.prefetch_plans > 0);
    for (std::int32_t upto : {1, 2, 6, 10}) {
        const auto got = on(p, [&](pg::conn& c) { return page(c, upto); });
        CHECK(got == on(plain, [&](pg::conn& c) { return page(c, upto); }));
        CHECK(got.size() == static_cast<std::size_t>(upto));
        CHECK(got.front().first.size() == 0);
        CHECK(got.front().second.size() == 3);
    }
    CHECK(p.stats().prefetch_hits > before.prefetch_hits);
}

TEST_CASE("a planned page read member by member is served from the batch") {
    seed();
    // The warm-up pages stay within the 32 reads a scope is learned from.
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    const auto page = [](pg::conn& c, std::int32_t upto) {
        const auto ids = c.query<std::int32_t>(sql{"SELECT id FROM pf_many WHERE id <= {} ORDER BY id", upto});
        std::vector<std::vector<std::int32_t>> cols(3);
        for (std::int32_t id : ids) cols[0].push_back(c.query_one<std::int32_t>(sql{"SELECT v FROM pf_many WHERE id = {}", id}));
        for (std::int32_t id : ids) cols[1].push_back(c.query_one<std::int32_t>(sql{"SELECT v + 1 FROM pf_many WHERE id = {}", id}));
        for (std::int32_t id : ids) cols[2].push_back(c.query_one<std::int32_t>(sql{"SELECT id * 2 FROM pf_many WHERE id = {}", id}));
        return cols;
    };
    for (int i = 0; i < 12; ++i) on(p, [&](pg::conn& c) { return page(c, 4 + i % 6); });
    const pg::pool_stats before = p.stats();
    CHECK(before.prefetch_plans > 0);
    const auto got = on(p, [&](pg::conn& c) { return page(c, 60); });
    CHECK(got == on(plain, [&](pg::conn& c) { return page(c, 60); }));
    CHECK(got[2].back() == 120);
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_plans == before.prefetch_plans + 1);
    CHECK(after.prefetch_hits >= before.prefetch_hits + 180);
    CHECK(after.prefetch_misses == before.prefetch_misses);
}

TEST_CASE("a read with another parameter than predicted runs normally") {
    seed();
    pg::pool p(opts(true));
    warm(p);
    const pg::pool_stats before = p.stats();
    const auto user = on(p, [](pg::conn& c) {
        me_root(c, 4);
        return c.query_one<std::int32_t, std::int32_t>(user_sql("u6"));
    });
    CHECK(user == std::tuple<std::int32_t, std::int32_t>{6, 1200});
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_plans == before.prefetch_plans + 1);
    CHECK(after.prefetch_misses == before.prefetch_misses + 1);
    CHECK(after.prefetch_unused > before.prefetch_unused);
}

TEST_CASE("a write drops what the scope prefetched") {
    seed();
    pg::pool p(opts(true));
    warm(p);
    const pg::pool_stats before = p.stats();
    const auto user = on(p, [](pg::conn& c) {
        const std::string name = me_root(c, 5);
        c.exec(sql{"UPDATE pf_users SET rating = rating + 1 WHERE username = {}", name});
        auto got = c.query_one<std::int32_t, std::int32_t>(user_sql(name));
        c.exec(sql{"UPDATE pf_users SET rating = rating - 1 WHERE username = {}", name});
        return got;
    });
    CHECK(user == std::tuple<std::int32_t, std::int32_t>{5, 1001});
    CHECK(p.stats().prefetch_plans == before.prefetch_plans + 1);
}

TEST_CASE("a prefetch statement that fails stays invisible to the program") {
    seed();
    pg::pool p(opts(true));
    const auto root = [](pg::conn& c, const char* k) {
        return c.query_one<std::int32_t>(sql{"SELECT id FROM pf_div_keys WHERE k = {}", k});
    };
    const auto ratio = [](pg::conn& c, std::int32_t id) {
        return c.query_one<std::int32_t>(sql{"SELECT 100 / d FROM pf_div WHERE id = {}", id});
    };
    const auto label = [](pg::conn& c, std::int32_t id) {
        return c.query_one<std::string>(sql{"SELECT label FROM pf_div WHERE id = {}", id});
    };
    const char* keys[] = {"ka", "kb", "kd"};
    for (int i = 0; i < 12; ++i)
        on(p, [&](pg::conn& c) {
            const std::int32_t id = root(c, keys[i % 3]);
            return ratio(c, id) + static_cast<std::int32_t>(label(c, id).size());
        });
    const pg::pool_stats before = p.stats();
    CHECK(before.prefetch_plans > 0);
    on(p, [&](pg::conn& c) {
        const std::int32_t id = root(c, "kc");
        CHECK(id == 3);
        CHECK(label(c, id) == "three");
        CHECK_THROWS_AS(ratio(c, id), pg::error);
        return 0;
    });
    CHECK(p.stats().prefetch_failures > before.prefetch_failures);
    const std::int32_t v = on(p, [&](pg::conn& c) { return ratio(c, root(c, "kb")); });
    CHECK(v == 50);
}

TEST_CASE("reads in a transaction are not prefetched") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    warm(p);
    const pg::pool_stats before = p.stats();
    const me_t got = p.transact([](pg::conn& c) { return me(c, 7); });
    const list_t page = on(p, [](pg::conn& c) {
        pg::transaction tx(c);
        list_t l = list(c, 2);
        tx.commit();
        return l;
    });
    CHECK(got == on(plain, [](pg::conn& c) { return me(c, 7); }));
    CHECK(page == on(plain, [](pg::conn& c) { return list(c, 2); }));
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_plans == before.prefetch_plans);
    CHECK(after.prefetch_hits == before.prefetch_hits);
}

TEST_CASE("a pool without prefetch counts none") {
    seed();
    pg::pool p(opts(false));
    warm(p);
    const pg::pool_stats s = p.stats();
    CHECK(s.borrows == 36);
    CHECK(s.prefetch_plans == 0);
    CHECK(s.prefetch_hits == 0);
    CHECK(s.prefetch_misses == 0);
    CHECK(s.prefetch_unused == 0);
    CHECK(s.prefetch_failures == 0);
}

TEST_CASE("a root carried by the batch is the root's own result, also with no row") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    warm(p);
    const auto missing = [](pg::conn& c) {
        poll_t r;
        const auto secret = c.query_opt<std::string>(sql{"SELECT secret FROM pf_judgers WHERE name = {}", "j9"});
        r.secret = secret.value_or("none");
        r.probes.push_back(
            c.query_opt<std::int32_t>(sql{"SELECT id FROM pf_queue WHERE status = 'Waiting' ORDER BY id LIMIT 1"}));
        return r;
    };
    const pg::pool_stats before = p.stats();
    const poll_t got = on(p, missing);
    CHECK(got == on(plain, missing));
    CHECK(got.secret == "none");
    CHECK(got.probes == std::vector<std::optional<std::int32_t>>{7});
    CHECK(on(p, [](pg::conn& c) { return poll(c, 3); }) == on(plain, [](pg::conn& c) { return poll(c, 3); }));
    CHECK(p.stats().prefetch_plans == before.prefetch_plans + 2);
}

TEST_CASE("a multi-row parent served from the batch keeps its order") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    const auto page = [](pg::conn& c, std::int32_t upto) {
        const auto total = c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_many WHERE id <= {}", upto});
        const auto rows = c.query<std::int32_t, std::int32_t>(
            sql{"SELECT id, v FROM pf_many WHERE id <= {} ORDER BY v DESC LIMIT 6", upto});
        std::vector<std::int32_t> vs;
        for (const auto& [id, v] : rows)
            vs.push_back(c.query_one<std::int32_t>(sql{"SELECT v + 1 FROM pf_many WHERE id = {}", id}));
        return std::tuple{total, rows, vs};
    };
    for (int i = 0; i < 12; ++i) on(p, [&](pg::conn& c) { return page(c, 20 + i); });
    const pg::pool_stats before = p.stats();
    const auto got = on(p, [&](pg::conn& c) { return page(c, 50); });
    CHECK(got == on(plain, [&](pg::conn& c) { return page(c, 50); }));
    CHECK(std::get<1>(got).front() == std::tuple<std::int32_t, std::int32_t>{50, 500});
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_plans == before.prefetch_plans + 1);
    CHECK(after.prefetch_hits >= before.prefetch_hits + 7);
}

TEST_CASE("a batch that fails to parse leaves the shared connection usable") {
    seed();
    pg::pool_options o = opts(true);
    o.max_conns = 1;
    pg::pool p(o);
    const auto scope = [](pg::conn& c, const char* k) {
        const auto id = c.query_one<std::int32_t>(sql{"SELECT id FROM pf_div_keys WHERE k = {}", k});
        return c.query_one<std::string>(sql{"SELECT label FROM pf_div WHERE id = {}; -- a note", id});
    };
    const char* keys[] = {"ka", "kb", "kd"};
    for (int i = 0; i < 16; ++i) CHECK(on(p, [&](pg::conn& c) { return scope(c, keys[i % 3]); }).size() > 2);
    CHECK(p.stats().prefetch_failures > 0);
    for (int i = 0; i < 5; ++i) CHECK(on(p, [&](pg::conn& c) { return c.query_one<std::int32_t>(sql{"SELECT {}", i}); }) == i);
    const auto n = p.transact([](pg::conn& c) { return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_div"}); });
    CHECK(n == 4);
    CHECK(on(p, [&](pg::conn& c) { return scope(c, "kb"); }) == "two");
}

TEST_CASE("a statement before a failing one in the batch is served right") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    const auto rows = [](pg::conn& c, std::int32_t below) {
        return c.query<std::int32_t>(sql{"SELECT id FROM pf_many WHERE id < {} ORDER BY id", below});
    };
    const auto count = [](pg::conn& c, std::int32_t below) {
        return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_many WHERE id < {}", below});
    };
    const auto twice = [](pg::conn& c, std::int32_t id) {
        return c.query<std::int32_t>(sql{"SELECT x FROM generate_series(1, 2) x WHERE x + {} > 0", id});
    };
    const auto risky = [](pg::conn& c, std::int32_t id) {
        return c.query<std::int32_t>(sql{"SELECT x FROM generate_series(1, 2) x WHERE 10 / ({} - 3) > -100", id});
    };
    for (int i = 0; i < 12; ++i)
        on(p, [&](pg::conn& c) {
            for (std::int32_t id : rows(c, 3)) {
                count(c, 3);
                twice(c, id);
                risky(c, id);
            }
            return 0;
        });
    const pg::pool_stats before = p.stats();
    CHECK(before.prefetch_plans > 0);
    const auto [ids, n] = on(p, [&](pg::conn& c) {
        const auto ids = rows(c, 6);
        return std::pair{ids, count(c, 6)};
    });
    CHECK(ids == std::vector<std::int32_t>{1, 2, 3, 4, 5});
    CHECK(n == on(plain, [&](pg::conn& c) { return count(c, 6); }));
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_failures > before.prefetch_failures);
    CHECK(after.prefetch_hits > before.prefetch_hits);
}

TEST_CASE("a key from a column with its own collation compares like a parameter") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    const auto read = [](pg::conn& c, std::int32_t id) {
        const auto slug = c.query_one<std::string>(sql{"SELECT slug FROM pf_coll WHERE id = {}", id});
        const auto less = c.query_one<bool>(sql{"SELECT {} < 'b'", slug});
        const auto words = c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_words WHERE w < {}", slug});
        return std::tuple{slug, less, words};
    };
    for (int i = 0; i < 12; ++i) on(p, [&](pg::conn& c) { return read(c, 1 + i % 3); });
    const pg::pool_stats before = p.stats();
    for (std::int32_t id = 1; id <= 3; ++id)
        CHECK(on(p, [&](pg::conn& c) { return read(c, id); }) == on(plain, [&](pg::conn& c) { return read(c, id); }));
    CHECK(p.stats().prefetch_hits >= before.prefetch_hits + 6);
}

TEST_CASE("a read-only session that a function made writable is never used again") {
    seed();
    pg::pool plain(opts(false));
    pg::pool p(opts(true));
    on(plain, [](pg::conn& c) { return c.exec(sql{"TRUNCATE pf_log"}); });
    const auto flip = [](pg::conn& c) {
        const std::string name = me_root(c, 1);
        return c.query_one<std::string>(sql{"SELECT pf_flip({})", name});
    };
    const auto touch = [](pg::conn& c) {
        const std::string name = c.query_one<std::string>(sql{"SELECT username FROM pf_users WHERE id = {}", 2});
        return c.query_one<std::int64_t>(sql{"SELECT pf_touch({})", name});
    };
    std::int64_t calls = 0;
    for (int i = 0; i < 24; ++i) {
        CHECK(on(p, flip) == "u1");
        ++calls;
        CHECK(on(p, touch) == calls);
    }
    const pg::pool_stats after = p.stats();
    CHECK(after.prefetch_hits > 0);
    CHECK(after.prefetch_failures == 1);  // the server refused the one speculative write
    const auto logged = on(plain, [](pg::conn& c) { return c.query_one<std::int64_t>(sql{"SELECT count(*) FROM pf_log"}); });
    CHECK(logged == calls);
}

TEST_CASE("a pooler pool does not prefetch") {
    seed();
    pg::pool_options o = opts(true);
    o.pooler = true;
    pg::pool p(o);
    warm(p);
    const pg::pool_stats s = p.stats();
    CHECK(s.prefetch_plans == 0);
    CHECK(s.prefetch_hits == 0);
}
