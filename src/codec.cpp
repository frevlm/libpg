#include "pg/codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>

namespace pg {
namespace {

[[noreturn]] void fail(error_kind k, std::string_view type, std::string_view reason) {
    throw error(k, std::string(type) + ": " + std::string(reason));
}
[[noreturn]] void bad_data(std::string_view type, std::string_view reason) {
    fail(error_kind::data, type, reason);
}

// ---- big-endian integers -------------------------------------------------

void put_be(std::uint64_t v, int bytes, std::string& out) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

std::uint64_t get_be(std::string_view b, std::size_t off, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | static_cast<unsigned char>(b[off + i]);
    return v;
}

void need_len(std::string_view b, std::size_t n, std::string_view type) {
    if (b.size() != n)
        bad_data(type, "expected " + std::to_string(n) + " bytes, got " + std::to_string(b.size()));
}

template <class T>
void put_int(T v, std::string& out) {
    put_be(static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(v)), sizeof(T), out);
}

template <class T>
T get_int(std::string_view b, std::string_view type) {
    need_len(b, sizeof(T), type);
    return static_cast<T>(static_cast<std::make_unsigned_t<T>>(get_be(b, 0, sizeof(T))));
}

// ---- powers of ten -------------------------------------------------------

constexpr std::uint64_t pow10(int e) {
    std::uint64_t v = 1;
    for (int i = 0; i < e; ++i) v *= 10;
    return v;
}

// ---- numeric internals ---------------------------------------------------

constexpr std::uint16_t sign_pos = 0x0000, sign_neg = 0x4000, sign_nan = 0xC000, sign_pinf = 0xD000,
                        sign_ninf = 0xF000;

// Strips leading and trailing zero groups; a zero value becomes weight 0, positive.
void normalize(numeric& n) {
    std::size_t lead = 0;
    while (lead < n.digits.size() && n.digits[lead] == 0) ++lead;
    if (lead == n.digits.size()) {
        n.digits.clear();
        n.weight = 0;
        n.negative = false;
        return;
    }
    n.digits.erase(n.digits.begin(), n.digits.begin() + static_cast<std::ptrdiff_t>(lead));
    n.weight = static_cast<std::int16_t>(n.weight - static_cast<int>(lead));
    while (n.digits.back() == 0) n.digits.pop_back();
}

void encode_numeric(const numeric& n, std::string& out) {
    std::uint16_t sign = sign_pos;
    std::size_t count = 0;
    std::int16_t weight = 0;
    std::uint16_t dscale = 0;
    switch (n.kind) {
        case numeric::special::nan: sign = sign_nan; break;
        case numeric::special::pos_inf: sign = sign_pinf; break;
        case numeric::special::neg_inf: sign = sign_ninf; break;
        case numeric::special::none:
            count = n.digits.size();
            if (count > 0) weight = n.weight;
            if (count > 0 && n.negative) sign = sign_neg;
            dscale = n.dscale;
            break;
    }
    if (count > 32767) bad_data("numeric", "too many digit groups");
    put_int<std::int16_t>(static_cast<std::int16_t>(count), out);
    put_int<std::int16_t>(weight, out);
    put_int<std::uint16_t>(sign, out);
    put_int<std::uint16_t>(dscale, out);
    for (std::size_t i = 0; i < count; ++i) {
        if (n.digits[i] < 0 || n.digits[i] > 9999) bad_data("numeric", "digit group out of range 0..9999");
        put_int<std::int16_t>(n.digits[i], out);
    }
}

// The 8 byte header of a binary numeric, its length checked against the digit count.
struct numeric_header {
    int count;
    std::int16_t weight;
    std::uint16_t sign, dscale;
};

numeric_header read_header(std::string_view b) {
    if (b.size() < 8) bad_data("numeric", "shorter than the 8 byte header");
    const int count = static_cast<std::int16_t>(get_be(b, 0, 2));
    if (count < 0 || b.size() != 8 + 2 * static_cast<std::size_t>(count))
        bad_data("numeric", "length does not match the digit count");
    return {count, static_cast<std::int16_t>(get_be(b, 2, 2)), static_cast<std::uint16_t>(get_be(b, 4, 2)),
            static_cast<std::uint16_t>(get_be(b, 6, 2))};
}

// Digit group i of a binary numeric whose header was read.
std::int16_t digit_at(std::string_view b, int i) {
    const auto d = static_cast<std::int16_t>(get_be(b, 8 + 2 * static_cast<std::size_t>(i), 2));
    if (d < 0 || d > 9999) bad_data("numeric", "digit group out of range 0..9999");
    return d;
}

numeric decode_numeric(std::string_view b) {
    const numeric_header h = read_header(b);
    numeric n;
    n.weight = h.weight;
    n.dscale = h.dscale;
    switch (h.sign) {
        case sign_pos: break;
        case sign_neg: n.negative = true; break;
        case sign_nan: n.kind = numeric::special::nan; return {n.kind, false, 0, 0, {}};
        case sign_pinf: n.kind = numeric::special::pos_inf; return {n.kind, false, 0, 0, {}};
        case sign_ninf: n.kind = numeric::special::neg_inf; return {n.kind, false, 0, 0, {}};
        default: bad_data("numeric", "unknown sign word");
    }
    n.digits.reserve(static_cast<std::size_t>(h.count));
    for (int i = 0; i < h.count; ++i) n.digits.push_back(digit_at(b, i));
    normalize(n);
    return n;
}

// Adds d * 10^e (an exact count of units) to mag, or throws when it does not fit or loses digits.
std::uint64_t add_term(std::uint64_t mag, int d, int e) {
    std::uint64_t term = static_cast<std::uint64_t>(d);
    if (e < 0) {
        if (e <= -4 || term % pow10(-e) != 0) bad_data("decimal", "value has digits beyond the scale");
        term /= pow10(-e);
    } else if (e > 18 || __builtin_mul_overflow(term, pow10(e), &term)) {
        bad_data("decimal", "value out of range");
    }
    if (__builtin_add_overflow(mag, term, &mag)) bad_data("decimal", "value out of range");
    return mag;
}

// The units of a magnitude with a sign, or throws when out of range.
std::int64_t signed_units(std::uint64_t mag, bool negative) {
    constexpr std::uint64_t limit = std::uint64_t{1} << 63;
    if (negative) {
        if (mag > limit) bad_data("decimal", "value out of range");
        return static_cast<std::int64_t>(0 - mag);
    }
    if (mag >= limit) bad_data("decimal", "value out of range");
    return static_cast<std::int64_t>(mag);
}

std::int64_t to_scaled(const numeric& n, int scale) {
    if (n.kind != numeric::special::none) bad_data("decimal", "NaN and infinity have no fixed-point value");
    std::uint64_t mag = 0;
    for (std::size_t i = 0; i < n.digits.size(); ++i) {
        if (n.digits[i] == 0) continue;
        mag = add_term(mag, n.digits[i], 4 * (n.weight - static_cast<int>(i)) + scale);
    }
    return signed_units(mag, n.negative);
}

// Base-10000 digit groups of a fixed-point value, most significant first,
// without leading or trailing zero groups.
struct scaled_groups {
    std::array<std::int16_t, 12> d{};  // 19 decimal digits: at most 5 whole and 5 fractional groups
    int count = 0;
    int weight = 0;  // power of 10000 of d[0]
};

// Groups, least significant first, appended to g most significant first.
struct low_first {
    std::array<std::int16_t, 6> d{};
    int n = 0;
    void push(std::uint64_t v) { d[static_cast<std::size_t>(n++)] = static_cast<std::int16_t>(v); }
    void move_to(scaled_groups& g) {
        while (n > 0) g.d[static_cast<std::size_t>(g.count++)] = d[static_cast<std::size_t>(--n)];
    }
};

scaled_groups groups_of(std::uint64_t mag, int scale) {
    scaled_groups g;
    low_first whole;
    for (std::uint64_t v = mag / pow10(scale); v != 0; v /= 10000) whole.push(v % 10000);
    whole.move_to(g);
    g.weight = g.count - 1;
    // Fractional groups count from the point: a partial last group is padded with zeros.
    low_first frac;
    std::uint64_t f = mag % pow10(scale);
    if (const int r = scale % 4; r != 0) {
        frac.push((f % pow10(r)) * pow10(4 - r));
        f /= pow10(r);
    }
    while (frac.n < (scale + 3) / 4) {
        frac.push(f % 10000);
        f /= 10000;
    }
    frac.move_to(g);
    int lead = 0;
    while (lead < g.count && g.d[static_cast<std::size_t>(lead)] == 0) ++lead;
    std::copy(g.d.begin() + lead, g.d.begin() + g.count, g.d.begin());
    g.count -= lead;
    g.weight = g.count == 0 ? 0 : g.weight - lead;
    while (g.count > 0 && g.d[static_cast<std::size_t>(g.count - 1)] == 0) --g.count;
    return g;
}

void append_group(std::string& s, int g) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04d", g);
    s += buf;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Splits "[+-]ddd[.ddd]" into sign, integer digits and fraction digits.
void split_decimal(std::string_view t, bool& neg, std::string& ip, std::string& fp) {
    std::size_t i = 0;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) neg = t[i++] == '-';
    while (i < t.size() && is_digit(t[i])) ip.push_back(t[i++]);
    if (i < t.size() && t[i] == '.') {
        for (++i; i < t.size() && is_digit(t[i]); ++i) fp.push_back(t[i]);
    }
    if (i != t.size() || (ip.empty() && fp.empty()))
        bad_data("numeric", "invalid numeric text '" + std::string(t) + "'");
}

}  // namespace

