# libpg 重新设计（v2）

这是对 `docs/design.md`（v1）的整体重构，不是补丁清单。v1 还没有使用方，所以 libpg 自己的 API
可以随便改；必须保持不变的，是 revlm、HHHOJ 迁移到 libpg 之后的**行为**。

本文由三份独立评审合并而成：同类库 API 设计、生产故障模式、格子层与代码生成。三份的分歧和裁决
记录在 §13。

**两个面分开设计。**

- **用户面**（C++ API）：受两个使用方现有写法的约束，例如 `f(Db&)` 签名、动态 SQL 片段、迁移语义。
- **线路面**（libpg 和 PG 之间的协议、语句、库内对象）：用户看不见，所以只按“对 PG 来说最好的做法”设计。

两个项目今天“没用到”某个协议能力，不构成线路面不用它的理由。线路面的设计见 §2.4、§5.1、§7.2、§9。

## 0. 依据

### 0.1 两个使用方

| 能力 | revlm | HHHOJ |
|---|---|---|
| 部署 | **API 默认 2 个副本，HPA 最多 6 个**；`terminationGracePeriodSeconds: 90`；只读根文件系统 | docker compose；server 和 `hhhoj queue` 是两个进程 |
| 调用模型 | asio worker 里同步调用 | httplib worker 线程 |
| 访问数据库的方式 | `Db db;` 后调用 `f(Db&, ...)`（非模板） | 同左 |
| 事务 | 约 12 处，READ COMMITTED | 约 20 处，READ COMMITTED |
| 行锁 | `FOR UPDATE` | `FOR UPDATE` / `FOR NO KEY UPDATE` |
| advisory lock | 无 | 迁移用 `pg_advisory_xact_lock(20261018)`；3 处业务锁，key 是 `hashtext` 的三种不同形状 |
| 动态 WHERE | `RequestStore::query`：15 个可选条件，结果 35 列 | `submissions_read`；`perm::where` 是**运行期把规则文本编译成 SQL** |
| IN 列表 / RETURNING / ON CONFLICT DO NOTHING | 有 | 有 |
| 聚合 | 少 | 多（`string_agg`、`CASE`） |
| 分页 | keyset | OFFSET |
| JSONB | 无 | 大量，读写都经过文本转 nlohmann |
| NUMERIC | `NUMERIC(18,6)` 余额，C++ 里用 `double` | 无 |
| 非确定性 ICU collation `ci` | 有 | 有 |
| 迁移 | 所有待执行迁移放**一个事务**，按 version 识别，**无锁** | 一个事务，xact 锁，按**名字**识别 |
| 写回缓存 | BalanceLedger（`atomic<int64>` 微美元）+ BatchWriter（扣款与流水在同一事务；重试 5 次后丢弃；积压上限 10 万条） | 无 |
| SQLSTATE 分类、超时、取消 | 无 | 无 |
| DSN | 拆成 5 个字段再拼回去（`sslmode`、多主机参数丢失） | 同左 |
| COPY / LISTEN / 数组列 / BYTEA / 异步 IO | 无 | 无 |

### 0.2 v1 的结构性问题

1. **全局单例**：连接池和缓存都是全局的，退出前必须手动调 `pg::shutdown()`。
2. **格子层不参与事务**：`pg::sql<Col>` 自己借连接；同一线程嵌套借连接会死锁。
3. **参数个数在编译期固定**：动态 WHERE、IN 列表、SQL 片段拼接都只能绕道 `detail::params`。
4. **只有一种错误类型，codec 不检查 PG 类型**：`count(*)` 读进 `int32_t`，要等计数超过 2^31 才在生产环境出错。
5. **只能走 `PQexecParams`**：多语句的迁移脚本跑不了。
6. **NUMERIC 只能当字符串**：余额这个最需要像变量一样用的格子，反而用不了。
7. **写回缓存的原子单位选错了**：按格子写回，可 revlm 要求原子的是“一笔账”，也就是一行流水加上它引起的余额变化。
   set/delta 混合状态机、`update(F)` 的锁序死锁，以及“结果未知时只重放 set”这类丢账规则，都是从这里来的。
8. **连接池只在归还时看 `PQstatus`**：服务端断开空闲连接、主库切换成只读，都要等到下一条查询失败才知道。

## 1. 总体结构

```
pg::pool       连接池对象：健康检查、熔断、统计
pg::conn       借出的一个连接。唯一的执行器，所有语句都在它上面执行
  └─ 引擎      线路面（用户不可见）：pipeline、二进制格式、语句缓存、截止时间
pg::tx         连接上的事务守卫；嵌套时就是 SAVEPOINT
pg::sql        可组合的 SQL：文本片段和参数交替排列
pg::cell<Col>  直写格子，绑定在 conn& 上
pg::cache      写回缓存对象：counter<Col> 计数器、post() 记账
pg::migrate    迁移，语义和两个项目现在的一致
pg_gen         代码生成：schema.sql 生成表类型；queries/*.sql 生成带类型的函数
```

核心只有一个概念：**`pg::conn&`**。不做模板 concept，不做虚基类，`pool` 也不是执行器。
在不在事务里，看这个连接当前的状态。

- 两个项目里大量 `f(Db&, ...)` 形式的非模板函数，签名可以原样保留。
- 生成的查询函数是普通函数，实现放在生成的 `.cpp` 里，不在每个 TU 里实例化。
- “helper 随手传 `pool`”这种便利被去掉了，而它正是嵌套借用的来源。

依据：tokio-postgres 为了统一 `Client` 和 `Transaction`，只好另加一个 `GenericClient` trait；sqlx
的维护者建议直接收 `&mut PgConnection`；sqlpp11 本身就是“事务是连接上的状态”。

## 2. 连接池

```cpp
namespace pg {
struct pool_options {
    std::string dsn;                    // 原样交给 libpq：PQconnectdbParams，expand_dbname=1
    int max_size = 8;
    std::chrono::milliseconds acquire_timeout{5000};
    std::chrono::milliseconds ping_after_idle{1000};
    std::chrono::seconds max_idle_time{300};
    std::chrono::seconds max_lifetime{1800};      // 每个连接的实际寿命在 [0.9, 1.0] × max_lifetime 之间随机
    std::chrono::milliseconds statement_timeout{30000};
    std::chrono::milliseconds lock_timeout{0};
    std::chrono::milliseconds idle_in_transaction_timeout{60000};
    std::string application_name;
    std::function<void(const query_event&)> on_query;      // §10
    std::function<void(const connect_event&)> on_connect;
};

class pool {
public:
    explicit pool(pool_options);
    ~pool();                                   // 不等待：先关掉空闲连接；借出去的连接归还时直接关闭
    conn acquire();
    void wait_ready(std::chrono::milliseconds deadline);   // 启动时带退避地建连，超时抛异常
    template <class F> auto transact(F&& f);                       // §3
    template <class F> auto transact(const tx_options&, F&& f);
    pool_stats stats() const;
};

// 在 libpq 连接之上附带池的记账。可移动、不可复制，析构时归还。
class conn {
public:
    std::int64_t exec(const sql&);
    template <class... T> std::vector<row_t<T...>> query(const sql&);  // 单列时元素是 T
    template <class... T> row_t<T...> query_one(const sql&);           // 恰好一行；0 行抛 not_found，多于一行抛 error
    template <class... T> std::optional<row_t<T...>> query_opt(const sql&);  // 0 或 1 行
    void exec_script(std::string_view);   // simple 协议，可以多条语句、不能带参数；执行过的连接不再回池
    bool in_transaction() const;
    PGconn* raw();
};
}
```

