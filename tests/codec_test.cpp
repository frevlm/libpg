#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "pg/pg.hpp"

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
T dec(const std::string& b) {
    return codec<T>::decode(b);
}

// The binary numeric of a decimal text, with as many fractional digits as it has.
std::string numeric_wire(std::string_view text) {
    const std::size_t point = text.find('.');
    const std::size_t scale = point == std::string_view::npos ? 0 : text.size() - point - 1;
    std::string out;
    detail::encode_scaled(detail::parse_scaled(text, static_cast<int>(scale)), static_cast<int>(scale), out);
    return out;
}

}  // namespace

TEST_CASE("bool wire format") {
    REQUIRE(enc(true) == bytes({1}));
    REQUIRE(enc(false) == bytes({0}));
    REQUIRE(dec<bool>(bytes({1})) == true);
    REQUIRE(dec<bool>(bytes({0})) == false);
    REQUIRE(codec<bool>::type == 16);
    REQUIRE(codec<bool>::accepts(16));
    REQUIRE_FALSE(codec<bool>::accepts(23));
    REQUIRE_THROWS_AS(dec<bool>(bytes({1, 0})), error);
}

TEST_CASE("integers accept exactly their own width") {
    REQUIRE(enc<std::int16_t>(-2) == bytes({0xff, 0xfe}));
    REQUIRE(enc<std::int32_t>(0x01020304) == bytes({1, 2, 3, 4}));
    REQUIRE(enc<std::int64_t>(-1) == bytes({255, 255, 255, 255, 255, 255, 255, 255}));
    REQUIRE(dec<std::int16_t>(bytes({0x80, 0})) == std::numeric_limits<std::int16_t>::min());
    REQUIRE(dec<std::int32_t>(bytes({0xff, 0xff, 0xff, 0xfe})) == -2);
    REQUIRE(dec<std::int64_t>(enc<std::int64_t>(std::numeric_limits<std::int64_t>::min())) ==
            std::numeric_limits<std::int64_t>::min());
    REQUIRE(codec<std::int16_t>::type == 21);
    REQUIRE(codec<std::int32_t>::type == 23);
    REQUIRE(codec<std::int64_t>::type == 20);
    REQUIRE(codec<std::int32_t>::accepts(23));
    REQUIRE_FALSE(codec<std::int32_t>::accepts(20));
    REQUIRE_FALSE(codec<std::int32_t>::accepts(21));
    REQUIRE_FALSE(codec<std::int64_t>::accepts(23));
    REQUIRE_THROWS_AS(dec<std::int32_t>(bytes({1, 2})), error);
}

TEST_CASE("both 64-bit signed integer types use the int8 codec") {
    REQUIRE(codec<long>::type == 20);
    REQUIRE(codec<long long>::type == 20);
    REQUIRE(codec<long>::accepts(20));
    REQUIRE_FALSE(codec<long long>::accepts(23));
    REQUIRE(enc<long>(-1) == enc<std::int64_t>(-1));
    REQUIRE(enc<long long>(-1) == enc<std::int64_t>(-1));
    REQUIRE(dec<long>(enc<std::int64_t>(-2)) == -2);
    REQUIRE(dec<long long>(enc<std::int64_t>(-2)) == -2);
}

TEST_CASE("float wire format") {
    REQUIRE(enc(1.0f) == bytes({0x3f, 0x80, 0, 0}));
    REQUIRE(enc(1.0) == bytes({0x3f, 0xf0, 0, 0, 0, 0, 0, 0}));
    REQUIRE(dec<double>(enc(0.1)) == 0.1);
    REQUIRE(dec<float>(enc(-2.5f)) == -2.5f);
    REQUIRE(std::isinf(dec<double>(enc(std::numeric_limits<double>::infinity()))));
    REQUIRE(std::isnan(dec<double>(enc(std::nan("")))));
    REQUIRE(codec<float>::type == 700);
    REQUIRE(codec<double>::type == 701);
    REQUIRE(codec<double>::accepts(701));
    REQUIRE_FALSE(codec<double>::accepts(700));
}

TEST_CASE("string is binary text and reads the text-like built-in types") {
    REQUIRE(codec<std::string>::type == 25);
    REQUIRE(enc(std::string("héllo")) == "héllo");
    REQUIRE(dec<std::string>("abc") == "abc");
    for (oid t : {25u, 1043u, 1042u, 19u}) REQUIRE(codec<std::string>::accepts(t));
    REQUIRE_FALSE(codec<std::string>::accepts(23));
    REQUIRE_FALSE(codec<std::string>::accepts(3802));
    REQUIRE(codec<std::string_view>::type == 25);
    REQUIRE(codec<const char*>::type == 25);
    REQUIRE(codec<char*>::type == 25);
}