// ---- numeric -------------------------------------------------------------

std::string numeric::to_string() const {
    switch (kind) {
        case special::nan: return "NaN";
        case special::pos_inf: return "Infinity";
        case special::neg_inf: return "-Infinity";
        case special::none: break;
    }
    auto group_at = [&](int power) {
        int idx = weight - power;
        return idx >= 0 && idx < static_cast<int>(digits.size()) ? digits[static_cast<std::size_t>(idx)] : 0;
    };
    std::string s;
    if (negative && !digits.empty()) s = "-";
    if (digits.empty() || weight < 0) {
        s += '0';
    } else {
        s += std::to_string(group_at(weight));
        for (int p = weight - 1; p >= 0; --p) append_group(s, group_at(p));
    }
    if (dscale == 0) return s;
    std::string frac;
    for (int p = -1; frac.size() < dscale; --p) append_group(frac, group_at(p));
    return s + '.' + frac.substr(0, dscale);
}

numeric numeric::parse(std::string_view text) {
    if (text == "NaN") return {special::nan, false, 0, 0, {}};
    if (text == "Infinity" || text == "+Infinity") return {special::pos_inf, false, 0, 0, {}};
    if (text == "-Infinity") return {special::neg_inf, false, 0, 0, {}};
    bool neg = false;
    std::string ip, fp;
    split_decimal(text, neg, ip, fp);
    if (fp.size() > 16383) bad_data("numeric", "more than 16383 fractional digits");
    numeric n;
    n.dscale = static_cast<std::uint16_t>(fp.size());
    ip.insert(0, (4 - ip.size() % 4) % 4, '0');
    fp.append((4 - fp.size() % 4) % 4, '0');
    n.weight = static_cast<std::int16_t>(ip.size() / 4 - 1);
    for (const std::string* part : {&ip, &fp})
        for (std::size_t i = 0; i < part->size(); i += 4)
            n.digits.push_back(static_cast<std::int16_t>(std::stoi(part->substr(i, 4))));
    normalize(n);
    n.negative = neg && !n.digits.empty();
    return n;
}