### 2.1 建连

- **DSN 原样传给 libpq**。用 `PQconninfoParse` 判断用户写了哪些键，只补缺少的：

  | 键 | 默认值 |
  |---|---|
  | `connect_timeout` | 5 |
  | `keepalives_idle` | 30 |
  | `keepalives_interval` | 10 |
  | `keepalives_count` | 3 |
  | `tcp_user_timeout` | 30000 |
  | `target_session_attrs` | `read-write` |

  多主机、`sslmode`、`load_balance_hosts` 都由 libpq 原生处理，库不写一行代码。
- **会话参数放进启动参数 `options`**：`-c TimeZone=UTC -c client_min_messages=warning`，以及上面几个超时。
  用户的 DSN 里已经有 `options` 时，把这些追加在后面。好处是建连后不用多一次往返，`RESET ALL` 之后仍然有效。
  - `TimeZone` 仍然要设：用户 SQL 里 `date_trunc`、`to_char` 的结果依赖它。
  - v1 里的 `DateStyle`、`IntervalStyle`、`extra_float_digits` 都不需要了：结果走二进制格式（§5.1），
    解码不再依赖服务端怎样格式化文本。
- **TLS**：库不改 libpq 的 `sslmode` 默认值，但会在 `connect_event::tls` 里报告实际是否用了 TLS。
  revlm 连非本机数据库时应该在 DSN 里写 `sslmode=verify-full`，因为 `channels.api_key` 是上游密钥。

### 2.2 借出与归还

- **借出时**：
  - 超过寿命或空闲时间的连接直接关掉；
  - 空闲超过 `ping_after_idle` 的连接，先发一条空查询 `;` 探活；
  - 探活和建连都失败时，抛 `connection_error`。
  - 库只做探活，**永远不重放用户的语句**。
- **还能不能写**：PG 14 起，服务器会通过 ParameterStatus 主动推送 `in_hot_standby` 和
  `default_transaction_read_only`。每次往返之后读这两个值，不需要额外的往返。
  主库被隔离成只读时，空闲连接在下一次 ping 时就会被发现。PG 13 及以下的退路是：收到 25006 就丢掉这个连接。
- **归还时**：只有同时满足 `CONNECTION_OK`、`PQTRANS_IDLE`、可写，并且没有执行过 `exec_script` 的连接
  才放回池里，其余一律关闭。
- **嵌套借用**：一个线程已经持有某个池的连接，再从同一个池 `acquire`，**立即抛异常**，不会死锁。
  （HikariCP 的池死锁公式是 `Tn × (Cm − 1) + 1`，禁止嵌套就是让 Cm = 1，于是任何池大小都不会死锁。）
- **建连熔断**：建连失败后进入 down 状态，退避时间从 250ms 开始翻倍，上限 5s。
  - down 期间，需要新连接的 `acquire` 立即抛出上一次的错误；
  - 退避到期后只放**一个**线程去探测，成功后恢复。
  - 这样数据库停机时 worker 线程会快速失败：HHHOJ 能返回 503，revlm 的 asio 循环不会被卡住；
    数据库恢复时也不会出现惊群。

### 2.3 不支持的部署

只支持直连和 PgBouncer **会话**池模式。在事务池模式下，启动参数、会话级状态、prepared statement、
LISTEN 和多语句脚本都不可靠，两个使用方也都不用它。正因为把这种模式明确排除在外，线路面才能放心使用
§2.4 里的全部能力。

### 2.4 连接引擎（线路面）

每个连接里有一个引擎，用户看不到它。所有语句都只走这一条代码路径：

```
用户调用 → 步骤序列 (Parse? Describe? Bind Execute … Sync) → 非阻塞发送 → poll → 读回结果
```

**pipeline 模式。** 连接建立后进入 libpq 的 pipeline 模式（PG 14 及以上），并设为非阻塞。
- 每次操作把需要的协议消息一次性发出去，最后跟一个 Sync，然后读回所有结果。
- 发送和接收在同一个 poll 循环里交替进行，避免 pipeline 在阻塞模式下“发送缓冲区满、服务端等客户端读”的互锁。
- `exec_script` 临时退出 pipeline 模式，用 simple 协议执行；这个连接执行完就关掉，不回池（§2.2）。

**按语句缓存 prepared statement。** 每个连接有一张 LRU 表，上限 256 条，键是渲染后的 SQL 文本。
- 第一次执行某条语句：Parse（参数类型由 codec 给出）加上 Describe，一次往返，拿到参数和结果列的 OID。
  **在 Execute 之前**就用 codec 的 `accepts` 核对结果列。类型不对时，语句根本不会执行，不会留下副作用。
- 之后再执行：Bind、Execute、Sync，一次往返。
- 遇到 `0A000 cached plan must not change result type` 或 `26000`（语句名不存在）：从缓存里删掉这条语句。
  事务外：重新 Parse 后重试一次。这类错误发生在执行之前，重试是安全的。
  事务内：整个事务已经失败，按可重试错误交给 `transact`（§8）。
- 动态拼出来的 SQL 形状很多时，LRU 会淘汰不常用的；被淘汰的语句发 Close 释放服务端资源，Close 跟着下一次 Sync 一起发出去，不额外往返。
- 计划缓存使用 PG 默认的 `plan_cache_mode = auto`。如果某个参数值分布极不均匀，导致通用计划很差，
  可以在 DSN 的 options 里改成 `force_custom_plan`，库不再加单独的开关。

**类型缓存。** 每个池有一张 OID 到类型信息的表（是否 enum、数组的元素类型等），第一次遇到未知 OID 时
查一次 `pg_type`。domain 不需要查：RowDescription 报告的已经是它的基础类型。

**往返次数：**

| 操作 | v1 | v2 引擎 |
|---|---|---|
| 一条查询（缓存命中） | 1 | 1 |
| `transact` 里的 BEGIN | 单独 1 次 | 和第一条语句同批发出，0 次 |
| 写回一批（回执、插入、更新、提交） | 约 5 次 | **1 次**（§7.2） |

**客户端截止时间。** 非阻塞 IO 让每次操作都可以有一个截止时间：`statement_timeout` 加 5 秒余量。
超时后用 PG 17 的 `PQcancelBlocking`（旧版 libpq 退回 `PQcancel`）发取消请求，再关掉这个连接，抛 `connection_error`。
服务端超时只管得到还活着的连接，这一层专门处理网络黑洞。取消是引擎内部行为，不提供给用户。

## 3. 事务

