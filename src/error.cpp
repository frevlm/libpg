#include "pg/error.hpp"

namespace pg {

error_kind classify(std::string_view s) {
    if (s == "25006") return error_kind::read_only;
    if (s == "40001" || s == "40P01") return error_kind::conflict;
    if (s == "55P03" || s == "57014") return error_kind::lock_or_cancel;
    if (s == "26000") return error_kind::stale_statement;
    // Connection exceptions and server shutdown or termination end the session.
    if (s.starts_with("08") || s == "57P01" || s == "57P02" || s == "57P03") return error_kind::broken;
    if (s.starts_with("22") || s.starts_with("23")) return error_kind::data;
    return error_kind::other;
}

}  // namespace pg
