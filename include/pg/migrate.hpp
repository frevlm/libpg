#pragma once
#include <string_view>
#include <vector>

#include "pool.hpp"

namespace pg {

// A migration script: multi-statement SQL and the name it is recorded under.
struct migration {
    std::string_view name;
    std::string_view sql;
};

struct migrate_options {
    std::vector<migration> migrations;  // applied in this order
};

// Brings the database up to date in one transaction under advisory lock
// 0x6c69627067 ("libpg"): creates libpg.migrations, rejects an applied migration
// whose checksum changed, runs the pending ones in order and records them.
// The checksum is the hex SHA-256 of the text with CRLF turned into LF and
// trailing whitespace stripped from every line. Any failure rolls back and throws.
void migrate(pool& p, const migrate_options& o);

}  // namespace pg