// ---- decimal helpers -----------------------------------------------------

namespace detail {

void encode_scaled(std::int64_t units, int scale, std::string& out) {
    const std::uint64_t mag = units < 0 ? 0 - static_cast<std::uint64_t>(units) : static_cast<std::uint64_t>(units);
    const scaled_groups g = groups_of(mag, scale);
    put_int<std::int16_t>(static_cast<std::int16_t>(g.count), out);
    put_int<std::int16_t>(static_cast<std::int16_t>(g.weight), out);
    put_int<std::uint16_t>(g.count > 0 && units < 0 ? sign_neg : sign_pos, out);
    put_int<std::uint16_t>(static_cast<std::uint16_t>(scale), out);
    for (int i = 0; i < g.count; ++i) put_int<std::int16_t>(g.d[static_cast<std::size_t>(i)], out);
}

std::int64_t decode_scaled(std::string_view bytes, int scale) {
    const numeric_header h = read_header(bytes);
    if (h.sign == sign_nan || h.sign == sign_pinf || h.sign == sign_ninf)
        bad_data("decimal", "NaN and infinity have no fixed-point value");
    if (h.sign != sign_pos && h.sign != sign_neg) bad_data("numeric", "unknown sign word");
    for (int i = 0; i < h.count; ++i) digit_at(bytes, i);
    std::uint64_t mag = 0;
    for (int i = 0; i < h.count; ++i) {
        if (const std::int16_t d = digit_at(bytes, i)) mag = add_term(mag, d, 4 * (h.weight - i) + scale);
    }
    return signed_units(mag, h.sign == sign_neg);
}

std::int64_t parse_scaled(std::string_view text, int scale) {
    return to_scaled(numeric::parse(text), scale);
}

std::string format_scaled(std::int64_t units, int scale) {
    std::uint64_t mag = units < 0 ? 0 - static_cast<std::uint64_t>(units) : static_cast<std::uint64_t>(units);
    std::string digits = std::to_string(mag);
    if (digits.size() <= static_cast<std::size_t>(scale)) digits.insert(0, scale + 1 - digits.size(), '0');
    if (scale > 0) digits.insert(digits.size() - static_cast<std::size_t>(scale), 1, '.');
    return units < 0 ? "-" + digits : digits;
}

// ---- arrays --------------------------------------------------------------

oid array_oid(oid element) {
    switch (element) {
        case 16: return 1000;
        case 17: return 1001;
        case 20: return 1016;
        case 21: return 1005;
        case 23: return 1007;
        case 25: return 1009;
        case 114: return 199;
        case 700: return 1021;
        case 701: return 1022;
        case 1082: return 1182;
        case 1184: return 1185;
        case 1700: return 1231;
        case 2950: return 2951;
        case 3802: return 3807;
    }
    throw error(error_kind::other, "no array type known for type OID " + std::to_string(element));
}

void encode_array(oid element, const std::vector<const std::optional<std::string>*>& elems, std::string& out) {
    bool nulls = false;
    for (const auto* e : elems) nulls = nulls || !*e;
    const bool empty = elems.empty();
    put_int<std::int32_t>(empty ? 0 : 1, out);
    put_int<std::int32_t>(nulls ? 1 : 0, out);
    put_int<std::int32_t>(static_cast<std::int32_t>(element), out);
    if (!empty) {
        put_int<std::int32_t>(static_cast<std::int32_t>(elems.size()), out);
        put_int<std::int32_t>(1, out);
    }
    for (const auto* e : elems) {
        put_int<std::int32_t>(*e ? static_cast<std::int32_t>((*e)->size()) : -1, out);
        if (*e) out += **e;
    }
}

param array_param(oid element, const std::vector<const std::optional<std::string>*>& elems) {
    std::size_t size = 12 + (elems.empty() ? 0 : 8);
    for (const auto* e : elems) size += 4 + (*e ? (*e)->size() : 0);
    std::string b;
    b.reserve(size);
    encode_array(element, elems, b);
    return {array_oid(element), true, std::move(b)};
}

}  // namespace detail

