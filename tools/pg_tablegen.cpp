// pg_tablegen DSN schema.sql out.hpp [options]
//
// Creates a scratch database on the server, runs schema.sql in it and writes,
// per table, column tags, a read shape `row` and a write shape `write`
// (CONTEXT.md, 生成代码). Query files are prepared in the same database and
// become typed functions (ADR 0007, 0008, 0026). Every C++ type comes from the
// type the server reports for a value, through libpg's codec table; a type
// outside that table is an error. The scratch database is always dropped.

#include <libpq-fe.h>

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

// ---------------------------------------------------------------- options

const char* const usage =
    "usage: pg_tablegen DSN schema.sql out.hpp [--namespace ns]\n"
    "       [--queries file.sql]... [--queries-out queries.hpp] [--check]\n";

struct usage_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct options {
    std::string dsn, schema, out, ns = "db";
    std::vector<std::string> queries;
    std::string queries_out;
    bool check = false;
};

// DSN schema.sql out.hpp, then options; a fourth positional argument is the namespace.
options parse_args(int argc, char** argv) {
    options o;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check") {
            o.check = true;
        } else if (a == "--namespace" || a == "--queries" || a == "--queries-out") {
            if (i + 1 >= argc) throw usage_error(a + " needs a value");
            std::string v = argv[++i];
            if (a == "--namespace") o.ns = v;
            else if (a == "--queries") o.queries.push_back(v);
            else o.queries_out = v;
        } else if (a.starts_with("--")) {
            throw usage_error("unknown option " + a);
        } else {
            pos.push_back(a);
        }
    }
    if (pos.size() < 3 || pos.size() > 4) throw usage_error("expected DSN schema.sql out.hpp");
    o.dsn = pos[0];
    o.schema = pos[1];
    o.out = pos[2];
    if (pos.size() == 4) o.ns = pos[3];
    if (!o.queries.empty() && o.queries_out.empty()) throw usage_error("--queries needs --queries-out");
    return o;
}

// ---------------------------------------------------------------- libpq RAII

struct conn_deleter {
    void operator()(PGconn* c) const { PQfinish(c); }
};
struct result_deleter {
    void operator()(PGresult* r) const { PQclear(r); }
};
using conn_ptr = std::unique_ptr<PGconn, conn_deleter>;
using result_ptr = std::unique_ptr<PGresult, result_deleter>;

// Takes ownership of r and throws unless it is a successful result.
result_ptr checked(PGresult* r, PGconn* c, const std::string& what) {
    result_ptr res(r);
    if (!res) throw std::runtime_error(what + ": " + PQerrorMessage(c));
    auto st = PQresultStatus(res.get());
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK && st != PGRES_EMPTY_QUERY)
        throw std::runtime_error(what + ": " + PQresultErrorMessage(res.get()));
    return res;
}

result_ptr exec(PGconn* c, const std::string& sql, const std::string& what) {
    return checked(PQexec(c, sql.c_str()), c, what);
}

// Prepares sql as the unnamed statement, its parameters typed by `types`, and
// returns the server's description of it.
result_ptr describe(PGconn* c, const std::string& sql, const std::string& what, const std::vector<Oid>& types = {}) {
    checked(PQprepare(c, "", sql.c_str(), static_cast<int>(types.size()), types.data()), c, what);
    return checked(PQdescribePrepared(c, ""), c, what);
}

std::string cell(const PGresult* r, int row, int col) { return PQgetvalue(r, row, col); }

// ------------------------------------------------------------------- naming

const std::set<std::string>& cxx_keywords() {
    static const std::set<std::string> k = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
        "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
        "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
        "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
        "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
        // Names the generated code itself relies on.
        "std", "pg", "table_", "conn"};
    return k;
}

// A valid C++ identifier derived from a PG identifier.
std::string sanitize(const std::string& s) {
    std::string out;
    for (char ch : s) {
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
        out += ok ? ch : '_';
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9')) out.insert(out.begin(), '_');
    return out;
}

std::string table_ident(const std::string& s) {
    auto id = sanitize(s);
    return cxx_keywords().count(id) ? id + "_" : id;
}

// A column's tag or shape member cannot take the pg_ prefix or the names of
// the row and write shapes.
std::string column_ident(const std::string& s) {
    auto id = table_ident(s);
    return id.starts_with("pg_") || id == "row" || id == "write" ? id + "_" : id;
}

bool is_identifier(const std::string& s) { return !s.empty() && sanitize(s) == s; }

// The body of a C++ string literal: quotes, backslashes and control
// characters escaped; other control characters use 3-digit octal escapes so
// they cannot swallow the characters that follow.
std::string escape_literal(const std::string& s) {
    std::string out;
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch < 0x20 || ch == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", ch);
            out += buf;
        } else {
            out += static_cast<char>(ch);
        }
    }
    return out;
}

