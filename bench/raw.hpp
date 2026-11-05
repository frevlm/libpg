// Thin raw-libpq helpers shared by the raw_* scenarios: a connection, checked results,
// and the minimal binary decoders (int8, text, numeric).
#pragma once
#include <libpq-fe.h>

#include <charconv>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace bench::raw {

inline constexpr Oid kInt8Oid = 20;

inline constexpr const char* kSelect = "SELECT name, balance FROM accounts WHERE id = $1";
inline constexpr const char* kUpdate = "UPDATE accounts SET balance = balance + $1 WHERE id = $2";
inline constexpr const char* kInsert = "INSERT INTO events(account_id, amount, note) VALUES ($1, $2, $3)";

struct PgResultDeleter {
    void operator()(PGresult* r) const { PQclear(r); }
};
using PgResult = std::unique_ptr<PGresult, PgResultDeleter>;

[[noreturn]] inline void fail(PGconn* c, const char* what) {
    throw std::runtime_error(std::string(what) + ": " + PQerrorMessage(c));
}

// Throws unless `r` has status `want`.
inline void expect(const PgResult& r, ExecStatusType want, PGconn* c) {
    if (!r) fail(c, "no result");
    if (PQresultStatus(r.get()) != want) fail(c, PQresStatus(PQresultStatus(r.get())));
}

// A connection with the same session settings pg::connect applies.
class Link {
public:
    explicit Link(const std::string& dsn) : c(PQconnectdb(dsn.c_str())) {
        if (PQstatus(c) != CONNECTION_OK) {
            std::string why = PQerrorMessage(c);
            PQfinish(c);
            throw std::runtime_error("connect: " + why);
        }
        const PgResult r{PQexec(c, "SET TimeZone = 'UTC'; SET DateStyle = 'ISO, MDY'; "
                                 "SET IntervalStyle = 'postgres'; SET extra_float_digits = 3")};
        expect(r, PGRES_COMMAND_OK, c);
    }
    ~Link() { PQfinish(c); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    PGconn* c;
};

inline void prepare(PGconn* c, const char* name, const char* sql, int n, const Oid* types) {
    const PgResult r{PQprepare(c, name, sql, n, types)};
    expect(r, PGRES_COMMAND_OK, c);
}

inline void exec_simple(PGconn* c, const char* sql) {
    const PgResult r{PQexec(c, sql)};
    expect(r, PGRES_COMMAND_OK, c);
}

// Runs one statement on a fresh connection (setup helpers).
inline void run_once(const std::string& dsn, const char* sql) {
    Link l(dsn);
    const PgResult r{PQexec(l.c, sql)};
    expect(r, PGRES_COMMAND_OK, l.c);
}

// Decimal text of a key, NUL terminated, for text-format parameters.
struct TextInt {
    char buf[24];
    explicit TextInt(std::int64_t v) { *std::to_chars(buf, buf + sizeof buf - 1, v).ptr = '\0'; }
};

// Big-endian int8 parameter.
struct BinInt8 {
    char buf[8];
    explicit BinInt8(std::int64_t v) {
        for (int i = 7; i >= 0; --i, v >>= 8) buf[i] = static_cast<char>(v & 0xff);
    }
};

inline std::int64_t be16(const char* p) {
    return static_cast<std::int16_t>((static_cast<std::uint8_t>(p[0]) << 8) | static_cast<std::uint8_t>(p[1]));
}
inline std::int64_t be64(const char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
    return static_cast<std::int64_t>(v);
}

// Binary numeric (ndigits, weight, sign, dscale, base-10000 digits) to an integer
// count of 1e-6 units, which is what a decimal<6> would hold.
inline std::int64_t numeric_micros(const char* p) {
    const int nd = static_cast<int>(be16(p)), weight = static_cast<int>(be16(p + 2));
    const int sign = static_cast<int>(be16(p + 4));
    if (sign == 0xC000) return 0;  // NaN
    std::int64_t v = 0;
    for (int i = 0; i < nd; ++i) v = v * 10000 + be16(p + 8 + 2 * i);
    int exp10 = 4 * (weight - (nd - 1)) + 6;  // v * 10^exp10 in 1e-6 units
    for (; exp10 > 0; --exp10) v *= 10;
    for (; exp10 < 0; ++exp10) v /= 10;
    return sign == 0x4000 ? -v : v;
}

}  // namespace bench::raw
