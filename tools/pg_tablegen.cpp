// pg_tablegen DSN schema.sql out.hpp [namespace]
//
// Creates a throwaway database on the server, runs schema.sql in it, reads the
// resulting tables from pg_catalog and writes one column tag per column
// (docs/design.md, "Generated column tags"). The database is always dropped.

#include <libpq-fe.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

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

std::string cell(const PGresult* r, int row, int col) { return PQgetvalue(r, row, col); }

// ------------------------------------------------------------------- model

struct column {
    std::string name;
    std::string ident;     // C++ name of the tag
    std::string cxx;       // C++ type without optional
    std::string pg_type;   // set only when the type fell back to std::string
    bool nullable = false;
};

struct table {
    std::string name;
    std::string schema;
    std::vector<column> columns;                    // attnum order
    std::vector<std::string> key;                   // primary key columns
    std::vector<std::vector<std::string>> uniques;  // by constraint name
};

using tables_t = std::map<std::string, table>;  // sorted by table name

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
        "std", "pg", "table_"};
    return k;
}

// Names that cannot be the tag of a column because the tag declares a member
// of the same name.
bool clashes_with_member(const std::string& s) {
    return s == "name" || s == "table" || s == "type";
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

// A C++ string literal; control characters use 3-digit octal escapes so they
// cannot swallow the characters that follow.
std::string quote_literal(const std::string& s) {
    std::string out = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch < 0x20 || ch == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", ch);
            out += buf;
        } else {
            out += static_cast<char>(ch);
        }
    }
    return out + '"';
}

std::string quote_ident(const std::string& s) { return "\"" + s + "\""; }

// ------------------------------------------------------------ type mapping

// PG type name (pg_type.typname) to C++ type; unlisted types become std::string.
const char* mapped_type(const std::string& typname) {
    static const std::map<std::string, const char*> m = {
        {"bool", "bool"},
        {"int2", "std::int16_t"},
        {"int4", "std::int32_t"},
        {"int8", "std::int64_t"},
        {"float8", "double"},
        {"text", "std::string"},
        {"varchar", "std::string"},
        {"bpchar", "std::string"},
        {"citext", "std::string"},
        {"numeric", "::pg::numeric"},
        {"json", "::pg::jsonb"},
        {"jsonb", "::pg::jsonb"},
        {"timestamptz", "::pg::timestamptz"},
    };
    auto it = m.find(typname);
    return it == m.end() ? nullptr : it->second;
}

std::string one_line(std::string s) {
    for (char& ch : s)
        if (ch == '\n' || ch == '\r') ch = ' ';
    return s;
}

std::string type_of(const column& c) { return c.nullable ? "std::optional<" + c.cxx + ">" : c.cxx; }

// --------------------------------------------------------------- catalog

// Schemas that belong to the server, not to schema.sql.
const char* const user_schemas_only =
    "n.nspname NOT IN ('pg_catalog', 'information_schema') "
    "AND n.nspname NOT LIKE 'pg\\_toast%' AND n.nspname NOT LIKE 'pg\\_temp\\_%' ";

