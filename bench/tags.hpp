// Column tags and write shapes of the bench schema, in the form pg_tablegen writes them.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "pg/pg.hpp"

namespace bench {

using money = pg::decimal<6>;

namespace tag {
struct accounts_ {
    static constexpr std::string_view name = "accounts";
    using key = std::tuple<std::int64_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct balance {
    using pg_table = accounts_;
    using pg_type = money;
    static constexpr std::string_view pg_name = "balance";
};
struct events_ {
    static constexpr std::string_view name = "events";
};
struct event_write {
    using pg_table = events_;
    std::int64_t account_id{};
    money amount{};
    std::string note{};
    static constexpr std::array<std::string_view, 3> pg_names{"account_id", "amount", "note"};
    template <class F>
    void pg_each(F&& f) const {
        f(account_id);
        f(amount);
        f(note);
    }
};
static_assert(pg::write_shape<event_write>);

struct bench_requests_ {
    static constexpr std::string_view name = "bench_requests";
};

// revlm's requests row without its identity column.
struct request_row {
    using pg_table = bench_requests_;
    static constexpr std::array<std::string_view, 34> pg_names{
        "time",          "request_id",      "response_id",       "endpoint",      "method",
        "user_id",       "token_id",        "input_tokens",      "output_tokens", "cache_read_tokens",
        "cache_creation_1h_tokens", "cache_creation_5m_tokens", "tier_multiplier", "service_tier",
        "channel_multiplier", "channel_id", "status_code",       "latency_ms",    "first_token_latency_ms",
        "error_class",   "error_message",   "is_stream",         "model",         "usd",
        "outcome",       "attempts",        "body_read_us",      "prep_us",       "upstream_head_us",
        "upstream_wait_us", "relay_us",     "client_write_us",   "request_bytes", "response_bytes"};
    pg::timestamptz time{};
    std::optional<std::string> request_id, response_id, endpoint, method;
    std::int64_t user_id = 0;
    std::int64_t token_id = 0;
    std::int32_t input_tokens = 0;
    std::int32_t output_tokens = 0;
    std::int32_t cache_read_tokens = 0;
    std::int32_t cache_creation_1h_tokens = 0;
    std::int32_t cache_creation_5m_tokens = 0;
    double tier_multiplier = 1.0;
    std::optional<std::string> service_tier;
    double channel_multiplier = 1.0;
    std::int64_t channel_id = 0;
    std::int32_t status_code = 0;
    std::int32_t latency_ms = 0;
    std::int32_t first_token_latency_ms = 0;
    std::optional<std::string> error_class, error_message;
    bool is_stream = false;
    std::optional<std::string> model;
    pg::decimal<6> usd{};
    std::optional<std::string> outcome;
    std::int32_t attempts = 0;
    std::int64_t body_read_us = 0;
    std::int64_t prep_us = 0;
    std::int64_t upstream_head_us = 0;
    std::int64_t upstream_wait_us = 0;
    std::int64_t relay_us = 0;
    std::int64_t client_write_us = 0;
    std::int64_t request_bytes = 0;
    std::int64_t response_bytes = 0;

    template <class F>
    void pg_each(F&& f) const {
        f(time);
        f(request_id);
        f(response_id);
        f(endpoint);
        f(method);
        f(user_id);
        f(token_id);
        f(input_tokens);
        f(output_tokens);
        f(cache_read_tokens);
        f(cache_creation_1h_tokens);
        f(cache_creation_5m_tokens);
        f(tier_multiplier);
        f(service_tier);
        f(channel_multiplier);
        f(channel_id);
        f(status_code);
        f(latency_ms);
        f(first_token_latency_ms);
        f(error_class);
        f(error_message);
        f(is_stream);
        f(model);
        f(usd);
        f(outcome);
        f(attempts);
        f(body_read_us);
        f(prep_us);
        f(upstream_head_us);
        f(upstream_wait_us);
        f(relay_us);
        f(client_write_us);
        f(request_bytes);
        f(response_bytes);
    }
};
static_assert(pg::write_shape<request_row>);
}  // namespace tag

constexpr std::size_t kBatchRows = 200;

// A batch of requests rows; error fields stay NULL.
inline std::vector<tag::request_row> batch_rows() {
    std::vector<tag::request_row> rows(kBatchRows);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        tag::request_row& r = rows[i];
        r.time = pg::timestamptz(std::chrono::microseconds{1'700'000'000'000'000});
        r.request_id = "req_" + std::to_string(i);
        r.response_id = "resp_" + std::to_string(i);
        r.endpoint = "/v1/messages";
        r.method = "POST";
        r.user_id = static_cast<std::int64_t>(i) + 1;
        r.token_id = static_cast<std::int64_t>(i) + 1;
        r.input_tokens = 1024;
        r.output_tokens = 256;
        r.cache_read_tokens = 128;
        r.cache_creation_1h_tokens = 0;
        r.cache_creation_5m_tokens = 64;
        r.tier_multiplier = 1.0;
        r.service_tier = "standard";
        r.channel_multiplier = 1.0;
        r.channel_id = static_cast<std::int64_t>(i % 7);
        r.status_code = 200;
        r.latency_ms = 850;
        r.first_token_latency_ms = 300;
        r.is_stream = true;
        r.model = "claude-sonnet-4";
        r.usd = pg::decimal<6>::from_units(1234);
        r.outcome = "ok";
        r.attempts = 1;
        r.body_read_us = 1200;
        r.prep_us = 30;
        r.upstream_head_us = 410'000;
        r.upstream_wait_us = 390'000;
        r.relay_us = 20'000;
        r.client_write_us = 900;
        r.request_bytes = 48'000;
        r.response_bytes = 12'000;
    }
    return rows;
}

}  // namespace bench
