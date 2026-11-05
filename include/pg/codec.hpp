#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "error.hpp"

namespace pg {

// Decimal text of a numeric column, kept as the server wrote it.
struct numeric {
    std::string text;
    bool operator==(const numeric&) const = default;
};

// Text of a json or jsonb column.
struct jsonb {
    std::string text;
    bool operator==(const jsonb&) const = default;
};

using timestamptz = std::chrono::sys_time<std::chrono::microseconds>;

// Text-format conversion between C++ values and PostgreSQL values:
//   static std::string encode(const T&);
//   static T decode(std::string_view);
// Users may specialize it for their own types.
template <class T>
struct codec;

template <>
struct codec<bool> {
    static std::string encode(bool v);
    static bool decode(std::string_view s);
};
template <>
struct codec<std::int16_t> {
    static std::string encode(std::int16_t v);
    static std::int16_t decode(std::string_view s);
};
template <>
struct codec<std::int32_t> {
    static std::string encode(std::int32_t v);
    static std::int32_t decode(std::string_view s);
};
template <>
struct codec<std::int64_t> {
    static std::string encode(std::int64_t v);
    static std::int64_t decode(std::string_view s);
};
template <>
struct codec<double> {
    static std::string encode(double v);
    static double decode(std::string_view s);
};
template <>
struct codec<std::string> {
    static std::string encode(const std::string& v);
    static std::string decode(std::string_view s);
};
template <>
struct codec<numeric> {
    static std::string encode(const numeric& v);
    static numeric decode(std::string_view s);
};
template <>
struct codec<jsonb> {
    static std::string encode(const jsonb& v);
    static jsonb decode(std::string_view s);
};
template <>
struct codec<timestamptz> {
    static std::string encode(const timestamptz& v);
    static timestamptz decode(std::string_view s);
};

// Parameter-only types.
template <>
struct codec<std::string_view> {
    static std::string encode(std::string_view v) { return std::string(v); }
};
template <>
struct codec<const char*> {
    static std::string encode(const char* v) { return v; }
};
template <>
struct codec<char*> {
    static std::string encode(const char* v) { return v; }
};
template <std::size_t N>
struct codec<char[N]> {
    static std::string encode(const char* v) { return v; }
};

// A nullable column. NULL itself is handled by the caller (detail::to_param,
// detail::decode_cell); the codec converts the present value.
template <class T>
struct codec<std::optional<T>> {
    static std::string encode(const std::optional<T>& v) {
        if (!v) throw error("cannot encode NULL as text");
        return codec<T>::encode(*v);
    }
    static std::optional<T> decode(std::string_view s) { return codec<T>::decode(s); }
};

namespace detail {

template <class T>
inline constexpr bool is_optional = false;
template <class T>
inline constexpr bool is_optional<std::optional<T>> = true;

// Query parameter: the encoded text, or nullopt for SQL NULL.
using param = std::optional<std::string>;

template <class T>
param to_param(const T& v) {
    if constexpr (is_optional<T>) {
        if (!v) return std::nullopt;
    }
    return codec<T>::encode(v);
}

// Decodes one result cell; `v` is nullptr for SQL NULL.
template <class T>
T decode_cell(const char* v, std::size_t n) {
    if (!v) {
        if constexpr (is_optional<T>) return std::nullopt;
        else throw error("NULL in a column read as a non-optional type");
    }
    return codec<T>::decode(std::string_view(v, n));
}

}  // namespace detail
}  // namespace pg