```cpp
namespace pg {
enum class isolation { read_committed, repeatable_read, serializable };
struct tx_options {
    isolation level = isolation::read_committed;
    bool read_only = false;
    std::chrono::milliseconds statement_timeout{0};   // 非 0 时用 SET LOCAL
    int max_attempts = 1;                             // 只有 pool::transact 会读这个字段
};

// 构造：连接空闲时执行 BEGIN；已经在事务里时执行 SAVEPOINT。
// commit()：对应 COMMIT 或 RELEASE。
// 析构时还没 commit：对应 ROLLBACK 或 ROLLBACK TO。
// 嵌套时 options 必须是默认值，否则抛 error（隔离级别不能在事务中途改变）。
class tx {
public:
    explicit tx(conn&, tx_options = {});
    void commit();
    ~tx();
};
}

// 闭包形式：借连接、BEGIN、执行、COMMIT，按 §8 的表格决定是否重试
auto id = pool.transact({.level = isolation::serializable, .max_attempts = 5}, [&](pg::conn& c) {
    ...
    return new_id;
});
```

- 闭包可能被执行多次，所以闭包里不能有库外的副作用。这一点写进文档，库没法检查。
- **COMMIT 期间连接断开**：抛 `commit_unknown`，**永不重试**，也不去查 `pg_xact_status`。
  原因是主库切换之后，新主库上的 xid 可能还没分配到，也可能已经被别的事务用掉，查出来的结果不可靠。
  需要恰好一次语义的业务，自己用唯一键做幂等；写回缓存则用 §7 的回执表。
- **只读快照**：revlm 的 Snapshot 重建改用 `{.level = repeatable_read, .read_only = true}`，
  让 5 条 SELECT 看到同一个时间点的数据。
- 不提供 advisory lock 帮手。HHHOJ 现有的三种 key 形状，任何统一的推导规则都对不上；
  对不上的后果是滚动发布期间新旧进程之间失去互斥。这些锁直接写成 SQL 语句或查询文件。

## 4. 语句 `pg::sql`

```cpp
namespace pg {
class sql {
public:
    sql() = default;
    consteval sql(const char* literal);                         // 只接受编译期字面量
    template <class... A>
    sql(format<std::type_identity_t<A>...> f, const A&... a);   // "{}" 的个数在编译期核对
    sql& operator+=(const sql&);
    static sql unchecked(std::string text);                     // 唯一的逃生口，可以 grep 出来审计
};
sql ident(std::string_view name);                               // 加双引号转义的标识符
std::string like_escape(std::string_view);
template <std::ranges::input_range R> sql join(R&& parts, const sql& sep);
}
```

- `sql` 存的是“文本片段和参数交替”的序列，**渲染时才给参数编号 `$n`**，所以片段可以任意拼接和嵌套。
- `{}` 的含义取决于实参类型：实参是 `pg::sql` 时，原样嵌入它的文本并接上它的参数；其他类型一律绑定为参数。
  jOOQ 的模板也是这个规则。
- 普通 `std::string` 不能隐式变成 SQL 文本。HHHOJ 的 `perm::where` 迁移时这样处理：
  - 字面值（`lit()`、`std::to_string`）改成 `{}` 参数；
  - 运算符 token 尽量用白名单映射到字面量片段；
  - 实在映射不了的，才用 `unchecked`。

```cpp
pg::sql w;
if (f.before_id) w += pg::sql(" AND r.id < {}", *f.before_id);
if (!f.user_ids.empty()) w += pg::sql(" AND r.user_id = ANY({})", f.user_ids);
w += perm::where(viewer);
```

**行为变化提醒**：revlm 现在拼 LIKE 时没有转义，用户输入里的 `%` 和 `_` 都是通配符。
迁移时要不要加上 `like_escape`，必须当作一个明确的决定，不能顺手改掉。

## 5. 编解码与结果映射

### 5.1 codec 契约

线路面一律使用**二进制格式**；用户面看到的类型不变。

```cpp
template <> struct pg::codec<std::int64_t> {
    static constexpr oid param_type = 20;              // 在 Parse 时声明的参数类型
    static constexpr format param_format = binary;
    static bool accepts(oid t, const type_info&) { return t == 20; }   // 在 Describe 之后、Execute 之前检查
    static void encode(std::int64_t, std::string& out);   // 追加写；网络字节序 8 字节
    static std::int64_t decode(std::string_view);         // 输入是二进制字节
};
```

**为什么线路面用二进制：**
- `timestamptz` 就是“自 2000-01-01 起的 int64 微秒”，`infinity` 是 int64 的最大和最小值。
  v1 里解析 ISO 文本、BC 年份、超过 4 位的年份，以及依赖 `DateStyle` 的那些代码全部删掉。
- `double` 就是 IEEE 8 字节，往返精确，`extra_float_digits` 不再需要。
- `numeric` 是 base-10000 的数字组，直接换算成 `decimal<S>` 的 int64，过程中不经过字符串。
- 数组带头部和元素长度，不需要处理文本格式里的引号和转义。
- 所以解码不依赖任何会话参数，§2.1 的启动参数因此也变少了。

**规则：**
- 结果**只有二进制一种格式**。libpq 不支持按列指定结果格式，这里干脆不留文本这条路。
- `std::string` 能接收的结果类型：text、varchar、bpchar、name、citext、enum、json，以及 jsonb（去掉版本字节）。
  其他类型（例如 `inet`）在 Describe 时就报错：`列 3 是 inet：在 SQL 里写 ::text，或者提供 codec<inet>`。
  错误发生在执行之前。
- **参数**格式可以逐个指定：
  - `std::string` / `string_view` 用**文本**格式，`param_type = 0`，交给服务端推断。所以 jsonb、citext、enum、
    `COLLATE ci` 这些参数的行为和现在一模一样。
  - 其他类型用二进制格式，并显式声明类型 OID。这也顺带解决了 `{} IS NULL` 推断不出参数类型的问题。
- `double` 只接受 float4 和 float8，读 numeric 时丢精度必须显式写出来。
- 用户自定义的 codec 必须提供二进制的 `encode`/`decode` 和 `accepts`。
  只想用文本的类型，就在 SQL 里写 `::text`，再按 `std::string` 读。
- **调试**：`on_query` 给出的是 SQL 文本；`pg::debug_render(sql)` 把参数按文本形式内联进去，只用于日志。

| C++ | PG |
|---|---|
| `bool`、`int16/32/64_t`、`double`、`std::string` | 同 v1 |
| `pg::decimal<S>` | `numeric(p,S)`，p ≤ 18。内部是 `int64 × 10^S`，有 `+ - == <`；小数位超出 S 时抛 `decode_error` |
| `pg::numeric` | 不带精度的 numeric 或 p > 18，原样保存文本 |
| `pg::timestamptz` | 微秒精度，可以从 `sys_seconds` 隐式构造 |
| `std::vector<T>` | 一维数组，读写都支持，主要用于 `= ANY({})` |
| `pg::bytea` | 原始字节 |
| `pg::uuid` | 16 字节 |
| `pg::jsonb` | 文本。可选头文件 `pg/nlohmann.hpp` 提供 `codec<nlohmann::json>` |
| `std::optional<T>` | 可空 |

### 5.2 按列名映射到结构体

pg_gen 为每张表生成一个读形状 `row`、一个写形状 `insert_row`，再加一个 `row_traits` 字段表。
这些是**生成出来的代码，不是反射**：

