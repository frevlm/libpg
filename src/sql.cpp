#include "pg/sql.hpp"

#include <algorithm>
#include <charconv>

namespace pg {

// Every brace of a checked format string is the first of a pair: `{}`, `{{` or `}}`.
// The first `{` or `}` at or after `pos`, or f.size().
static std::size_t next_brace(std::string_view f, std::size_t pos) {
    return std::min(std::min(f.find('{', pos), f.find('}', pos)), f.size());
}

void sql::append_literal(std::string_view f, std::size_t& pos) {
    while (pos < f.size()) {
        const std::size_t brace = next_brace(f, pos);
        text_.append(f, pos, brace - pos);
        if (brace == f.size()) {
            pos = brace;
            return;
        }
        pos = brace + 2;
        if (f[brace] == '{' && f[brace + 1] == '}') return;
        text_ += f[brace];
    }
}

void sql::append_param(detail::param p) {
    params_.push_back(std::move(p));
    marks_.push_back(text_.size());
}

sql& sql::operator+=(const sql& other) {
    for (std::size_t m : other.marks_) marks_.push_back(text_.size() + m);
    text_ += other.text_;
    params_.insert_at_end(other.params_.begin(), other.params_.end());
    pins_ = -1;
    return *this;
}

rendered sql::render() const {
    rendered r{{}, std::span<const detail::param>(params_.data(), params_.size())};
    r.text.reserve(text_.size() + 4 * marks_.size());
    std::size_t from = 0;
    for (std::size_t i = 0; i < marks_.size(); ++i) {
        r.text.append(text_, from, marks_[i] - from);
        r.text += '$';
        char digits[8];
        const auto end = std::to_chars(digits, digits + sizeof digits, i + 1).ptr;
        r.text.append(digits, end);
        from = marks_[i];
    }
    r.text.append(text_, from);
    return r;
}

sql unsafe_sql(std::string text) {
    sql s;
    s.text_ = std::move(text);
    return s;
}

sql join(std::span<const sql> parts, const sql& sep) {
    sql out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

namespace detail {

bool pins_of(const sql& q, std::string_view text) { return q.pins_ >= 0 ? q.pins_ == 1 : pins_text(text); }

sql substitute(const sql& q, std::span<const sql* const> with) {
    sql out;
    std::size_t from = 0;
    for (std::size_t i = 0; i < q.marks_.size(); ++i) {
        out.text_.append(q.text_, from, q.marks_[i] - from);
        from = q.marks_[i];
        if (i < with.size() && with[i]) {
            out += *with[i];
        } else {
            out.params_.push_back(q.params_[i]);
            out.marks_.push_back(out.text_.size());
        }
    }
    out.text_.append(q.text_, from);
    out.pins_ = -1;
    return out;
}

sql strip_semicolon(const sql& q) {
    sql out = q;
    const std::size_t floor = out.marks_.empty() ? 0 : out.marks_[out.marks_.size() - 1];
    std::size_t end = out.text_.size();
    while (end > floor && (out.text_[end - 1] == ';' || ascii_space(out.text_[end - 1]))) --end;
    out.text_.resize(end);
    return out;
}

sql param_sql(param p) {
    sql out;
    out.params_.push_back(std::move(p));
    out.marks_.push_back(0);
    return out;
}

std::string quote_ident(std::string_view name) {
    std::string out = "\"";
    for (char c : name) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}

}  // namespace detail

sql ident(std::string_view name) { return unsafe_sql(detail::quote_ident(name)); }

}  // namespace pg
