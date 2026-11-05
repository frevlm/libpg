// The engine: every statement of libpg runs through execute(), on a connection
// in libpq pipeline mode. Internal to the library.
#pragma once
#include <libpq-fe.h>

#include <cstdint>
#include <list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pg/connection.hpp"

#ifndef LIBPQ_HAS_PIPELINING
#error "libpg needs libpq 14 or newer (pipeline mode)"
#endif

namespace pg::detail {

// Statement text -> server side statement name, least recently used first out.
class stmt_cache {
public:
    static constexpr std::size_t capacity = 256;

    // The name of a cached statement, which becomes the most recently used.
    const std::string* find(std::string_view sql);

    // Adds a statement; returns the name of the one it pushed out, if any.
    std::optional<std::string> add(std::string sql, std::string name);

    std::optional<std::string> erase(std::string_view sql);

private:
    struct entry {
        std::string sql;
        std::string name;
    };
    std::list<entry> lru_;
    // The keys view the strings in lru_ nodes, which never move.
    std::unordered_map<std::string_view, std::list<entry>::iterator> index_;
};

// A pooled connection: in pipeline and nonblocking mode, with its statement cache.
struct session {
    explicit session(PGconn* conn) : c(conn) {}
    ~session() { PQfinish(c); }
    session(const session&) = delete;
    session& operator=(const session&) = delete;

    PGconn* c;
    stmt_cache cache;
    std::uint64_t next_name = 0;
    std::vector<std::string> closes;  // statements to deallocate in front of the next operation
    bool begin_pending = false;       // a transaction started; BEGIN goes in front of the next statement
    bool broken = false;              // an operation stopped mid-pipeline: the connection is never reused
};

// One statement of an operation. Both views must outlive execute().
struct statement {
    std::string_view sql;
    const params* p;
};

// What an operation produced: one result per statement, in order. When a
// statement failed, `failure` is its error, `failed` its index, and the
// statements after it were not executed (their results are null).
struct outcome {
    std::vector<result> results;
    std::size_t failed = 0;
    std::optional<error> failure;
    bool stale = false;  // the failed statement was stale and is no longer cached
};

// Runs the statements as one pipeline ending in one Sync: one round trip. A
// pending BEGIN goes in front; closes of evicted statements go before that, in
// a Sync group of their own. A statement the server
// no longer has, or whose result shape changed, is dropped from the cache; when
// the connection was idle the operation is retried once. Statement errors are
// returned; a client error throws before anything is sent; a broken connection
// throws pg::error without SQLSTATE and marks the session broken. On return,
// and on every throw that leaves the session unbroken, the connection has read
// everything up to the Sync.
outcome execute(session& s, std::span<const statement> stmts);

// No transaction is open or pending on the session.
bool idle(const session& s);

// The connection's last error message, without the trailing newline.
std::string conn_message(PGconn* c);

}  // namespace pg::detail
