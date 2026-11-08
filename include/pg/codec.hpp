#pragma once
#include <array>
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

// What the engine knows about a result column's type: its OID and the pg_type
// row fields a codec may need to decide whether it accepts the column.
struct type_desc {
    oid id = 0;
    std::string_view name;  // pg_type.typname
    char category = 0;      // pg_type.typcategory ('S' string, 'N' numeric, ...)
    char kind = 0;          // pg_type.typtype ('b' base, 'e' enum, 'd' domain, ...)
};

// Arbitrary precision numeric in PostgreSQL's own form: base-10000 digits,
// the weight of the first digit and the display scale.
struct numeric {
    enum class special : std::uint8_t { none, nan, pos_inf, neg_inf };
    special kind = special::none;
    bool negative = false;
    std::int16_t weight = 0;           // power of 10000 of digits[0]
    std::uint16_t dscale = 0;          // decimal digits after the point
    std::vector<std::int16_t> digits;  // each 0..9999, no leading or trailing zero groups

    std::string to_string() const;                // "-12.3400", "NaN", "Infinity"
    static numeric parse(std::string_view text);  // throws pg::error on bad text
    bool operator==(const numeric&) const = default;
};

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

    // Parses "-12.5"; more than Scale fractional digits or overflow throws pg::error.
    static decimal parse(std::string_view text);
    std::string to_string() const;  // always exactly Scale fractional digits

    friend constexpr decimal operator+(decimal a, decimal b) { return {a.units + b.units}; }
    friend constexpr decimal operator-(decimal a, decimal b) { return {a.units - b.units}; }
    constexpr decimal operator-() const { return {-units}; }
    constexpr decimal& operator+=(decimal b) { units += b.units; return *this; }
    constexpr decimal& operator-=(decimal b) { units -= b.units; return *this; }
    friend constexpr auto operator<=>(decimal, decimal) = default;
};

// Text of a json or jsonb value.
struct jsonb {
    std::string text;
    bool operator==(const jsonb&) const = default;
};

struct uuid {
    std::array<std::uint8_t, 16> bytes{};
    std::string to_string() const;                // canonical 8-4-4-4-12 lower case
    static uuid parse(std::string_view text);     // throws pg::error on bad text
    bool operator==(const uuid&) const = default;
};

using bytea = std::vector<std::byte>;

// timestamptz as microseconds since the Unix epoch; ±infinity are max()/min().
using timestamptz = std::chrono::sys_time<std::chrono::microseconds>;
// date as days since the Unix epoch; ±infinity are max()/min().
using date = std::chrono::sys_days;

// Conversion between a C++ type and PostgreSQL's wire format. Results are
// always binary. A specialization provides:
//
//   static constexpr oid param_oid;        // OID sent with a parameter; 0 lets the server infer it
//   static constexpr bool param_binary;    // false: encode() writes text format
//   static void encode(const T&, std::string& out);           // appends the parameter bytes
//   static bool accepts(const type_desc&);                    // can decode this result column type
//   static T decode(std::string_view bytes, const type_desc&); // binary result value, never NULL
//
// Parameter-only types (string_view, const char*) omit accepts/decode. Users
// may specialize codec for their own types with the same members.
template <class T>
struct codec;

#define PG_DECLARE_CODEC(T)                                       \
    template <>                                                   \
    struct codec<T> {                                             \
        static const oid param_oid;                               \
        static const bool param_binary;                           \
        static void encode(const T& v, std::string& out);         \
        static bool accepts(const type_desc& t);                  \
        static T decode(std::string_view b, const type_desc& t);  \
    };