```cpp
namespace db::requests {
struct row {                                // SELECT * 读出来的样子，可空性完全按 attnotnull
    std::int64_t id;
    pg::timestamptz time;
    std::optional<std::string> request_id;
    std::int64_t user_id;
    pg::decimal<6> usd;
    /* … */
};
struct insert_row {                         // INSERT 接收的样子
    pg::def<std::int64_t> id;               // identity，不赋值时写 DEFAULT
    pg::def<pg::timestamptz> time;          // DEFAULT now()
    std::optional<std::string> request_id;  // 可空且无默认值：nullopt 就是 NULL
    std::int64_t user_id;                   // NOT NULL 且无默认值：必须给出
    /* … */
};
row insert(pg::conn&, const insert_row&);                  // INSERT … RETURNING *
void insert(pg::conn&, std::span<const insert_row>);       // 用 unnest；按“哪些 def 列给了值”分组
}
template <> struct pg::row_traits<db::requests::row> {
    static constexpr auto fields = pg::fields(pg::field("id", &db::requests::row::id), /* … */);
};
```

- `pg::def<T>`：没有赋值就是 DEFAULT。T 本身可以是 `optional<U>`，这样“可空且有默认值”的列，
  DEFAULT 和 NULL 也能区分开。
- `conn::query<R>` 在 `R` 有 `row_traits` 时按**列名**映射。列的集合必须和字段集合完全相等，
  多一列或少一列都抛异常，并列出名字。这样就避免了 35 列的结果里，同类型的相邻列错位却没人发现。
- 一次性的结果类型（比如聚合）可以手写 `row_traits`，每个字段一行。
- `pg::columns<R>("r")` 渲染出 `"r"."id", "r"."time", …`。
- 两三列的小查询继续用按位置的 `query<A, B>`。

## 6. 代码生成 `pg_gen`

所有工作都在临时数据库里完成（`CREATE DATABASE … TEMPLATE template0`，最后 `DROP … WITH (FORCE)`），
SQL 由 PG 自己解释，不写解析器。生成器要求 PG ≥ 16，只影响 CI，不影响生产库。

### 6.1 表

和 v1 一样生成 `table_`、列标签和唯一键定位器。新增内容：

- `numeric(p,s)` 且 p ≤ 18 时映射成 `decimal<s>`；
- §5.2 里的 `row`、`insert_row`、`row_traits` 和 `insert`；
- 单列主键时，`key_of<table_>` 直接是这个标量类型。
- 遇到没有内置 codec 的 PG 类型就报错，并指出两种解决办法：用 `--type` 指定一个已经有 codec 的 C++ 类型，
  或者把列类型改掉。v1 那种“静默生成 `std::string` 加一行注释”的做法，在二进制格式下读不出来，所以去掉。
- `libpg` schema 里的对象（§9）不参与生成。

### 6.2 查询文件

```sql
-- name: list_recent_requests :many
-- params: user_id, limit
SELECT id, model, usd, time FROM requests WHERE user_id = $1 ORDER BY id DESC LIMIT $2;

-- name: find_user_by_email :opt
-- params: email
SELECT id, username FROM users WHERE email = $1;

-- name: set_note :exec
-- params: id, note?
UPDATE problems SET note = $2 WHERE id = $1;

-- name: list_requests :many
-- params: limit?
SELECT r.* FROM requests r WHERE true /*@filter*/ ORDER BY r.id /*@dir*/ LIMIT $1;
```

- **SQL 本身一个字节都不改**，拿到 psql 里照样能跑。
  - 参数名写在 `-- params:` 头里，名字个数和 `$n` 的个数对不上就报错；
  - 名字后面加 `?` 表示这个参数可以为 NULL，生成 `std::optional<T>`。
  - 不用 `@name` 这种写法，因为需要自己写词法扫描，而 `@>`、`<@`、`?` 本身就是 JSONB 运算符。
- **注解**：`:one` 返回恰好一行的 `T`；`:opt` 返回 `optional<T>`；`:many` 返回 `vector<T>`；`:exec` 返回受影响的行数。
- **洞 `/*@name*/`**：它在 SQL 里是注释，PREPARE 和 describe 时自然忽略，结果的列形状不受影响。
  在生成的函数里，它变成一个 `const pg::sql&` 参数。动态 WHERE 因此也能拿到生成的行类型，
  revlm 的 `RequestStore::query` 和 HHHOJ 的 `SubmissionCatalog::list` 正需要这个。
  这是 sqlc #364、#2021、#2060 讨论多年都没有落地的东西。
- **类型复用**：结果列恰好是某张表的全部列且顺序一致时（用 `PQftable`/`PQftablecol` 判断），
  直接返回 `db::<表>::row`。
- **类型覆盖**：`pg_gen --type requests.usd=double` 按列覆盖，`--type jsonb=nlohmann::json` 按 PG 类型覆盖。
  迁移期间 revlm 可以先保留 `double`。要换结果类型时也可以在 SQL 里写 CAST：
  `sum(usd)::numeric(18,6)` 会生成 `decimal<6>`，因为 RowDescription 带有 typmod。

```cpp
namespace db::q {   // 声明在头文件里，定义在生成的 .cpp 里
std::vector<db::requests::row> list_requests(pg::conn&, const pg::sql& filter, const pg::sql& dir,
                                             std::optional<std::int64_t> limit);
}
```

### 6.3 可空性推断

一个结果列被推断为非空，必须**同时满足**以下三条：

1. `PQftable`/`PQftablecol` 能定位到表 T 的某一列，且这一列 `attnotnull`；
2. 在 `EXPLAIN (VERBOSE, FORMAT JSON, GENERIC_PLAN)` 的计划里，T 没有出现在任何外连接的可空侧。
   可空侧指 Left 连接的 inner 子树、Right 连接的 outer 子树、Full 连接的两侧。
   同一张表在可空侧和非可空侧都出现时（自连接），保守地当作可空；
3. 这一列不是 PG 18 `RETURNING old.*` / `new.*` 产生的列。

不满足的一律生成 `optional`。列别名后缀 `!` 强制非空、`?` 强制可空，和 sqlx、PgTyped 的约定一致。
推断错误的代价是运行期抛 `decode_error`，不会读出脏值。

这里用 `GENERIC_PLAN` 而不是 sqlx 那种“参数全填 NULL 再 EXPLAIN”：后者会把 `WHERE x = NULL` 常量折叠掉，
外连接节点可能整个从计划里消失，推断结果就悄悄错了。

### 6.4 离线模式与漂移检查

- 生成的文件**提交进仓库**，平常构建不需要数据库。
- CI 里跑 `pg_gen --check`，做两件事：
  1. 重新生成，和仓库里的版本对比；
  2. 在第二个临时库里依次执行 `migrations/`，再和用 `schema.sql` 建出来的库比较系统目录。
     代码是按 schema.sql 生成的，生产库却是靠 migrations 建出来的，两者之间的漂移以前没有任何检查能发现。
- CMake 只提供函数，不强制使用：`libpg_generate(TARGET … SCHEMA … QUERIES … OUT …)`。

## 7. 格子层

### 7.1 直写 `pg::cell<Col>`

```cpp
pg::cell<db::users::balance_usd> bal(c, user_id);   // 绑定 conn&；c 在事务里，格子操作就在事务里
auto v = bal.get();          // SELECT；行不存在抛 not_found
bal = v;                     // UPDATE；0 行受影响抛 not_found
auto after = (bal += d);     // UPDATE … SET c = c + $1 RETURNING c，一次往返，返回新值
```

