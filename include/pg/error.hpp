#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace pg {

// Every failure falls in exactly one of these (ADR 0015).
enum class error_kind {
    broken,           // connection lost before COMMIT was sent, or outside a transaction
    commit_unknown,   // connection lost while COMMIT was in flight
    read_only,        // 25006
    conflict,         // 40001, 40P01
    lock_or_cancel,   // 55P03, 57014
    stale_statement,  // 26000, and 0A000 when a cached statement's result type changed
    deadline,         // the client deadline passed; the query was cancelled
    connect_failed,   // no connection could be opened, or the pool breaker is open
    data,             // 22xxx, 23xxx
    other,            // anything else, including client-side misuse
};

// What each part of libpg does about an error of one kind.
struct disposition {
    enum class writeback_action { replay, isolate };
    bool discard_conn;        // the pool closes the connection instead of reusing it
    bool retry_closure;       // a closure transaction runs again
    writeback_action writeback;  // replay the batch, or redo it posting by posting
};

// The one table that decides it.
constexpr disposition dispose(error_kind k) {
    using wb = disposition::writeback_action;
    switch (k) {
        case error_kind::broken: return {true, true, wb::replay};
        case error_kind::commit_unknown: return {true, false, wb::replay};
        case error_kind::read_only: return {true, true, wb::replay};
        case error_kind::conflict: return {false, true, wb::replay};
        case error_kind::lock_or_cancel: return {false, false, wb::replay};
        case error_kind::stale_statement: return {false, true, wb::replay};
        case error_kind::deadline: return {true, false, wb::replay};
        case error_kind::connect_failed: return {true, false, wb::replay};
        case error_kind::data: return {false, false, wb::isolate};
        case error_kind::other: return {false, false, wb::isolate};
    }
    return {true, false, wb::isolate};
}

// The kind of a server error from its SQLSTATE.
error_kind classify(std::string_view sqlstate);

// Every error raised by libpg. what() is "SQLSTATE: message" (just the message
// when there is no SQLSTATE) and never contains SQL text; the statement is in `sql`.
struct error : std::runtime_error {
    error_kind kind;
    std::string sqlstate;
    std::string sql;

    error(error_kind k, const std::string& message, std::string state = {}, std::string statement = {})
        : std::runtime_error(state.empty() ? message : state + ": " + message),
          kind(k),
          sqlstate(std::move(state)),
          sql(std::move(statement)) {}
};

// A cell's row does not exist, or a query that must return one row returned none.
struct not_found : error {
    explicit not_found(const std::string& message) : error(error_kind::other, message) {}
};

// The connection was lost while COMMIT was in flight: the transaction may or
// may not have taken effect. Never retried.
struct commit_unknown : error {
    explicit commit_unknown(const std::string& message) : error(error_kind::commit_unknown, message) {}
};

// The write-back backlog is over its limit; the posting was not recorded.
struct backlog_full : error {
    explicit backlog_full(const std::string& message) : error(error_kind::other, message) {}
};

// The thread already holds a connection of this pool (ADR 0003).
struct nested_borrow : error {
    explicit nested_borrow(const std::string& message) : error(error_kind::other, message) {}
};

}  // namespace pg
