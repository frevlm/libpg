#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "pg/pg.hpp"

namespace {

// The bytes of each parameter; nullopt for NULL.
std::vector<std::optional<std::string>> bytes_of(const pg::rendered& r) {
    std::vector<std::optional<std::string>> out;
    for (const auto& p : r.params) out.push_back(p.bytes);
    return out;
}

}  // namespace

TEST_CASE("placeholders render as numbered parameters") {
    const pg::sql q{"SELECT id FROM users WHERE name = {} AND age > {}", std::string("ann"), std::int32_t{30}};
    const pg::rendered r = q.render();
    REQUIRE(r.text == "SELECT id FROM users WHERE name = $1 AND age > $2");
    REQUIRE(r.params.size() == 2);
    REQUIRE(r.params[0].type == 0);
    REQUIRE_FALSE(r.params[0].binary);
    REQUIRE(r.params[0].bytes == "ann");
    REQUIRE(r.params[1].type == 23);
    REQUIRE(r.params[1].binary);
}

TEST_CASE("a statement without arguments is its text") {
    REQUIRE(pg::sql{"SELECT 1"}.render().text == "SELECT 1");
    REQUIRE(pg::sql{"SELECT 1"}.render().params.empty());
}

TEST_CASE("doubled braces are literal braces") {
    const pg::sql q{"SELECT '{{}}'::jsonb, {}, '}}{{'", std::int32_t{1}};
    REQUIRE(q.render().text == "SELECT '{}'::jsonb, $1, '}{'");
}

TEST_CASE("an empty statement renders empty") {
    const pg::sql none;
    REQUIRE(none.empty());
    REQUIRE(none.render().text.empty());
    REQUIRE((pg::sql{"x"} + none).render().text == "x");
}

TEST_CASE("concatenation renumbers parameters") {
    pg::sql q{"SELECT * FROM t WHERE a = {}", std::int32_t{1}};
    q += pg::sql{" AND b = {}", std::string("two")};
    q = q + pg::sql{" AND c = {} ORDER BY id", std::int64_t{3}};
    const pg::rendered r = q.render();
    REQUIRE(r.text == "SELECT * FROM t WHERE a = $1 AND b = $2 AND c = $3 ORDER BY id");
    REQUIRE(r.params.size() == 3);
    REQUIRE(r.params[1].bytes == "two");
}

TEST_CASE("a statement argument is spliced and renumbered") {
    const pg::sql where{"name = {} AND {}", std::string("x"), pg::sql{"age > {}", std::int32_t{5}}};
    const pg::sql q{"SELECT {} FROM t WHERE {} LIMIT {}", std::int32_t{0}, where, std::int32_t{10}};
    const pg::rendered r = q.render();
    REQUIRE(r.text == "SELECT $1 FROM t WHERE name = $2 AND age > $3 LIMIT $4");
    REQUIRE(r.params.size() == 4);
    REQUIRE(r.params[1].bytes == "x");
}

TEST_CASE("join puts the separator between parts") {
    const std::vector<pg::sql> parts{pg::sql{"a = {}", std::int32_t{1}}, pg::sql{"b"}, pg::sql{"c = {}", std::int32_t{3}}};
    const pg::rendered r = pg::join(parts, pg::sql{" AND "}).render();
    REQUIRE(r.text == "a = $1 AND b AND c = $2");
    REQUIRE(r.params.size() == 2);
    REQUIRE(pg::join({}, pg::sql{", "}).render().text.empty());
}

TEST_CASE("optional arguments bind NULL when empty") {
    const pg::sql q{"VALUES ({}, {})", std::optional<std::int32_t>{}, std::optional<std::string>{"v"}};
    const pg::rendered r = q.render();
    REQUIRE(bytes_of(r) == std::vector<std::optional<std::string>>{std::nullopt, std::string("v")});
    REQUIRE(r.params[0].type == 23);
}

TEST_CASE("string literals, views and C strings are text parameters") {
    const char* c = "c";
    const pg::sql q{"VALUES ({}, {}, {})", "lit", std::string_view("view"), c};
    const pg::rendered r = q.render();
    REQUIRE(bytes_of(r) == std::vector<std::optional<std::string>>{std::string("lit"), std::string("view"), std::string("c")});
    for (const auto& p : r.params) REQUIRE(p.type == 0);
}

TEST_CASE("unsafe_sql takes runtime text verbatim") {
    const std::string table = "dyn_" + std::to_string(7);
    const pg::sql q = pg::unsafe_sql("SELECT * FROM " + table + " WHERE {x}") + pg::sql{" AND id = {}", std::int32_t{1}};
    REQUIRE(q.render().text == "SELECT * FROM dyn_7 WHERE {x} AND id = $1");
}

TEST_CASE("identifiers are quoted") {
    REQUIRE(pg::detail::ident("we\"ird").render().text == "\"we\"\"ird\"");
}

static_assert(!pg::detail::pins_text("SELECT 1"));
static_assert(pg::detail::pins_text("LISTEN x"));

TEST_CASE("the pin verdict is decided at compile time and matches the rendered text") {
    const auto check = [](const pg::sql& q, bool want) {
        const std::string text = q.render().text;
        REQUIRE(pg::detail::pins_of(q, text) == pg::detail::pins_text(text));
        REQUIRE(pg::detail::pins_of(q, text) == want);
    };
    check(pg::sql{"SELECT id FROM t WHERE id = {}", 1}, false);
    check(pg::sql{"select pg_advisory_lock({})", 1}, true);
    check(pg::sql{"  -- c\n SELECT currval('s')"}, true);
    check(pg::sql{"SET search_path = public"}, true);
    check(pg::sql{"INSERT INTO t VALUES ({})", 2}, false);
    check(pg::sql{"{} UNION SELECT 1", pg::sql{"SELECT 2"}}, false);
    check(pg::sql{"{}", pg::sql{"SELECT set_config('a','b',false)"}}, true);
}