std::string quote_literal(const std::string& s) { return "\"" + escape_literal(s) + "\""; }

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

// -------------------------------------------------------------------- types

// The C++ type libpg's codecs give a value the server reports as `type` with
// `typmod`, or empty. NUMERIC(p, s) with p <= 18 and 0 <= s <= 18 is
// pg::decimal<s>. The server reports a domain as its base type.
std::string cxx_type(Oid type, int typmod) {
    static const std::map<Oid, std::string> m = {
        {16, "bool"}, {21, "std::int16_t"}, {23, "std::int32_t"}, {20, "std::int64_t"},
        {700, "float"}, {701, "double"}, {25, "std::string"}, {1043, "std::string"},
        {1042, "std::string"}, {19, "std::string"}, {3802, "::pg::jsonb"}, {2950, "::pg::uuid"},
        {17, "::pg::bytea"}, {1184, "::pg::timestamptz"}, {1082, "::pg::date"},
    };
    if (type == 1700 && typmod >= 4) {
        int precision = ((typmod - 4) >> 16) & 0xffff;
        int scale = (((typmod - 4) & 0x7ff) ^ 1024) - 1024;
        if (precision <= 18 && scale >= 0 && scale <= 18) return "::pg::decimal<" + std::to_string(scale) + ">";
    }
    auto it = m.find(type);
    return it == m.end() ? "" : it->second;
}

// The scratch connection, the NOT NULL table columns as (table OID, attnum),
// and the problems found so far, which are reported together.
struct session {
    PGconn* c;
    std::set<std::pair<Oid, int>> not_null;
    std::vector<std::string> problems;

    // The C++ type of a value; a type without one is a problem of `where`.
    std::string map(Oid type, int typmod, const std::string& where) {
        if (auto t = cxx_type(type, typmod); !t.empty()) return t;
        auto r = exec(c, "SELECT format_type(" + std::to_string(type) + ", " + std::to_string(typmod) + ")",
                      "format_type");
        problems.push_back(where + ": PG type " + cell(r.get(), 0, 0) + " is not in libpg's codec table");
        return "void";
    }
};

// ------------------------------------------------------------------- tables

struct column {
    std::string name;
    std::string ident;  // C++ name of the tag and of the shape members
    std::string cxx;    // C++ type without optional
    bool nullable = false;
    bool has_default = false;
    char identity = 0;   // pg_attribute.attidentity: 'a' always, 'd' by default
    char generated = 0;  // pg_attribute.attgenerated: 's' stored, 'v' virtual
};

struct table {
    std::string name;
    std::string relation;         // schema-qualified and quoted, for SQL
    std::vector<column> columns;  // attnum order
    std::vector<size_t> key;      // primary key columns (indexes into columns) in key order
};

using tables_t = std::map<std::string, table>;  // sorted by table name

