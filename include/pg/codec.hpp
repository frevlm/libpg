#pragma once
#include <array>
#include <bit>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "error.hpp"

namespace pg {

// A PostgreSQL type OID.
using oid = unsigned int;

namespace detail {
// Fixed point <-> text and binary numeric; used by decimal<S>.
std::int64_t parse_scaled(std::string_view text, int scale);
std::string format_scaled(std::int64_t units, int scale);
void encode_scaled(std::int64_t units, int scale, std::string& out);
std::int64_t decode_scaled(std::string_view bytes, int scale);
}  // namespace detail

// Fixed-point decimal with Scale digits after the point, held as an integer
// count of 10^-Scale units. Maps NUMERIC(p, Scale) with p <= 18.
template <int Scale>
struct decimal {
    static_assert(Scale >= 0 && Scale <= 18, "decimal scale must be 0..18");
    std::int64_t units = 0;

    static constexpr std::int64_t one() {
        std::int64_t v = 1;
        for (int i = 0; i < Scale; ++i) v *= 10;
        return v;
    }
    static constexpr decimal from_units(std::int64_t u) { return decimal{u}; }
    constexpr double to_double() const { return static_cast<double>(units) / static_cast<double>(one()); }

    // Parses "-12.5"; a nonzero digit beyond Scale, or overflow, throws pg::error.
    static decimal parse(std::string_view text) { return {detail::parse_scaled(text, Scale)}; }
    std::string to_string() const { return detail::format_scaled(units, Scale); }  // exactly Scale decimals

    friend constexpr decimal operator+(decimal a, decimal b) { return {a.units + b.units}; }
    friend constexpr decimal operator-(decimal a, decimal b) { return {a.units - b.units}; }
    constexpr decimal operator-() const { return {-units}; }
    constexpr decimal& operator+=(decimal b) { units += b.units; return *this; }
    constexpr decimal& operator-=(decimal b) { units -= b.units; return *this; }
    friend constexpr auto operator<=>(decimal, decimal) = default;
};

// The text of a jsonb value.
struct jsonb {
    std::string text;
    bool operator==(const jsonb&) const = default;
};

struct uuid {
    std::array<std::uint8_t, 16> bytes{};
    std::string to_string() const;             // canonical 8-4-4-4-12, lower case
    static uuid parse(std::string_view text);  // throws pg::error on bad text
    bool operator==(const uuid&) const = default;
};

using bytea = std::vector<std::byte>;
// Microseconds since the Unix epoch; ±infinity are max()/min().
using timestamptz = std::chrono::sys_time<std::chrono::microseconds>;
// Days since the Unix epoch; ±infinity are max()/min().
using date = std::chrono::sys_days;

// The binary wire form of a C++ type. A specialization provides:
//   static constexpr oid type;                       // OID sent with a parameter
//   static void encode(const T&, std::string& out);  // appends the binary bytes
//   static bool accepts(oid column);                 // result column types it decodes
//   static T decode(std::string_view bytes);         // a binary result value, never NULL
// Parameter-only types (string views, C strings, vectors) omit accepts and decode.
template <class T>
struct codec;

namespace detail {

// Appends the low `bytes` bytes of v, big-endian.
inline void put_be(std::uint64_t v, std::size_t bytes, std::string& out) {
    char b[8];  // one append: per-byte push_back costs a capacity check each
    for (std::size_t i = 0; i < bytes; ++i) b[i] = static_cast<char>(v >> (8 * (bytes - 1 - i)));
    out.append(b, bytes);
}
// The big-endian integer of exactly `bytes` bytes; another length throws pg::error.
std::uint64_t get_be(std::string_view b, std::size_t bytes);

template <oid Type>
struct exact {
    static constexpr oid type = Type;
    static constexpr bool accepts(oid column) { return column == Type; }
};

// Integers and floats: their bits, big-endian.
template <class T, oid Type>
struct bits_codec : exact<Type> {
    using U = std::conditional_t<sizeof(T) == 2, std::uint16_t,
                                 std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>;
    static void encode(T v, std::string& out) { put_be(std::bit_cast<U>(v), sizeof(T), out); }
    static T decode(std::string_view b) { return std::bit_cast<T>(static_cast<U>(get_be(b, sizeof(T)))); }
};

template <class T> inline constexpr bool is_optional = false;
template <class T> inline constexpr bool is_optional<std::optional<T>> = true;
template <class T> struct value_of { using type = T; };
template <class T> struct value_of<std::optional<T>> { using type = T; };
// T without cv, reference and one std::optional layer.
template <class T>
using value_t = typename value_of<std::remove_cvref_t<T>>::type;
// The codec a value of type T (optional or not) goes through.
template <class T>
using codec_of = codec<std::decay_t<value_t<T>>>;

// The array type of an element type; 0 for one without.
constexpr oid array_of(oid element) {
    constexpr oid pairs[][2] = {{16, 1000}, {17, 1001},  {20, 1016},   {21, 1005},   {23, 1007},   {25, 1009}, {700, 1021},
                                {701, 1022}, {1082, 1182}, {1184, 1185}, {1700, 1231}, {2950, 2951}, {3802, 3807}};
    for (const auto& p : pairs)
        if (p[0] == element) return p[1];
    return 0;
}

// A one-dimensional binary array, element by element; an empty optional is NULL.
struct array_builder {
    oid element = 0;
    std::string body;
    std::uint32_t count = 0;
    bool nulls = false;

