// pg_tablegen DSN schema.sql out.hpp [options]
//
// Creates a scratch database on the server, runs schema.sql in it and writes,
// per table, column tags, locators, a read shape `row` and a write shape
// `write` (CONTEXT.md, 生成代码). Query files are prepared in the same database
// and become typed functions (ADR 0008, 0009). --migrations builds a second
// scratch database from a migrations directory and compares the two. Scratch
// databases are always dropped.

#include <libpq-fe.h>

#include <algorithm>
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
    "       [--override table.column=Type]... [--override-type pgtype=Type]...\n"
    "       [--queries file.sql]... [--queries-out queries.hpp]\n"
    "       [--migrations dir] [--check]\n";

struct usage_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct options {
    std::string dsn, schema, out, ns = "db";
    std::map<std::string, std::string> column_types;  // "table.column" -> C++ type
    std::map<std::string, std::string> pg_types;      // PG type name -> C++ type
    std::vector<std::string> queries;
    std::string queries_out;
    std::string migrations;
    bool check = false;
};

std::pair<std::string, std::string> split_assignment(const std::string& s, const std::string& flag) {
    auto eq = s.find('=');
    if (eq == 0 || eq == std::string::npos || eq + 1 == s.size())
        throw usage_error(flag + " expects name=Type, got \"" + s + "\"");
    return {s.substr(0, eq), s.substr(eq + 1)};
}

// Applies one option that takes a value.
void set_option(options& o, const std::string& flag, const std::string& v) {
    if (flag == "--namespace") o.ns = v;
    else if (flag == "--override") o.column_types.insert(split_assignment(v, flag));
    else if (flag == "--override-type") o.pg_types.insert(split_assignment(v, flag));
    else if (flag == "--queries") o.queries.push_back(v);
    else if (flag == "--queries-out") o.queries_out = v;
    else if (flag == "--migrations") o.migrations = v;
    else throw usage_error("unknown option " + flag);
}