// Columns of every ordinary or partitioned (parent) table outside the
// server's own schemas, in attnum order, with each table's primary key. NOT
// NULL columns are recorded in the session.
void read_columns(session& s, tables_t& out) {
    auto r = exec(s.c,
        "SELECT cl.relname, format('%I.%I', n.nspname, cl.relname), a.attname, a.attnotnull, a.atthasdef, "
        "       a.attidentity, a.attgenerated, cl.oid, a.attnum, array_position(k.conkey, a.attnum) "
        "FROM pg_catalog.pg_class cl "
        "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = cl.oid AND a.attnum > 0 AND NOT a.attisdropped "
        "LEFT JOIN pg_catalog.pg_constraint k ON k.conrelid = cl.oid AND k.contype = 'p' "
        "WHERE cl.relkind IN ('r', 'p') AND NOT cl.relispartition "
        "  AND n.nspname NOT IN ('pg_catalog', 'information_schema') "
        "  AND n.nspname NOT LIKE 'pg\\_toast%' AND n.nspname NOT LIKE 'pg\\_temp\\_%' "
        "ORDER BY cl.relname COLLATE \"C\", a.attnum",
        "read columns");
    for (int i = 0; i < PQntuples(r.get()); ++i) {
        auto& t = out[cell(r.get(), i, 0)];
        auto relation = cell(r.get(), i, 1);
        if (!t.relation.empty() && t.relation != relation)
            throw std::runtime_error("table " + t.name + " is defined as both " + t.relation + " and " + relation);
        t.name = cell(r.get(), i, 0);
        t.relation = relation;
        auto name = cell(r.get(), i, 2);
        // An empty attidentity / attgenerated reads as '\0'.
        t.columns.push_back({name, column_ident(name), {}, cell(r.get(), i, 3) != "t", cell(r.get(), i, 4) == "t",
                             cell(r.get(), i, 5)[0], cell(r.get(), i, 6)[0]});
        if (!t.columns.back().nullable)
            s.not_null.emplace(std::stoul(cell(r.get(), i, 7)), std::stoi(cell(r.get(), i, 8)));
        if (!PQgetisnull(r.get(), i, 9)) {
            auto pos = std::stoul(cell(r.get(), i, 9));
            if (t.key.size() < pos) t.key.resize(pos);
            t.key[pos - 1] = t.columns.size() - 1;
        }
    }
}

// Column types as the server describes `SELECT *` of each table.
tables_t read_tables(session& s) {
    tables_t tables;
    read_columns(s, tables);
    for (auto& [name, t] : tables) {
        auto d = describe(s.c, "SELECT * FROM " + t.relation, "describe table " + name);
        for (int i = 0; i < PQnfields(d.get()); ++i) {
            auto& col = t.columns.at(i);
            col.cxx = s.map(PQftype(d.get(), i), PQfmod(d.get(), i), name + "." + col.name);
        }
    }
    return tables;
}

// ------------------------------------------------------------- shapes

// One member of a generated shape.
struct member {
    std::string ident;
    std::string type;
    std::string column;  // the result column's name, for a read shape member
};

std::string optional_of(const std::string& cxx, bool nullable) {
    return nullable ? "std::optional<" + cxx + ">" : cxx;
}

void emit_members(std::ostream& o, const std::vector<member>& ms) {
    for (const auto& m : ms) o << "    " << m.type << " " << m.ident << "{};\n";
}

// pg_each(f): calls f on each member in order.
void emit_each(std::ostream& o, const std::vector<member>& ms, const char* qualifier) {
    o << "    template <class F>\n";
    if (ms.empty()) {
        o << "    void pg_each(F&&)" << qualifier << " {}\n";
        return;
    }
    o << "    void pg_each(F&& f)" << qualifier << " {\n";
    for (const auto& m : ms) o << "        f(" << m.ident << ");\n";
    o << "    }\n";
}

std::string names_array(const std::string& member_name, const std::vector<std::string>& cols) {
    std::vector<std::string> q;
    for (const auto& n : cols) q.push_back(quote_literal(n));
    return "std::array<std::string_view, " + std::to_string(cols.size()) + "> " + member_name + "{" + join(q, ", ") + "}";
}

void emit_read_shape(std::ostream& o, const std::string& name, const std::vector<member>& ms) {
    std::vector<std::string> names;
    for (const auto& m : ms) names.push_back(m.column);
    o << "struct " << name << " {\n";
    emit_members(o, ms);
    o << "    static constexpr std::size_t pg_columns = " << ms.size() << ";\n";
    o << "    static constexpr " << names_array("pg_names", names) << ";\n";
    emit_each(o, ms, "");
    o << "};\n";
}

// ------------------------------------------------------------ table output

void emit_table_struct(std::ostream& o, const table& t) {
    o << "struct table_ {\n"
      << "    static constexpr std::string_view name = " << quote_literal(t.name) << ";\n";
    if (!t.key.empty()) {
        std::vector<std::string> types, names;
        for (auto k : t.key) {
            types.push_back(t.columns[k].cxx);
            names.push_back(t.columns[k].name);
        }
        o << "    using key = std::tuple<" << join(types, ", ") << ">;\n"
          << "    static constexpr " << names_array("key_columns", names) << ";\n";
    }
    o << "};\n";
}

void emit_column(std::ostream& o, const column& c) {
    o << "struct " << c.ident << " {\n"
      << "    using pg_table = table_;\n"
      << "    using pg_type = " << optional_of(c.cxx, c.nullable) << ";\n"
      << "    static constexpr std::string_view pg_name = " << quote_literal(c.name) << ";\n};\n";
}

