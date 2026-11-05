// Raw libpq scenarios: the v1 wire path without the pool (text, unnamed), and
// prototypes of the planned engine (prepared, binary, pipeline, 1-RTT transaction).
#include <functional>

#include "harness.hpp"
#include "raw.hpp"

namespace bench {
namespace {

using namespace raw;
using Step = std::function<std::uint64_t(Ctx&)>;
using Factory = Step (*)(PGconn*);

// One dedicated connection per thread; the factory prepares and returns the step.
void sweep_threads(const Cfg& cfg, const char* name, Factory make) {
    for (int t : cfg.threads) {
        note(std::string(name) + " threads=" + std::to_string(t));
        const Result r = [&] {
            return run_point(t, cfg.duration, [&](Ctx& x) {
                Link link(cfg.dsn);
                Step step = make(link.c);
                x.loop([&] { return step(x); });
            });
        }();
        emit(name, t, 0, r);
    }
}

// What the select returns, touched the way a caller would: copy name, keep balance.
std::uint64_t touch_text(const PGresult* r) {
    if (PQntuples(r) != 1) throw std::runtime_error("select: no row");
    const std::string name(PQgetvalue(r, 0, 0), static_cast<std::size_t>(PQgetlength(r, 0, 0)));
    const std::string balance(PQgetvalue(r, 0, 1), static_cast<std::size_t>(PQgetlength(r, 0, 1)));
    return name.size() + balance.size() + static_cast<unsigned char>(balance[0]);
}

std::uint64_t touch_binary(const PGresult* r) {
    if (PQntuples(r) != 1) throw std::runtime_error("select: no row");
    const std::string name(PQgetvalue(r, 0, 0), static_cast<std::size_t>(PQgetlength(r, 0, 0)));
    return name.size() + static_cast<std::uint64_t>(numeric_micros(PQgetvalue(r, 0, 1)));
}

// --- raw_text_unnamed -------------------------------------------------------------

Step text_unnamed(PGconn* c) {
    return [c](Ctx& x) -> std::uint64_t {
        const TextInt id(x.rng.key());
        const char* v[1] = {id.buf};
        const PgResult r{PQexecParams(c, kSelect, 1, nullptr, v, nullptr, nullptr, 0)};
        expect(r, PGRES_TUPLES_OK, c);
        x.sink += touch_text(r.get());
        return 1;
    };
}

// --- raw_prepared_text ------------------------------------------------------------

Step prepared_text(PGconn* c) {
    prepare(c, "sel", kSelect, 1, nullptr);
    return [c](Ctx& x) -> std::uint64_t {
        const TextInt id(x.rng.key());
        const char* v[1] = {id.buf};
        const PgResult r{PQexecPrepared(c, "sel", 1, v, nullptr, nullptr, 0)};
        expect(r, PGRES_TUPLES_OK, c);
        x.sink += touch_text(r.get());
        return 1;
    };
}

// --- raw_prepared_binary ----------------------------------------------------------

constexpr int kBinary = 1;

Step prepared_binary(PGconn* c) {
    const Oid types[1] = {kInt8Oid};
    prepare(c, "sel", kSelect, 1, types);
    return [c](Ctx& x) -> std::uint64_t {
        const BinInt8 id(x.rng.key());
        const char* v[1] = {id.buf};
        const int len[1] = {8}, fmt[1] = {kBinary};
        const PgResult r{PQexecPrepared(c, "sel", 1, v, len, fmt, kBinary)};
        expect(r, PGRES_TUPLES_OK, c);
        x.sink += touch_binary(r.get());
        return 1;
    };
}

// --- raw_pipeline_k ---------------------------------------------------------------

void send_select(PGconn* c, std::int64_t key) {
    const BinInt8 id(key);
    const char* v[1] = {id.buf};
    const int len[1] = {8}, fmt[1] = {kBinary};
    if (!PQsendQueryPrepared(c, "sel", 1, v, len, fmt, kBinary)) fail(c, "send select");
}

// Reads one result of a pipeline and the NULL that ends it.
PgResult next_result(PGconn* c, ExecStatusType want) {
    PgResult r{PQgetResult(c)};
    expect(r, want, c);
    if (PQgetResult(c) != nullptr) fail(c, "result not terminated");
    return r;
}

void read_sync(PGconn* c) {
    const PgResult s{PQgetResult(c)};
    expect(s, PGRES_PIPELINE_SYNC, c);
}

template <int K>
Step pipeline(PGconn* c) {
    const Oid types[1] = {kInt8Oid};
    prepare(c, "sel", kSelect, 1, types);
    if (!PQenterPipelineMode(c)) fail(c, "enter pipeline");
    return [c](Ctx& x) -> std::uint64_t {
        for (int i = 0; i < K; ++i) send_select(c, x.rng.key());
        if (PQpipelineSync(c) != 1) fail(c, "pipeline sync");
        for (int i = 0; i < K; ++i) x.sink += touch_binary(next_result(c, PGRES_TUPLES_OK).get());
        read_sync(c);
        return K;
    };
}

// --- transactions of three statements ---------------------------------------------

enum Kind { kSel, kUpd, kIns };

// One execution of a prepared statement of the transaction. Keys go binary, numerics text.
struct Call {
    BinInt8 id;
    const char* v[3];
    int len[3], fmt[3];
    int n;
    const char* name;