每个运算符都是一次网络往返。这正是 v1 “格子就是变量，每次访问都到数据库”的设计本意，所以保留下来。
另外支持按唯一键定位：`cell<Col>(c, db::users::by_email, email)`。

### 7.2 写回缓存：原子单位是“一笔”

计数器（余额）的粒度是**格子**；写回的原子单位是**一笔**，即若干插入行加上若干 delta。
revlm 的扣款本来就是从流水推出来的，两者是同一个事实，必须一起提交或一起放弃。
v1 和 v2 初稿里的“写回 set”、`update(F)`、`forget`、LRU 淘汰都没有使用方，全部删掉。
以后真需要“最后写入者为准”的寄存器，再作为 `post` 的第三种部件 `pg::assign<Col>` 加进来，其余 API 不用动。

```cpp
namespace pg {
template <class Col> struct delta { key_of<typename Col::table> key; typename Col::type by; };

struct cache_options {
    std::chrono::milliseconds interval{200};
    std::size_t max_batch = 128;                 // 攒满一批就立即唤醒 flusher
    std::size_t max_pending = 100'000;           // 超过后 post 抛 backlog_full
    std::chrono::seconds full_refresh{60};       // 兜底的全量对齐周期，见“跨副本一致”
    std::function<void(const pg::error&)> on_error;     // 每次写回失败都调用，用于告警
    std::function<void(const lost_write&)> on_lost;     // 被永久放弃的笔，逐条交给它
};

class cache {
public:
    cache(pg::pool&, cache_options = {});        // 检查 libpg schema 是否已经建好（§9）；
                                                 // flusher 和监听器各独占一个连接
    ~cache();                                    // 等价于 close(10s)

    // 一笔账：其中的行和 delta 要么在同一个事务里一起提交，要么都不提交。
    // 返回时内存已经反映了这些 delta。行在调用线程上编码，所以编码错误抛给调用方。不做 IO。
    template <class... Part> void post(Part&&... parts);    // Part 是 insert_row 或 pg::delta<Col>

    template <class Col> counter<Col>& counter();            // 每列一个，第一次调用时创建
    close_report close(std::chrono::milliseconds deadline);  // 停止接收；写到截止时间，剩下的交给 on_lost
    bool flush(std::chrono::milliseconds timeout);
    cache_stats stats() const;   // pending_rows, oldest_pending, batches_ok/failed, lost, consecutive_failures
};

template <class Col> class counter {
public:
    std::optional<value_type> peek(const key_type&) const noexcept;   // 只读内存；没加载过返回 nullopt
    value_type get(const key_type&);                // 没加载时执行一条 SELECT
    value_type add(const key_type&, value_type d);  // 等于只含这一个 delta 的 post，返回新值
    std::size_t refresh(const sql& where = {});     // 一条 SELECT 重新对齐；正常情况下由 cache 自己调用
    cached<Col> operator[](key_type);               // 一个格子的名字：可以复制，不会悬空
};
}

// revlm
cache.post(db::requests::insert_row{.user_id = uid, /* … */ .usd = usd},
           pg::delta<db::users::balance_usd>{uid, -usd});
```

**内存模型**。热路径上的读和写都不碰全局锁，也不做 IO：

```cpp
struct slot { std::atomic<rep> value; rep base; std::atomic<bool> loaded; std::mutex mu; };
// 每列一个：std::shared_mutex + unordered_map<key_type, unique_ptr<slot>>
```

- **读**：`shared_lock` 查表，再 `value.load()`。和 revlm 现在的 `has_balance` 一模一样。
- **写**：`shared_lock` 查表，`value.fetch_add(d)`，然后入队。键不存在时才短暂拿一次 `unique_lock` 插入新槽。
- **不变式**：`value == base + 已经 post 但还没结算的 delta`。
  - 提交成功：`base += d`；
  - 永久失败：`value -= d`；
  - 对齐：`value += db − base; base = db`。

  写者只碰 `value`，从来不需要知道 `base`。v1 那套“重新加载，再叠加还没写回的 delta”的逻辑就此消失。
- 内存里不做溢出检查。数据库列真的溢出时会报永久错误，这一笔被撤回，内存自己回到正确的值。

**批次和回执（线路面）**。每批写入都天然幂等，所以“提交结果未知”不需要判定，原样重放就行。
回执表放在 libpg 自己的 schema 里，由 `pg::migrate` 建立（§9），使用方不需要为它写迁移。

一批数据**只用一次往返**：pipeline 里发出下面这些语句，最后跟一个 Sync。在扩展协议里，同一个 Sync
之前的多条语句会自动组成一个**隐式事务**，到 Sync 时一起提交，所以不需要 BEGIN 和 COMMIT。

```text
head = 结果未定的那一批（最多一批）；为空时从队列里按 FIFO 取最多 max_batch 笔，冻结内容并分配 id

INSERT INTO libpg.flush_receipts(id) VALUES ($id)                -- 故意不写 ON CONFLICT
每张表一条：INSERT INTO t (c…) SELECT * FROM unnest($1::t1[], …)        -- 参数个数 = 列数，和行数无关
每个计数列一条：UPDATE t SET c = t.c + v.d FROM unnest($1::k[], $2::c[]) v(k, d)
                WHERE t.k = v.k RETURNING t.k, t.c                -- delta 按键求和并排序，放在最后
Sync                                                              -- 隐式事务在这里提交

结果：
成功           → 按“已提交”结算；RETURNING 里没出现的键说明行已删除 → 丢弃这个槽，把这个 delta 交给 on_lost
                 （对应的流水行照常插入：revlm 的 requests 表故意不建外键）
回执表上的 23505 → 这一批之前已经提交过（包括“确认包丢了”的那次）；错误让后面的语句全部跳过，
                 按“已提交”结算。原事务还在进行中时，这条 INSERT 会等它结束，再按结果走对应的分支
其他 22xxx / 23xxx → 换成显式 BEGIN，用同一个 id 重做，每一笔套一个 SAVEPOINT，找出出错的那一笔撤回并交给 on_lost
                 （很少走到这里，多几次往返无所谓）
其他任何情况（断连、COMMIT 结果未知、40001、40P01、25006、55P03、57014、42xxx、XX000 …）
               → head 原样保留，退避后用同一个 id 重放
```

- **回执冲突本身就是答案**：不需要先查一次“这批提交过没有”，也不需要 `ON CONFLICT` 之后再判断行数。
  唯一约束一冲突，事务自然中止，后面的语句一条都不会执行。
- **为什么 delta 要先按键求和**：`UPDATE … FROM` 里同一个键出现两次时，PG 只用其中一行，用哪一行不确定。
- **为什么按键排序**：所有副本按同样的顺序加行锁，跨副本死锁更少。即使发生死锁，也只是 40P01，重放是安全的。
- **为什么用 unnest**：Bind 消息里参数个数是 Int16，多行 VALUES 最多 65535 个参数，requests 有 35 列。
  另外，语句文本和行数无关，所以能命中 §2.4 的语句缓存。
- **为什么不用 COPY**：COPY 不能放进 pipeline，也不能 RETURNING。一批最多 128 笔，unnest 足够。
- flusher 每小时清理一次一天以前的回执。时间一律用数据库的 `now()`，库里不依赖墙钟。
- 对比旧方案：靠 xid 判定提交结果，在主库切换后会报错或查到别的事务；“只重放 set”等于丢账。