std::vector<member> read_members(const table& t) {
    std::vector<member> ms;
    for (const auto& c : t.columns) ms.push_back({c.ident, optional_of(c.cxx, c.nullable), c.name});
    return ms;
}

// Generated-always identity and generated columns cannot be written.
bool writable(const column& c) { return c.identity != 'a' && c.generated == 0; }

// A column the server can fill in is a pg::field: unset means DEFAULT.
std::string write_type(const column& c) {
    auto t = optional_of(c.cxx, c.nullable);
    return c.has_default || c.identity == 'd' ? "::pg::field<" + t + ">" : t;
}

void emit_write_shape(std::ostream& o, const table& t) {
    std::vector<member> ms;
    std::vector<std::string> names;
    for (const auto& c : t.columns) {
        if (!writable(c)) continue;
        ms.push_back({c.ident, write_type(c), {}});
        names.push_back(c.name);
    }
    o << "struct write {\n"
      << "    using pg_table = table_;\n";
    emit_members(o, ms);
    o << "    static constexpr " << names_array("pg_names", names) << ";\n";
    emit_each(o, ms, " const");
    o << "};\n";
}

// Every name declared in a table's namespace must be distinct.
void check_names(const table& t) {
    std::set<std::string> seen = {"table_", "row", "write"};
    for (const auto& c : t.columns)
        if (!seen.insert(c.ident).second)
            throw std::runtime_error("table " + t.name + ": generated name " + c.ident + " is used twice");
}

void emit_namespace(std::ostream& o, const std::string& ns, const table& t) {
    check_names(t);
    o << "\nnamespace " << ns << "::" << table_ident(t.name) << " {\n";
    emit_table_struct(o, t);
    for (const auto& c : t.columns) emit_column(o, c);
    emit_read_shape(o, "row", read_members(t));
    emit_write_shape(o, t);
    o << "}\n";
}

// Two tables must not escape to the same C++ namespace.
void check_table_names(const tables_t& tables) {
    std::map<std::string, std::string> seen;
    for (const auto& [name, t] : tables) {
        auto [it, fresh] = seen.emplace(table_ident(name), name);
        if (!fresh)
            throw std::runtime_error("tables \"" + it->second + "\" and \"" + name +
                                     "\" both map to C++ name " + it->first);
    }
}

std::string generated_by(const std::string& from) {
    return "// Generated by pg_tablegen from " + from + " \xE2\x80\x94 do not edit.\n#pragma once\n\n";
}

std::string render_tables(const tables_t& tables, const std::string& ns, const std::string& schema_name) {
    std::ostringstream o;
    o << generated_by(schema_name)
      << "#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <optional>\n#include <string>\n"
      << "#include <string_view>\n#include <tuple>\n\n"
      << "#include \"pg/codec.hpp\"\n#include \"pg/shape.hpp\"\n";
    check_table_names(tables);
    for (const auto& [name, t] : tables) emit_namespace(o, ns, t);
    return o.str();
}

// ------------------------------------------------------------ query files

enum class result_kind { one, opt, many, exec };

struct query {
    std::string file;
    std::string name;
    result_kind kind = result_kind::many;
    std::string body;  // SQL exactly as in the file, outer blank space trimmed
    std::string where() const { return file + ": query " + name; }
};

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

result_kind parse_kind(const std::string& s, const std::string& where) {
    if (s == ":one") return result_kind::one;
    if (s == ":opt") return result_kind::opt;
    if (s == ":many") return result_kind::many;
    if (s == ":exec") return result_kind::exec;
    throw std::runtime_error(where + ": annotation must be :one, :opt, :many or :exec, got \"" + s + "\"");
}

// `-- name: fn :kind`
query parse_name_line(const std::string& file, const std::string& line) {
    std::istringstream in(line.substr(8));
    query q;
    q.file = file;
    std::string kind, extra;
    if (!(in >> q.name >> kind) || in >> extra)
        throw std::runtime_error(file + ": expected \"-- name: fn :one|:opt|:many|:exec\", got \"" + trim(line) + "\"");
    if (!is_identifier(q.name) || cxx_keywords().count(q.name))
        throw std::runtime_error(file + ": \"" + q.name + "\" is not a usable C++ function name");
    q.kind = parse_kind(kind, q.where());
    return q;
}

