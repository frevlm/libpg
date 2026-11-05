#pragma once
#include <libpq-fe.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "codec.hpp"
#include "error.hpp"

namespace pg {

namespace detail {

using params = std::vector<param>;

// A pooled connection together with its prepared statement cache (connection.cpp).
struct session;

struct result_deleter {
    void operator()(PGresult* r) const { PQclear(r); }
};
using result = std::unique_ptr<PGresult, result_deleter>;

template <class... A>
params make_params(const A&... args) {
    params p;
    p.reserve(sizeof...(A));
    (p.push_back(to_param(args)), ...);
    return p;
}

// Why the parameters cannot be sent, if they cannot: more than 65535 of them,
// or a string containing NUL (libpq's text parameters end at NUL).
std::optional<std::string> unsendable(const params& p);

// Throws unless the result has exactly `want` columns.
void check_columns(const PGresult* r, std::size_t want);

// The number of rows a command affected (PQcmdTuples).
std::int64_t affected_rows(const PGresult* r);

template <class T>
T cell(const PGresult* r, int row, int col) {
    const char* v = PQgetisnull(r, row, col) ? nullptr : PQgetvalue(r, row, col);
    return decode_cell<T>(v, static_cast<std::size_t>(PQgetlength(r, row, col)));
}

template <class... T>
struct row_type {
    using type = std::tuple<T...>;
};
template <class T>
struct row_type<T> {
    using type = T;
};

}  // namespace detail

// A row of a query: std::tuple<T...>, or T itself when there is only one.
template <class... T>
using row_t = typename detail::row_type<T...>::type;

namespace detail {

template <class... T>
row_t<T...> decode_row(const PGresult* r, int row) {
    if constexpr (sizeof...(T) == 1) return cell<T...>(r, row, 0);
    else {
        int col = 0;
        return std::tuple<T...>{cell<T>(r, row, col++)...};
    }
}

// Every row of the result; throws unless it has sizeof...(T) columns.
template <class... T>
std::vector<row_t<T...>> decode_rows(const PGresult* r) {
    static_assert(sizeof...(T) > 0, "query needs at least one column type");
    check_columns(r, sizeof...(T));
    std::vector<row_t<T...>> rows;
    rows.reserve(static_cast<std::size_t>(PQntuples(r)));
    for (int i = 0; i < PQntuples(r); ++i) rows.push_back(decode_row<T...>(r, i));
    return rows;
}

}  // namespace detail

// Sets the global connection pool. Connections are opened lazily, at most
// max_conns of them. Connections of an earlier configuration are discarded.
void connect(std::string dsn, int max_conns = 8);

// A connection leased from the pool for the lifetime of the object.
// Holding a conn while the same thread leases another one (directly, or through
// sql/cached/flush) deadlocks when the pool is exhausted; do not nest leases.
// String parameters containing NUL throw pg::error.
class conn {
public:
    conn();
    ~conn();
    conn(const conn&) = delete;
    conn& operator=(const conn&) = delete;

    // Runs a statement; returns the number of affected rows. Parameters are $1, $2, ...
    template <class... A>
    std::int64_t exec(std::string_view sql, const A&... args) {
        return exec_params(sql, detail::make_params(args...));
    }

    // Decodes every row as std::tuple<T...> (T itself for a single column).
    template <class... T, class... A>
    std::vector<row_t<T...>> query(std::string_view sql, const A&... args) {
        return query_params<T...>(sql, detail::make_params(args...));
    }

    // At most one row; more than one throws pg::error.
    template <class... T, class... A>
    std::optional<row_t<T...>> query_one(std::string_view sql, const A&... args) {
        auto rows = query_params<T...>(sql, detail::make_params(args...));
        if (rows.size() > 1) throw error("query_one: " + std::to_string(rows.size()) + " rows");
        if (rows.empty()) return std::nullopt;
        return std::move(rows.front());
    }

    // exec with already encoded parameters.
    std::int64_t exec_params(std::string_view sql, const detail::params& p);

    // query with already encoded parameters.
    template <class... T>
    std::vector<row_t<T...>> query_params(std::string_view sql, const detail::params& p) {
        const detail::result r = run(sql, p);
        return detail::decode_rows<T...>(r.get());
    }

    // The libpq connection. It is in pipeline mode and nonblocking: the blocking
    // calls (PQexec, PQprepare, PQexecPrepared ...) are not allowed on it.
    PGconn* raw() { return c_; }

private:
    friend class transaction;
    friend class batch;

    detail::result run(std::string_view sql, const detail::params& p);

    detail::session* s_;
    PGconn* c_;
    std::uint64_t generation_;
};

// A transaction on the connection. The constructor sends nothing: BEGIN goes in
// front of the next statement, in the same round trip. commit() sends COMMIT,
// the destructor ROLLBACK unless commit() was called; both send nothing when
// no statement ran. Throws pg::error when a transaction is already open or
// pending on the connection (no nesting).
class transaction {
public:
    explicit transaction(conn& c);
    ~transaction();
    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;

    void commit();

private:
    conn& c_;
    bool open_ = true;
};

}  // namespace pg
