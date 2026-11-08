#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "pg/codec.hpp"

using namespace pg;
using namespace std::chrono;

namespace {

template <class T>
std::string enc(const T& v) {
    std::string out;
    codec<T>::encode(v, out);
    return out;
}

std::string bytes(std::initializer_list<int> l) {
    std::string s;
    for (int b : l) s.push_back(static_cast<char>(b));
    return s;
}

template <class T>
T dec(const std::string& b, oid id = 0) {
    type_desc t;
    t.id = id;
    return codec<T>::decode(b, t);
}

type_desc ty(oid id) { return {id, "", 0, 0}; }

numeric num(const std::string& wire) { return dec<numeric>(wire, 1700); }

}  // namespace

TEST_CASE("bool wire format") {
    CHECK(enc(true) == bytes({1}));
    CHECK(enc(false) == bytes({0}));
    CHECK(dec<bool>(bytes({1})) == true);
    CHECK(dec<bool>(bytes({0})) == false);
    CHECK(codec<bool>::param_oid == 16);
    CHECK(codec<bool>::param_binary);
    CHECK(codec<bool>::accepts(ty(16)));
    CHECK_FALSE(codec<bool>::accepts(ty(23)));
    REQUIRE_THROWS_AS(dec<bool>(bytes({1, 0})), error);
}

TEST_CASE("integer wire format") {
    CHECK(enc<std::int16_t>(-2) == bytes({0xff, 0xfe}));
    CHECK(enc<std::int32_t>(0x01020304) == bytes({1, 2, 3, 4}));
    CHECK(enc<std::int64_t>(-1) == bytes({255, 255, 255, 255, 255, 255, 255, 255}));
    CHECK(dec<std::int16_t>(bytes({0x80, 0})) == std::numeric_limits<std::int16_t>::min());
    CHECK(dec<std::int32_t>(bytes({0xff, 0xff, 0xff, 0xfe})) == -2);
    CHECK(dec<std::int64_t>(enc<std::int64_t>(std::numeric_limits<std::int64_t>::min())) ==
          std::numeric_limits<std::int64_t>::min());
    CHECK(codec<std::int16_t>::param_oid == 21);
    CHECK(codec<std::int32_t>::param_oid == 23);
    CHECK(codec<std::int64_t>::param_oid == 20);
    CHECK(codec<std::int32_t>::accepts(ty(23)));
    CHECK_FALSE(codec<std::int32_t>::accepts(ty(20)));
    REQUIRE_THROWS_AS(dec<std::int32_t>(bytes({1, 2})), error);
}

TEST_CASE("float wire format") {
    CHECK(enc(1.0f) == bytes({0x3f, 0x80, 0, 0}));
    CHECK(enc(1.0) == bytes({0x3f, 0xf0, 0, 0, 0, 0, 0, 0}));
    CHECK(dec<double>(enc(0.1)) == 0.1);
    CHECK(dec<float>(enc(-2.5f)) == -2.5f);
    CHECK(std::isinf(dec<double>(enc(std::numeric_limits<double>::infinity()))));
    CHECK(std::isnan(dec<double>(enc(std::nan("")))));
    CHECK(codec<float>::param_oid == 700);
    CHECK(codec<double>::param_oid == 701);
    CHECK(codec<double>::accepts(ty(701)));
    CHECK_FALSE(codec<double>::accepts(ty(700)));
}

TEST_CASE("string is a text parameter and accepts text-like columns") {
    CHECK(codec<std::string>::param_oid == 0);
    CHECK_FALSE(codec<std::string>::param_binary);
    CHECK(enc(std::string("héllo")) == "héllo");
    CHECK(dec<std::string>("abc") == "abc");
    CHECK(codec<std::string>::accepts(*detail::builtin_type(25)));
    CHECK(codec<std::string>::accepts(*detail::builtin_type(1043)));
    CHECK(codec<std::string>::accepts(*detail::builtin_type(1042)));
    CHECK(codec<std::string>::accepts(*detail::builtin_type(19)));
    CHECK(codec<std::string>::accepts({99999, "citext", 'S', 'b'}));
    CHECK(codec<std::string>::accepts({99998, "mood", 'E', 'e'}));
    CHECK_FALSE(codec<std::string>::accepts(*detail::builtin_type(23)));
}

