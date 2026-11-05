#include "pg/batch.hpp"

#include <utility>

#include "engine.hpp"

namespace pg {

namespace detail {

const PGresult* batch_state::get(std::size_t i) const {
    if (!ran) throw error("batch: get() before run()");
    if (i < failed) return results[i].get();
    if (i == failed) throw *failure;
    throw error("batch: statement not executed, statement " + std::to_string(failed) + " failed");
}

}  // namespace detail

batch::batch(conn& c) : c_(c), state_(std::make_shared<detail::batch_state>()) {}

std::size_t batch::add(std::string_view sql, detail::params p) {
    state_->stmts.push_back({std::string(sql), std::move(p)});
    return state_->stmts.size() - 1;
}

pending<std::int64_t> batch::exec_params(std::string_view sql, detail::params p) {
    const std::size_t i = add(sql, std::move(p));
    return {state_, i, &detail::affected_rows};
}

// A connection or client error counts as the failure of the first statement.
void batch::run() {
    const std::shared_ptr<detail::batch_state> st = std::exchange(state_, std::make_shared<detail::batch_state>());
    std::vector<detail::statement> views;
    views.reserve(st->stmts.size());
    for (const auto& e : st->stmts) views.push_back({e.sql, &e.p});
    detail::outcome o;
    try {
        o = detail::execute(*c_.s_, views);
    } catch (const error& e) {
        o.failure = e;
        o.failed = 0;
    }
    st->results = std::move(o.results);
    st->failed = o.failure ? o.failed : st->stmts.size();
    st->failure = std::move(o.failure);
    st->ran = true;
    if (st->failure) throw *st->failure;
}

}  // namespace pg