// Splits a query file into blocks headed by `-- name:`; text before the first
// block is ignored. A `-- params:` line is rejected in favor of :name.
std::vector<query> parse_query_file(const std::string& file, const std::string& text) {
    std::vector<query> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF files read as LF; the body is otherwise verbatim
        if (line.starts_with("-- name:")) {
            out.push_back(parse_name_line(file, line));
        } else if (line.starts_with("-- params:")) {
            throw std::runtime_error((out.empty() ? file : out.back().where()) +
                                     ": use :name placeholders instead of -- params:");
        } else if (!out.empty()) {
            out.back().body += line + "\n";
        }
    }
    for (auto& q : out) {
        q.body = trim(q.body);
        if (q.body.empty()) throw std::runtime_error(q.where() + " has no SQL");
    }
    return out;
}

bool ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }

// End of the quoted run that opens at `start` (just past the opening quote).
// A doubled quote is an escaped quote; with `backslash`, a backslash escapes
// the next character.
size_t quoted_end(const std::string& b, size_t start, char quote, bool backslash) {
    size_t j = start;
    while (j < b.size()) {
        if (backslash && b[j] == '\\' && j + 1 < b.size()) j += 2;
        else if (b[j] == quote && j + 1 < b.size() && b[j + 1] == quote) j += 2;
        else if (b[j++] == quote) break;
    }
    return j;
}

// End of a `--` or `/* */` comment or a `$tag$` string starting at i, else npos.
size_t opaque_end(const std::string& b, size_t i) {
    if (b.compare(i, 2, "--") == 0) {
        auto nl = b.find('\n', i);
        return nl == std::string::npos ? b.size() : nl;
    }
    if (b.compare(i, 2, "/*") == 0) {
        auto end = b.find("*/", i + 2);
        return end == std::string::npos ? b.size() : end + 2;
    }
    if (b[i] != '$') return std::string::npos;
    size_t j = i + 1;
    while (j < b.size() && ident_char(b[j])) ++j;
    if (j >= b.size() || b[j] != '$' || (j > i + 1 && !ident_start(b[i + 1]))) return std::string::npos;
    std::string delim = b.substr(i, j - i + 1);
    auto end = b.find(delim, j + 1);
    return end == std::string::npos ? b.size() : end + delim.size();
}

// A query body read for its two uses. `server` is what the scratch database
// prepares: $1..$n for the distinct placeholder names (`params`, by first
// appearance), holes left as comments. `format` is the pg::sql format string:
// `{}` for every placeholder and hole occurrence, `args` naming what each one
// receives, and every other brace doubled. `holes` are the distinct hole names.
struct scanned {
    std::string server, format;
    std::vector<std::string> params, holes, args;
    bool outer_join = false;  // LEFT, RIGHT or FULL JOIN outside literals and comments
};