    Call(Kind kind, std::int64_t key) : id(key) {
        static const char* const delta = "1.000000";
        switch (kind) {
            case kSel:
                name = "sel", n = 1;
                v[0] = id.buf, len[0] = 8, fmt[0] = 1;
                break;
            case kUpd:
                name = "upd", n = 2;
                v[0] = delta, len[0] = 0, fmt[0] = 0;
                v[1] = id.buf, len[1] = 8, fmt[1] = 1;
                break;
            case kIns:
                name = "ins", n = 3;
                v[0] = id.buf, len[0] = 8, fmt[0] = 1;
                v[1] = delta, len[1] = 0, fmt[1] = 0;
                v[2] = "bench", len[2] = 0, fmt[2] = 0;
                break;
        }
    }
    Call(const Call&) = delete;

    bool send(PGconn* c) const { return PQsendQueryPrepared(c, name, n, v, len, fmt, kBinary) == 1; }
    PgResult exec(PGconn* c) const { return PgResult{PQexecPrepared(c, name, n, v, len, fmt, kBinary)}; }
};

void prepare_tx3(PGconn* c) {
    const Oid sel[1] = {kInt8Oid}, upd[2] = {0, kInt8Oid}, ins[3] = {kInt8Oid, 0, 0};
    prepare(c, "sel", kSelect, 1, sel);
    prepare(c, "upd", kUpdate, 2, upd);
    prepare(c, "ins", kInsert, 3, ins);
}

// BEGIN, three prepared statements, COMMIT: five round trips.
Step tx3_sequential(PGconn* c) {
    prepare_tx3(c);
    return [c](Ctx& x) -> std::uint64_t {
        const std::int64_t id = x.rng.key();
        exec_simple(c, "BEGIN");
        const Call sel(kSel, id), upd(kUpd, id), ins(kIns, id);
        const PgResult r = sel.exec(c);
        expect(r, PGRES_TUPLES_OK, c);
        x.sink += touch_binary(r.get());
        expect(upd.exec(c), PGRES_COMMAND_OK, c);
        expect(ins.exec(c), PGRES_COMMAND_OK, c);
        exec_simple(c, "COMMIT");
        return 1;
    };
}

// The same three statements as one implicit transaction in one pipeline: one round trip.
Step tx3_pipelined(PGconn* c) {
    prepare_tx3(c);
    if (!PQenterPipelineMode(c)) fail(c, "enter pipeline");
    return [c](Ctx& x) -> std::uint64_t {
        const std::int64_t id = x.rng.key();
        const Call sel(kSel, id), upd(kUpd, id), ins(kIns, id);
        if (!sel.send(c) || !upd.send(c) || !ins.send(c)) fail(c, "send");
        if (PQpipelineSync(c) != 1) fail(c, "pipeline sync");
        x.sink += touch_binary(next_result(c, PGRES_TUPLES_OK).get());
        next_result(c, PGRES_COMMAND_OK);
        next_result(c, PGRES_COMMAND_OK);
        read_sync(c);
        return 1;
    };
}

// events grows with every transaction; start each point from an empty table.
void reset_events(const Cfg& cfg) { run_once(cfg.dsn, "TRUNCATE events"); }

void run_text_unnamed(const Cfg& c) { sweep_threads(c, "raw_text_unnamed", text_unnamed); }
void run_prepared_text(const Cfg& c) { sweep_threads(c, "raw_prepared_text", prepared_text); }
void run_prepared_binary(const Cfg& c) { sweep_threads(c, "raw_prepared_binary", prepared_binary); }
void run_pipeline_1(const Cfg& c) { sweep_threads(c, "raw_pipeline_1", pipeline<1>); }
void run_pipeline_16(const Cfg& c) { sweep_threads(c, "raw_pipeline_16", pipeline<16>); }
void run_tx3_sequential(const Cfg& c) {
    reset_events(c);
    sweep_threads(c, "raw_tx3_sequential", tx3_sequential);
}
void run_tx3_pipelined(const Cfg& c) {
    reset_events(c);
    sweep_threads(c, "raw_tx3_pipelined", tx3_pipelined);
}

}  // namespace

void register_raw(std::vector<Scenario>& v) {
    v.push_back({"raw_text_unnamed", run_text_unnamed});
    v.push_back({"raw_prepared_text", run_prepared_text});
    v.push_back({"raw_prepared_binary", run_prepared_binary});
    v.push_back({"raw_pipeline_1", run_pipeline_1});
    v.push_back({"raw_pipeline_16", run_pipeline_16});
    v.push_back({"raw_tx3_sequential", run_tx3_sequential});
    v.push_back({"raw_tx3_pipelined", run_tx3_pipelined});
}

}  // namespace bench
