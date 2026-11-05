#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include "pg/codec.hpp"

using namespace std::chrono;

TEST_CASE("bool uses t and f") {
    REQUIRE(pg::codec<bool>::decode("t") == true);
    REQUIRE(pg::codec<bool>::decode("f") == false);
    REQUIRE(pg::codec<bool>::encode(true) == "t");
    REQUIRE(pg::codec<bool>::encode(false) == "f");
    REQUIRE_THROWS_AS(pg::codec<bool>::decode("true"), pg::error);
}

TEST_CASE("integers round-trip at their limits") {
    using lim16 = std::numeric_limits<std::int16_t>;
    using lim32 = std::numeric_limits<std::int32_t>;
    using lim64 = std::numeric_limits<std::int64_t>;
    REQUIRE(pg::codec<std::int16_t>::decode(pg::codec<std::int16_t>::encode(lim16::min())) == lim16::min());
    REQUIRE(pg::codec<std::int32_t>::decode(pg::codec<std::int32_t>::encode(lim32::max())) == lim32::max());
    REQUIRE(pg::codec<std::int64_t>::decode(pg::codec<std::int64_t>::encode(lim64::min())) == lim64::min());
    REQUIRE(pg::codec<std::int64_t>::encode(-42) == "-42");
}

TEST_CASE("integer decode rejects junk and overflow") {
    REQUIRE_THROWS_AS(pg::codec<std::int32_t>::decode("12x"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<std::int32_t>::decode(""), pg::error);
    REQUIRE_THROWS_AS(pg::codec<std::int32_t>::decode(" 1"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<std::int16_t>::decode("32768"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<std::int32_t>::decode("2147483648"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<std::int64_t>::decode("9223372036854775808"), pg::error);
    REQUIRE(pg::codec<std::int16_t>::decode("32767") == 32767);
}

TEST_CASE("double round-trips exactly") {
    const double values[] = {0.1, -0.1, 1.0 / 3.0, 1e300, -1e-300, std::numeric_limits<double>::max(), std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(), 123456789.123456789, 0.0};
    for (double v : values) {
        const double back = pg::codec<double>::decode(pg::codec<double>::encode(v));
        REQUIRE(back == v);
    }
}

TEST_CASE("double keeps the sign of zero") {
    const double back = pg::codec<double>::decode(pg::codec<double>::encode(-0.0));
    REQUIRE(back == 0.0);
    REQUIRE(std::signbit(back));
}

TEST_CASE("double reads and writes the PG spellings of NaN and infinity") {
    REQUIRE(std::isnan(pg::codec<double>::decode("NaN")));
    REQUIRE(pg::codec<double>::decode("Infinity") == std::numeric_limits<double>::infinity());
    REQUIRE(pg::codec<double>::decode("-Infinity") == -std::numeric_limits<double>::infinity());
    REQUIRE(pg::codec<double>::encode(std::nan("")) == "NaN");
    REQUIRE(pg::codec<double>::encode(std::numeric_limits<double>::infinity()) == "Infinity");
    REQUIRE(pg::codec<double>::encode(-std::numeric_limits<double>::infinity()) == "-Infinity");
}

TEST_CASE("double decode rejects junk") {
    REQUIRE_THROWS_AS(pg::codec<double>::decode("1.5x"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<double>::decode(""), pg::error);
}

TEST_CASE("text types keep their text") {
    REQUIRE(pg::codec<std::string>::decode("a b") == "a b");
    REQUIRE(pg::codec<pg::numeric>::decode("12.3400").text == "12.3400");
    REQUIRE(pg::codec<pg::numeric>::encode(pg::numeric{"-0.5"}) == "-0.5");
    REQUIRE(pg::codec<pg::jsonb>::decode("{\"a\": 1}").text == "{\"a\": 1}");
    REQUIRE(pg::codec<pg::jsonb>::encode(pg::jsonb{"[1, 2]"}) == "[1, 2]");
}

TEST_CASE("parameter-only types encode to their text") {
    REQUIRE(pg::codec<const char*>::encode("abc") == "abc");
    REQUIRE(pg::codec<std::string_view>::encode(std::string_view("abcdef").substr(1, 3)) == "bcd");
    REQUIRE(pg::codec<char[4]>::encode("abc") == "abc");
}

TEST_CASE("timestamptz decodes 0 to 6 fractional digits") {
    const pg::timestamptz base = sys_days{year{2024} / March / 5} + hours{7} + minutes{8} + seconds{9};
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09+00") == base);
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09.5+00") == base + microseconds{500000});
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09.12+00") == base + microseconds{120000});
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09.123456+00") == base + microseconds{123456});
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09.000001+00") == base + microseconds{1});
}

TEST_CASE("timestamptz honours a non-zero offset") {
    const pg::timestamptz utc = sys_days{year{2024} / March / 5} + hours{2};
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-05 04:00:00+02") == utc);
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-04 21:00:00-05") == utc);
    REQUIRE(pg::codec<pg::timestamptz>::decode("2024-03-05 07:30:00+05:30") == utc);
}

TEST_CASE("timestamptz encode round-trips, including before 1970") {
    const pg::timestamptz values[] = {
        sys_days{year{2024} / March / 5} + hours{7} + microseconds{123456},
        sys_days{year{1969} / December / 31} + hours{23} + minutes{59} + microseconds{999999},
        sys_days{year{1970} / January / 1},
    };
    for (const pg::timestamptz& v : values) REQUIRE(pg::codec<pg::timestamptz>::decode(pg::codec<pg::timestamptz>::encode(v)) == v);
    REQUIRE(pg::codec<pg::timestamptz>::encode(values[0]) == "2024-03-05 07:00:00.123456+00");
}

TEST_CASE("timestamptz decode rejects malformed text") {
    REQUIRE_THROWS_AS(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09.1234567+00"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<pg::timestamptz>::decode("2024-13-05 07:08:09+00"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<pg::timestamptz>::decode("2024-03-05 07:08:09+00x"), pg::error);
    REQUIRE_THROWS_AS(pg::codec<pg::timestamptz>::decode("+infinity"), pg::error);
}

TEST_CASE("optional maps NULL to nullopt") {
    REQUIRE(pg::detail::decode_cell<std::optional<std::int32_t>>(nullptr, 0) == std::nullopt);
    REQUIRE(pg::detail::decode_cell<std::optional<std::int32_t>>("7", 1) == 7);
    REQUIRE_THROWS_AS(pg::detail::decode_cell<std::int32_t>(nullptr, 0), pg::error);
    REQUIRE(pg::detail::to_param(std::optional<std::int32_t>{}) == std::nullopt);
    REQUIRE(pg::detail::to_param(std::optional<std::int32_t>{9}) == std::optional<std::string>{"9"});
}

TEST_CASE("timestamptz decodes long years, BC and infinity") {
    using C = pg::codec<pg::timestamptz>;
    REQUIRE(C::decode("12345-01-01 00:00:00+00") == sys_days{year{12345} / January / 1});
    REQUIRE(C::decode("0001-01-01 00:00:00+00 BC") == sys_days{year{0} / January / 1});
    REQUIRE(C::decode("0044-03-15 12:00:00.5+00 BC") ==
            sys_days{year{-43} / March / 15} + hours{12} + microseconds{500000});
    REQUIRE(C::decode("infinity") == pg::timestamptz::max());
    REQUIRE(C::decode("-infinity") == pg::timestamptz::min());
}

TEST_CASE("timestamptz encodes BC, long years and infinity") {
    using C = pg::codec<pg::timestamptz>;
    REQUIRE(C::encode(sys_days{year{0} / January / 1}) == "0001-01-01 00:00:00.000000+00 BC");
    REQUIRE(C::encode(sys_days{year{-43} / March / 15}) == "0044-03-15 00:00:00.000000+00 BC");
    REQUIRE(C::encode(sys_days{year{12345} / January / 1}) == "12345-01-01 00:00:00.000000+00");
    REQUIRE(C::encode(pg::timestamptz::max()) == "infinity");
    REQUIRE(C::encode(pg::timestamptz::min()) == "-infinity");
}

TEST_CASE("timestamptz errors show the whole input") {
    try {
        pg::codec<pg::timestamptz>::decode("2024-13-05 07:08:09+00");
        FAIL("no throw");
    } catch (const pg::error& e) {
        REQUIRE(std::string(e.what()) == "invalid timestamptz: '2024-13-05 07:08:09+00'");
    }
    try {
        pg::codec<pg::timestamptz>::decode("2024-03-xx 07:08:09+00");
        FAIL("no throw");
    } catch (const pg::error& e) {
        REQUIRE(std::string(e.what()) == "invalid timestamptz: '2024-03-xx 07:08:09+00'");
    }
}
