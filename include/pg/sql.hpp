#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "codec.hpp"

namespace pg {

namespace detail {

// One bound parameter: its type OID and its binary bytes; no bytes is SQL NULL.
struct param {
    oid type = 0;
    std::optional<std::string> bytes;
    bool operator==(const param&) const = default;
};

template <class T>
param to_param(const T& v) {
    if constexpr (std::is_same_v<T, param>) return v;
    else if constexpr (is_optional<T>) return v ? to_param(*v) : param{codec_of<T>::type, std::nullopt};
    else {
        param p{codec_of<T>::type, std::string()};
        codec_of<T>::encode(v, *p.bytes);
        return p;
    }
}

// A type that binds as a parameter; a detail::param binds as itself.
template <class T>
concept bindable = std::is_same_v<std::remove_cvref_t<T>, param> || requires { codec_of<T>::type; };

// Number of `{}` placeholders in a format string; -1 when a brace is unpaired.
consteval int count_placeholders(std::string_view f) {
    int n = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        const char c = f[i];
        if (c != '{' && c != '}') continue;
        if (i + 1 == f.size() || (f[i + 1] != c && !(c == '{' && f[i + 1] == '}'))) return -1;
        if (c == '{' && f[i + 1] == '}') ++n;
        ++i;
    }
    return n;
}

// A format string literal checked at compile time against the argument count.
template <class... A>
struct format_string {
    std::string_view text;

    template <std::size_t N>
    consteval format_string(const char (&s)[N]) : text(s, N - 1) {
        const int n = count_placeholders(text);
        if (n < 0) throw "pg::sql: unpaired brace in format string; write {{ or }} for a literal brace";
        if (n != static_cast<int>(sizeof...(A))) throw "pg::sql: placeholder count differs from argument count";
    }
};

// A double-quoted SQL identifier.
std::string quote_ident(std::string_view name);

}  // namespace detail

// A statement rendered: text with $1..$n and the parameters it numbers, viewing
// the statement it came from.
struct rendered {
    std::string text;
    std::span<const detail::param> params;
};

// A statement: text and parameters, numbered $1..$n only by render(), so
// statements concatenate and nest freely.
//
//   pg::sql q{"SELECT id FROM users WHERE name = {} AND age > {}", name, age};
//   pg::sql q2{"SELECT * FROM t WHERE {}", pg::sql{"active"}};  // a pg::sql argument is spliced
//
// `{}` takes one argument; `{{` and `}}` are literal braces.
class sql {
public:
    sql() = default;

    template <class... A>
        requires((std::is_same_v<A, sql> || detail::bindable<A>) && ...)
    sql(detail::format_string<std::type_identity_t<A>...> f, const A&... args) {
        std::size_t pos = 0;
        (append_arg(f.text, pos, args), ...);
        append_literal(f.text, pos);
    }

    sql& operator+=(const sql& other);
    friend sql operator+(sql a, const sql& b) { return a += b; }

    rendered render() const;
    bool empty() const { return text_.empty() && params_.empty(); }

private:
    friend sql unsafe_sql(std::string text);

    // Appends the format text from `pos` to the next placeholder (or the end),
    // unescaping braces; `pos` moves past the placeholder.
    void append_literal(std::string_view f, std::size_t& pos);

    template <class A>
    void append_arg(std::string_view f, std::size_t& pos, const A& arg) {
        append_literal(f, pos);
        if constexpr (std::is_same_v<A, sql>) {
            *this += arg;
        } else {
            params_.push_back(detail::to_param(arg));
            marks_.push_back(text_.size());
        }
    }

    std::string text_;                // the text without placeholders
    std::vector<std::size_t> marks_;  // params_[i] goes at offset marks_[i] of text_
    std::vector<detail::param> params_;
};

// The only way to turn a runtime string into SQL text: it is used as is and must
// not hold anything an attacker controls.
sql unsafe_sql(std::string text);

// The statements with `sep` between each two.
sql join(std::span<const sql> parts, const sql& sep);

// A table or column name, double-quoted.
sql ident(std::string_view name);

}  // namespace pg
