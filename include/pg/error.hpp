#pragma once
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace pg {

// Every failure falls in exactly one of these.
enum class error_kind {
    broken,           // the connection was lost (outside COMMIT)
    commit_unknown,   // the connection was lost while COMMIT was in flight
    conflict,         // 40001, 40P01
    stale_statement,  // 26000; 0A000 when a cached plan's result type changed
    read_only,        // 25006
    lock_or_cancel,   // 55P03, 57014
    connect_failed,   // no connection could be opened
    data,             // classes 22 and 23
    other,            // anything else, client-side misuse included
};

// The kind of a server error from its SQLSTATE; `message` (the primary message)
// tells the stale 0A000 apart from other unsupported features.
error_kind classify(std::string_view sqlstate, std::string_view message = {});

// Whether pool::transact runs its closure again after an error of this kind.
constexpr bool retryable(error_kind k) {
    return k == error_kind::broken || k == error_kind::conflict || k == error_kind::stale_statement ||
           k == error_kind::read_only;
}

// Every error raised by libpg. what() is "SQLSTATE: message" (the message alone
// without a SQLSTATE) and never holds SQL text; the statement is in `sql`.
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

// A query that must return a row returned none.
struct not_found : error {
    explicit not_found(const std::string& message) : error(error_kind::other, message) {}
};

// The connection was lost while COMMIT was in flight: it may or may not have committed.
struct commit_unknown : error {
    explicit commit_unknown(const std::string& message) : error(error_kind::commit_unknown, message) {}
};

// The write-back backlog is over its limit; the posting was not recorded.
struct backlog_full : error {
    explicit backlog_full(const std::string& message) : error(error_kind::other, message) {}
};

// The thread already holds a conn of this pool.
struct nested_borrow : error {
    explicit nested_borrow(const std::string& message) : error(error_kind::other, message) {}
};

}  // namespace pg