TEST_CASE("jsonb strips the version byte, json is plain text") {
    CHECK(codec<jsonb>::param_oid == 0);
    CHECK_FALSE(codec<jsonb>::param_binary);
    CHECK(enc(jsonb{"{\"a\":1}"}) == "{\"a\":1}");
    CHECK(codec<jsonb>::accepts(ty(114)));
    CHECK(codec<jsonb>::accepts(ty(3802)));
    CHECK_FALSE(codec<jsonb>::accepts(ty(25)));
    CHECK(dec<jsonb>(bytes({1}) + "{\"a\": 1}", 3802).text == "{\"a\": 1}");
    CHECK(dec<jsonb>("[1,2]", 114).text == "[1,2]");
    REQUIRE_THROWS_AS(dec<jsonb>(bytes({2}) + "{}", 3802), error);
    REQUIRE_THROWS_AS(dec<jsonb>("", 3802), error);
}

TEST_CASE("numeric wire format") {
    SECTION("zero") {
        numeric z = numeric::parse("0");
        CHECK(enc(z) == bytes({0, 0, 0, 0, 0, 0, 0, 0}));
        CHECK(num(enc(z)) == z);
        CHECK(z.to_string() == "0");
    }
    SECTION("1234567.89") {
        numeric n = numeric::parse("1234567.89");
        // groups 123 4567 8900; weight 1; dscale 2
        CHECK(enc(n) == bytes({0, 3, 0, 1, 0, 0, 0, 2, 0, 123, 0x11, 0xd7, 0x22, 0xc4}));
        CHECK(num(enc(n)) == n);
        CHECK(n.to_string() == "1234567.89");
    }
    SECTION("negative") {
        numeric n = numeric::parse("-12.3400");
        CHECK(n.negative);
        CHECK(n.dscale == 4);
        CHECK(enc(n) == bytes({0, 2, 0, 0, 0x40, 0, 0, 4, 0, 12, 0x0d, 0x48}));
        CHECK(num(enc(n)).to_string() == "-12.3400");
    }
    SECTION("weight below zero") {
        numeric n = numeric::parse("0.0001");
        CHECK(n.weight == -1);
        CHECK(n.digits == std::vector<std::int16_t>{1});
        CHECK(enc(n) == bytes({0, 1, 0xff, 0xff, 0, 0, 0, 4, 0, 1}));
        CHECK(num(enc(n)).to_string() == "0.0001");
    }
    SECTION("weight far below zero") {
        numeric n = numeric::parse("0.000000012");
        CHECK(n.weight == -2);
        CHECK(num(enc(n)).to_string() == "0.000000012");
    }
    SECTION("trailing zero groups are stripped but dscale is kept") {
        numeric n = numeric::parse("100000000");
        CHECK(n.weight == 2);
        CHECK(n.digits == std::vector<std::int16_t>{1});
        CHECK(n.to_string() == "100000000");
        CHECK(numeric::parse("1.50").to_string() == "1.50");
    }
    SECTION("leading zeros and signs") {
        CHECK(numeric::parse("+0012").to_string() == "12");
        CHECK(numeric::parse(".5").to_string() == "0.5");
        CHECK(numeric::parse("-0.00").to_string() == "0.00");
    }
    SECTION("specials") {
        CHECK(enc(numeric::parse("NaN")) == bytes({0, 0, 0, 0, 0xc0, 0, 0, 0}));
        CHECK(enc(numeric::parse("Infinity")) == bytes({0, 0, 0, 0, 0xd0, 0, 0, 0}));
        CHECK(enc(numeric::parse("-Infinity")) == bytes({0, 0, 0, 0, 0xf0, 0, 0, 0}));
        CHECK(num(bytes({0, 0, 0, 0, 0xc0, 0, 0, 0})).to_string() == "NaN");
        CHECK(num(bytes({0, 0, 0, 0, 0xd0, 0, 0, 0})).to_string() == "Infinity");
        CHECK(num(bytes({0, 0, 0, 0, 0xf0, 0, 0, 0})).to_string() == "-Infinity");
    }
    SECTION("bad input") {
        REQUIRE_THROWS_AS(numeric::parse(""), error);
        REQUIRE_THROWS_AS(numeric::parse("1.2.3"), error);
        REQUIRE_THROWS_AS(numeric::parse("abc"), error);
        REQUIRE_THROWS_AS(num(bytes({0, 1, 0, 0, 0, 0, 0, 0})), error);
        REQUIRE_THROWS_AS(num(bytes({0, 1, 0, 0, 0, 0, 0, 0, 0x27, 0x10})), error);  // 10000
        REQUIRE_THROWS_AS(num(bytes({0, 0, 0, 0, 0x12, 0x34, 0, 0})), error);
    }
    CHECK(codec<numeric>::param_oid == 1700);
    CHECK(codec<numeric>::accepts(ty(1700)));
}