**对齐公式**。`refresh` 用一条语句同时读出库里的值，以及“head 那一批的回执是否可见”，两者来自同一个快照：

```sql
SELECT k, c, EXISTS (SELECT 1 FROM libpg.flush_receipts WHERE id = $head) FROM t WHERE k = ANY($keys)
```

内存值 = 库里的值 + 本进程还没结算的 delta − （回执可见时，head 里这个键的 delta）。

计算过程不需要 flush 锁，也不会阻塞写者。结算和对齐都要改 `base`，它们由每列一把 settle 锁串行化。
写者从来不拿这把锁。flusher 只在一批数据结算的那一刻持有它，不会在整个网络往返期间一直持有。

**跨副本一致（线路面）**。revlm 有 2 到 6 个副本，每个副本各有一份内存余额。数据库自己知道哪些行变了，
所以由数据库来通知各副本，而不是让各副本定时去猜：

- **触发器**：在 `migrate_options::watch` 里声明要监视的列（例如 `"users.balance_usd"`），`pg::migrate`
  就为这一列建立**语句级**触发器：`AFTER INSERT OR UPDATE OF c OR DELETE ON t REFERENCING … TABLE
  FOR EACH STATEMENT`。触发器把变化的主键收集起来，调用 `pg_notify('libpg_watch', '<列编号>|k1,k2,…')`；
  单条消息不能超过 8000 字节，所以按 7000 字节切分成多条。
  - 一批写回只产生一次触发，不是每行一次。
  - 不管是谁写的都会通知：flusher、管理后台的直写、别的进程、手工执行的 SQL。
- **监听**：每个 `cache` 有一个专用连接，执行 `LISTEN libpg_watch`。
  - 收到通知后，对其中的键做一次 `refresh`。
  - 自己的 flusher 发出的通知按 `PGnotify::be_pid` 识别并忽略，因为它们已经在结算时处理过了。
  - INSERT 进来的新键直接建槽，DELETE 掉的键丢弃对应的槽。
- **丢失的通知**：监听连接断开期间的通知会丢。重新连上后先做一次全量 `refresh`，另外每隔 `full_refresh`
  （60 秒）再全量对齐一次作为兜底。
- **效果**：
  - 别的副本的扣款，等它的 flusher 提交后几毫秒内就能看到，所以透支上限从“5 秒的消费”缩小到“一个
    写回周期（200ms）的消费”；
  - 充值几毫秒内对所有副本可见；
  - 管理员充值只要正常写库，不需要再手动调 `refresh`。
- **已知代价**：带 NOTIFY 的事务在提交时会争一把全库范围的锁，这些提交是串行的。每个副本每 200ms 一批，
  量级远低于它的上限。HHHOJ 不监视任何列，不受影响。

**运行边界**：

- **积压**：`post` 在积压超过 `max_pending` 时抛 `backlog_full`。应用在请求入口检查 `stats()`，
  超过阈值就返回 503，所以这个异常正常情况下不会被触发。库不再偷偷丢最旧的记录，而 revlm 现在就是这么做的。
- **崩溃**：所有尚未提交的笔都会丢失，最多 `max_pending` 笔。这一点照实写进文档。
- **关停**：revlm 的顺序是 `server.drain()` → `cache.close(25s)` → pool 析构。
  丢掉的每一笔都以结构化日志的形式交出来，可以人工补账。
- **管理员充值**：直写就行，`pool.transact([&](pg::conn& c) { pg::cell<db::users::balance_usd>(c, uid) += amount; });`。
  触发器会通知包括本进程在内的所有副本。这修好了 revlm 现在“用库里的值覆盖内存，导致队列里还没写库的扣款
  凭空消失”的问题。
- **绝对值写入**：迁移时要把 `balance_usd` 从 `UserStore::update_user` 的 SET 里去掉。
  触发器能让各副本看到绝对值写入，但看到的时候，别的副本还没写回的扣款已经被这个绝对值覆盖了。

## 8. 错误

```cpp
namespace pg {
struct error : std::runtime_error {};              // 库自己产生的错误：只有消息
struct server_error : error {                      // 服务端返回的错误
    std::string sqlstate, detail, hint, schema, table, column, constraint;
    std::string query;                             // 这条 SQL 的文本，只给日志用，不放进 what()
};
struct connection_error : error {};                // CONNECTION_BAD、08xxx、建连失败、熔断中
struct commit_unknown : connection_error {};
struct pool_timeout : error {};
struct backlog_full : error {};
struct not_found : error {};
struct decode_error : error {};
// server_error 的子类：unique_violation 23505, foreign_key_violation 23503, not_null_violation 23502,
// check_violation 23514, serialization_failure 40001, deadlock_detected 40P01,
// read_only_transaction 25006, lock_not_available 55P03, query_canceled 57014
}
```

`what()` 只包含 SQLSTATE 和 PG 的主消息，所以应用把 `e.what()` 返回给客户端时，不会泄漏 SQL。

**一张表决定所有处置**，连接池、`transact` 和写回缓存都按它执行，没有第二套分类：

| 情况 | 连接 | `transact`（max_attempts > 1 时） | 写回缓存 |
|---|---|---|---|
| `CONNECTION_BAD`，发生在 COMMIT 之前 | 丢弃 | 重试 | 重放同一批 |
| `CONNECTION_BAD`，发生在 COMMIT 期间 | 丢弃 | 抛 `commit_unknown` | 重放同一批（靠回执保证幂等） |
| 25006，或者连接不可写 | 丢弃 | 重试 | 重放 |
| 40001 / 40P01 | 保留 | 重试 | 重放 |
| 55P03 / 57014 | 保留 | 抛出 | 重放 |
| 0A000（缓存的计划结果类型变了）/ 26000 | 保留，丢掉那条缓存语句 | 事务外：重新 Parse 后重试一次（引擎内部）；事务内：重试整个闭包 | 重放 |
| 客户端截止时间到 | 先取消，再丢弃 | 抛出 | 重放 |
| 建连失败、53300、57P03 | — | 抛出（熔断） | 稍后重放 |
| 22xxx / 23xxx | 保留 | 抛出 | 逐笔隔离，交给 on_lost |
| 其他（42xxx、XX000 …） | 保留 | 抛出 | 重放，并通过 on_error 告警 |

重试的等待时间是指数退避加随机抖动。

## 9. 迁移 `pg::migrate` 与 libpg 自己的库内对象

用户面的语义和两个项目现在的一样：所有待执行的迁移放在**一个事务**里，全部成功或全部回滚。
线路面的记账则由 libpg 自己管，放进它独占的 schema `libpg`，不再借用使用方的表。

```cpp
struct migrate_options {
    std::optional<legacy_table> import_from;      // 例如 {"schema_migrations", by_version}；HHHOJ 用 by_name
    std::optional<std::int64_t> legacy_lock_key;  // 滚动发布过渡期也拿旧锁：HHHOJ 是 20261018
    std::vector<std::string> watch;               // 要跨副本通知的列，例如 {"users.balance_usd"}（§7.2）
    std::chrono::milliseconds lock_timeout{3000};
    int lock_retries = 10;
};
void migrate(pg::pool&, std::span<const migration>, const migrate_options&);
```