// Reads :name and :'name' placeholders and /*@name*/ holes; strings, quoted
// identifiers, dollar quotes and comments are copied untouched. Throws on a
// bare $n and on unusable names.
scanned scan(const query& q) {
    const std::string& b = q.body;
    scanned s;
    std::map<std::string, std::pair<bool, size_t>> seen;  // name -> (is a hole, $n)
    std::string prev;                                     // the previous word, lower case
    auto copy = [&](size_t from, size_t to) {
        for (size_t k = from; k < to; ++k) {
            s.server += b[k];
            s.format += b[k];
            if (b[k] == '{' || b[k] == '}') s.format += b[k];
        }
        return to;
    };
    auto use = [&](const std::string& name, bool hole) {
        if (!is_identifier(name) || cxx_keywords().count(name))
            throw std::runtime_error(q.where() + ": " + (hole ? "hole" : "placeholder") + " \"" + name +
                                     "\" is not a usable C++ name");
        auto& list = hole ? s.holes : s.params;
        auto [it, fresh] = seen.try_emplace(name, hole, list.size() + 1);
        if (it->second.first != hole) throw std::runtime_error(q.where() + ": \"" + name + "\" is both a placeholder and a hole");
        if (fresh) list.push_back(name);
        if (!hole) s.server += "$" + std::to_string(it->second.second);
        s.format += "{}";
        s.args.push_back(name);
    };
    for (size_t i = 0; i < b.size();) {
        char ch = b[i], next = i + 1 < b.size() ? b[i + 1] : '\0';
        if (b.compare(i, 3, "/*@") == 0) {
            auto end = b.find("*/", i + 3);
            if (end == std::string::npos) throw std::runtime_error(q.where() + ": a hole must be /*@name*/");
            use(b.substr(i + 3, end - i - 3), true);
            s.server.append(b, i, end + 2 - i);
            i = end + 2;
        } else if (size_t e = opaque_end(b, i); e != std::string::npos) {
            i = copy(i, e);
        } else if (ch == '\'' || ch == '"') {
            i = copy(i, quoted_end(b, i + 1, ch, false));
        } else if (ident_start(ch)) {
            size_t k = i;
            std::string word;
            for (; k < b.size() && ident_char(b[k]); ++k) word += static_cast<char>(std::tolower(b[k]));
            if (word == "e" && k < b.size() && b[k] == '\'') k = quoted_end(b, k + 1, '\'', true);
            if ((prev == "left" || prev == "right" || prev == "full") && (word == "join" || word == "outer"))
                s.outer_join = true;
            prev = word;
            i = copy(i, k);
        } else if (ch == '$' && next >= '0' && next <= '9') {
            auto end = b.find_first_not_of("0123456789", i + 1);
            throw std::runtime_error(q.where() + ": write :name instead of " + b.substr(i, end - i));
        } else if (ch == ':' && next == ':') {
            i = copy(i, i + 2);
        } else if (ch == ':' && next == '\'') {
            auto close = b.find('\'', i + 2);
            if (close == std::string::npos) throw std::runtime_error(q.where() + ": unterminated :'name' placeholder");
            use(b.substr(i + 2, close - i - 2), false);
            i = close + 1;
        } else if (ch == ':' && ident_start(next)) {
            size_t k = i + 1;
            while (k < b.size() && ident_char(b[k])) ++k;
            use(b.substr(i + 1, k - i - 1), false);
            i = k;
        } else {
            i = copy(i, i + 1);
        }
    }
    return s;
}

// ------------------------------------------------------------ query typing

struct typed_query {
    query q;
    scanned s;
    std::vector<member> params;   // one per distinct placeholder
    std::vector<member> columns;  // read shape members
};

// The placeholders' C++ types. The server may type a parameter as a domain,
// so the types are read back as the result columns of `SELECT $1, ..., $n`,
// which the server reports as base types.
std::vector<member> param_members(session& s, const typed_query& t, const PGresult* d) {
    std::vector<Oid> types;
    std::vector<std::string> refs;
    for (int i = 0; i < PQnparams(d); ++i) {
        types.push_back(PQparamtype(d, i));
        refs.push_back("$" + std::to_string(i + 1));
    }
    if (types.empty()) return {};
    auto base = describe(s.c, "SELECT " + join(refs, ", "), t.q.where() + ": parameter types", types);
    std::vector<member> out;
    for (int i = 0; i < PQnfields(base.get()); ++i) {
        const auto& name = t.s.params.at(i);
        auto where = t.q.file + ": " + t.q.name + " parameter :" + name;
        out.push_back({name, s.map(PQftype(base.get(), i), PQfmod(base.get(), i), where), {}});
    }
    return out;
}

// Result columns. A `!` or `?` suffix on the name makes the member plain or
// optional; otherwise it is plain only for a NOT NULL table column in a query
// with no outer join.
std::vector<member> column_members(session& s, const typed_query& t, const PGresult* d) {
    std::vector<member> out;
    std::set<std::string> idents;
    for (int i = 0; i < PQnfields(d); ++i) {
        std::string label = PQfname(d, i), name = label;
        std::optional<bool> nullable;
        if (name.size() > 1 && (name.back() == '!' || name.back() == '?')) {
            nullable = name.back() == '?';
            name.pop_back();
        }
        if (label == "?column?")
            s.problems.push_back(t.q.where() + ": result column " + std::to_string(i + 1) + " needs a name (AS ...)");
        bool not_null = !t.s.outer_join && s.not_null.count({PQftable(d, i), PQftablecol(d, i)});
        auto type = s.map(PQftype(d, i), PQfmod(d, i), t.q.file + ": " + t.q.name + "." + name);
        out.push_back({column_ident(name), optional_of(type, nullable.value_or(!not_null)), label});
        if (!idents.insert(out.back().ident).second)
            throw std::runtime_error(t.q.where() + ": two result columns map to member " + out.back().ident + "; alias one");
    }
    return out;
}

