#include <algorithm>
#include <cstring>
#include <limits>

#include "pg/codec.hpp"

namespace pg {
namespace {

[[noreturn]] void bad_data(std::string_view type, std::string_view reason) {
    throw error(error_kind::data, std::string(type) + ": " + std::string(reason));
}

constexpr std::uint64_t pow10(int e) {
    std::uint64_t v = 1;
    for (int i = 0; i < e; ++i) v *= 10;
    return v;
}

constexpr std::uint16_t sign_neg = 0x4000;

// Adds d * 10^e units to mag; a fraction of a unit or an overflow throws.
std::uint64_t add_term(std::uint64_t mag, std::uint64_t d, int e) {
    if (e < 0) {
        if (e <= -4 || d % pow10(-e) != 0) bad_data("decimal", "value has digits beyond the scale");
        d /= pow10(-e);
    } else if (e > 18 || __builtin_mul_overflow(d, pow10(e), &d)) {
        bad_data("decimal", "value out of range");
    }
    if (__builtin_add_overflow(mag, d, &mag)) bad_data("decimal", "value out of range");
    return mag;
}

std::int64_t signed_units(std::uint64_t mag, bool negative) {
    constexpr std::uint64_t limit = std::uint64_t{1} << 63;
    if (mag > limit || (mag == limit && !negative)) bad_data("decimal", "value out of range");
    return negative ? static_cast<std::int64_t>(0 - mag) : static_cast<std::int64_t>(mag);
}

std::uint64_t magnitude(std::int64_t v) {
    return v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
}

int hex_value(char c) {
    const std::size_t at = std::string_view("0123456789abcdef0123456789ABCDEF").find(c);
    return at == std::string_view::npos ? -1 : static_cast<int>(at % 16);
}

constexpr std::int64_t pg_epoch_days = 10957;  // 1970-01-01 -> 2000-01-01
constexpr std::int64_t pg_epoch_us = pg_epoch_days * 86'400'000'000;

}  // namespace

namespace detail {

std::uint64_t get_be(std::string_view b, std::size_t bytes) {
    if (b.size() != bytes)
        bad_data("binary value", "expected " + std::to_string(bytes) + " bytes, got " + std::to_string(b.size()));
    std::uint64_t v = 0;
    for (unsigned char c : b) v = (v << 8) | c;
    return v;
}

std::string array_builder::finish() const {
    std::string out;
    put_be(count ? 1 : 0, 4, out);
    put_be(nulls ? 1 : 0, 4, out);
    put_be(element, 4, out);
    if (count) {
        put_be(count, 4, out);
        put_be(1, 4, out);  // lower bound
    }
    return out + body;
}

std::int64_t parse_scaled(std::string_view text, int scale) {
    std::string_view t = text;
    const bool neg = t.starts_with('-');
    if (neg || t.starts_with('+')) t.remove_prefix(1);
    std::uint64_t mag = 0;
    int frac = -1;  // digits taken after the point; -1 before the point
    bool any = false;
    for (char c : t) {
        if (c == '.' && frac < 0) {
            frac = 0;
            continue;
        }
        if (c < '0' || c > '9') bad_data("decimal", "invalid text '" + std::string(text) + "'");
        any = true;
        if (frac >= scale) {
            if (c != '0') bad_data("decimal", "value has digits beyond the scale");
            continue;
        }
        if (frac >= 0) ++frac;
        mag = add_term(add_term(0, mag, 1), static_cast<std::uint64_t>(c - '0'), 0);
    }
    if (!any) bad_data("decimal", "invalid text '" + std::string(text) + "'");
    return signed_units(add_term(0, mag, scale - std::max(frac, 0)), neg);
}

std::string format_scaled(std::int64_t units, int scale) {
    std::string digits = std::to_string(magnitude(units));
    const auto s = static_cast<std::size_t>(scale);
    if (digits.size() <= s) digits.insert(0, s + 1 - digits.size(), '0');
    if (s > 0) digits.insert(digits.size() - s, 1, '.');
    return units < 0 ? "-" + digits : digits;
}

void encode_scaled(std::int64_t units, int scale, std::string& out) {
    std::uint64_t m = magnitude(units), div = 10'000, mul = 1;
    const int pad = (4 - scale % 4) % 4;  // base-10000 groups count from the point: the lowest one is short
    for (int i = 0; i < pad; ++i) div /= 10, mul *= 10;
    int g[6], n = 0;  // the groups, lowest first
    for (; m; m /= div, div = 10'000, mul = 1) g[n++] = static_cast<int>(m % div * mul);
    int low = 0;  // trailing zero groups are left out
    while (low < n && g[low] == 0) ++low;
    put_be(static_cast<std::uint64_t>(n - low), 2, out);
    put_be(static_cast<std::uint16_t>(n > low ? n - 1 - (scale + pad) / 4 : 0), 2, out);
    put_be(units < 0 && n > low ? sign_neg : 0, 2, out);
    put_be(static_cast<std::uint64_t>(scale), 2, out);
    for (int i = n; i-- > low;) put_be(static_cast<std::uint64_t>(g[i]), 2, out);
}

std::int64_t decode_scaled(std::string_view b, int scale) {
    if (b.size() < 8) bad_data("numeric", "shorter than its header");
    const auto count = static_cast<std::int16_t>(get_be(b.substr(0, 2), 2));
    const auto weight = static_cast<std::int16_t>(get_be(b.substr(2, 2), 2));
    const auto sign = get_be(b.substr(4, 2), 2);
    if (count < 0 || b.size() != 8 + 2 * static_cast<std::size_t>(count))
        bad_data("numeric", "length does not match the digit count");
    if (sign != 0 && sign != sign_neg) bad_data("decimal", "NaN and infinity have no fixed-point value");
    std::uint64_t mag = 0;
    for (int i = 0; i < count; ++i) {
        const std::uint64_t d = get_be(b.substr(8 + 2 * static_cast<std::size_t>(i), 2), 2);
        if (d > 9999) bad_data("numeric", "digit group out of range");
        if (d != 0) mag = add_term(mag, d, 4 * (weight - i) + scale);
    }
    return signed_units(mag, sign == sign_neg);
}

}  // namespace detail

std::string uuid::to_string() const {
    static constexpr char hex[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += hex[bytes[i] >> 4];
        s += hex[bytes[i] & 15];
    }
    return s;
}

uuid uuid::parse(std::string_view text) {
    uuid u;
    std::size_t n = 0;
    for (std::size_t i = 0; text.size() == 36 && n < 16; i += 2) {
        if ((i == 8 || i == 13 || i == 18 || i == 23) && text[i++] != '-') break;
        const int hi = hex_value(text[i]), lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) break;
        u.bytes[n++] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    if (n != 16) bad_data("uuid", "invalid text '" + std::string(text) + "'");
    return u;
}

void codec<bool>::encode(const bool& v, std::string& out) { out.push_back(v ? 1 : 0); }
bool codec<bool>::decode(std::string_view b) { return detail::get_be(b, 1) != 0; }

void codec<bytea>::encode(const bytea& v, std::string& out) {
    out.append(reinterpret_cast<const char*>(v.data()), v.size());
}
bytea codec<bytea>::decode(std::string_view b) {
    const auto* p = reinterpret_cast<const std::byte*>(b.data());
    return bytea(p, p + b.size());
}

void codec<jsonb>::encode(const jsonb& v, std::string& out) {
    out.push_back(1);
    out += v.text;
}
jsonb codec<jsonb>::decode(std::string_view b) {
    if (b.empty() || b[0] != 1) bad_data("jsonb", "unsupported binary version");
    return {std::string(b.substr(1))};
}

void codec<uuid>::encode(const uuid& v, std::string& out) {
    out.append(reinterpret_cast<const char*>(v.bytes.data()), v.bytes.size());
}
uuid codec<uuid>::decode(std::string_view b) {
    if (b.size() != 16) bad_data("uuid", "expected 16 bytes");
    uuid u;
    std::memcpy(u.bytes.data(), b.data(), 16);
    return u;
}

void codec<timestamptz>::encode(const timestamptz& v, std::string& out) {
    using lim = std::numeric_limits<std::int64_t>;
    std::int64_t us = v.time_since_epoch().count();
    if (v == timestamptz::max()) us = lim::max();
    else if (v == timestamptz::min()) us = lim::min();
    else if (__builtin_sub_overflow(us, pg_epoch_us, &us)) bad_data("timestamptz", "value out of range");
    detail::put_be(static_cast<std::uint64_t>(us), 8, out);
}
timestamptz codec<timestamptz>::decode(std::string_view b) {
    using lim = std::numeric_limits<std::int64_t>;
    auto us = static_cast<std::int64_t>(detail::get_be(b, 8));
    if (us == lim::max()) return timestamptz::max();
    if (us == lim::min()) return timestamptz::min();
    if (__builtin_add_overflow(us, pg_epoch_us, &us) || us == lim::max() || us == lim::min())
        bad_data("timestamptz", "value out of range");
    return timestamptz(std::chrono::microseconds(us));
}

void codec<date>::encode(const date& v, std::string& out) {
    using lim = std::numeric_limits<std::int32_t>;
    std::int64_t days = static_cast<std::int64_t>(v.time_since_epoch().count()) - pg_epoch_days;
    if (v == date::max()) days = lim::max();
    else if (v == date::min()) days = lim::min();
    else if (days <= lim::min() || days >= lim::max()) bad_data("date", "value out of range");
    detail::put_be(static_cast<std::uint32_t>(days), 4, out);
}
date codec<date>::decode(std::string_view b) {
    using lim = std::numeric_limits<std::int32_t>;
    const auto days = static_cast<std::int32_t>(detail::get_be(b, 4));
    if (days == lim::max()) return date::max();
    if (days == lim::min()) return date::min();
    return date(std::chrono::days(days + pg_epoch_days));
}

}  // namespace pg