    template <class T>
    void add(const T& v) {
        element = codec_of<T>::type;
        ++count;
        if constexpr (is_optional<T>) {
            if (!v) {
                nulls = true;
                return put_be(0xffffffff, 4, body);
            }
        }
        const std::size_t at = body.size();
        body.append(4, '\0');
        if constexpr (is_optional<T>) codec_of<T>::encode(*v, body);
        else codec_of<T>::encode(v, body);
        const auto n = static_cast<std::uint32_t>(body.size() - at - 4);
        for (int i = 0; i < 4; ++i) body[at + i] = static_cast<char>(n >> (24 - 8 * i));
    }
    std::string finish() const;  // the header, then the elements
};

}  // namespace detail

#define PG_CODEC(T, OID)                                       \
    template <>                                                \
    struct codec<T> : detail::exact<OID> {                     \
        static void encode(const T& v, std::string& out);      \
        static T decode(std::string_view b);                   \
    };
PG_CODEC(bool, 16)
PG_CODEC(bytea, 17)
PG_CODEC(date, 1082)
PG_CODEC(timestamptz, 1184)
PG_CODEC(uuid, 2950)
PG_CODEC(jsonb, 3802)  // binary jsonb: version byte 1, then the text
#undef PG_CODEC

template <>
struct codec<std::int16_t> : detail::bits_codec<std::int16_t, 21> {};
template <>
struct codec<std::int32_t> : detail::bits_codec<std::int32_t, 23> {};
// int64_t and the other 64-bit signed type (long on one platform, long long on the other).
template <class T>
    requires(std::is_integral_v<T> && std::is_signed_v<T> && sizeof(T) == 8)
struct codec<T> : detail::bits_codec<T, 20> {};
template <>
struct codec<float> : detail::bits_codec<float, 700> {};
template <>
struct codec<double> : detail::bits_codec<double, 701> {};

// text; reads text, varchar, bpchar and name columns. Binary text is the raw bytes.
template <>
struct codec<std::string> {
    static constexpr oid type = 25;
    static constexpr bool accepts(oid c) { return c == 25 || c == 1043 || c == 1042 || c == 19; }
    static void encode(std::string_view v, std::string& out) { out.append(v); }
    static std::string decode(std::string_view b) { return std::string(b); }
};
template <>
struct codec<std::string_view> {
    static constexpr oid type = 25;
    static void encode(std::string_view v, std::string& out) { out.append(v); }
};
template <>
struct codec<const char*> : codec<std::string_view> {};
template <>
struct codec<char*> : codec<std::string_view> {};

// NUMERIC; decoding a value with a nonzero digit beyond S, or out of range, throws pg::error.
template <int S>
struct codec<decimal<S>> : detail::exact<1700> {
    static void encode(const decimal<S>& v, std::string& out) { detail::encode_scaled(v.units, S, out); }
    static decimal<S> decode(std::string_view b) { return {detail::decode_scaled(b, S)}; }
};

// A one-dimensional array parameter of a type that has a codec.
template <class T>
    requires requires { detail::codec_of<T>::type; }
struct codec<std::vector<T>> {
    static constexpr oid type = detail::array_of(detail::codec_of<T>::type);
    static_assert(type != 0, "no array type for this element type");
    static void encode(const std::vector<T>& v, std::string& out) {
        detail::array_builder b{detail::codec_of<T>::type};
        for (const T& x : v) b.add(x);
        out += b.finish();
    }
};

}  // namespace pg
