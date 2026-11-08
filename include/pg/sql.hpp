#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "codec.hpp"

namespace pg {

class sql;

namespace detail {

using params = std::vector<param>;

// A short vector: up to N elements live in the object, more in a heap buffer.
template <class T, std::size_t N>
class small_vec {
public:
    small_vec() {}
    small_vec(const small_vec& o) { copy_into(o); }
    small_vec(small_vec&& o) noexcept { move_into(o); }
    small_vec& operator=(const small_vec& o) {
        if (this != &o) {
            clear();
            copy_into(o);
        }
        return *this;
    }
    small_vec& operator=(small_vec&& o) noexcept {
        if (this != &o) {
            clear();
            move_into(o);
        }
        return *this;
    }
    ~small_vec() { clear(); }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T* data() { return heap_ ? heap_ : inline_ptr(); }
    const T* data() const { return heap_ ? heap_ : inline_ptr(); }
    T* begin() { return data(); }
    const T* begin() const { return data(); }
    T* end() { return data() + size_; }
    const T* end() const { return data() + size_; }
    T& operator[](std::size_t i) { return data()[i]; }
    const T& operator[](std::size_t i) const { return data()[i]; }

    void reserve(std::size_t n) {
        if (n <= N || n <= cap_) return;
        T* p = alloc(n);
        T* src = data();
        for (std::size_t i = 0; i < size_; ++i) {
            new (p + i) T(std::move(src[i]));
            src[i].~T();
        }
        if (heap_) ::operator delete(heap_);
        heap_ = p;
        cap_ = n;
    }

    void push_back(T v) {
        if (!heap_ && size_ < N) {
            new (inline_ptr() + size_) T(std::move(v));
            ++size_;
            return;
        }
        if (size_ == cap_) grow(heap_ ? 2 * cap_ : 2 * N);
        new (heap_ + size_) T(std::move(v));
        ++size_;
    }

    template <class It>
    void insert_at_end(It first, It last) {
        for (; first != last; ++first) push_back(*first);
    }

    void clear() {
        T* p = data();
        for (std::size_t i = 0; i < size_; ++i) p[i].~T();
        if (heap_) ::operator delete(heap_);
        heap_ = nullptr;
        size_ = 0;
        cap_ = N;
    }

private:
    static T* alloc(std::size_t n) { return static_cast<T*>(::operator new(n * sizeof(T))); }
    T* inline_ptr() { return std::launder(reinterpret_cast<T*>(storage_)); }
    const T* inline_ptr() const { return std::launder(reinterpret_cast<const T*>(storage_)); }

    // Moves the `size_` elements into a new heap buffer of `cap` slots.
    void grow(std::size_t cap) {
        T* p = alloc(cap);
        T* src = data();
        for (std::size_t i = 0; i < size_; ++i) {
            new (p + i) T(std::move(src[i]));
            src[i].~T();
        }
        if (heap_) ::operator delete(heap_);
        heap_ = p;
        cap_ = cap;
    }

    void copy_into(const small_vec& o) {
        size_ = o.size_;
        cap_ = o.cap_;
        if (o.heap_) {
            heap_ = alloc(cap_);
            for (std::size_t i = 0; i < size_; ++i) new (heap_ + i) T(o.heap_[i]);
        } else {
            for (std::size_t i = 0; i < size_; ++i) new (inline_ptr() + i) T(o.inline_ptr()[i]);
        }
    }

    void move_into(small_vec& o) {
        size_ = o.size_;
        cap_ = o.cap_;
        if (o.heap_) {
            heap_ = o.heap_;
            o.heap_ = nullptr;
        } else {
            for (std::size_t i = 0; i < size_; ++i) {
                new (inline_ptr() + i) T(std::move(o.inline_ptr()[i]));
                o.inline_ptr()[i].~T();
            }
        }
        o.size_ = 0;
        o.cap_ = N;
    }

    alignas(T) unsigned char storage_[sizeof(T) * (N ? N : 1)];
    T* heap_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cap_ = N;
};

// Number of `{}` placeholders in a format string; -1 when a brace is unpaired.
consteval int count_placeholders(std::string_view f) {
    int n = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        const char c = f[i];
        if (c != '{' && c != '}') continue;
        const bool paired = i + 1 < f.size() && (f[i + 1] == c || (c == '{' && f[i + 1] == '}'));
        if (!paired) return -1;
        if (c == '{' && f[i + 1] == '}') ++n;
        ++i;
    }
    return n;
}

constexpr bool ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

constexpr bool ascii_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// Whether s[i..] equals lower_needle, folding ASCII letters and nothing else.
constexpr bool folded_at(std::string_view s, std::size_t i, std::string_view lower_needle) {
    if (i + lower_needle.size() > s.size()) return false;
    for (std::size_t k = 0; k < lower_needle.size(); ++k) {
        const char c = s[i + k];
        if ((ascii_letter(c) ? static_cast<char>(c | 0x20) : c) != lower_needle[k]) return false;
    }
    return true;
}

// The first character of the statement that is not a blank or in a comment.
constexpr std::size_t first_token(std::string_view sql) {
    std::size_t i = 0;
    for (;;) {
        while (i < sql.size() && ascii_space(sql[i])) ++i;
        if (i + 1 < sql.size() && sql[i] == '-' && sql[i + 1] == '-') {
            i += 2;
            while (i < sql.size() && sql[i] != '\n') ++i;
            continue;
        }
        if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
            i = i + 1 < sql.size() ? i + 2 : sql.size();
            continue;
        }
        return i;
    }
}