// ---- uuid ----------------------------------------------------------------

namespace {
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

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
    auto bad = [&] { bad_data("uuid", "invalid uuid text '" + std::string(text) + "'"); };
    if (text.size() != 36) bad();
    uuid u;
    std::size_t n = 0;
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') bad();
            continue;
        }
        int hi = hex_value(text[i]), lo = i + 1 < 36 ? hex_value(text[i + 1]) : -1;
        if (hi < 0 || lo < 0) bad();
        u.bytes[n++] = static_cast<std::uint8_t>(hi * 16 + lo);
        ++i;
    }
    return u;
}

// ---- codecs --------------------------------------------------------------

const oid codec<bool>::param_oid = 16;
void codec<bool>::encode(const bool& v, std::string& out) { out.push_back(v ? 1 : 0); }
bool codec<bool>::accepts(const type_desc& t) { return t.id == 16; }
bool codec<bool>::decode(std::string_view b, const type_desc&) {
    need_len(b, 1, "bool");
    return b[0] != 0;
}

#define PG_INT_CODEC(T, OID, NAME)                                                           \
    const oid codec<T>::param_oid = OID;                                                     \
    void codec<T>::encode(const T& v, std::string& out) { put_int<T>(v, out); }              \
    bool codec<T>::accepts(const type_desc& t) { return t.id == OID; }                       \
    T codec<T>::decode(std::string_view b, const type_desc&) { return get_int<T>(b, NAME); }

