#pragma once
#include <stdexcept>
#include <string>
#include <utility>

namespace pg {

// Every error raised by libpg.
struct error : std::runtime_error {
    // SQLSTATE reported by the server; empty for client and connection errors.
    std::string sqlstate;

    explicit error(const std::string& what, std::string state = {})
        : std::runtime_error(what), sqlstate(std::move(state)) {}
};

// The row addressed by a cell does not exist.
struct not_found : error {
    using error::error;
};

}  // namespace pg