TEST_CASE("decimal round trips through the binary form") {
    using D = decimal<6>;
    auto rt = [](std::int64_t u) { return dec<D>(enc(D{u}), 1700).units; };

    // NUMERIC(18,6) extremes
    CHECK(rt(999999999999999999) == 999999999999999999);
    CHECK(rt(-999999999999999999) == -999999999999999999);
    CHECK(rt(0) == 0);
    CHECK(rt(1) == 1);
    CHECK(rt(-1500000) == -1500000);
    CHECK(rt(std::numeric_limits<std::int64_t>::max()) == std::numeric_limits<std::int64_t>::max());
    CHECK(rt(std::numeric_limits<std::int64_t>::min()) == std::numeric_limits<std::int64_t>::min());

    CHECK(enc(D{1500000}) == bytes({0, 2, 0, 0, 0, 0, 0, 6, 0, 1, 0x13, 0x88}));
    CHECK(enc(D{0}) == bytes({0, 0, 0, 0, 0, 0, 0, 6}));
    CHECK(enc(decimal<2>{5}) == bytes({0, 1, 0xff, 0xff, 0, 0, 0, 2, 0x01, 0xf4}));  // 0.05 = 0500 e-4
    CHECK(codec<D>::param_oid == 1700);
    CHECK(codec<D>::accepts(ty(1700)));
    CHECK_FALSE(codec<D>::accepts(ty(701)));
}

TEST_CASE("decimal decoding is exact or throws") {
    auto wire = [](const char* text) { return enc(numeric::parse(text)); };
    CHECK(dec<decimal<2>>(wire("1.5000"), 1700).units == 150);
    CHECK(dec<decimal<2>>(wire("-0.07"), 1700).units == -7);
    CHECK(dec<decimal<0>>(wire("12.0"), 1700).units == 12);
    CHECK(dec<decimal<8>>(wire("0.00000001"), 1700).units == 1);
    CHECK(dec<decimal<18>>(wire("0.000000000000000001"), 1700).units == 1);
    CHECK(dec<decimal<0>>(wire("9223372036854775807"), 1700).units == std::numeric_limits<std::int64_t>::max());
    CHECK(dec<decimal<0>>(wire("-9223372036854775808"), 1700).units == std::numeric_limits<std::int64_t>::min());
    CHECK(dec<decimal<2>>(wire("100000000000"), 1700).units == 10000000000000);

    REQUIRE_THROWS_AS(dec<decimal<2>>(wire("1.005"), 1700), error);         // scale loss
    REQUIRE_THROWS_AS(dec<decimal<6>>(wire("0.0000001"), 1700), error);     // scale loss, tiny
    REQUIRE_THROWS_AS(dec<decimal<0>>(wire("9223372036854775808"), 1700), error);
    REQUIRE_THROWS_AS(dec<decimal<0>>(wire("-9223372036854775809"), 1700), error);
    REQUIRE_THROWS_AS(dec<decimal<18>>(wire("10"), 1700), error);          // 10 * 10^18 overflows
    REQUIRE_THROWS_AS(dec<decimal<2>>(wire("NaN"), 1700), error);
    REQUIRE_THROWS_AS(dec<decimal<2>>(wire("Infinity"), 1700), error);
    REQUIRE_THROWS_AS(dec<decimal<2>>(wire("-Infinity"), 1700), error);
    try {
        dec<decimal<2>>(wire("1.005"), 1700);
        FAIL("expected throw");
    } catch (const error& e) {
        CHECK(e.kind == error_kind::data);
    }
}

TEST_CASE("decimal text helpers") {
    CHECK(detail::format_scaled(-5, 2) == "-0.05");
    CHECK(detail::format_scaled(0, 3) == "0.000");
    CHECK(detail::format_scaled(123456, 0) == "123456");
    CHECK(detail::format_scaled(std::numeric_limits<std::int64_t>::min(), 0) == "-9223372036854775808");
    CHECK(decimal<6>::from_units(-12500000).to_string() == "-12.500000");
    CHECK(decimal<6>::parse("-12.5").units == -12500000);
    CHECK(decimal<2>::parse("1.50").units == 150);
    CHECK(decimal<0>::parse("7").units == 7);
    REQUIRE_THROWS_AS(decimal<6>::parse("1.1234567"), error);
    REQUIRE_THROWS_AS(decimal<0>::parse("9223372036854775808"), error);
    REQUIRE_THROWS_AS(decimal<2>::parse("x"), error);
    REQUIRE_THROWS_AS(decimal<2>::parse("NaN"), error);
}