TEST_CASE("jsonb carries the version byte") {
    REQUIRE(codec<jsonb>::type == 3802);
    REQUIRE(enc(jsonb{"{\"a\":1}"}) == bytes({1}) + "{\"a\":1}");
    REQUIRE(dec<jsonb>(bytes({1}) + "{\"a\": 1}").text == "{\"a\": 1}");
    REQUIRE(codec<jsonb>::accepts(3802));
    REQUIRE_FALSE(codec<jsonb>::accepts(114));
    REQUIRE_THROWS_AS(dec<jsonb>(bytes({2}) + "{}"), error);
    REQUIRE_THROWS_AS(dec<jsonb>(""), error);
}

TEST_CASE("decimal binary form") {
    using D = decimal<6>;
    REQUIRE(enc(D{1500000}) == bytes({0, 2, 0, 0, 0, 0, 0, 6, 0, 1, 0x13, 0x88}));
    REQUIRE(enc(D{0}) == bytes({0, 0, 0, 0, 0, 0, 0, 6}));
    REQUIRE(enc(decimal<2>{5}) == bytes({0, 1, 0xff, 0xff, 0, 0, 0, 2, 0x01, 0xf4}));  // 0.05 = 0500e-4
    REQUIRE(enc(decimal<2>{-1234}) == bytes({0, 2, 0, 0, 0x40, 0, 0, 2, 0, 12, 0x0d, 0x48}));
    REQUIRE(enc(decimal<0>{100000000}) == bytes({0, 1, 0, 2, 0, 0, 0, 0, 0, 1}));
    REQUIRE(codec<D>::type == 1700);
    REQUIRE(codec<D>::accepts(1700));
    REQUIRE_FALSE(codec<D>::accepts(701));
    for (std::int64_t u : {std::int64_t{999999999999999999}, std::int64_t{-999999999999999999}, std::int64_t{0},
                           std::int64_t{1}, std::int64_t{-1500000}, std::numeric_limits<std::int64_t>::max(),
                           std::numeric_limits<std::int64_t>::min()})
        REQUIRE(dec<D>(enc(D{u})).units == u);
}

TEST_CASE("decimal decoding is exact or throws") {
    REQUIRE(dec<decimal<2>>(numeric_wire("1.5000")).units == 150);
    REQUIRE(dec<decimal<2>>(numeric_wire("-0.07")).units == -7);
    REQUIRE(dec<decimal<0>>(numeric_wire("12.0")).units == 12);
    REQUIRE(dec<decimal<18>>(numeric_wire("0.000000000000000001")).units == 1);
    REQUIRE(dec<decimal<0>>(numeric_wire("9223372036854775807")).units == std::numeric_limits<std::int64_t>::max());
    REQUIRE(dec<decimal<0>>(numeric_wire("-9223372036854775808")).units == std::numeric_limits<std::int64_t>::min());
    REQUIRE(dec<decimal<2>>(numeric_wire("100000000000")).units == 10000000000000);

    REQUIRE_THROWS_AS(dec<decimal<2>>(numeric_wire("1.234")), error);
    REQUIRE_THROWS_AS(dec<decimal<6>>(numeric_wire("0.0000001")), error);
    REQUIRE_THROWS_AS(dec<decimal<18>>(numeric_wire("10")), error);
    REQUIRE_THROWS_AS(dec<decimal<2>>(bytes({0, 0, 0, 0, 0xc0, 0, 0, 0})), error);  // NaN
    REQUIRE_THROWS_AS(dec<decimal<2>>(bytes({0, 0, 0, 0, 0xd0, 0, 0, 0})), error);  // Infinity
    REQUIRE_THROWS_AS(dec<decimal<2>>(bytes({0, 1, 0, 0, 0, 0, 0, 0})), error);     // short
    REQUIRE_THROWS_AS(dec<decimal<2>>(bytes({0, 1, 0, 0, 0, 0, 0, 0, 0x27, 0x10})), error);  // group 10000
    try {
        dec<decimal<2>>(numeric_wire("1.234"));
        FAIL("1.234 was read as decimal<2>");
    } catch (const error& e) {
        REQUIRE(e.kind == error_kind::data);
    }
}