PG_DECLARE_CODEC(bool)
PG_DECLARE_CODEC(std::int16_t)
PG_DECLARE_CODEC(std::int32_t)
PG_DECLARE_CODEC(std::int64_t)
PG_DECLARE_CODEC(float)
PG_DECLARE_CODEC(double)
PG_DECLARE_CODEC(std::string)  // text parameter; accepts category 'S' types and enums
PG_DECLARE_CODEC(numeric)
PG_DECLARE_CODEC(jsonb)        // text parameter; accepts json and jsonb
PG_DECLARE_CODEC(uuid)
PG_DECLARE_CODEC(bytea)
PG_DECLARE_CODEC(timestamptz)
PG_DECLARE_CODEC(date)
#undef PG_DECLARE_CODEC

namespace detail {
// Binary numeric <-> fixed point; used by codec<decimal<S>>.
void encode_scaled(std::int64_t units, int scale, std::string& out);
std::int64_t decode_scaled(std::string_view bytes, int scale);
std::int64_t parse_scaled(std::string_view text, int scale);
std::string format_scaled(std::int64_t units, int scale);
}  // namespace detail

template <int S>
decimal<S> decimal<S>::parse(std::string_view text) {
    return {detail::parse_scaled(text, S)};
}
template <int S>
std::string decimal<S>::to_string() const {
    return detail::format_scaled(units, S);
}

template <int S>
struct codec<decimal<S>> {
    static constexpr oid param_oid = 1700;
    static constexpr bool param_binary = true;
    static void encode(const decimal<S>& v, std::string& out) { detail::encode_scaled(v.units, S, out); }
    static bool accepts(const type_desc& t) { return t.id == 1700; }
    // A value with more than S nonzero fractional digits, or out of range, throws pg::error.
    static decimal<S> decode(std::string_view b, const type_desc&) { return {detail::decode_scaled(b, S)}; }
};

// Parameter-only string types: text format, type inferred by the server.
template <>
struct codec<std::string_view> {
    static constexpr oid param_oid = 0;
    static constexpr bool param_binary = false;
    static void encode(std::string_view v, std::string& out) { out.append(v); }
};
template <>
struct codec<const char*> : codec<std::string_view> {};
template <>
struct codec<char*> : codec<std::string_view> {};
template <std::size_t N>
struct codec<char[N]> : codec<std::string_view> {};

namespace detail {

template <class T>
inline constexpr bool is_optional = false;
template <class T>
inline constexpr bool is_optional<std::optional<T>> = true;

template <class T>
struct value_of {
    using type = T;
};
template <class T>
struct value_of<std::optional<T>> {
    using type = T;
};
// T with one std::optional layer removed.
template <class T>
using value_t = typename value_of<std::remove_cvref_t<T>>::type;

// One bound parameter: its type OID (0 = inferred), format, and bytes; no
// bytes means SQL NULL.
struct param {
    oid type = 0;
    bool binary = false;
    std::optional<std::string> bytes;
    bool operator==(const param&) const = default;
};

template <class T>
param to_param(const T& v) {
    using V = std::decay_t<value_t<T>>;
    using C = codec<V>;
    param p{C::param_oid, C::param_binary, std::nullopt};
    if constexpr (is_optional<std::remove_cvref_t<T>>) {
        if (!v) return p;
        C::encode(*v, p.bytes.emplace());
    } else {
        C::encode(v, p.bytes.emplace());
    }
    return p;
}

// Whether a result column of type `t` can be read as T (optional unwrapped).
template <class T>
bool accepts(const type_desc& t) {
    return codec<value_t<T>>::accepts(t);
}

// Decodes one binary result value; `v` is nullptr for SQL NULL, which only an
// optional T may hold.
template <class T>
T decode_value(const char* v, std::size_t n, const type_desc& t) {
    if (!v) {
        if constexpr (is_optional<T>) return std::nullopt;
        else throw error(error_kind::other, "NULL in a column read as a non-optional type");
    }
    return codec<value_t<T>>::decode(std::string_view(v, n), t);
}

// The name, category and kind of a built-in type, or nullptr when `id` is not
// one libpg knows without asking the server.
const type_desc* builtin_type(oid id);

}  // namespace detail
}  // namespace pg