TEST_CASE("uuid wire format and text") {
    uuid u = uuid::parse("550E8400-e29b-41d4-A716-446655440000");
    CHECK(u.bytes[0] == 0x55);
    CHECK(u.bytes[15] == 0x00);
    CHECK(u.to_string() == "550e8400-e29b-41d4-a716-446655440000");
    CHECK(enc(u) == bytes({0x55, 0x0e, 0x84, 0, 0xe2, 0x9b, 0x41, 0xd4, 0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0, 0}));
    CHECK(dec<uuid>(enc(u), 2950) == u);
    CHECK(codec<uuid>::param_oid == 2950);
    CHECK(codec<uuid>::accepts(ty(2950)));
    REQUIRE_THROWS_AS(uuid::parse("550e8400e29b41d4a716446655440000"), error);
    REQUIRE_THROWS_AS(uuid::parse("550e8400-e29b-41d4-a716-44665544000g"), error);
    REQUIRE_THROWS_AS(dec<uuid>("short", 2950), error);
}

TEST_CASE("bytea is raw bytes") {
    bytea b{std::byte{0}, std::byte{0xff}, std::byte{'a'}};
    CHECK(enc(b) == bytes({0, 0xff, 'a'}));
    CHECK(dec<bytea>(bytes({0, 0xff, 'a'}), 17) == b);
    CHECK(dec<bytea>("", 17).size() == 0);
    CHECK(codec<bytea>::param_oid == 17);
    CHECK(codec<bytea>::accepts(ty(17)));
}