TEST_CASE("decimal text") {
    REQUIRE(decimal<2>{-5}.to_string() == "-0.05");
    REQUIRE(decimal<3>{0}.to_string() == "0.000");
    REQUIRE(decimal<0>{123456}.to_string() == "123456");
    REQUIRE(decimal<0>{std::numeric_limits<std::int64_t>::min()}.to_string() == "-9223372036854775808");
    REQUIRE(decimal<6>::from_units(-12500000).to_string() == "-12.500000");
    REQUIRE(decimal<6>::parse("-12.5").units == -12500000);
    REQUIRE(decimal<2>::parse("1.50").units == 150);
    REQUIRE(decimal<2>::parse("+.5").units == 50);
    REQUIRE(decimal<2>::parse("3.100").units == 310);
    REQUIRE(decimal<0>::parse("7").units == 7);
    REQUIRE_THROWS_AS(decimal<6>::parse("1.1234567"), error);
    REQUIRE_THROWS_AS(decimal<0>::parse("9223372036854775808"), error);
    REQUIRE_THROWS_AS(decimal<2>::parse("x"), error);
    REQUIRE_THROWS_AS(decimal<2>::parse("1.2.3"), error);
    REQUIRE_THROWS_AS(decimal<2>::parse(""), error);
    REQUIRE_THROWS_AS(decimal<2>::parse("NaN"), error);
    REQUIRE(decimal<6>::from_units(3500000).to_double() == 3.5);
    REQUIRE(decimal<2>::parse("1.25") + decimal<2>::parse("0.75") == decimal<2>::parse("2"));
    REQUIRE(decimal<2>{1} < decimal<2>{2});
}

TEST_CASE("a vector is a one-dimensional array parameter of its element type") {
    using V = std::vector<std::int32_t>;
    REQUIRE(codec<V>::type == 1007);
    REQUIRE(codec<std::vector<std::int64_t>>::type == 1016);
    REQUIRE(codec<std::vector<long long>>::type == 1016);
    REQUIRE(codec<std::vector<std::string>>::type == 1009);
    REQUIRE(codec<std::vector<decimal<6>>>::type == 1231);
    REQUIRE(codec<std::vector<bool>>::type == 1000);
    REQUIRE(codec<std::vector<std::optional<std::int16_t>>>::type == 1005);
    REQUIRE(enc(V{3, 1, 2}) == bytes({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 23, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0,
                                      0, 3, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 2}));
    REQUIRE(enc(V{}) == bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 23}));
    REQUIRE(enc(std::vector<bool>{true, false}) ==
            bytes({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0}));
    REQUIRE(enc(std::vector<std::optional<std::int32_t>>{7, std::nullopt}) ==
            bytes({0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 23, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 7, 255, 255,
                   255, 255}));
}

TEST_CASE("uuid wire format and text") {
    const uuid u = uuid::parse("550E8400-e29b-41d4-A716-446655440000");
    REQUIRE(u.bytes[0] == 0x55);
    REQUIRE(u.to_string() == "550e8400-e29b-41d4-a716-446655440000");
    REQUIRE(enc(u) == bytes({0x55, 0x0e, 0x84, 0, 0xe2, 0x9b, 0x41, 0xd4, 0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0, 0}));
    REQUIRE(dec<uuid>(enc(u)) == u);
    REQUIRE(codec<uuid>::type == 2950);
    REQUIRE_THROWS_AS(uuid::parse("550e8400e29b41d4a716446655440000"), error);
    REQUIRE_THROWS_AS(uuid::parse("550e8400-e29b-41d4-a716-44665544000g"), error);
    REQUIRE_THROWS_AS(dec<uuid>("short"), error);
}

TEST_CASE("bytea is raw bytes") {
    const bytea b{std::byte{0}, std::byte{0xff}, std::byte{'a'}};
    REQUIRE(enc(b) == bytes({0, 0xff, 'a'}));
    REQUIRE(dec<bytea>(bytes({0, 0xff, 'a'})) == b);
    REQUIRE(dec<bytea>("").empty());
    REQUIRE(codec<bytea>::type == 17);
}

