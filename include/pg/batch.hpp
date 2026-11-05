#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "connection.hpp"

namespace pg {

namespace detail {

// The statements of one run() of a batch, and what the server returned for them.
struct batch_state {
    struct entry {
        std::string sql;
        params p;
    };
    std::vector<entry> stmts;
    std::vector<result> results;  // one per statement once run
    std::size_t failed = 0;       // index of the failed statement, stmts.size() when none failed
    std::optional<error> failure;
    bool ran = false;

    // The result of statement `i`; throws pg::error before run(), for the failed
    // statement (its error) and for the statements after it (not executed).
    const PGresult* get(std::size_t i) const;
};

}  // namespace detail

// The result of one statement of a batch, available once the batch has run.
template <class T>
class pending {
public:
    // Decodes the result on every call. Throws pg::error before run(), when this
    // statement failed, or when an earlier statement of the batch failed.
    T get() const { return decode_(state_->get(index_)); }

private:
    friend class batch;
    using decoder = T (*)(const PGresult*);
    pending(std::shared_ptr<const detail::batch_state> s, std::size_t i, decoder d)
        : state_(std::move(s)), index_(i), decode_(d) {}

    std::shared_ptr<const detail::batch_state> state_;
    std::size_t index_;
    decoder decode_;
};

// Statements sent together: run() sends all of them and one Sync, one round
// trip, and reads every result.
//
// Outside a transaction the statements form one implicit transaction: either
// all of them take effect or none does. Inside a pg::transaction they join it
// (a pending BEGIN goes in front). run() throws the pg::error of the first
// statement that failed; the statements after it were not executed. The
// statement cache of the connection is shared with conn::exec/query.
// After run() the batch is empty and can be filled again.
class batch {
public:
    explicit batch(conn& c);

    // Affected rows, like conn::exec.
    template <class... A>
    pending<std::int64_t> exec(std::string_view sql, const A&... args) {
        return exec_params(sql, detail::make_params(args...));
    }

    // Every row, like conn::query; the column count is checked by get().
    template <class... T, class... A>
    pending<std::vector<row_t<T...>>> query(std::string_view sql, const A&... args) {
        return query_params<T...>(sql, detail::make_params(args...));
    }

    // exec with already encoded parameters.
    pending<std::int64_t> exec_params(std::string_view sql, detail::params p);

    // query with already encoded parameters.
    template <class... T>
    pending<std::vector<row_t<T...>>> query_params(std::string_view sql, detail::params p) {
        const std::size_t i = add(sql, std::move(p));
        return {state_, i, &detail::decode_rows<T...>};
    }

    // Sends every statement added since the last run() in one round trip. A
    // parameter the client cannot send throws before anything is sent.
    void run();

private:
    std::size_t add(std::string_view sql, detail::params p);

    conn& c_;
    std::shared_ptr<detail::batch_state> state_;
};

}  // namespace pg