TEST_CASE("timestamptz is microseconds since 2000-01-01") {
    constexpr std::int64_t pg_epoch_us = 946684800LL * 1000000;
    auto at = [](sys_days d) { return time_point_cast<microseconds>(d); };

    CHECK(enc(at(sys_days{year{2000} / 1 / 1})) == enc<std::int64_t>(0));
    CHECK(enc(at(sys_days{year{1970} / 1 / 1})) == enc<std::int64_t>(-pg_epoch_us));
    CHECK(dec<timestamptz>(enc<std::int64_t>(0), 1184) == at(sys_days{year{2000} / 1 / 1}));

    // before 2000 and before 1970
    timestamptz t1999 = at(sys_days{year{1999} / 12 / 31}) + hours(23) + minutes(59) + seconds(59) + microseconds(999999);
    CHECK(enc(t1999) == enc<std::int64_t>(-1));
    CHECK(dec<timestamptz>(enc<std::int64_t>(-1), 1184) == t1999);
    timestamptz t1969 = at(sys_days{year{1969} / 12 / 31}) + microseconds(1);
    CHECK(enc(t1969) == enc<std::int64_t>(-86400LL * 1000000 + 1 - pg_epoch_us));
    CHECK(dec<timestamptz>(enc(t1969), 1184) == t1969);
    timestamptz t1600 = at(sys_days{year{1600} / 3 / 1});
    CHECK(dec<timestamptz>(enc(t1600), 1184) == t1600);

    // infinities
    CHECK(enc(timestamptz::max()) == bytes({0x7f, 255, 255, 255, 255, 255, 255, 255}));
    CHECK(enc(timestamptz::min()) == bytes({0x80, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(dec<timestamptz>(bytes({0x7f, 255, 255, 255, 255, 255, 255, 255}), 1184) == timestamptz::max());
    CHECK(dec<timestamptz>(bytes({0x80, 0, 0, 0, 0, 0, 0, 0}), 1184) == timestamptz::min());

    // the epoch shift must not overflow
    REQUIRE_THROWS_AS(dec<timestamptz>(enc<std::int64_t>(std::numeric_limits<std::int64_t>::max() - 1), 1184), error);
    REQUIRE_THROWS_AS(enc(timestamptz(microseconds(std::numeric_limits<std::int64_t>::min() + 1))), error);
    REQUIRE_THROWS_AS(dec<timestamptz>("short", 1184), error);

    CHECK(codec<timestamptz>::param_oid == 1184);
    CHECK(codec<timestamptz>::accepts(ty(1184)));
    CHECK_FALSE(codec<timestamptz>::accepts(ty(1114)));
}

TEST_CASE("date is days since 2000-01-01") {
    CHECK(enc(sys_days{year{2000} / 1 / 1}) == enc<std::int32_t>(0));
    CHECK(enc(sys_days{year{1970} / 1 / 1}) == enc<std::int32_t>(-10957));
    CHECK(enc(sys_days{year{2000} / 1 / 2}) == enc<std::int32_t>(1));
    CHECK(dec<date>(enc<std::int32_t>(-10957), 1082) == sys_days{year{1970} / 1 / 1});
    CHECK(dec<date>(enc<std::int32_t>(366), 1082) == sys_days{year{2001} / 1 / 1});
    CHECK(enc(date::max()) == bytes({0x7f, 255, 255, 255}));
    CHECK(enc(date::min()) == bytes({0x80, 0, 0, 0}));
    CHECK(dec<date>(bytes({0x7f, 255, 255, 255}), 1082) == date::max());
    CHECK(dec<date>(bytes({0x80, 0, 0, 0}), 1082) == date::min());
    CHECK(codec<date>::param_oid == 1082);
    CHECK(codec<date>::accepts(ty(1082)));
    CHECK_FALSE(codec<date>::accepts(ty(1114)));
}

TEST_CASE("builtin_type table") {
    struct want {
        oid id;
        const char* name;
        char category;
        char kind;
    };
    for (want w : {want{16, "bool", 'B', 'b'}, {17, "bytea", 'U', 'b'}, {19, "name", 'S', 'b'}, {20, "int8", 'N', 'b'},
                   {21, "int2", 'N', 'b'}, {23, "int4", 'N', 'b'}, {25, "text", 'S', 'b'}, {26, "oid", 'N', 'b'},
                   {114, "json", 'U', 'b'}, {700, "float4", 'N', 'b'}, {701, "float8", 'N', 'b'},
                   {705, "unknown", 'X', 'p'}, {1042, "bpchar", 'S', 'b'}, {1043, "varchar", 'S', 'b'},
                   {1082, "date", 'D', 'b'}, {1114, "timestamp", 'D', 'b'}, {1184, "timestamptz", 'D', 'b'},
                   {1700, "numeric", 'N', 'b'}, {2950, "uuid", 'U', 'b'}, {3802, "jsonb", 'U', 'b'}}) {
        const type_desc* t = detail::builtin_type(w.id);
        REQUIRE(t != nullptr);
        CHECK(t->id == w.id);
        CHECK(t->name == w.name);
        CHECK(t->category == w.category);
        CHECK(t->kind == w.kind);
    }
    CHECK(detail::builtin_type(123456) == nullptr);
}

TEST_CASE("to_param and decode_value") {
    auto p = detail::to_param(std::int32_t{258});
    CHECK(p.type == 23);
    CHECK(p.binary);
    CHECK(p.bytes == bytes({0, 0, 1, 2}));

    auto s = detail::to_param(std::string("x"));
    CHECK(s.type == 0);
    CHECK_FALSE(s.binary);
    CHECK(s.bytes == "x");

    auto sv = detail::to_param(std::string_view("sv"));
    CHECK(sv.bytes == "sv");

    auto none = detail::to_param(std::optional<std::int64_t>{});
    CHECK(none.type == 20);
    CHECK_FALSE(none.bytes.has_value());
    auto some = detail::to_param(std::optional<std::int64_t>{1});
    CHECK(some.bytes == enc<std::int64_t>(1));

    type_desc int4{23, "int4", 'N', 'b'};
    std::string five = enc<std::int32_t>(5);
    CHECK(detail::decode_value<std::int32_t>(five.data(), five.size(), int4) == 5);
    CHECK(detail::decode_value<std::optional<std::int32_t>>(five.data(), five.size(), int4) == 5);
    CHECK_FALSE(detail::decode_value<std::optional<std::int32_t>>(nullptr, 0, int4).has_value());
    REQUIRE_THROWS_AS(detail::decode_value<std::int32_t>(nullptr, 0, int4), error);
    CHECK(detail::accepts<std::optional<std::int32_t>>(int4));
    CHECK_FALSE(detail::accepts<std::int64_t>(int4));
}