// DSN schema.sql out.hpp, then options; a fourth positional argument is the namespace.
options parse_args(int argc, char** argv) {
    options o;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--check") {
            o.check = true;
        } else if (a.starts_with("--")) {
            if (i + 1 >= argc) throw usage_error(a + " needs a value");
            set_option(o, a, argv[++i]);
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

std::string read_file(const std::string& path);

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

// Runs sql with text parameters.
result_ptr exec_params(PGconn* c, const std::string& sql, const std::vector<std::string>& params,
                       const std::string& what) {
    std::vector<const char*> v;
    for (const auto& p : params) v.push_back(p.c_str());
    return checked(PQexecParams(c, sql.c_str(), static_cast<int>(v.size()), nullptr, v.data(), nullptr, nullptr, 0),
                   c, what);
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

// Names a column's tag or shape member cannot take because the generated code
// reserves them: the pg_ prefix and the row and write shape names.
bool clashes_with_member(const std::string& s) {
    return s.starts_with("pg_") || s == "row" || s == "write";
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

std::string column_ident(const std::string& s) {
    auto id = table_ident(s);
    return clashes_with_member(id) ? id + "_" : id;
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

std::string quote_ident(const std::string& s) { return "\"" + s + "\""; }

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

// ------------------------------------------------------------ PG types

// A PG type with its domains resolved.
struct pg_type {
    std::vector<std::string> names;  // the type itself, then each domain's base down to the base type
    char kind = 0;                   // pg_type.typtype of the base type
    int typmod = -1;                 // the column's typmod, else the nearest domain's
    bool not_null = false;           // some domain on the way says NOT NULL
    const std::string& base() const { return names.back(); }
};

// Looks up types by OID, following domains to their base type.
class type_catalog {
public:
    explicit type_catalog(PGconn* c) : c_(c) {}

    pg_type resolve(Oid id, int typmod) {
        pg_type t = chain(id);
        if (typmod >= 0) t.typmod = typmod;
        return t;
    }

private:
    const pg_type& chain(Oid id) {
        auto it = cache_.find(id);
        if (it != cache_.end()) return it->second;
        auto r = exec_params(c_,
            "WITH RECURSIVE d(typname, typtype, typbasetype, typtypmod, typnotnull, depth) AS ("
            "  SELECT typname::text, typtype, typbasetype, typtypmod, typnotnull, 0 "
            "  FROM pg_catalog.pg_type WHERE oid = $1::oid "
            "  UNION ALL "
            "  SELECT t.typname::text, t.typtype, t.typbasetype, t.typtypmod, t.typnotnull, d.depth + 1 "
            "  FROM d JOIN pg_catalog.pg_type t ON t.oid = d.typbasetype WHERE d.typtype = 'd') "
            "SELECT typname, typtype, typtypmod, typnotnull FROM d ORDER BY depth",
            {std::to_string(id)}, "read type " + std::to_string(id));
        pg_type t;
        for (int i = 0; i < PQntuples(r.get()); ++i) {
            t.names.push_back(cell(r.get(), i, 0));
            t.kind = cell(r.get(), i, 1)[0];
            int mod = std::stoi(cell(r.get(), i, 2));
            if (t.typmod < 0 && mod >= 0) t.typmod = mod;
            t.not_null = t.not_null || cell(r.get(), i, 3) == "t";
        }
        if (t.names.empty()) throw std::runtime_error("unknown type OID " + std::to_string(id));
        return cache_.emplace(id, std::move(t)).first->second;
    }

    PGconn* c_;
    std::map<Oid, pg_type> cache_;
};

// NUMERIC(p, s) with p <= 18 and 0 <= s <= 18 is a fixed-point decimal; any
// other numeric is arbitrary precision.
std::string numeric_type(int typmod) {
    if (typmod < 4) return "::pg::numeric";
    int precision = ((typmod - 4) >> 16) & 0xffff;
    int scale = (((typmod - 4) & 0x7ff) ^ 1024) - 1024;
    if (precision > 18 || scale < 0 || scale > 18) return "::pg::numeric";
    return "::pg::decimal<" + std::to_string(scale) + ">";
}

// The C++ type libpg's codecs give a PG base type, if any.
std::optional<std::string> builtin_type(const pg_type& t) {
    static const std::map<std::string, std::string> m = {
        {"bool", "bool"},
        {"int2", "std::int16_t"},
        {"int4", "std::int32_t"},
        {"int8", "std::int64_t"},
        {"float4", "float"},
        {"float8", "double"},
        {"text", "std::string"},
        {"varchar", "std::string"},
        {"bpchar", "std::string"},
        {"citext", "std::string"},
        {"name", "std::string"},
        {"bytea", "::pg::bytea"},
        {"json", "::pg::jsonb"},
        {"jsonb", "::pg::jsonb"},
        {"timestamptz", "::pg::timestamptz"},
        {"date", "::pg::date"},
        {"uuid", "::pg::uuid"},
    };
    if (t.kind == 'e') return "std::string";
    if (t.base() == "numeric") return numeric_type(t.typmod);
    auto it = m.find(t.base());
    if (it == m.end()) return std::nullopt;
    return it->second;
}

// Picks the C++ type of a value: a column override, then a type override for
// the type or any domain under it, then the builtin mapping. Records each
// problem instead of stopping at the first.
class type_mapper {
public:
    explicit type_mapper(const options& o) : o_(o) {}

    // `column` is "table.column" when the value comes from a table column.
    std::optional<std::string> map(const pg_type& t, const std::string& column) {
        if (auto it = o_.column_types.find(column); it != o_.column_types.end()) {
            used_.insert(column);
            return it->second;
        }
        for (const auto& n : t.names)
            if (auto it = o_.pg_types.find(n); it != o_.pg_types.end()) return it->second;
        return builtin_type(t);
    }

    // Like map(), but an unmapped type is recorded as a problem of `where`.
    std::string map_or_report(const pg_type& t, const std::string& column, const std::string& where) {
        if (auto m = map(t, column)) return *m;
        problems_.push_back(where + ": PG type " + describe(t) + " has no C++ mapping; " + advice(t, column));
        return "void";
    }

    void report(std::string problem) { problems_.push_back(std::move(problem)); }

    // Throws with every recorded problem, including column overrides that
    // matched no column.
    void throw_problems() {
        for (const auto& [col, type] : o_.column_types)
            if (!used_.count(col)) problems_.push_back("--override " + col + "=" + type + " matches no column");
        if (problems_.empty()) return;
        throw std::runtime_error(join(problems_, "\n"));
    }

private:
    static std::string describe(const pg_type& t) {
        return t.names.size() == 1 ? t.base() : t.names.front() + " (base type " + t.base() + ")";
    }
    static std::string advice(const pg_type& t, const std::string& column) {
        std::string s;
        if (!column.empty()) s += "pass --override " + column + "=Type or ";
        return s + "--override-type " + t.base() + "=Type (Type needs a pg::codec), or cast it to ::text in SQL";
    }

    const options& o_;
    std::set<std::string> used_;
    std::vector<std::string> problems_;
};

// What reading a schema or query needs: the scratch connection, its types
// and the type mapping.
struct session {
    PGconn* c;
    type_catalog types;
    type_mapper mapper;
    session(PGconn* conn, const options& o) : c(conn), types(conn), mapper(o) {}
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
    std::string schema;
    std::vector<column> columns;                    // attnum order
    std::vector<std::string> key;                   // primary key columns
    std::vector<std::vector<std::string>> uniques;  // by constraint name
};

using tables_t = std::map<std::string, table>;  // sorted by table name

// Schemas that belong to the server, not to schema.sql.
const char* const user_schemas_only =
    "n.nspname NOT IN ('pg_catalog', 'information_schema') "
    "AND n.nspname NOT LIKE 'pg\\_toast%' AND n.nspname NOT LIKE 'pg\\_temp\\_%' ";

char first_char(const std::string& s) { return s.empty() ? 0 : s[0]; }

column read_column(session& s, const PGresult* r, int i, const std::string& table_name) {
    column col;
    col.name = cell(r, i, 1);
    col.ident = column_ident(col.name);
    auto type = s.types.resolve(static_cast<Oid>(std::stoul(cell(r, i, 2))), std::stoi(cell(r, i, 3)));
    auto key = table_name + "." + col.name;
    col.cxx = s.mapper.map_or_report(type, key, key);
    col.nullable = !(cell(r, i, 4) == "t" || type.not_null);
    col.has_default = cell(r, i, 5) == "t";
    col.identity = first_char(cell(r, i, 6));
    col.generated = first_char(cell(r, i, 7));
    return col;
}

// Columns of every ordinary or partitioned (parent) table, in attnum order.
void read_columns(session& s, tables_t& out) {
    auto r = exec(s.c,
        std::string(
        "SELECT cl.relname, a.attname, a.atttypid, a.atttypmod, a.attnotnull, a.atthasdef, "
        "       a.attidentity, a.attgenerated, n.nspname "
        "FROM pg_catalog.pg_class cl "
        "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = cl.oid AND a.attnum > 0 AND NOT a.attisdropped "
        "WHERE cl.relkind IN ('r', 'p') AND NOT cl.relispartition AND ") + user_schemas_only +
        "ORDER BY cl.relname COLLATE \"C\", a.attnum",
        "read columns");
    for (int i = 0; i < PQntuples(r.get()); ++i) {
        auto& t = out[cell(r.get(), i, 0)];
        std::string schema = cell(r.get(), i, 8);
        if (!t.schema.empty() && t.schema != schema)
            throw std::runtime_error("table " + t.name + " is defined in both schema " + t.schema + " and " + schema);
        t.name = cell(r.get(), i, 0);
        t.schema = schema;
        t.columns.push_back(read_column(s, r.get(), i, t.name));
    }
}

// One row per constraint column, in constraint-name then column-position order.
void read_constraints(PGconn* c, tables_t& out) {
    auto r = exec(c,
        std::string(
        "SELECT cl.relname, k.contype, a.attname, k.ord "
        "FROM (SELECT con.conrelid, con.conname, con.contype, u.attnum, u.ord "
        "      FROM pg_catalog.pg_constraint con, unnest(con.conkey) WITH ORDINALITY u(attnum, ord) "
        "      WHERE con.contype IN ('p', 'u')) k "
        "JOIN pg_catalog.pg_class cl ON cl.oid = k.conrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = k.conrelid AND a.attnum = k.attnum "
        "WHERE cl.relkind IN ('r', 'p') AND NOT cl.relispartition AND ") + user_schemas_only +
        "ORDER BY cl.relname COLLATE \"C\", k.conname COLLATE \"C\", k.ord",
        "read constraints");
    for (int i = 0; i < PQntuples(r.get()); ++i) {
        auto& t = out.at(cell(r.get(), i, 0));
        std::string name = cell(r.get(), i, 2);
        if (cell(r.get(), i, 1) == "p") {
            t.key.push_back(name);
            continue;
        }
        if (cell(r.get(), i, 3) == "1") t.uniques.emplace_back();
        t.uniques.back().push_back(name);
    }
}

tables_t read_tables(session& s) {
    tables_t tables;
    read_columns(s, tables);
    read_constraints(s.c, tables);
    return tables;
}

const column& find_column(const table& t, const std::string& name) {
    for (const auto& c : t.columns)
        if (c.name == name) return c;
    throw std::runtime_error("table " + t.name + ": no column " + name);
}

// ------------------------------------------------------------- shapes

// One member of a generated shape.
struct member {
    std::string ident;
    std::string type;
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

void emit_read_shape(std::ostream& o, const std::string& name, const std::vector<member>& ms) {
    o << "struct " << name << " {\n";
    emit_members(o, ms);
    o << "    static constexpr std::size_t pg_columns = " << ms.size() << ";\n";
    emit_each(o, ms, "");
    o << "};\n";
}

// ------------------------------------------------------------ table output

std::string key_types(const table& t, const std::vector<std::string>& cols) {
    std::vector<std::string> types;
    for (const auto& n : cols) types.push_back(find_column(t, n).cxx);
    return "std::tuple<" + join(types, ", ") + ">";
}

std::string names_array(const std::string& member_name, const std::vector<std::string>& cols) {
    std::vector<std::string> q;
    for (const auto& n : cols) q.push_back(quote_literal(n));
    return "std::array<std::string_view, " + std::to_string(cols.size()) + "> " + member_name + "{" + join(q, ", ") + "}";
}

void emit_table_struct(std::ostream& o, const table& t) {
    o << "struct table_ {\n"
      << "    static constexpr std::string_view name = " << quote_literal(t.name) << ";\n";
    if (!t.key.empty())
        o << "    using key = " << key_types(t, t.key) << ";\n"
          << "    static constexpr " << names_array("key_columns", t.key) << ";\n";
    o << "};\n";
}

void emit_column(std::ostream& o, const column& c) {
    o << "struct " << c.ident << " {\n"
      << "    using pg_table = table_;\n"
      << "    using pg_type = " << optional_of(c.cxx, c.nullable) << ";\n"
      << "    static constexpr std::string_view pg_name = " << quote_literal(c.name) << ";\n};\n";
}

// Locator name for a unique constraint: by_a or by_a_and_b.
std::string locator_name(const table& t, const std::vector<std::string>& cols) {
    std::vector<std::string> ids;
    for (const auto& n : cols) ids.push_back(sanitize(find_column(t, n).name));
    return "by_" + join(ids, "_and_");
}

void emit_locator(std::ostream& o, const table& t, const std::vector<std::string>& cols) {
    auto name = locator_name(t, cols);
    o << "struct " << name << "_t {\n"
      << "    using pg_table = table_;\n"
      << "    using key = " << key_types(t, cols) << ";\n"
      << "    static constexpr " << names_array("key_columns", cols) << ";\n"
      << "};\n"
      << "inline constexpr " << name << "_t " << name << "{};\n";
}

std::vector<member> read_members(const table& t) {
    std::vector<member> ms;
    for (const auto& c : t.columns) ms.push_back({c.ident, optional_of(c.cxx, c.nullable)});
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
        ms.push_back({c.ident, write_type(c)});
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
    auto add = [&](const std::string& n) {
        if (!seen.insert(n).second)
            throw std::runtime_error("table " + t.name + ": generated name " + n + " is used twice");
    };
    for (const auto& c : t.columns) add(c.ident);
    for (const auto& u : t.uniques) {
        add(locator_name(t, u));
        add(locator_name(t, u) + "_t");
    }
}

void emit_namespace(std::ostream& o, const std::string& ns, const table& t) {
    check_names(t);
    o << "\nnamespace " << ns << "::" << table_ident(t.name) << " {\n";
    emit_table_struct(o, t);
    for (const auto& c : t.columns) emit_column(o, c);
    for (const auto& u : t.uniques) emit_locator(o, t, u);
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
    std::vector<std::string> params;  // names of $1..$n
    std::string body;                 // SQL exactly as in the file, outer blank space trimmed
    std::string where() const { return file + ": query " + name; }
};

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_words(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep))
        if (auto w = trim(cur); !w.empty()) out.push_back(w);
    return out;
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
    auto words = split_words(line.substr(8), ' ');
    query q;
    q.file = file;
    if (words.size() != 2) throw std::runtime_error(file + ": expected \"-- name: fn :one|:opt|:many|:exec\", got \"" + trim(line) + "\"");
    q.name = words[0];
    if (!is_identifier(q.name) || cxx_keywords().count(q.name))
        throw std::runtime_error(file + ": \"" + q.name + "\" is not a usable C++ function name");
    q.kind = parse_kind(words[1], q.where());
    return q;
}

// Splits a query file into blocks headed by `-- name:` and an optional
// `-- params:` line; text before the first block is ignored.
std::vector<query> parse_query_file(const std::string& file, const std::string& text) {
    std::vector<query> out;
    std::istringstream in(text);
    std::string line;
    bool header = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF files read as LF; the body is otherwise verbatim
        if (line.starts_with("-- name:")) {
            out.push_back(parse_name_line(file, line));
            header = true;
        } else if (header && line.starts_with("-- params:")) {
            for (auto& p : split_words(line.substr(10), ',')) out.back().params.push_back(p);
        } else if (!out.empty()) {
            header = false;
            out.back().body += line + "\n";
        }
    }
    for (auto& q : out) {
        q.body = trim(q.body);
        if (q.body.empty()) throw std::runtime_error(q.where() + " has no SQL");
    }
    return out;
}

// The query body as a pg::sql format string: braces doubled and each hole
// `/*@name*/` replaced by `{}`; `holes` receives the hole names in order.
std::string format_body(const query& q, std::vector<std::string>& holes) {
    std::string out;
    for (size_t i = 0; i < q.body.size(); ++i) {
        char ch = q.body[i];
        if (q.body.compare(i, 3, "/*@") == 0) {
            auto end = q.body.find("*/", i + 3);
            auto name = end == std::string::npos ? "" : q.body.substr(i + 3, end - i - 3);
            if (!is_identifier(name)) throw std::runtime_error(q.where() + ": a hole must be /*@name*/");
            holes.push_back(name);
            out += "{}";
            i = end + 1;
        } else {
            out += ch;
            if (ch == '{' || ch == '}') out += ch;
        }
    }
    return out;
}

// --------------------------------------------------------- nullability

std::string explain(PGconn* c, const query& q) {
    auto r = exec(c, "EXPLAIN (VERBOSE, FORMAT JSON, GENERIC_PLAN) " + q.body, q.where() + ": EXPLAIN");
    return cell(r.get(), 0, 0);
}

// Tables (and their partition roots) scanned on the nullable side of an outer
// join anywhere in the plan, including inside CTEs referenced from there.
std::set<Oid> nullable_relations(PGconn* c, const std::string& plan, const std::string& where) {
    auto r = exec_params(c,
        "WITH nodes AS ("
        "  SELECT n FROM jsonb_path_query($1::jsonb, 'strict $.**') n WHERE jsonb_typeof(n) = 'object'), "
        "sides AS ("
        "  SELECT ch FROM nodes, jsonb_array_elements(n->'Plans') ch "
        "  WHERE (n->>'Join Type' IN ('Left', 'Full') AND ch->>'Parent Relationship' = 'Inner') "
        "     OR (n->>'Join Type' IN ('Right', 'Full') AND ch->>'Parent Relationship' = 'Outer')), "
        "under AS ("
        "  SELECT u FROM sides, jsonb_path_query(ch, 'strict $.**') u WHERE jsonb_typeof(u) = 'object'), "
        "cte_under AS ("
        "  SELECT u FROM nodes, jsonb_path_query(n, 'strict $.**') u "
        "  WHERE jsonb_typeof(u) = 'object' "
        "    AND n->>'Subplan Name' IN (SELECT 'CTE ' || (x->>'CTE Name') FROM under x(x))), "
        "rels AS ("
        "  SELECT format('%I.%I', u->>'Schema', u->>'Relation Name')::regclass::oid AS o "
        "  FROM (SELECT u FROM under UNION ALL SELECT u FROM cte_under) x WHERE u ? 'Relation Name') "
        "SELECT o FROM rels UNION SELECT pg_partition_root(o) FROM rels WHERE pg_partition_root(o) IS NOT NULL",
        {plan}, where + ": read plan");
    std::set<Oid> out;
    for (int i = 0; i < PQntuples(r.get()); ++i) out.insert(static_cast<Oid>(std::stoul(cell(r.get(), i, 0))));
    return out;
}

// What the plan says about rows that have no source: tables referenced by the
// result but not scanned (a dummy Result replaced them, or every partition was
// pruned), and whether any node computes grouping sets, whose super-aggregate
// rows null the grouping columns.
struct plan_gaps {
    std::set<Oid> scanned;
    bool grouping_sets = false;
};

plan_gaps read_plan_gaps(PGconn* c, const std::string& plan, const std::string& where) {
    auto r = exec_params(c,
        "WITH nodes AS ("
        "  SELECT n FROM jsonb_path_query($1::jsonb, 'strict $.**') n WHERE jsonb_typeof(n) = 'object'), "
        "rels AS ("
        "  SELECT format('%I.%I', n->>'Schema', n->>'Relation Name')::regclass::oid AS o "
        "  FROM nodes WHERE n ? 'Relation Name' AND n ? 'Schema') "
        "SELECT 'g', 0 FROM nodes WHERE n ? 'Grouping Sets' OR n->>'Group Strategy' = 'Mixed' "
        "UNION ALL SELECT 's', o FROM rels "
        "UNION ALL SELECT 's', pg_partition_root(o) FROM rels WHERE pg_partition_root(o) IS NOT NULL",
        {plan}, where + ": read plan");
    plan_gaps out;
    for (int i = 0; i < PQntuples(r.get()); ++i) {
        if (cell(r.get(), i, 0) == "g") out.grouping_sets = true;
        else out.scanned.insert(static_cast<Oid>(std::stoul(cell(r.get(), i, 1))));
    }
    return out;
}

// Result positions of a top-level INSERT/UPDATE/DELETE/MERGE whose RETURNING
// entry is qualified by something other than the target or a scanned table:
// `old.x` or `new.x`.
std::set<int> returning_old_new(PGconn* c, const std::string& plan, const std::string& where) {
    auto r = exec_params(c,
        "WITH p AS (SELECT $1::jsonb -> 0 -> 'Plan' AS p), "
        "aliases AS ("
        "  SELECT quote_ident(n->>'Alias') AS a FROM p, jsonb_path_query(p, 'strict $.**') n "
        "  WHERE jsonb_typeof(n) = 'object' AND n ? 'Relation Name') "
        "SELECT i - 1 FROM p, jsonb_array_elements_text(p->'Output') WITH ORDINALITY o(e, i) "
        "WHERE p->>'Node Type' = 'ModifyTable' "
        "  AND substring(e from '^((?:\"(?:[^\"]|\"\")*\"|[[:alpha:]_][[:alnum:]_$]*))\\.') "
        "      NOT IN (SELECT a FROM aliases)",
        {plan}, where + ": read plan");
    std::set<int> out;
    for (int i = 0; i < PQntuples(r.get()); ++i) out.insert(std::stoi(cell(r.get(), i, 0)));
    return out;
}

// The table column a result column comes from: "table.column" and whether it
// is NOT NULL (by the column or its domain).
struct origin {
    std::string key;
    bool not_null = false;
};

std::optional<origin> column_origin(session& s, Oid table_oid, int attnum) {
    if (table_oid == InvalidOid || attnum <= 0) return std::nullopt;
    auto r = exec_params(s.c,
        "SELECT c.relname, a.attname, a.attnotnull, a.atttypid "
        "FROM pg_catalog.pg_class c JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid "
        "WHERE c.oid = $1::oid AND a.attnum = $2::int2",
        {std::to_string(table_oid), std::to_string(attnum)}, "read column origin");
    if (PQntuples(r.get()) != 1) return std::nullopt;
    auto type = s.types.resolve(static_cast<Oid>(std::stoul(cell(r.get(), 0, 3))), -1);
    return origin{cell(r.get(), 0, 0) + "." + cell(r.get(), 0, 1), cell(r.get(), 0, 2) == "t" || type.not_null};
}

// A result column as described by the server.
struct result_column {
    std::string name;             // without a `!` / `?` suffix
    std::optional<bool> forced;   // nullable as the suffix says
    std::optional<origin> from;   // table column it reads, if any
    Oid table = InvalidOid;
    pg_type type;
};

result_column describe_column(session& s, const PGresult* d, int i) {
    result_column rc;
    rc.name = PQfname(d, i);
    if (rc.name.size() > 1 && (rc.name.back() == '!' || rc.name.back() == '?')) {
        rc.forced = rc.name.back() == '?';
        rc.name.pop_back();
    }
    rc.table = PQftable(d, i);
    rc.from = column_origin(s, rc.table, PQftablecol(d, i));
    rc.type = s.types.resolve(PQftype(d, i), PQfmod(d, i));
    return rc;
}

// Nullability per ADR 0009: non-null only for a NOT NULL table column that is
// not on the nullable side of an outer join, not an unscanned table, not under
// grouping sets and not a RETURNING old/new value.
std::vector<bool> nullability(session& s, const query& q, const std::vector<result_column>& cols) {
    bool need_plan = std::any_of(cols.begin(), cols.end(),
                                 [](const auto& c) { return !c.forced && c.from && c.from->not_null; });
    std::set<Oid> rels;
    std::set<int> old_new;
    plan_gaps gaps;
    if (need_plan) {
        auto plan = explain(s.c, q);
        rels = nullable_relations(s.c, plan, q.where());
        old_new = returning_old_new(s.c, plan, q.where());
        gaps = read_plan_gaps(s.c, plan, q.where());
    }
    std::vector<bool> out;
    for (size_t i = 0; i < cols.size(); ++i) {
        const auto& c = cols[i];
        bool not_null = c.from && c.from->not_null && !gaps.grouping_sets && gaps.scanned.count(c.table) &&
                        !rels.count(c.table) && !old_new.count(static_cast<int>(i));
        out.push_back(c.forced ? *c.forced : !not_null);
    }
    return out;
}

// ------------------------------------------------------------ query typing

struct typed_query {
    query q;
    std::vector<member> params;       // C++ parameter types
    std::vector<std::string> holes;   // hole names, one per occurrence
    std::vector<member> columns;      // read shape members
    std::string format;               // pg::sql format text of the body
};

result_ptr prepare_and_describe(PGconn* c, const query& q) {
    checked(PQprepare(c, "", q.body.c_str(), 0, nullptr), c, q.where());
    return checked(PQdescribePrepared(c, ""), c, q.where() + ": describe");
}

std::vector<member> param_members(session& s, const query& q, const PGresult* d) {
    int n = PQnparams(d);
    if (static_cast<int>(q.params.size()) != n)
        throw std::runtime_error(q.where() + ": SQL has " + std::to_string(n) + " parameter(s) but \"-- params:\" names " +
                                 std::to_string(q.params.size()));
    std::vector<member> out;
    for (int i = 0; i < n; ++i) {
        auto where = q.where() + " parameter $" + std::to_string(i + 1) + " (" + q.params[i] + ")";
        auto type = s.mapper.map_or_report(s.types.resolve(PQparamtype(d, i), -1), "", where);
        out.push_back({q.params[i], type});
    }
    return out;
}

std::vector<member> column_members(session& s, const query& q, const PGresult* d) {
    std::vector<result_column> cols;
    for (int i = 0; i < PQnfields(d); ++i) cols.push_back(describe_column(s, d, i));
    auto nullable = nullability(s, q, cols);
    std::vector<member> out;
    for (size_t i = 0; i < cols.size(); ++i) {
        const auto& c = cols[i];
        auto where = q.where() + " column " + c.name;
        if (c.name == "?column?") s.mapper.report(q.where() + ": result column " + std::to_string(i + 1) + " needs a name (AS ...)");
        auto type = s.mapper.map_or_report(c.type, c.from ? c.from->key : "", where);
        out.push_back({column_ident(c.name), optional_of(type, nullable[i])});
    }
    return out;
}

// Parameter, hole and member names must each be distinct C++ names.
void check_query_names(const typed_query& t) {
    std::set<std::string> args;
    for (const auto& p : t.params)
        if (!is_identifier(p.ident) || cxx_keywords().count(p.ident) || !args.insert(p.ident).second)
            throw std::runtime_error(t.q.where() + ": parameter name \"" + p.ident + "\" is not a distinct C++ name");
    std::set<std::string> holes(t.holes.begin(), t.holes.end());
    for (const auto& h : holes)
        if (cxx_keywords().count(h) || !args.insert(h).second)
            throw std::runtime_error(t.q.where() + ": hole name \"" + h + "\" is not a distinct C++ name");
    std::set<std::string> members;
    for (const auto& m : t.columns)
        if (!members.insert(m.ident).second)
            throw std::runtime_error(t.q.where() + ": two result columns map to member " + m.ident + "; alias one");
}

typed_query type_query(session& s, const query& q) {
    if (PQserverVersion(s.c) < 160000)
        throw std::runtime_error("query files need PostgreSQL 16 or newer (EXPLAIN GENERIC_PLAN)");
    typed_query t;
    t.q = q;
    t.format = format_body(q, t.holes);
    auto d = prepare_and_describe(s.c, q);
    t.params = param_members(s, q, d.get());
    if (q.kind != result_kind::exec) t.columns = column_members(s, q, d.get());
    if (q.kind != result_kind::exec && t.columns.empty())
        throw std::runtime_error(q.where() + " returns no columns; annotate it :exec");
    check_query_names(t);
    return t;
}

std::vector<typed_query> type_queries(session& s, const std::vector<std::string>& files) {
    std::vector<typed_query> out;
    std::set<std::string> names;
    for (const auto& f : files) {
        auto display = std::filesystem::path(f).filename().string();
        for (const auto& q : parse_query_file(display, read_file(f))) {
            if (!names.insert(q.name).second || !names.insert(q.name + "_row").second)
                throw std::runtime_error(q.where() + ": the name is used twice");
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

std::string return_type(const typed_query& t) {
    auto row = t.q.name + "_row";
    switch (t.q.kind) {
    case result_kind::one: return row;
    case result_kind::opt: return "std::optional<" + row + ">";
    case result_kind::many: return "std::vector<" + row + ">";
    case result_kind::exec: return "std::int64_t";
    }
    return {};
}

std::string call(const typed_query& t) {
    auto row = "<" + t.q.name + "_row>";
    switch (t.q.kind) {
    case result_kind::one: return "conn.query_one" + row;
    case result_kind::opt: return "conn.query_opt" + row;
    case result_kind::many: return "conn.query" + row;
    case result_kind::exec: return "conn.exec";
    }
    return {};
}

// The statement's format string as adjacent literals, one per SQL line. A
// leading comment holds one `{}` per parameter so they render as $1..$n.
void emit_format(std::ostream& o, const typed_query& t) {
    std::string text;
    if (!t.params.empty()) text = "/*" + join(std::vector<std::string>(t.params.size(), "{}"), ",") + "*/";
    text += t.format;
    size_t start = 0;
    while (start < text.size()) {
        auto nl = text.find('\n', start);
        auto end = nl == std::string::npos ? text.size() : nl + 1;
        o << "        " << quote_literal(text.substr(start, end - start)) << (end == text.size() ? "" : "\n");
        start = end;
    }
}

void emit_function(std::ostream& o, const typed_query& t) {
    std::vector<std::string> decls = {"::pg::conn& conn"};
    std::vector<std::string> args;
    for (const auto& p : t.params) {
        decls.push_back(param_decl(p));
        args.push_back(p.ident);
    }
    std::set<std::string> seen;
    for (const auto& h : t.holes)
        if (seen.insert(h).second) decls.push_back("const ::pg::sql& " + h);
    args.insert(args.end(), t.holes.begin(), t.holes.end());
    o << "inline " << return_type(t) << " " << t.q.name << "(" << join(decls, ", ") << ") {\n"
      << "    return " << call(t) << "(::pg::sql{\n";
    emit_format(o, t);
    o << (args.empty() ? "" : ",\n        " + join(args, ", ")) << "});\n}\n";
}

std::string render_queries(const std::vector<typed_query>& qs, const std::string& ns,
                           const std::vector<std::string>& files) {
    std::vector<std::string> names;
    for (const auto& f : files) names.push_back(std::filesystem::path(f).filename().string());
    std::ostringstream o;
    o << generated_by(names.empty() ? "no query files" : join(names, ", "))
      << "#include <cstddef>\n#include <cstdint>\n#include <optional>\n#include <string>\n"
      << "#include <string_view>\n#include <vector>\n\n"
      << "#include \"pg/codec.hpp\"\n#include \"pg/pool.hpp\"\n#include \"pg/sql.hpp\"\n\n"
      << "// Each statement starts with a comment holding its parameters, so they are\n"
      << "// bound as $1..$n and the query body keeps its own $n references; holes are\n"
      << "// spliced where the body has /*@name*/.\n\n"
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

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<std::string> read_if_exists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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

PGconn* connect_or_throw(const char* const* keys, const char* const* values) {
    conn_ptr c(PQconnectdbParams(keys, values, 0));
    if (PQstatus(c.get()) != CONNECTION_OK) throw std::runtime_error(std::string("connect: ") + PQerrorMessage(c.get()));
    return c.release();
}

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
    std::vector<std::string> keys, values;
    for (auto* o = opts.get(); o->keyword; ++o) {
        if (!o->val || std::string(o->keyword) == "dbname") continue;
        keys.push_back(o->keyword);
        values.push_back(o->val);
    }
    keys.push_back("dbname");
    values.push_back(dbname);
    std::vector<const char*> k, v;
    for (size_t i = 0; i < keys.size(); ++i) {
        k.push_back(keys[i].c_str());
        v.push_back(values[i].c_str());
    }
    k.push_back(nullptr);
    v.push_back(nullptr);
    return conn_ptr(connect_or_throw(k.data(), v.data()));
}

// A database that exists for the lifetime of the object.
class temp_database {
public:
    explicit temp_database(PGconn* admin)
        : admin_(admin),
          name_("tablegen_" + std::to_string(getpid()) + "_" + std::to_string(std::random_device{}())) {
        exec(admin_, "CREATE DATABASE " + quote_ident(name_) + " TEMPLATE template0", "create database");
    }
    temp_database(const temp_database&) = delete;
    temp_database& operator=(const temp_database&) = delete;
    ~temp_database() {
        std::string sql = "DROP DATABASE IF EXISTS " + quote_ident(name_) + " WITH (FORCE)";
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

// ------------------------------------------------------------ drift

// Every table column (position, type, nullability, default, identity,
// generation) and every constraint except NOT NULL, one line each, outside the
// libpg schema.
std::set<std::string> catalog_lines(PGconn* c) {
    auto r = exec(c,
        std::string(
        "SELECT format('%I.%I column %s %I %s%s%s%s%s', n.nspname, cl.relname, "
        "  row_number() OVER (PARTITION BY cl.oid ORDER BY a.attnum), a.attname, "
        "  format_type(a.atttypid, a.atttypmod), "
        "  CASE WHEN a.attnotnull THEN ' not null' ELSE '' END, "
        "  COALESCE(' default ' || pg_get_expr(d.adbin, d.adrelid), ''), "
        "  CASE a.attidentity WHEN 'a' THEN ' identity always' WHEN 'd' THEN ' identity by default' ELSE '' END, "
        "  CASE WHEN a.attgenerated <> '' THEN ' generated' ELSE '' END) "
        "FROM pg_catalog.pg_class cl "
        "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = cl.oid AND a.attnum > 0 AND NOT a.attisdropped "
        "LEFT JOIN pg_catalog.pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
        "WHERE cl.relkind IN ('r', 'p') AND n.nspname <> 'libpg' AND ") + user_schemas_only +
        "UNION ALL "
        "SELECT format('%I.%I constraint %s', n.nspname, cl.relname, pg_get_constraintdef(con.oid)) "
        "FROM pg_catalog.pg_constraint con "
        "JOIN pg_catalog.pg_class cl ON cl.oid = con.conrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
        "WHERE con.contype <> 'n' AND n.nspname <> 'libpg' AND " + user_schemas_only,
        "read catalog");
    std::set<std::string> out;
    for (int i = 0; i < PQntuples(r.get()); ++i) out.insert(cell(r.get(), i, 0));
    return out;
}

std::vector<std::filesystem::path> migration_files(const std::string& dir) {
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".sql") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
    if (files.empty()) throw std::runtime_error("no .sql files in " + dir);
    return files;
}

std::set<std::string> migrated_catalog(PGconn* admin, const options& o) {
    scratch db(admin, o.dsn);
    for (const auto& f : migration_files(o.migrations))
        exec(db.c(), read_file(f.string()), "migration " + f.filename().string());
    return catalog_lines(db.c());
}

std::vector<std::string> only_in(const std::set<std::string>& a, const std::set<std::string>& b) {
    std::vector<std::string> out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

// Throws when the migrations build a different schema than schema.sql.
void check_drift(PGconn* admin, const options& o, const std::set<std::string>& schema) {
    auto migrated = migrated_catalog(admin, o);
    auto missing = only_in(schema, migrated), extra = only_in(migrated, schema);
    if (missing.empty() && extra.empty()) return;
    std::string msg = "schema drift between " + o.schema + " and " + o.migrations;
    for (const auto& l : missing) msg += "\n  only in schema.sql: " + l;
    for (const auto& l : extra) msg += "\n  only in migrations: " + l;
    throw std::runtime_error(msg);
}

// ------------------------------------------------------------------ main

int run(const options& o) {
    auto admin = connect_to(o.dsn, "postgres");
    scratch db(admin.get(), o.dsn);
    exec(db.c(), read_file(o.schema), "schema.sql");
    session s(db.c(), o);
    auto tables = read_tables(s);
    auto queries = type_queries(s, o.queries);
    s.mapper.throw_problems();
    if (!o.migrations.empty()) check_drift(admin.get(), o, catalog_lines(db.c()));
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
