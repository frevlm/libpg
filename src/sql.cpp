#include <algorithm>
#include <charconv>

#include "pg/sql.hpp"

namespace pg {

void sql::append_literal(std::string_view f, std::size_t& pos) {
    while (pos < f.size()) {
        // Every brace of a checked format string starts a pair: `{}`, `{{` or `}}`.
        const std::size_t brace = std::min({f.find('{', pos), f.find('}', pos), f.size()});
        text_.append(f, pos, brace - pos);
        pos = std::min(brace + 2, f.size());
        if (brace == f.size() || (f[brace] == '{' && f[brace + 1] == '}')) return;
        text_ += f[brace];
    }
}

sql& sql::operator+=(const sql& other) {
    for (std::size_t m : other.marks_) marks_.push_back(text_.size() + m);
    text_ += other.text_;
    params_.insert(params_.end(), other.params_.begin(), other.params_.end());
    return *this;
}

rendered sql::render() const {
    rendered r{{}, params_};
    r.text.reserve(text_.size() + 4 * marks_.size());
    std::size_t from = 0;
    for (std::size_t i = 0; i < marks_.size(); ++i) {
        r.text.append(text_, from, marks_[i] - from);
        char digits[24] = {'$'};
        r.text.append(digits, std::to_chars(digits + 1, digits + sizeof digits, i + 1).ptr);
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

std::string detail::quote_ident(std::string_view name) {
    std::string out = "\"";
    for (char c : name) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + '"';
}

sql ident(std::string_view name) { return unsafe_sql(detail::quote_ident(name)); }

}  // namespace pg