// Columns of every ordinary or partitioned (parent) table. A domain column is
// reported with its base type and is NOT NULL if the column or any domain on
// the way down says so.
void read_columns(PGconn* c, tables_t& out) {
    auto r = exec(c,
        std::string(
        "WITH RECURSIVE dom(root, cur, nn) AS ("
        "  SELECT oid, typbasetype, typnotnull FROM pg_catalog.pg_type WHERE typtype = 'd' "
        "  UNION ALL "
        "  SELECT dom.root, t.typbasetype, dom.nn OR t.typnotnull "
        "  FROM dom JOIN pg_catalog.pg_type t ON t.oid = dom.cur AND t.typtype = 'd') "
        "SELECT cl.relname, a.attname, bt.typname, format_type(bt.oid, NULL), "
        "       NOT (a.attnotnull OR COALESCE(dom.nn, false)), n.nspname "
        "FROM pg_catalog.pg_class cl "
        "JOIN pg_catalog.pg_namespace n ON n.oid = cl.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = cl.oid AND a.attnum > 0 AND NOT a.attisdropped "
        "JOIN pg_catalog.pg_type t ON t.oid = a.atttypid "
        "LEFT JOIN dom ON dom.root = t.oid "
        "     AND NOT EXISTS (SELECT 1 FROM pg_catalog.pg_type b WHERE b.oid = dom.cur AND b.typtype = 'd') "
        "JOIN pg_catalog.pg_type bt ON bt.oid = COALESCE(dom.cur, t.oid) "
        "WHERE cl.relkind IN ('r', 'p') AND NOT cl.relispartition AND ") + user_schemas_only +
        "ORDER BY cl.relname COLLATE \"C\", a.attnum",
        "read columns");
    for (int i = 0; i < PQntuples(r.get()); ++i) {
        auto& t = out[cell(r.get(), i, 0)];
        std::string schema = cell(r.get(), i, 5);
        if (!t.schema.empty() && t.schema != schema)
            throw std::runtime_error("table " + t.name + " is defined in both schema " + t.schema + " and " + schema);
        t.name = cell(r.get(), i, 0);
        t.schema = schema;
        column col;
        col.name = cell(r.get(), i, 1);
        col.ident = column_ident(col.name);
        const char* mapped = mapped_type(cell(r.get(), i, 2));
        col.cxx = mapped ? mapped : "std::string";
        if (!mapped) col.pg_type = one_line(cell(r.get(), i, 3));
        col.nullable = cell(r.get(), i, 4) == "t";
        t.columns.push_back(std::move(col));
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

const column& find_column(const table& t, const std::string& name) {
    for (const auto& c : t.columns)
        if (c.name == name) return c;
    throw std::runtime_error("table " + t.name + ": no column " + name);
}

// ---------------------------------------------------------------- output

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

std::string key_types(const table& t, const std::vector<std::string>& cols) {
    std::vector<std::string> types;
    for (const auto& n : cols) types.push_back(find_column(t, n).cxx);
    return "std::tuple<" + join(types, ", ") + ">";
}

std::string key_names(const std::vector<std::string>& cols) {
    std::vector<std::string> q;
    for (const auto& n : cols) q.push_back(quote_literal(n));
    return "std::array<std::string_view, " + std::to_string(cols.size()) + "> key_columns{" + join(q, ", ") + "}";
}

void emit_table_struct(std::ostream& o, const table& t) {
    o << "struct table_ {\n"
      << "    static constexpr std::string_view name = " << quote_literal(t.name) << ";\n";
    if (!t.key.empty())
        o << "    using key = " << key_types(t, t.key) << ";\n"
          << "    static constexpr " << key_names(t.key) << ";\n";
    o << "};\n";
}

void emit_column(std::ostream& o, const column& c) {
    o << "struct " << c.ident << " {\n"
      << "    using table = table_;\n"
      << "    using type = " << type_of(c) << ";";
    if (!c.pg_type.empty()) o << "  // PG type: " << c.pg_type;
    o << "\n    static constexpr std::string_view name = " << quote_literal(c.name) << ";\n};\n";
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
      << "    using table = table_;\n"
      << "    using key = " << key_types(t, cols) << ";\n"
      << "    static constexpr " << key_names(cols) << ";\n"
      << "};\n"
      << "inline constexpr " << name << "_t " << name << "{};\n";
}

// Every name declared in a table's namespace must be distinct.
void check_names(const table& t) {
    std::set<std::string> seen = {"table_"};
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

std::string render(const tables_t& tables, const std::string& ns, const std::string& schema_name) {
    std::ostringstream o;
    o << "// Generated by pg_tablegen from " << schema_name << " \xE2\x80\x94 do not edit.\n"
      << "#pragma once\n\n"
      << "#include <array>\n#include <cstdint>\n#include <optional>\n#include <string>\n"
      << "#include <string_view>\n#include <tuple>\n\n"
      << "#include \"pg/codec.hpp\"\n";
    check_table_names(tables);
    for (const auto& [name, t] : tables) emit_namespace(o, ns, t);
    return o.str();
}

// ------------------------------------------------------------------ main

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
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
    if (!dbname.empty()) {
        keys.push_back("dbname");
        values.push_back(dbname);
    }
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
          name_("tablegen_" + std::to_string(getpid()) + "_" + std::to_string(std::random_device{}())) {}
    temp_database(const temp_database&) = delete;
    temp_database& operator=(const temp_database&) = delete;
    ~temp_database() {
        if (!created_) return;
        std::string sql = "DROP DATABASE IF EXISTS " + quote_ident(name_) + " WITH (FORCE)";
        result_ptr r(PQexec(admin_, sql.c_str()));
        if (!r || PQresultStatus(r.get()) != PGRES_COMMAND_OK)
            std::cerr << "pg_tablegen: warning: cannot drop database " << name_ << "\n";
    }
    const std::string& name() const { return name_; }
    void create() {
        created_ = true;
        exec(admin_, "CREATE DATABASE " + quote_ident(name_) + " TEMPLATE template0", "create database");
    }

private:
    PGconn* admin_;
    std::string name_;
    bool created_ = false;
};

// Runs schema.sql in a throwaway database and returns what it created.
tables_t load_tables(const std::string& dsn, const std::string& schema_sql) {
    auto admin = connect_to(dsn, "");
    temp_database db(admin.get());
    db.create();
    auto c = connect_to(dsn, db.name());
    exec(c.get(), "SET client_min_messages TO warning", "set client_min_messages");
    exec(c.get(), schema_sql, "schema.sql");
    tables_t tables;
    read_columns(c.get(), tables);
    read_constraints(c.get(), tables);
    return tables;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: pg_tablegen DSN schema.sql out.hpp [namespace]\n";
        return 2;
    }
    try {
        std::string ns = argc == 5 ? argv[4] : "db";
        auto tables = load_tables(argv[1], read_file(argv[2]));
        write_file(argv[3], render(tables, ns, std::filesystem::path(argv[2]).filename().string()));
    } catch (const std::exception& e) {
        std::cerr << "pg_tablegen: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