PG_INT_CODEC(std::int16_t, 21, "int2")
PG_INT_CODEC(std::int32_t, 23, "int4")
PG_INT_CODEC(std::int64_t, 20, "int8")
#undef PG_INT_CODEC

const oid codec<float>::param_oid = 700;
void codec<float>::encode(const float& v, std::string& out) { put_be(std::bit_cast<std::uint32_t>(v), 4, out); }
bool codec<float>::accepts(const type_desc& t) { return t.id == 700; }
float codec<float>::decode(std::string_view b, const type_desc&) {
    need_len(b, 4, "float4");
    return std::bit_cast<float>(static_cast<std::uint32_t>(get_be(b, 0, 4)));
}

const oid codec<double>::param_oid = 701;
void codec<double>::encode(const double& v, std::string& out) { put_be(std::bit_cast<std::uint64_t>(v), 8, out); }
bool codec<double>::accepts(const type_desc& t) { return t.id == 701; }
double codec<double>::decode(std::string_view b, const type_desc&) {
    need_len(b, 8, "float8");
    return std::bit_cast<double>(get_be(b, 0, 8));
}

const oid codec<std::string>::param_oid = 0;
void codec<std::string>::encode(const std::string& v, std::string& out) { out.append(v); }
bool codec<std::string>::accepts(const type_desc& t) { return t.category == 'S' || t.kind == 'e'; }
std::string codec<std::string>::decode(std::string_view b, const type_desc&) { return std::string(b); }

const oid codec<numeric>::param_oid = 1700;
void codec<numeric>::encode(const numeric& v, std::string& out) { encode_numeric(v, out); }
bool codec<numeric>::accepts(const type_desc& t) { return t.id == 1700; }
numeric codec<numeric>::decode(std::string_view b, const type_desc&) { return decode_numeric(b); }

const oid codec<jsonb>::param_oid = 0;
void codec<jsonb>::encode(const jsonb& v, std::string& out) { out.append(v.text); }
bool codec<jsonb>::accepts(const type_desc& t) { return t.id == 114 || t.id == 3802; }
jsonb codec<jsonb>::decode(std::string_view b, const type_desc& t) {
    if (t.id != 3802) return {std::string(b)};
    if (b.empty() || b[0] != 1) bad_data("jsonb", "unsupported jsonb binary version");
    return {std::string(b.substr(1))};
}

const oid codec<uuid>::param_oid = 2950;
void codec<uuid>::encode(const uuid& v, std::string& out) {
    out.append(reinterpret_cast<const char*>(v.bytes.data()), v.bytes.size());
}
bool codec<uuid>::accepts(const type_desc& t) { return t.id == 2950; }
uuid codec<uuid>::decode(std::string_view b, const type_desc&) {
    need_len(b, 16, "uuid");
    uuid u;
    std::memcpy(u.bytes.data(), b.data(), 16);
    return u;
}

const oid codec<bytea>::param_oid = 17;
void codec<bytea>::encode(const bytea& v, std::string& out) {
    out.append(reinterpret_cast<const char*>(v.data()), v.size());
}
bool codec<bytea>::accepts(const type_desc& t) { return t.id == 17; }
bytea codec<bytea>::decode(std::string_view b, const type_desc&) {
    auto* p = reinterpret_cast<const std::byte*>(b.data());
    return bytea(p, p + b.size());
}

namespace {
constexpr std::int64_t unix_to_pg_seconds = 946684800;  // 1970-01-01 -> 2000-01-01
constexpr std::int64_t unix_to_pg_micros = unix_to_pg_seconds * 1'000'000;
constexpr std::int64_t unix_to_pg_days = 10957;
}  // namespace

