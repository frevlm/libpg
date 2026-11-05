# libpg 设计

libpg 是 PostgreSQL 的 C++20 客户端库，直接用 libpq，自己管理连接。它要完整取代
sqlpp11（revlm、HHHOJ 现在用的就是 sqlpp11），由两层组成：

1. **查询层**：连接池、参数化 SQL、带类型的结果、事务。集合查询（列表、排行、JOIN）
   就写 SQL 文本，参数和结果的类型由 C++ 检查。
2. **格子层**：数据库里的一个格子 `(表, 行, 列)` 当成变量用。
   - `pg::sql<Col>`：每次读写都访问数据库。
   - `pg::cached<Col>`：读内存；写先改内存，再由后台线程异步写库。

列的类型、主键的类型和个数，都由 `pg_tablegen` 从项目的 schema.sql 生成，写错了编译不过。

## 目录

```
include/pg/pg.hpp          总头文件，包含下面全部
include/pg/error.hpp       pg::error, pg::not_found
include/pg/codec.hpp       pg::codec<T>、内置类型
include/pg/connection.hpp  pg::connect, pg::conn, pg::transaction
include/pg/batch.hpp       pg::batch, pg::pending
include/pg/sql.hpp         列标签约定, pg::sql<Col>
include/pg/cached.hpp      pg::cached<Col>, pg::flush, pg::shutdown, pg::cache_options
src/codec.cpp src/connection.cpp src/cached.cpp src/batch.cpp
src/engine.hpp src/engine.cpp   引擎（库内部）：pipeline 模式下执行所有语句，语句缓存
tools/pg_tablegen.cpp
tests/                     每个 *_test.cpp 一个可执行文件，在 tests/CMakeLists.txt 里用 libpg_test(name) 登记
scripts/test-pg.sh         ctest 用的临时集群
```

构建与测试：

```
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

ctest 会先启动一个临时集群（`pg_start` fixture），测试从环境变量 `LIBPG_TEST_DSN` 读连接串。

## 错误

```cpp
namespace pg {
struct error : std::runtime_error {      // 所有 libpg 错误
    std::string sqlstate;                // 服务器返回的 SQLSTATE，连接错误时为空
};
struct not_found : error {};             // 格子定位的行不存在
}
```

## 编解码 `pg::codec<T>`

全部使用文本格式（paramFormats 和 resultFormat 都是 0）。

```cpp
template <class T> struct codec;   // static std::string encode(const T&);
                                   // static T decode(std::string_view);
