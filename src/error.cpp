#include "pg/error.hpp"

namespace pg {

error_kind classify(std::string_view s, std::string_view message) {
    if (s == "40001" || s == "40P01") return error_kind::conflict;
    if (s == "25006") return error_kind::read_only;
    if (s == "26000") return error_kind::stale_statement;
    if (s == "0A000" && message.find("cached plan must not change result type") != std::string_view::npos)
        return error_kind::stale_statement;
    if (s == "55P03" || s == "57014") return error_kind::lock_or_cancel;
    if (s.starts_with("22") || s.starts_with("23")) return error_kind::data;
    if (s.starts_with("08")) return error_kind::broken;
    return error_kind::other;
}

}  // namespace pg