**libpg schema 里有什么**

| 对象 | 作用 |
|---|---|
| `libpg.migrations(name, checksum, applied_at, duration_ms)` | 迁移记账 |
| `libpg.flush_receipts(id, at)` | 写回批次的回执（§7.2） |
| `libpg.watch_<表>_<列>` 函数，以及挂在用户表上的同名触发器 | 跨副本通知（§7.2） |
| `libpg.version` | 记录 libpg 自身的对象版本，供以后升级这些对象 |

**一次 migrate 做的事**，全部在同一个事务里：

1. `pg_advisory_xact_lock(<libpg 固定 key>)`。设置了 `legacy_lock_key` 时，再拿一次旧 key，这样滚动发布期间新旧
   二进制仍然互斥。等所有旧版本都下线后，这个选项就可以删掉。
2. `SET LOCAL lock_timeout`、`SET LOCAL statement_timeout = 0`。
3. 按 `libpg.version` 建立或升级 libpg 自己的对象（`CREATE SCHEMA IF NOT EXISTS libpg` 等）。
4. **只在第一次运行时**导入旧表：按版本号或名字（HHHOJ 有一套名字规范化规则，导入时沿用）把已经应用过的迁移
   记进 `libpg.migrations`。旧表只读，不修改；不再需要它时，使用方自己删掉。
5. 依次执行待执行的迁移：用 simple 协议的 `exec_script`，每个迁移执行完写一行记账。
6. 按 `watch` 同步触发器：缺的创建，多余的删除。
7. COMMIT。

- **checksum**：对内容做规范化（CRLF 转成 LF，去掉行尾空白）后再求 SHA-256。所以 Git 的换行符转换不会造成
  误报，而真正修改了已经应用过的迁移时，会报错。
- **遇到 55P03**：回滚，退避（从 1s 开始翻倍，上限 30s）后从头再跑。滚动发布时，`ALTER TABLE` 拿锁排队会让
  这张表的所有读写都排在它后面，几秒内就能把池打满。“短 lock_timeout 加重试”是业界的标准解法。
- **`pg_gen --check` 和 revlm 的 `check-schema.sh`**：比较时排除 `libpg` schema，以及名字以 `libpg_` 开头的触发器。
- `-- no-transaction` 等真正需要 `CREATE INDEX CONCURRENTLY` 时再加。
- CMake 函数 `libpg_embed_migrations(TARGET dir)` 负责嵌入迁移文件。

## 10. 可观测性

```cpp
struct query_event {
    std::string_view name;        // pg_gen 生成的查询名；手写的 sql 为空。指标用它做标签，避免标签基数爆炸
    std::string_view sql;         // 渲染后带 $n 的文本，不含参数值
    std::chrono::nanoseconds elapsed;
    std::int64_t rows;
    std::string_view sqlstate;    // 成功时为空
    int backend_pid;              // 对应 pg_stat_activity.pid
};
struct connect_event {
    std::string_view host; int port; bool tls; int server_version;
    std::chrono::nanoseconds elapsed; const pg::error* failure;
};
struct pool_stats {
    int max_size, open, idle, in_use, waiting; bool down;
    std::uint64_t acquires, acquire_timeouts, connects, connect_failures,
                  closed_broken, closed_lifetime, closed_idle;
    std::chrono::nanoseconds acquire_wait_total;
};
```

- 回调在执行语句的线程上调用，调用时不持有池锁，回调为空时没有任何开销。
- 慢查询日志由应用自己写：`if (e.elapsed > 200ms) log(e)`。
- 不依赖任何指标库。revlm 的 `/readyz` 可以接上 `pool.stats().down` 和 `cache.stats().oldest_pending`。

## 11. 测试

单元测试和集成测试都跑在现有的 `scripts/test-pg.sh` 临时集群上。下面是故障注入测试，不引入新的依赖：

| 故障 | 注入方式 | 断言 |
|---|---|---|
| 连接被杀 | 旁路连接执行 `pg_terminate_backend(pid)` | 空闲连接在下次 acquire 时重连；事务进行中时，`transact` 重试成功 |
| 主库变只读 | `ALTER SYSTEM SET default_transaction_read_only = on` 后 reload | 空闲连接被丢弃，建连被拒并进入熔断；cache 积压但不丢数据，恢复后自动追上 |
| 服务器重启 | `pg_ctl restart -m fast` | 每个空闲连接最多失败一次 |
| 连接数用尽 | `ALTER ROLE … CONNECTION LIMIT 1` | 熔断时立即失败，而不是等满 5 秒 |
| 提交结果未知 | 正常提交一批后，用同一个 id 再重放一次 | 库里只加了一次 |
| 跨副本 | 两个 cache 实例并发写回重叠的键 1000 次 | 总额正确 |
| 停机后关停 | `pg_ctl stop` 后持续 post，直到 `backlog_full`；然后 `close(1s)` | on_lost 收到的条数等于积压的条数 |
| 迁移锁排队 | 旁路事务持有 ACCESS SHARE 锁 2 秒 | migrate 重试后成功，其他会话没有被阻塞 |
| 二进制编解码 | 每个内置类型的边界值往返：`numeric` 的 NaN 和超出精度、`timestamptz` 的 ±infinity、空数组和含 NULL 的数组、jsonb | 逐字节一致；超出 `decimal<S>` 时抛 decode_error |
| 表结构变了 | 语句已经缓存后执行 `ALTER TABLE … ADD COLUMN`，再跑 `SELECT *` | 事务外自动重新 Parse 后成功；事务内由 transact 重试后成功 |
| 写回原子性 | 一批里让第 k 笔违反 CHECK 约束 | 只撤回第 k 笔，其余提交；回执只有一行 |
| 跨副本通知 | A 副本写回，B 副本 peek；杀掉 B 的监听连接后再写 | B 在 1 秒内看到新值；重连后全量对齐，最终一致 |
| 截止时间 | 用 iptables 丢掉某个连接的包 | 到截止时间后取消并丢弃这个连接，不会无限阻塞 |
| 可空推断 | 生成器测试：LEFT JOIN 保留侧和可空侧的列、自连接、`RETURNING old.*` | 生成的类型符合 §6.3 |

网络黑洞（验证 keepalive）只能靠 Toxiproxy 或 iptables，作为 Linux CI 上的可选测试。

## 12. 明确不做

| 不做 | 原因 |
|---|---|
| 查询 DSL、ORM | HHHOJ 已经在至少 18 处用 verbatim 绕开 DSL；SQL 文本加 §6 的代码生成已经有类型安全 |
| 反射 | 用户已经否决 |
| 模板 executor concept、虚基类 executor | 只有 `conn&` 一种执行器就够了（§1） |
| 用 `pg_xact_status` 判定提交结果 | 主库切换后不可靠（§3、§7.2） |
| 公开的取消句柄 | 取消只由引擎在截止时间到达时内部使用；关停时应该等请求结束，而不是打断它 |
| advisory lock 帮手 | key 的推导规则和现有锁对不上，滚动发布期间会失去互斥 |
| 写回 set、`update(F)`、`forget`、LRU、按唯一键定位的缓存格子 | 没有使用方；`update(F)` 有锁序死锁；按唯一键定位要在热路径上多一次 SELECT |
| 异步 API；公开给用户的 pipeline、COPY、LISTEN | 用户面不需要。线路面已经在内部使用 pipeline、LISTEN/NOTIFY 和二进制格式（§2.4、§5.1、§7.2）；COPY 不能放进 pipeline，unnest 已经够用 |
| 文本结果格式 | 只保留二进制一种格式，不维护两套解码器 |
| 本地 WAL 落盘 | revlm 是只读根文件系统；准入控制加 on_lost 日志已经够用 |
| PgBouncer 事务池模式 | 见 §2.3 |