typed_query type_query(session& s, const query& q) {
    typed_query t{q, scan(q), {}, {}};
    auto d = describe(s.c, t.s.server, q.where());
    t.params = param_members(s, t, d.get());
    if (q.kind == result_kind::exec) return t;
    t.columns = column_members(s, t, d.get());
    if (t.columns.empty()) throw std::runtime_error(q.where() + " returns no columns; annotate it :exec");
    return t;
}

std::string read_file(const std::string& path);

std::vector<typed_query> type_queries(session& s, const std::vector<std::string>& files) {
    std::vector<typed_query> out;
    std::set<std::string> names;
    for (const auto& f : files) {
        for (const auto& q : parse_query_file(std::filesystem::path(f).filename().string(), read_file(f))) {
            for (const auto& n : {q.name, q.name + "_row", q.name + "_params"})
                if (!names.insert(n).second) throw std::runtime_error(q.where() + ": the name is used twice");
            out.push_back(type_query(s, q));
        }
    }
    return out;
}

// ------------------------------------------------------------ query output

// How a parameter of C++ type T is passed.
std::string param_decl(const member& p) {
    static const std::set<std::string> by_value = {"bool", "std::int16_t", "std::int32_t", "std::int64_t", "float", "double"};
    if (p.type == "std::string") return "std::string_view " + p.ident;
    if (by_value.count(p.type)) return p.type + " " + p.ident;
    return "const " + p.type + "& " + p.ident;
}

// The function's return type and the conn member that runs the statement.
std::pair<std::string, std::string> result_of(const typed_query& t) {
    auto row = t.q.name + "_row";
    switch (t.q.kind) {
    case result_kind::one: return {row, "conn.query_one<" + row + ">"};
    case result_kind::opt: return {"std::optional<" + row + ">", "conn.query_opt<" + row + ">"};
    case result_kind::many: return {"std::vector<" + row + ">", "conn.query<" + row + ">"};
    case result_kind::exec: break;
    }
    return {"std::int64_t", "conn.exec"};
}

// The format string as adjacent literals, one per SQL line.
void emit_format(std::ostream& o, const std::string& text) {
    for (size_t start = 0; start < text.size();) {
        auto nl = text.find('\n', start);
        auto end = nl == std::string::npos ? text.size() : nl + 1;
        o << "        " << quote_literal(text.substr(start, end - start)) << (end == text.size() ? "" : "\n");
        start = end;
    }
}

// One input is a parameter of the function; two or more are the fields of
// `<name>_params`.
void emit_function(std::ostream& o, const typed_query& t) {
    bool packed = t.params.size() + t.s.holes.size() >= 2;
    std::vector<std::string> decls = {"::pg::conn& conn"}, args;
    for (const auto& a : t.s.args) args.push_back(packed ? "p." + a : a);
    if (packed) {
        o << "struct " << t.q.name << "_params {\n";
        for (const auto& p : t.params)
            o << "    " << (p.type == "std::string" ? "std::string_view" : p.type) << " " << p.ident << ";\n";
        for (const auto& h : t.s.holes) o << "    ::pg::sql " << h << ";\n";
        o << "};\n";
        decls.push_back("const " + t.q.name + "_params& p");
    } else {
        for (const auto& p : t.params) decls.push_back(param_decl(p));
        for (const auto& h : t.s.holes) decls.push_back("const ::pg::sql& " + h);
    }
    auto [type, call] = result_of(t);
    o << "inline " << type << " " << t.q.name << "(" << join(decls, ", ") << ") {\n"
      << "    return " << call << "(::pg::sql{\n";
    emit_format(o, t.s.format);
    o << (args.empty() ? "" : ",\n        " + join(args, ", ")) << "});\n}\n";
}

std::string render_queries(const std::vector<typed_query>& qs, const std::string& ns,
                           const std::vector<std::string>& files) {
    std::vector<std::string> names;
    for (const auto& f : files) names.push_back(std::filesystem::path(f).filename().string());
    std::ostringstream o;
    o << generated_by(names.empty() ? "no query files" : join(names, ", "))
      << "#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <optional>\n#include <string>\n"
      << "#include <string_view>\n#include <vector>\n\n"
      << "#include \"pg/codec.hpp\"\n#include \"pg/pool.hpp\"\n#include \"pg/sql.hpp\"\n\n"
      << "namespace " << ns << " {\n";
    for (const auto& t : qs) {
        o << "\n// " << t.q.where() << "\n";
        if (t.q.kind != result_kind::exec) emit_read_shape(o, t.q.name + "_row", t.columns);
        emit_function(o, t);
    }
    o << "\n}\n";
    return o.str();
}