const oid codec<timestamptz>::param_oid = 1184;
void codec<timestamptz>::encode(const timestamptz& v, std::string& out) {
    std::int64_t us = v.time_since_epoch().count();
    if (v == timestamptz::max()) {
        us = std::numeric_limits<std::int64_t>::max();
    } else if (v == timestamptz::min()) {
        us = std::numeric_limits<std::int64_t>::min();
    } else if (__builtin_sub_overflow(us, unix_to_pg_micros, &us)) {
        bad_data("timestamptz", "value out of range");
    }
    put_int<std::int64_t>(us, out);
}
bool codec<timestamptz>::accepts(const type_desc& t) { return t.id == 1184; }
timestamptz codec<timestamptz>::decode(std::string_view b, const type_desc&) {
    auto us = get_int<std::int64_t>(b, "timestamptz");
    if (us == std::numeric_limits<std::int64_t>::max()) return timestamptz::max();
    if (us == std::numeric_limits<std::int64_t>::min()) return timestamptz::min();
    if (__builtin_add_overflow(us, unix_to_pg_micros, &us) || us == std::numeric_limits<std::int64_t>::max() ||
        us == std::numeric_limits<std::int64_t>::min())
        bad_data("timestamptz", "value out of range");
    return timestamptz(std::chrono::microseconds(us));
}

const oid codec<date>::param_oid = 1082;
void codec<date>::encode(const date& v, std::string& out) {
    std::int32_t days = 0;
    if (v == date::max()) {
        days = std::numeric_limits<std::int32_t>::max();
    } else if (v == date::min()) {
        days = std::numeric_limits<std::int32_t>::min();
    } else {
        std::int64_t d = static_cast<std::int64_t>(v.time_since_epoch().count()) - unix_to_pg_days;
        if (d <= std::numeric_limits<std::int32_t>::min() || d >= std::numeric_limits<std::int32_t>::max())
            bad_data("date", "value out of range");
        days = static_cast<std::int32_t>(d);
    }
    put_int<std::int32_t>(days, out);
}
bool codec<date>::accepts(const type_desc& t) { return t.id == 1082; }
date codec<date>::decode(std::string_view b, const type_desc&) {
    auto days = get_int<std::int32_t>(b, "date");
    if (days == std::numeric_limits<std::int32_t>::max()) return date::max();
    if (days == std::numeric_limits<std::int32_t>::min()) return date::min();
    return date(std::chrono::days(static_cast<std::int64_t>(days) + unix_to_pg_days));
}

// ---- built-in type table -------------------------------------------------

namespace detail {

namespace {

// Index of id in the built-in table, -1 when it is not a built-in type.
constexpr int type_index(oid id) {
    switch (id) {
        case 16: return 0;
        case 17: return 1;
        case 19: return 2;
        case 20: return 3;
        case 21: return 4;
        case 23: return 5;
        case 25: return 6;
        case 26: return 7;
        case 114: return 8;
        case 700: return 9;
        case 701: return 10;
        case 705: return 11;
        case 1042: return 12;
        case 1043: return 13;
        case 1082: return 14;
        case 1114: return 15;
        case 1184: return 16;
        case 1700: return 17;
        case 2950: return 18;
        case 3802: return 19;
    }
    return -1;
}

}  // namespace

const type_desc* builtin_type(oid id) {
    static const type_desc table[] = {
        {16, "bool", 'B', 'b', {}},      {17, "bytea", 'U', 'b', {}},   {19, "name", 'S', 'b', {}},
        {20, "int8", 'N', 'b', {}},      {21, "int2", 'N', 'b', {}},    {23, "int4", 'N', 'b', {}},
        {25, "text", 'S', 'b', {}},      {26, "oid", 'N', 'b', {}},     {114, "json", 'U', 'b', {}},
        {700, "float4", 'N', 'b', {}},   {701, "float8", 'N', 'b', {}}, {705, "unknown", 'X', 'p', {}},
        {1042, "bpchar", 'S', 'b', {}},  {1043, "varchar", 'S', 'b', {}}, {1082, "date", 'D', 'b', {}},
        {1114, "timestamp", 'D', 'b', {}}, {1184, "timestamptz", 'D', 'b', {}}, {1700, "numeric", 'N', 'b', {}},
        {2950, "uuid", 'U', 'b', {}},    {3802, "jsonb", 'U', 'b', {}},
    };
    const int i = type_index(id);
    return i < 0 ? nullptr : &table[i];
}

}  // namespace detail
}  // namespace pg
