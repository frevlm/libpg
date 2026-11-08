// SHA-256 (FIPS 180-4), in-tree so that libpg needs no crypto library.
#pragma once
#include <string>
#include <string_view>

namespace pg::detail {

// The lowercase hex SHA-256 digest of the bytes.
std::string sha256_hex(std::string_view data);

// The SHA-256 of a migration's text with CRLF turned into LF and trailing
// whitespace stripped from every line, so that line-ending conversion and
// editor whitespace changes do not change the checksum.
std::string migration_checksum(std::string_view text);

}  // namespace pg::detail