// ------------------------------------------------------------ files

std::optional<std::string> read_if_exists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string read_file(const std::string& path) {
    auto text = read_if_exists(path);
    if (!text) throw std::runtime_error("cannot read " + path);
    return *text;
}

void write_file(const std::string& path, const std::string& text) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path);
    out << text;
}

struct output {
    std::string path;
    std::string text;
};

// --check: every output must already be on disk unchanged.
int check_outputs(const std::vector<output>& outs) {
    int rc = 0;
    for (const auto& o : outs) {
        if (read_if_exists(o.path) == o.text) continue;
        std::cerr << "pg_tablegen: " << o.path << " is out of date; rerun pg_tablegen without --check\n";
        rc = 1;
    }
    return rc;
}

// ------------------------------------------------------------ server

// Connects with the DSN's own settings except for the database name; works
// for both URI and key=value DSNs.
conn_ptr connect_to(const std::string& dsn, const std::string& dbname) {
    char* err = nullptr;
    std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> opts(PQconninfoParse(dsn.c_str(), &err), PQconninfoFree);
    if (!opts) {
        std::string msg = err ? err : "invalid DSN";
        PQfreemem(err);
        throw std::runtime_error("DSN: " + msg);
    }
    std::vector<const char*> k, v;
    for (auto* o = opts.get(); o->keyword; ++o) {
        if (!o->val || std::string(o->keyword) == "dbname") continue;
        k.push_back(o->keyword);
        v.push_back(o->val);
    }
    k.push_back("dbname");
    v.push_back(dbname.c_str());
    k.push_back(nullptr);
    v.push_back(nullptr);
    conn_ptr c(PQconnectdbParams(k.data(), v.data(), 0));
    if (PQstatus(c.get()) != CONNECTION_OK) throw std::runtime_error(std::string("connect: ") + PQerrorMessage(c.get()));
    return c;
}

// A database that exists for the lifetime of the object.
class temp_database {
public:
    explicit temp_database(PGconn* admin)
        : admin_(admin),
          name_("tablegen_" + std::to_string(getpid()) + "_" + std::to_string(std::random_device{}())) {
        exec(admin_, "CREATE DATABASE \"" + name_ + "\" TEMPLATE template0", "create database");
    }
    temp_database(const temp_database&) = delete;
    temp_database& operator=(const temp_database&) = delete;
    ~temp_database() {
        std::string sql = "DROP DATABASE IF EXISTS \"" + name_ + "\" WITH (FORCE)";
        result_ptr r(PQexec(admin_, sql.c_str()));
        if (!r || PQresultStatus(r.get()) != PGRES_COMMAND_OK)
            std::cerr << "pg_tablegen: warning: cannot drop database " << name_ << "\n";
    }
    const std::string& name() const { return name_; }

private:
    PGconn* admin_;
    std::string name_;
};

// A scratch database and a connection to it; the connection closes before
// the database is dropped.
class scratch {
public:
    scratch(PGconn* admin, const std::string& dsn) : db_(admin), conn_(connect_to(dsn, db_.name())) {
        exec(c(), "SET client_min_messages TO warning", "set client_min_messages");
    }
    PGconn* c() const { return conn_.get(); }

private:
    temp_database db_;
    conn_ptr conn_;
};

// ------------------------------------------------------------------ main

int run(const options& o) {
    auto admin = connect_to(o.dsn, "postgres");
    scratch db(admin.get(), o.dsn);
    exec(db.c(), read_file(o.schema), "schema.sql");
    session s{db.c(), {}, {}};
    auto tables = read_tables(s);
    auto queries = type_queries(s, o.queries);
    if (!s.problems.empty()) throw std::runtime_error(join(s.problems, "\n"));
    std::vector<output> outs{{o.out, render_tables(tables, o.ns, std::filesystem::path(o.schema).filename().string())}};
    if (!o.queries_out.empty()) outs.push_back({o.queries_out, render_queries(queries, o.ns, o.queries)});
    if (o.check) return check_outputs(outs);
    for (const auto& out : outs) write_file(out.path, out.text);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_args(argc, argv));
    } catch (const usage_error& e) {
        std::cerr << "pg_tablegen: " << e.what() << "\n" << usage;
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "pg_tablegen: " << e.what() << "\n";
        return 1;
    }
}