## 13. 取舍记录

三份评审意见冲突的地方，以及最后怎么定的：

| 议题 | 方案 | 采用 | 理由 |
|---|---|---|---|
| 执行器 | A：只用具体类型 `conn&`；B：保留 concept；C：虚基类 | A | 概念最少；非模板，和现有 `Db&` 签名一致；去掉了嵌套借用的来源 |
| 提交结果未知 | A：改用 `_if_assigned` 的 xid；B：回执表；C：查 xid，查不出就撤回再 refresh | B（缓存）；`transact` 直接抛 `commit_unknown` | xid 在主库切换后会报错或指向别的事务；回执让“未知”这个状态不复存在 |
| 写回的单位 | B：`append(row, add…)`；C：`post(parts…)` | C 的形式 | 能表达多行、多个 delta，`append` 也不再单独存在 |
| 积压溢出 | B：抛 `backlog_full`；C：丢最旧的并撤回 | B | 不偷偷丢数据，准入由应用决定 |
| 多副本对齐 | B：rebase 公式；C：`refresh` 加不变式 | 两者合并 | C 的不变式让写者无锁；B 的“回执可见”判断让对齐不需要 flush 锁 |
| 查询参数名 | A：`@name` 加词法扫描；C：`-- params:` 文件头 | C | SQL 一个字节都不改，不写扫描器，避开 JSONB 运算符的冲突 |
| 动态查询的结果类型 | A：`/*@hole*/` | 采用 | 洞是注释，不需要任何改写 |
| 迁移 | A：保留现有语义；B：会话锁加 checksum | A，再加上 B 的 lock_timeout 和重试 | 不破坏现有行为；B 的锁排队问题确实存在 |
| `statement_timeout` 默认值 | v2 初稿：0；B：30s | 30s | 库的默认值应该能防事故。两个项目迁移时先用 `log_min_duration_statement` 找出慢查询，显式设置超时，不让行为悄悄改变 |
| prepared statement | A：默认关闭；B：删掉 | **改为线路面默认开启：按语句缓存** | 用户面不暴露任何开关。在执行前 Describe 才能检查类型、才能用二进制格式；事务池模式已经被排除在外；`0A000` 由引擎处理 |
| 线路格式 | v2 初稿：文本 | 二进制 | 解码不依赖会话参数，`timestamptz` 的文本解析整个删掉；`numeric` 直接转成 `decimal`；参数里的 `std::string` 仍然用文本，所以服务端的推断行为不变 |
| 写回批次的事务 | BEGIN … COMMIT，约 5 次往返 | pipeline 隐式事务，1 次往返；回执上的 23505 就是“已经提交过”的信号 | 少了 BEGIN/COMMIT，也少了“先查一下回执”这个分支 |
| 跨副本一致 | B：应用每 5 秒调一次 refresh | 语句级触发器加 NOTIFY，监听连接收到后定向 refresh；每 60 秒全量对齐兜底 | 由数据库告诉各副本哪些键变了；任何写入方都会被感知，包括管理后台和手工 SQL |
| 迁移记账 | A：沿用现有表 | libpg 自己的 `libpg` schema；第一次运行时从旧表导入；过渡期同时拿新旧两把锁 | 不改用户的表；回执和触发器函数也有了归属；checksum 先规范化换行再算 |
| `cell` 上的运算符 | A：删掉，避免把网络往返藏起来；C：保留 | 保留 | 这是“格子就是变量”这个核心设计本身 |
| `fetch_add` 这个名字 | 返回新值，但 `std::atomic::fetch_add` 返回旧值 | 改名：`+=` 和 `add` | 和标准库的语义保持一致 |

## 14. 两个项目怎么迁移

**两个项目都要做的**

- `struct Db : pg::conn { Db() : pg::conn(app_pool().acquire()) {} };`，所有 `f(Db&, …)` 的签名不变。
- DSN 改成原样传入，不再拆成字段。
- `ensure_schema` 换成 `pg::migrate`，用 `import_from` 导入旧表（§9）。HHHOJ 过渡期设置 `legacy_lock_key = 20261018`。
- 先开一周 `log_min_duration_statement = 5s`，找出长查询，给它们单独设置超时。

**revlm**

- 固定查询放进 `queries/*.sql`；`RequestStore::query` 改用 `/*@filter*/` 洞，得到生成的 `db::requests::row`。
- BalanceLedger 和 BatchWriter 换成一个 `pg::cache`：`counter<db::users::balance_usd>`（类型是 `decimal<6>`）
  加上 `post(insert_row, delta)`。迁移期间可以用 `--type users.balance_usd=double` 过渡。
- `migrate_options::watch = {"users.balance_usd"}`，跨副本一致由库负责；snapshot 本身用只读的可重复读事务。
- 管理员充值改成直写，删掉 `admin_adjust_balance`。
- 在请求入口检查 `cache.stats()` 做准入；关停时调用 `cache.close(25s)`。

**HHHOJ**

- `perm::where` 返回 `pg::sql`（见 §4）。
- `problem_donations.cpp:302` 的字符串拼接改成参数绑定；三处 advisory lock 写进查询文件，key 保持不变。
- JSONB 改用 `pg/nlohmann.hpp`，不再需要先转成 `::text`。
- 判题队列可以改成 `FOR UPDATE SKIP LOCKED` 的单条查询。这是应用层的改动，可选。
- 启动时调用 `pool.wait_ready()`，替代现在“崩溃后靠 `restart: always` 重启”的做法。

## 15. 实现顺序

每一步都能单独通过测试：

1. 错误层次；连接引擎（pipeline、非阻塞 poll 循环、截止时间、语句缓存、类型缓存）；`pool` 和 `conn`
   （DSN 补全、启动参数、探活、可写检查、熔断、嵌套检测、统计和钩子）；`exec_script`。
2. `pg::sql`（组合、格式串、`ident`、`like_escape`、`unchecked`）；二进制 codec（`accepts`、追加编码）；
   `decimal`、数组、`bytea`、`uuid`。
3. `tx` 守卫（嵌套时用 SAVEPOINT）；`pool::transact`（按 §8 的表格重试）。
4. `pg_gen` 表部分（`row`、`insert_row`、`row_traits`、`insert`）；按列名映射；`--check` 的两项检查。
5. `pg_gen` 查询文件（文件头、注解、洞、类型复用、类型覆盖）；可空性推断。
6. `cell<Col>`。
7. `pg::migrate` 和 `libpg` schema（记账、回执、watch 触发器）；CMake 函数；包导出（作为子项目被引入时默认不构建测试）。
8. `cache`：`counter`、`post`、pipeline 写回批次、`refresh`、监听连接、`close`、`stats`。
9. 可选的 `pg/nlohmann.hpp`；§11 的故障注入测试，和对应的步骤一起交付。
