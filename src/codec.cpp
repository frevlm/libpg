#include "pg/codec.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace pg {
namespace {

using namespace std::chrono;

[[noreturn]] void bad_value(const char* what, std::string_view s) {
    throw error(std::string("invalid ") + what + ": '" + std::string(s) + "'");
}

// Parses the whole of `s` as a number of type N; trailing junk and overflow throw.
template <class N>
N parse_number(const char* what, std::string_view s) {
    N v{};
    const char* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || p != end) bad_value(what, s);
    return v;
}

template <class I>
I decode_int(std::string_view s) {
    return parse_number<I>("integer", s);
}

// Walks a timestamp text left to right.
struct cursor {
    std::string_view all;
    std::size_t pos = 0;

    bool eat(char c) {
        const bool hit = pos < all.size() && all[pos] == c;
        pos += hit;
        return hit;
    }
    void expect(char c) {
        if (!eat(c)) bad_value("timestamptz", all);
    }
    // Reads exactly `digits` decimal digits.
    int digits(std::size_t digits) {
        if (pos + digits > all.size()) bad_value("timestamptz", all);
        int v = 0;
        for (std::size_t i = 0; i < digits; ++i, ++pos) {
            if (!std::isdigit(static_cast<unsigned char>(all[pos]))) bad_value("timestamptz", all);
            v = v * 10 + (all[pos] - '0');
        }
        return v;
    }
    // Reads a year of at least 4 digits.
    std::int64_t year_digits() {
        std::size_t n = 0;
        while (pos + n < all.size() && std::isdigit(static_cast<unsigned char>(all[pos + n]))) ++n;
        std::int64_t v = 0;
        if (n < 4 || n > 6) bad_value("timestamptz", all);
        for (std::size_t i = 0; i < n; ++i, ++pos) v = v * 10 + (all[pos] - '0');
        return v;
    }
    // Reads ".f" with 1 to 6 digits, scaled to microseconds; "" is 0.
    microseconds fraction() {
        if (!eat('.')) return microseconds{0};
        std::size_t n = 0;
        while (pos + n < all.size() && std::isdigit(static_cast<unsigned char>(all[pos + n]))) ++n;
        if (n < 1 || n > 6) bad_value("timestamptz", all);
        std::int64_t v = digits(n);
        for (std::size_t i = n; i < 6; ++i) v *= 10;
        return microseconds{v};
    }
    // Reads "+HH" or "-HH", optionally followed by ":MM".
    minutes utc_offset() {
        const bool minus = eat('-');
        if (!minus) expect('+');
        minutes m = hours{digits(2)};
        if (eat(':')) m += minutes{digits(2)};
        return minus ? -m : m;
    }
    void end() const {
        if (pos != all.size()) bad_value("timestamptz", all);
    }
};

template <class V>
std::string format_floating(V v) {
    std::array<char, 64> buf;
    auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    return std::string(buf.data(), r.ptr);
}

}  // namespace

std::string codec<bool>::encode(bool v) { return v ? "t" : "f"; }
bool codec<bool>::decode(std::string_view s) {
    if (s == "t") return true;
    if (s == "f") return false;
    bad_value("boolean", s);
}

std::string codec<std::int16_t>::encode(std::int16_t v) { return std::to_string(v); }
std::int16_t codec<std::int16_t>::decode(std::string_view s) { return decode_int<std::int16_t>(s); }
std::string codec<std::int32_t>::encode(std::int32_t v) { return std::to_string(v); }
std::int32_t codec<std::int32_t>::decode(std::string_view s) { return decode_int<std::int32_t>(s); }
std::string codec<std::int64_t>::encode(std::int64_t v) { return std::to_string(v); }
std::int64_t codec<std::int64_t>::decode(std::string_view s) { return decode_int<std::int64_t>(s); }

std::string codec<double>::encode(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
    return format_floating(v);
}
double codec<double>::decode(std::string_view s) {
    if (s == "NaN") return std::nan("");
    if (s == "Infinity") return HUGE_VAL;
    if (s == "-Infinity") return -HUGE_VAL;
    return parse_number<double>("float8", s);
}

std::string codec<std::string>::encode(const std::string& v) { return v; }
std::string codec<std::string>::decode(std::string_view s) { return std::string(s); }

std::string codec<numeric>::encode(const numeric& v) { return v.text; }
numeric codec<numeric>::decode(std::string_view s) { return numeric{std::string(s)}; }

std::string codec<jsonb>::encode(const jsonb& v) { return v.text; }
jsonb codec<jsonb>::decode(std::string_view s) { return jsonb{std::string(s)}; }

// Days since 1970-01-01 of a proleptic Gregorian date (astronomical year).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct civil {
    std::int64_t y;
    unsigned m, d;
};

civil civil_from_days(std::int64_t z) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return {static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2), m, d};
}

// Largest day count whose microseconds still fit in the timestamptz rep.
constexpr std::int64_t max_days = 106751990;

std::string codec<timestamptz>::encode(const timestamptz& v) {
    if (v == timestamptz::max()) return "infinity";
    if (v == timestamptz::min()) return "-infinity";
    const sys_days whole_days = floor<days>(v);
    const civil ymd = civil_from_days(whole_days.time_since_epoch().count());
    const hh_mm_ss<microseconds> tod{v - whole_days};
    const bool bc = ymd.y <= 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u %02d:%02d:%02d.%06d+00%s",
                  static_cast<long long>(bc ? 1 - ymd.y : ymd.y), ymd.m, ymd.d,
                  static_cast<int>(tod.hours().count()), static_cast<int>(tod.minutes().count()),
                  static_cast<int>(tod.seconds().count()), static_cast<int>(tod.subseconds().count()),
                  bc ? " BC" : "");
    return buf;
}

// Parses "YYYY-MM-DD HH:MM:SS[.f]+HH[:MM][ BC]", "infinity" or "-infinity" into UTC.
timestamptz codec<timestamptz>::decode(std::string_view s) {
    if (s == "infinity") return timestamptz::max();
    if (s == "-infinity") return timestamptz::min();
    cursor c{s};
    std::int64_t y = c.year_digits();
    c.expect('-');
    const unsigned mo = c.digits(2);
    c.expect('-');
    const unsigned d = c.digits(2);
    c.expect(' ');
    const hours h{c.digits(2)};
    c.expect(':');
    const minutes mi{c.digits(2)};
    c.expect(':');
    const seconds sec{c.digits(2)};
    const microseconds frac = c.fraction();
    const minutes off = c.utc_offset();
    if (c.pos != s.size()) {
        c.expect(' ');
        c.expect('B');
        c.expect('C');
        if (y == 0) bad_value("timestamptz", s);
        y = 1 - y;
    }
    c.end();
    const std::int64_t day = days_from_civil(y, mo, d);
    const civil back = civil_from_days(day);
    if (mo < 1 || mo > 12 || d < 1 || back.y != y || back.m != mo || back.d != d) bad_value("timestamptz", s);
    if (day > max_days || day < -max_days) bad_value("timestamptz", s);
    return sys_days{days{day}} + h + mi + sec + frac - off;
}

}  // namespace pg