// Whether a statement may change or read server session state that a later
// statement on the same conn could depend on (see pins_session).
constexpr bool pins_text(std::string_view sql) {
    std::size_t i = first_token(sql);
    const std::size_t start = i;
    while (i < sql.size() && (ascii_letter(sql[i]) || sql[i] == '_')) ++i;
    const std::string_view word = sql.substr(start, i - start);
    auto is = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && folded_at(a, 0, b);
    };
    const bool reads = is(word, "select") || is(word, "insert") || is(word, "update") || is(word, "delete") ||
                       is(word, "with") || is(word, "values") || is(word, "table");
    if (!reads) return true;
    // One pass over the text: a needle can only start on its first letter.
    for (std::size_t k = 0; k < sql.size(); ++k) {
        const char c = sql[k];
        switch (ascii_letter(c) ? static_cast<char>(c | 0x20) : c) {
            case 'a': if (folded_at(sql, k, "advisory")) return true; break;
            case 's': if (folded_at(sql, k, "set_config")) return true; break;
            case 'c': if (folded_at(sql, k, "currval")) return true; break;
            case 'l': if (folded_at(sql, k, "lastval")) return true; break;
            case 'p': if (folded_at(sql, k, "pg_backend_pid")) return true; break;
        }
    }
    return false;
}

// Whether a format string starts with a `{}` placeholder: its first word comes
// from an argument, so the verdict is not known at compile time.
constexpr bool starts_with_placeholder(std::string_view f) {
    const std::size_t i = first_token(f);
    return i + 1 < f.size() && f[i] == '{' && f[i + 1] == '}';
}

// The pin verdict of a statement: the compile time one when known, else the
// one of its rendered text.
bool pins_of(const sql& q, std::string_view text);

// The statement with parameter slot i replaced by *with[i] wherever with[i] is
// not null; the other parameters stay.
sql substitute(const sql& q, std::span<const sql* const> with);

// A statement that is one parameter and no text.
sql param_sql(param p);

// The statement without trailing blanks and semicolons after its last parameter.
sql strip_semicolon(const sql& q);

// A format string literal checked at compile time against the argument count.
template <class... A>
struct format_string {
    std::string_view text;
    bool pins = true;

    template <std::size_t N>
    consteval format_string(const char (&s)[N]) : text(s, N - 1), pins(pins_text(text)) {
        const int n = count_placeholders(text);
        if (n < 0) throw "pg::sql: unpaired brace in format string; write {{ or }} for a literal brace";
        if (n != static_cast<int>(sizeof...(A))) throw "pg::sql: placeholder count differs from argument count";
    }
};

// A type that can be bound as a statement parameter.
template <class T>
concept bindable = requires { &codec<std::decay_t<value_t<T>>>::encode; };

}  // namespace detail

// The rendered form of a statement: text with $1..$n and the parameters it
// numbers. Views the statement it came from.
struct rendered {
    std::string text;
    std::span<const detail::param> params;
};

// A statement: text fragments and parameters in alternation. Parameters are
// encoded when the statement is built and numbered $1..$n only by render(),
// so statements concatenate and nest freely.
//
//   pg::sql q{"SELECT id FROM users WHERE name = {} AND age > {}", name, age};
//   pg::sql where{"active"};
//   pg::sql q2{"SELECT * FROM t WHERE {}", where};   // a pg::sql argument is spliced
//
// `{}` takes one argument, `{{` and `}}` are literal braces.
class sql {
public:
    sql() = default;

    template <class... A>
        requires((std::is_same_v<std::remove_cvref_t<A>, sql> || detail::bindable<A>) && ...)
    sql(detail::format_string<std::type_identity_t<A>...> f, const A&... args) {
        text_.reserve(f.text.size());
        params_.reserve(sizeof...(A));
        marks_.reserve(sizeof...(A));
        if constexpr ((std::is_same_v<std::remove_cvref_t<A>, sql> || ...)) pins_ = -1;
        else if (detail::starts_with_placeholder(f.text)) pins_ = -1;
        else pins_ = static_cast<std::int8_t>(f.pins ? 1 : 0);
        std::size_t pos = 0;
        (append_arg(f.text, pos, args), ...);
        append_literal(f.text, pos);
    }

    sql& operator+=(const sql& other);
    friend sql operator+(sql a, const sql& b) { return a += b; }

    // The text with $1..$n; its params view this statement.
    rendered render() const;

    bool empty() const { return params_.empty() && text_.empty(); }

private:
    friend sql unsafe_sql(std::string text);
    friend bool detail::pins_of(const sql& q, std::string_view text);
    friend sql detail::substitute(const sql& q, std::span<const sql* const> with);
    friend sql detail::param_sql(detail::param p);
    friend sql detail::strip_semicolon(const sql& q);

    // Appends the format text from `pos` up to the next placeholder (or the end),
    // unescaping braces; `pos` moves past the placeholder.
    void append_literal(std::string_view f, std::size_t& pos);

    template <class A>
    void append_arg(std::string_view f, std::size_t& pos, const A& arg) {
        append_literal(f, pos);
        if constexpr (std::is_same_v<A, sql>) *this += arg;
        else append_param(detail::to_param(arg));
    }

    void append_param(detail::param p);

    // The text without placeholders; params_[i] goes at offset marks_[i] of it.
    std::string text_;
    detail::small_vec<std::size_t, 4> marks_;
    detail::small_vec<detail::param, 4> params_;
    std::int8_t pins_ = -1;  // 1 pins, 0 floats, -1 unknown (decided from the rendered text)
};

// The only way to turn a runtime string into SQL text. The text is used as is:
// it must not contain anything an attacker controls.
sql unsafe_sql(std::string text);

// The statements with `sep` between each two.
sql join(std::span<const sql> parts, const sql& sep);

namespace detail {
// A double-quoted SQL identifier.
std::string quote_ident(std::string_view name);
// The identifier as a statement.
sql ident(std::string_view name);
}  // namespace detail

}  // namespace pg