TEST_CASE("timestamptz is microseconds since 2000-01-01") {
    constexpr std::int64_t pg_epoch_us = 946684800LL * 1000000;
    auto at = [](sys_days d) { return time_point_cast<microseconds>(d); };
    REQUIRE(enc(at(sys_days{year{2000} / 1 / 1})) == enc<std::int64_t>(0));
    REQUIRE(enc(at(sys_days{year{1970} / 1 / 1})) == enc<std::int64_t>(-pg_epoch_us));
    const timestamptz t1999 = at(sys_days{year{1999} / 12 / 31}) + hours(23) + minutes(59) + seconds(59) + microseconds(999999);
    REQUIRE(enc(t1999) == enc<std::int64_t>(-1));
    REQUIRE(dec<timestamptz>(enc<std::int64_t>(-1)) == t1999);
    const timestamptz t1600 = at(sys_days{year{1600} / 3 / 1});
    REQUIRE(dec<timestamptz>(enc(t1600)) == t1600);
    REQUIRE(enc(timestamptz::max()) == bytes({0x7f, 255, 255, 255, 255, 255, 255, 255}));
    REQUIRE(enc(timestamptz::min()) == bytes({0x80, 0, 0, 0, 0, 0, 0, 0}));
    REQUIRE(dec<timestamptz>(enc(timestamptz::max())) == timestamptz::max());
    REQUIRE(dec<timestamptz>(enc(timestamptz::min())) == timestamptz::min());
    REQUIRE_THROWS_AS(dec<timestamptz>(enc<std::int64_t>(std::numeric_limits<std::int64_t>::max() - 1)), error);
    REQUIRE_THROWS_AS(enc(timestamptz(microseconds(std::numeric_limits<std::int64_t>::min() + 1))), error);
    REQUIRE(codec<timestamptz>::type == 1184);
    REQUIRE_FALSE(codec<timestamptz>::accepts(1114));
}

TEST_CASE("date is days since 2000-01-01") {
    REQUIRE(enc(sys_days{year{2000} / 1 / 1}) == enc<std::int32_t>(0));
    REQUIRE(enc(sys_days{year{1970} / 1 / 1}) == enc<std::int32_t>(-10957));
    REQUIRE(dec<date>(enc<std::int32_t>(366)) == sys_days{year{2001} / 1 / 1});
    REQUIRE(enc(date::max()) == bytes({0x7f, 255, 255, 255}));
    REQUIRE(dec<date>(bytes({0x80, 0, 0, 0})) == date::min());
    REQUIRE(codec<date>::type == 1082);
    REQUIRE_FALSE(codec<date>::accepts(1114));
}

TEST_CASE("to_param binds every value binary with its type") {
    const auto p = detail::to_param(std::int32_t{258});
    REQUIRE(p.type == 23);
    REQUIRE(p.bytes == bytes({0, 0, 1, 2}));
    REQUIRE(detail::to_param(std::string("x")) == detail::param{25, std::string("x")});
    REQUIRE(detail::to_param(std::string_view("sv")) == detail::param{25, std::string("sv")});
    REQUIRE(detail::to_param(std::optional<std::int64_t>{}) == detail::param{20, std::nullopt});
    REQUIRE(detail::to_param(std::optional<long long>{7}) == detail::param{20, enc<std::int64_t>(7)});
}

TEST_CASE("SQLSTATEs classify into kinds") {
    REQUIRE(classify("40001") == error_kind::conflict);
    REQUIRE(classify("40P01") == error_kind::conflict);
    REQUIRE(classify("25006") == error_kind::read_only);
    REQUIRE(classify("26000") == error_kind::stale_statement);
    REQUIRE(classify("0A000", "cached plan must not change result type") == error_kind::stale_statement);
    REQUIRE(classify("0A000", "FOR UPDATE is not allowed with aggregate functions") == error_kind::other);
    REQUIRE(classify("55P03") == error_kind::lock_or_cancel);
    REQUIRE(classify("57014") == error_kind::lock_or_cancel);
    REQUIRE(classify("22012") == error_kind::data);
    REQUIRE(classify("23505") == error_kind::data);
    REQUIRE(classify("08006") == error_kind::broken);
    REQUIRE(classify("42601") == error_kind::other);
    REQUIRE(retryable(error_kind::broken));
    REQUIRE(retryable(error_kind::conflict));
    REQUIRE(retryable(error_kind::stale_statement));
    REQUIRE(retryable(error_kind::read_only));
    REQUIRE_FALSE(retryable(error_kind::commit_unknown));
    REQUIRE_FALSE(retryable(error_kind::data));
}