```

内置：

| C++ | PG |
|---|---|
| `bool` | boolean（读 `t`/`f`） |
| `std::int16_t` `std::int32_t` `std::int64_t` | smallint integer bigint |
| `double` | float8（`encode` 要保证往返精确，用 `std::to_chars`） |
| `std::string` | text 以及任何读成文本的类型 |
| `pg::numeric` | numeric，`struct numeric { std::string text; }`，原样保留十进制文本 |
| `pg::jsonb` | json/jsonb，`struct jsonb { std::string text; }` |
| `pg::timestamptz` | timestamptz，`using timestamptz = std::chrono::sys_time<std::chrono::microseconds>`。会话固定 `TimeZone = 'UTC'`、`DateStyle = 'ISO, MDY'`，解析 `YYYY-MM-DD HH:MM:SS[.ffffff]+00[ BC]`，年份可以超过 4 位。PG 的 `infinity` / `-infinity` 对应 `timestamptz::max()` / `timestamptz::min()`，两个方向都转换 |
| `std::optional<T>` | 可空列，SQL NULL 对应 `nullopt` |

只用作参数（只需要 `encode`）：`const char*`、`std::string_view`。

用户可以为自己的类型特化 `pg::codec`。

## 查询层

```cpp
namespace pg {
// 设置全局连接池。连接在第一次 conn 构造时才建立（懒连接），最多 max_conns 个。
// 每个新连接都会设置 TimeZone = 'UTC'、DateStyle = 'ISO, MDY'、IntervalStyle = 'postgres'、
// extra_float_digits = 3，不受环境变量、DSN 和数据库默认值影响。
void connect(std::string dsn, int max_conns = 8);

// 从池里借一个连接，析构时归还；池用尽时阻塞等待。
// 同一个线程在持有 conn 的同时再借连接（包括通过 sql/cached/flush 间接借），
// 池满时会死锁；不要嵌套借用。
// 含有 NUL 字符的字符串参数抛 pg::error（libpq 的文本参数遇到 NUL 就截断）。
// 归还时如果连接已坏（PQstatus != CONNECTION_OK），就关掉它，下次借用时重新建立。
class conn {
public:
    conn();
    // 执行语句，返回受影响的行数（PQcmdTuples）。参数按 $1 $2 ... 顺序绑定。
    template <class... A> std::int64_t exec(std::string_view sql, const A&... args);
    // 每一行解码成 std::tuple<T...>；只有一个 T 时直接是 T。
    template <class... T, class... A> std::vector<row_t<T...>> query(std::string_view sql, const A&... args);
    // 至多一行；超过一行抛 pg::error。
    template <class... T, class... A> std::optional<row_t<T...>> query_one(std::string_view sql, const A&... args);
    // libpq 连接。它处于 pipeline 模式和非阻塞模式：PQexec、PQprepare、PQexecPrepared
    // 这类同步函数不能在它上面调用。
    PGconn* raw();
};

// 构造时不发任何东西，只在连接上记下“BEGIN 待发”（连接在事务里或已有待发的 BEGIN 时
// 抛 pg::error，不支持嵌套事务）。BEGIN 和之后的第一条语句在同一次往返里发出。
// commit() 时 COMMIT；析构时如果还没提交就 ROLLBACK。一条语句都没执行过时，
// commit() 和析构都不发任何东西。事务里已经有语句失败时，commit() 抛 pg::error，
// 不会把服务器实际执行的 ROLLBACK 当成提交成功。
class transaction {
public:
    explicit transaction(conn&);
    void commit();
};

// 一次往返发出多条语句（pg/batch.hpp）。
template <class T> class pending {
public:
    T get() const;   // run() 之前、本条失败、或前面有语句失败时抛 pg::error；每次调用都重新解码
};
class batch {
public:
    explicit batch(conn&);
    template <class... A> pending<std::int64_t> exec(std::string_view sql, const A&... args);
    template <class... T, class... A> pending<std::vector<row_t<T...>>> query(std::string_view sql, const A&... args);
    void run();   // 全部语句加一个 Sync，一次往返
};
}
```

batch 的语义：

- 不在事务里时，Sync 之前的所有语句构成一个隐式事务：要么全部生效，要么全部不生效。
- 在 `pg::transaction` 里时，语句加入这个事务；待发的 BEGIN 排在最前面。
- `run()` 抛出第一条失败语句的 `pg::error`；它后面的语句没有执行，`get()` 抛 `pg::error`。
  失败语句之前的语句 `get()` 返回它们执行时的结果（不在事务里时它们已经随隐式事务回滚）。
- 参数含 NUL 或超过 65535 个时，`run()` 在发送任何东西之前抛出。
- 列数检查和 `query` 一样，在 `get()` 里做。
- 和 `conn::exec/query` 共用连接的语句缓存。`run()` 之后 batch 变空，可以继续添加语句再 `run()`。

结果列数和 `T...` 的个数不一致，或者非 optional 的列读到 NULL，都抛 `pg::error`。

连接池：等待的线程按到达顺序排队，`conn` 析构时把连接（连接已坏被丢弃时是新建一个连接的名额）
直接交给最老的等待者，没人等才放回空闲列表（后进先出，热连接优先）。因此不会有线程在
归还后立刻抢回自己的连接，饿死别的等待者。新建连接失败时，名额同样传给下一个等待者。

### 引擎

所有语句（`conn::exec/query`、`batch`、`transaction`、`sql`、`cached`）都走同一条路径
（`src/engine.cpp` 的 `execute`）。连接建立并执行完设置会话参数的 `PQexec` 之后，进入
pipeline 模式和非阻塞模式，此后不再调用任何同步的 libpq 函数。

一次操作（一条语句，或一个 batch）：

1. 先检查参数（NUL、个数），不合格就抛出，什么都不发。
2. 依次排队：上次挤出缓存的语句的 Close 和一个 Sync（单独一组，有待关闭的语句时才有）；
   然后是待发的 BEGIN、每条语句（缓存未命中时先 `PQsendPrepare`，再 `PQsendQueryPrepared`；
   同一个 batch 里相同的 SQL 文本只 Prepare 一次），最后一个 Sync。两组在同一次往返里发出。
3. 一个循环读结果直到最后一个 `PGRES_PIPELINE_SYNC`：`PQflush` 发送剩余输出，读出所有已就绪的结果，
   然后 `poll()` 等 socket 可读（还有没发完的输出时也等可写），再 `PQconsumeInput`。
   边发边收，所以无论 batch 多大，都不会出现双方都在等对方读的死锁。

因此缓存未命中只要一次往返（Prepare + Execute + Sync 一起发）；只有 Prepare 成功的语句才加入
LRU。挤出的语句的 Close 跟着下一次操作发出，不额外往返。Close 自成一个 Sync 组：
libpq 17 及以上用 `PQsendClosePrepared`（关闭不存在的语句不算错误），14–16 用
`DEALLOCATE sN`（语句不存在时报 26000）。一个 Close 失败只会中止它自己那一组，不影响后面的语句。
成功或报 26000 的 Close 就算完成；其他失败（被中止，或者在已中止的事务里被拒绝）留到下一次再发。
因为服务器报 26000 而被丢弃的缓存项不再发 Close。

预编译语句缓存：每个池里的连接带一个 SQL 文本到语句名（`s1`、`s2` ...）的 LRU 缓存，上限 256 条。
参数类型由服务器推断，参数和结果都是文本格式。服务器报 `0A000`（缓存的计划不允许改变结果类型，
例如 `SELECT *` 之后表被 `ALTER`）或 `26000`（语句不存在，例如被 `DEALLOCATE ALL`）时丢弃该条缓存
（服务器上的旧语句随下一次操作关闭）；操作之前连接空闲（不在事务里，也没有待发的 BEGIN）就
重新预编译并把整个操作重试一次（隐式事务已经回滚，重试是安全的），在事务里则直接抛出（事务已经中止）。

错误：语句错误带 SQLSTATE 和服务器的消息。连接断开时抛不带 SQLSTATE 的 `pg::error`，
归还时池丢弃这个连接。任何异常都不会让连接停在 pipeline 中间：要么已经读到 Sync，
要么连接被标记为不可用（之后的操作直接抛出，归还时关闭）。

归还：连接 `CONNECTION_OK`、`PQTRANS_IDLE`、没有被标记为不可用时才放回池里。归还时还待发的
BEGIN 从来没发出去过，直接清掉，连接仍然是空闲的。

需要 libpq 14 或更新（pipeline 模式）。libpq 17 及以上用 `PQsendPipelineSync` 和 `PQsendClosePrepared`，
14–16 用 `PQpipelineSync` 和 `DEALLOCATE`。CMake 选项 `LIBPG_FORCE_LIBPQ16_PATH=ON` 在新版 libpq 上
强制走 14–16 的路径，用来测试这条路径。已知限制：在 14–16 上，如果用户在显式事务里绕过 libpg
执行了 `DEALLOCATE`，之后关闭被挤出的同名语句时 `DEALLOCATE` 报错，会中止这个事务。

写回缓存的 flush 也用 batch 减少往返：BEGIN、`SET CONSTRAINTS ALL IMMEDIATE`、
`pg_current_xact_id()` 一次往返；每组的 `SAVEPOINT g`、批量 UPDATE、`RELEASE` 一次往返；
放弃一组时的 `ROLLBACK TO` 和 `RELEASE` 一次往返；逐个格子的 `SAVEPOINT w`、UPDATE、`RELEASE`
一次往返。

## 生成的列标签

`pg_tablegen DSN schema.sql out.hpp [namespace]`（namespace 默认 `db`）的做法：
连上 DSN 指向的服务器，`CREATE DATABASE` 一个临时库（`TEMPLATE template0`），在临时库里
执行 schema.sql，然后查 `pg_catalog` 得到表、列、类型、是否可空、主键、唯一约束，生成头文件，
最后 `DROP DATABASE ... WITH (FORCE)`，无论成功还是失败都会执行。schema.sql 里写什么
（`BEGIN/COMMIT`、`SET search_path`、`public.x`）都只影响临时库。DSN 的用户需要 CREATEDB 权限。
SQL 由 PG 自己解释，不写解析器。

只生成普通表和分区表的父表（`relkind IN ('r','p') AND NOT relispartition`）。
domain 类型按它的基础类型映射，domain 上的 NOT NULL 也算作不可空。
两张表（或同一张表里的两列）转义成同一个 C++ 名字时，生成器报错退出。

每张表 `t` 生成 `namespace db::t`：

```cpp
namespace db::problems {
struct table_ {
    static constexpr std::string_view name = "problems";
    using key = std::tuple<std::int32_t>;                    // 主键列的类型，按约束里的列顺序
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct ac_num {                                              // 每列一个标签
    using table = table_;
    using type = std::int32_t;                               // 可空列是 std::optional<...>
    static constexpr std::string_view name = "ac_num";
};
// 每个 UNIQUE 约束（主键除外）生成一个定位器
struct by_username_t {
    using table = table_;
    using key = std::tuple<std::string>;
    static constexpr std::array<std::string_view, 1> key_columns{"username"};
};
inline constexpr by_username_t by_username{};
}
```

- 名字：多列唯一键用 `by_a_and_b`。是 C++ 关键字的标识符后面加 `_`。
- 类型映射：bool、int2/4/8、float8、text/varchar/bpchar/citext 以及带 COLLATE 的 text、numeric、
  json/jsonb、timestamptz；其他类型生成 `std::string`，并加一行注释写明 PG 类型名。
- 没有主键的表：`table_` 里不生成 `key` 和 `key_columns`，`pg::sql` 因此不能用在这张表上，
  但标签照常生成。

## 格子层：`pg::sql<Col>`

```cpp
template <class Col> class sql {
public:
    using value_type = typename Col::type;
    explicit sql(/* Col::table::key 里的各个类型 */);           // 按主键定位
    template <class By> sql(By, /* By::key 里的各个类型 */);     // 按唯一键定位
    value_type get() const;          operator value_type() const;  // SELECT；行不存在抛 not_found
    sql& operator=(const value_type&);                             // UPDATE；0 行受影响抛 not_found
    // 只对算术类型提供，在库里原子执行：UPDATE t SET c = c + $1
    sql& operator+=(value_type); sql& operator-=(value_type); sql& operator++(); sql& operator--();
};
```

- 每次操作临时从池里借一个连接。
- 标识符一律用双引号括起来。
- 不会自动 INSERT：表里有 NOT NULL 列，库没法替用户编出一行来。行由用户自己用 `conn::exec` 插入。

## 格子层：`pg::cached<Col>`

同一个进程里，同一个格子 `(表, 主键, 列)` 只有**一个**内存槽，所有 `cached` 实例共享它。
按唯一键构造时，先查出主键，所以槽总是以主键标识。

```cpp
template <class Col> class cached {
public:
    explicit cached(/* 主键 */);  template <class By> cached(By, /* 唯一键 */);
    value_type get() const;  operator value_type() const;     // 第一次读时从库加载，之后只读内存
    cached& operator=(const value_type&);                     // 改内存，标记为 set
    cached& operator+=(value_type); -= ++ --                   // 只对算术类型：改内存，累计 delta；
                                                               // 整数溢出时抛 pg::error，内存不变
    template <class F> void update(F f);                       // 在槽锁内执行 f(value_type&)，标记为 set
};

struct cache_options {
    std::chrono::milliseconds interval{100};                  // flusher 的周期
    std::function<void(const pg::error&)> on_error;           // 后台写回失败时调用；为空时写到 stderr。
                                                              // 调用时不持有任何 libpg 内部锁
};
void configure_cache(cache_options);
void flush();      // 同步写回当前所有脏格子；有操作失败时抛出第一个错误（不再交给 on_error）
void shutdown();   // flush 后停止 flusher；之后再写入会重新启动 flusher。不抛异常，错误交给 on_error。
                   // 在 on_error 里（flusher 线程上）调用时只做 flush，不停止 flusher
}
```

**退出前必须调用 `pg::shutdown()`**（通常放在 `main` 的最后）。库不在 `atexit` 或静态析构里
写库：那时用户的 static 和 libpq 依赖的 OpenSSL 可能都已经清理掉了，在这个阶段做网络 IO
或者回调 `on_error` 都不安全。进程退出时如果还有脏格子，只往 stderr 打印一行
「N 个格子没有写回」，不做其他事情。

脏状态（每个槽一份）：

- `set`：在这一批里被赋值过，写回时执行 `UPDATE t SET c = $内存值`。
- 只有 `delta`：写回时执行 `UPDATE t SET c = c + $delta`，这样别的进程对同一列的累加不会被覆盖。
- 在 delta 之后又出现 set：变成 set，以内存值为准。

flusher：一个后台线程，每隔 `interval` 在全局锁内把脏列表 swap 出来；在每个槽的锁内取走
它的待写操作并清掉脏状态；所有更新放在一个事务里提交。事务开始后先执行
`SET CONSTRAINTS ALL IMMEDIATE`，让可延迟约束也在 UPDATE 那一步报错。取走的操作按
（语句种类：哪一列、set 还是 delta）分组，每组最多 1000 个格子用**一条** `UPDATE ... FROM unnest(键数组..., 值数组)
WITH ORDINALITY` 写回（参数是文本数组，列和键的 SQL 类型第一次用到时从 `pg_attribute` 查出并缓存），
`RETURNING` 的序号告诉我们哪些键匹配到了行，没匹配到的格子按 `not_found` 处理。每组外面套一个
SAVEPOINT：整组因永久性错误失败时，`ROLLBACK TO SAVEPOINT`，这一组退回逐个格子的老路径
（每条 UPDATE 各套一个 SAVEPOINT），由它找出出错的那一个，其余照常提交，一个格子失败不影响同一批的其他格子。
只有一个格子的组直接走老路径。槽的注册表按 64 个分片，每片一把读写锁，已存在的槽只取共享锁，
和脏列表、flusher 控制用的锁互不相干。失败分两类：

- **暂时性**（连接已断开，即 `PQstatus == CONNECTION_BAD`；或 SQLSTATE 为 `08`、`40001`、`40P01`、
  `53`、`57`）：整批操作合并回去
  （期间又发生过 set 就丢掉旧的；delta 加回去），报告错误，下一轮重试。
- **永久性**（其他所有错误，比如 `22` 数据错误、`23` 约束违反、`42`、客户端在编码参数时
  抛出的错误；0 行受影响按 `not_found` 处理也属于这一类）：`ROLLBACK TO SAVEPOINT`，丢掉这一个操作，报告错误，其余操作照常提交。
  丢掉之后内存和库已经不一致，所以把槽标记为未加载：下次访问时重新从库读，再叠加还没写回的
  delta；如果槽里已经有新的 set，就以 set 为准，不重读。

所有 flush 由一把 flush 锁串行化，**加载（包括重新加载）也持有这把锁**。所以加载时不会有
已经取走、但结果还没确定的操作，「库里的值 + 还没取走的 delta」一定等于正确的内存值。
代价是第一次读一个格子可能要等正在进行的那一批写回完成。

`update(f)` 在槽内值的副本上执行 `f`，`f` 正常返回后才写回槽里并标记 set；`f` 抛异常时，
内存和脏状态都不变。

COMMIT 本身因为连接断开而失败时，提交结果是未知的。set 可以安全重放，delta 不行（会加两次）。
所以提交前先取 `pg_current_xact_id()`，COMMIT 出现连接错误时，换一个连接查
`pg_xact_status(xid)`：`committed` 视为成功，`aborted` 视为暂时性失败，其他结果报告错误，
只重放 set，不重放 delta。

已知限制（这是设计的一部分，不是 bug）：

- 只保证同一进程内一致。别的进程修改的是绝对值时，本进程看不到。累加类修改在库里不会丢，
  但本进程的内存值不会反映别人的修改。
- 进程崩溃时，最后一个 `interval` 内的修改会丢失。
- 槽不会被淘汰，一直留在内存里。
