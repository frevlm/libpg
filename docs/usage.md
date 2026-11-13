# 使用说明

按“要做什么”查该用哪个接口。术语见 [CONTEXT.md](../CONTEXT.md)，设计决定见 [adr/](adr/)。

| 要做什么 | 用什么 | 写 SQL 吗 |
|---|---|---|
| 余额、计数这类高频累加的数 | 写回缓存 `pg::writeback` + `pg::posting` | 不写 |
| 计费流水、日志这类跟着累加一起写的行 | `posting::insert` | 不写 |
| 插一行 | `pg::insert(c, row)` | 不写 |
| 一次插很多行 | `pg::insert_all(c, rows)` | 不写 |
| 固定的查询、报表、联表 | 查询文件，生成带类型的函数 | 写在 `.sql` 文件里 |
| 拼出来的动态查询 | 语句 `pg::sql{"... {} ...", v}` | 写 |

类型是严格对齐的：参数一律二进制并显式声明类型，结果列按类型（读形状还按列名）精确核对，对不上就抛 `pg::error`，
没有隐式转换（[ADR 0027](adr/0027-strict-type-alignment.md)）。第 6 节有 C++ 类型和 PostgreSQL 类型的对应表。

## 1. 生成代码

`pg_tablegen` 从 schema 生成表的类型：

```sh
pg_tablegen postgresql://user@host/postgres schema.sql db_tables.hpp [--namespace db] \
    [--queries queries.sql]... [--queries-out db_queries.hpp] [--check]
```

| 选项 | 作用 |
|---|---|
| `--namespace ns` | 生成代码的命名空间，默认 `db`；也可以作为第四个位置参数 |
| `--queries file.sql` | 查询文件，可以重复；需要同时给 `--queries-out` |
| `--queries-out out.hpp` | 查询函数写到这个文件 |
| `--check` | 只检查已有的生成文件和重新生成的结果是否一致，不写文件，适合放在 CI 里 |

它在服务器上建一个临时库，执行 schema，读出类型，然后删掉这个库（需要 CREATEDB 权限）。每张表 `t` 生成以下内容：

- `db::t::<列名>`：列标签，成员 `pg_table`、`pg_type`、`pg_name` 分别是表、列的 C++ 类型和列名，例如 `db::users::balance`。
  `pg_table` 带表名 `name`，有主键时还带 `key`（主键列的 C++ 类型）和 `key_columns`。
- `db::t::row`：读形状，对应 `SELECT *` 读出的一行；带列名 `pg_names`，结果列数、列名或类型对不上时在语句执行前抛错。
- `db::t::write`：写形状。有 DEFAULT 的列是 `pg::field<T>`，不设置就写 DEFAULT；可空的列是 `std::optional<T>`。

类型只用第 6 节的表映射。表里或查询结果里有表之外的类型（`inet`、枚举、json、数组、不带 (p, s) 或精度超过 18 的 numeric）
时生成报错，指出表、列和类型；在 SQL 里写 `::text` 之类的转换，或者不选这一列。

下面的例子假设生成了 `db::users`（含 `balance` 列，`numeric(12,2)`，主键 `id`）、`db::events` 等表。

## 2. 写回缓存：余额、计数

钱、额度、token 用量这类“每个请求都要累加”的数，不要在请求里写库：

```cpp
pg::pool p({.dsn = "postgresql://revlm@postgres/revlm"});
pg::writeback wb(p, {.on_lost = [](const pg::lost_posting& l) { log(l.reason, l.description); }});

pg::decimal<2> left = wb.get<db::users::balance>(user_id);   // 读内存，不往返
if (left < cost) return reject();

db::events::write ev;                                         // 计费流水
ev.type = "charge";
ev.time = now();
wb.submit(pg::posting{}.insert(ev).add<db::users::balance>(-cost, user_id));
```

`pg::writeback` 在单独的库 `pg_writeback` 里，用时 `#include <pg/writeback.hpp>` 并链接 `pg_writeback`。

- `get<Col>(key...)`：读计数器的值。键是该表的主键，各列按顺序给。内存里没有这个键时用一条 SELECT 加载，行不存在抛 `pg::not_found`。
  计数器的值类型是整数或 `pg::decimal<S>`。在调用线程持有这个池的 `conn` 时（例如 `pool::transact` 里面）会抛 `nested_borrow`，
  不管键在不在内存里。
- `submit(posting)`：立即把增量加到内存，排队，然后返回，不访问数据库。积压达到 `backlog_limit` 时抛 `backlog_full`，什么都不记录。
- `flush()`：返回时，此前提交的每一笔都已结算（提交，或交给 `on_lost`）。
- `stats()`：`backlog`（已记账未结算的笔数）、`committed`、`lost`、`replays`（重放过的批次数）。

选项：

| 选项 | 默认 | 说明 |
|---|---|---|
| `interval` | 200 ms | 每隔这么久写一个批次 |
| `refresh` | 1 s | 加载超过这么久的槽被重读，用来感知别的副本和别的写入者 |
| `backlog_limit` | 100000 | 已记账未结算的笔数上限 |
| `shutdown_timeout` | 10 s | 析构时最多等这么久写完积压，没写完的交给 `on_lost` |
| `on_lost` | 空 | 被放弃的笔；在写回线程上调用，不能回调同一个 `writeback`；为空时打印到 stderr |

行为：

- 一笔 `pg::posting` 里的插入行和增量一起提交，或者一起放弃。`posting{}.insert(row)` 和 `.add<Col>(delta, key...)` 可以链式调用。
- 后台线程把积压的笔写成一个事务，开头插一条回执。断线或提交结果未知时原样重放，回执冲突说明已经提交过，
  所以不会重复扣款（[ADR 0012](adr/0012-idempotent-write-back-via-receipts.md)）。
- 同一张表、同一组列的插入行在一批里合成一条 `INSERT ... SELECT FROM unnest(...)`，每个计数器一条 `UPDATE ... FROM unnest(...)`。
- 数据错误（行不存在、约束）把批次对半拆开，直到找出出错的那一笔，交给 `on_lost`，它的增量从内存撤回。
- 内存值靠 `refresh` 定期重读保持新鲜，没有跨副本通知（[ADR 0028](adr/0028-writeback-freshness-by-reread.md)）：
  别的副本或手工 SQL 的改动，最多滞后约 `refresh` 加一个 `interval` 才反映到 `get`。
- 构造时在库里建 `libpg.flush_receipts`（需要能建 schema 的权限）。

管理员充值这类低频、需要立刻生效的写入，直接在事务里执行 SQL；计数器列只应通过增量修改，绝对值写入会覆盖别的副本还没写回的增量。

## 3. 插入

```cpp
db::users::write u;
u.username = "alice";
u.email = std::nullopt;          // 写 NULL
// u.joined 不设置：写 DEFAULT
std::int64_t n = pg::insert(c, u);

std::vector<db::users::write> batch = load_users();
std::int64_t rows = pg::insert_all(c, batch);   // 一条二进制 COPY，整批原子
```

`insert_all` 比逐行 `insert` 快约 8 倍。在部署机上测 34 列、4 个索引的表：每行总 CPU 从 150 µs 降到 18 µs，单线程 56k 行每秒。

- `pg::field` 列在第一行设置了才写，之后任何一行的设置情况与第一行不同就抛错；空区间返回 0，不发往返。
- 发数据之前先用服务端的描述核对每个目标列的类型，成员类型和列类型不一致在发送任何行之前抛错。
- COPY 自己是原子的，不另开事务；在调用方的事务里 COPY 失败就直接抛出，事务已经中止，只能回滚。
- COPY 期间这个 `conn` 独占一条线路连接。

## 4. 查询文件：固定的查询

```sql
-- name: group_members :many
SELECT m.user_id, m.role, u.username, g.title
FROM memberships m
LEFT JOIN users u ON u.id = m.user_id
LEFT JOIN groups g ON g.id = m.group_id
WHERE m.group_id = :group_id /*@filter*/
ORDER BY m.user_id;
```

参数在 SQL 里就地命名：`:name`，或 psql 的 `:'name'`；`::` 是类型转换，不是占位符。两个以上输入（参数加洞）时生成
`db::group_members_params`，调用方按名字填：

```cpp
auto rows = db::group_members(c, {.group_id = 1, .filter = pg::sql{"AND m.role = {}", role}});
auto user = db::user_by_id(c, 7);     // 只有 0 或 1 个输入时按位置传
```

| 注解 | 返回 | 行数不对时 |
|---|---|---|
| `:one` | 行结构体 | 没有行抛 `not_found`，多于一行抛 `pg::error` |
| `:opt` | `std::optional<行结构体>` | 多于一行抛 `pg::error` |
| `:many` | `std::vector<行结构体>` | |
| `:exec` | 受影响的行数 | |

- 参数名按首次出现编号成 `$1..$n`，同一个名字复用一个参数。字符串、E 字符串、双引号标识符、美元引号和
  `--`、`/* */` 注释里的冒号不算。
- 2 个以上输入生成 `<函数名>_params` 结构体，成员按首次出现顺序：参数是 `std::string_view`、标量或其他类型，
  洞是 `pg::sql`；0 或 1 个输入直接按位置传。
- 每个结果列都有生成的行结构体（`<函数名>_row`），带 `pg_names`，执行前检查列数、列名和类型。
- 可空性：结果列来自 NOT NULL 的表列（服务端报告它的来源表和列），且查询里没有 LEFT/RIGHT/FULL JOIN 时是非空，
  否则是 `std::optional`。列别名写 `"x!"` 强制非空，写 `"x?"` 强制可空。
- `/*@名字*/` 是洞，生成函数里会多一个 `pg::sql` 参数，用来放动态的 WHERE 片段。
- SQL 文件保持 psql 写法，`\set name value` 之后可以照原样执行。

## 5. 语句：动态 SQL

前面几节覆盖不到的情况才手写语句：

```cpp
using pg::sql;
sql q{"SELECT id, username FROM users WHERE rank = {}", std::int16_t{3}};
if (only_active) q += sql{" AND active"};
auto rows = c.query<std::int32_t, std::string>(q);                         // vector<tuple>
auto one  = c.query_one<db::users::row>(sql{"SELECT * FROM users WHERE id = {}", id});
auto opt  = c.query_opt<std::string>(sql{"SELECT username FROM users WHERE id = {}", id});
std::int64_t n = c.exec(sql{"DELETE FROM users WHERE active = {}", false});
```

- `{}` 位置一律变成参数，值不会拼进 SQL 文本，所以不存在注入；`{}` 的个数和实参个数在编译期核对，字面量大括号写 `{{`、`}}`。
- 参数的 C++ 类型就是它的服务端类型：`int` 是 int4，`std::int64_t` 是 int8，`std::string` 是 text。类型对不上由服务端在 Prepare 时拒绝，
  例如和 smallint 列比较要传 `std::int16_t`，或者在 SQL 里写 `{}::int4`。
- 语句可以嵌套：`sql{"... WHERE {}", other_sql}`，也可以用 `+=` 拼接。
- 表名、列名这类标识符用 `pg::ident(name)` 插入，`pg::join(parts, sep)` 用分隔符连接多段语句。
- 任意字符串当 SQL 文本只有 `pg::unsafe_sql(text)` 这一个入口，专门留给审计时搜索。
- `query<T...>` 的类型参数是各列的 C++ 类型；一个读形状（`db::users::row` 或查询生成的 `_row`）也行，此时按列名核对。
  结果里的 NULL 要用 `std::optional<T>` 接，否则抛错。
- `std::vector<T>` 绑定为数组参数，如 `sql{"id = ANY({})", ids}`；数组只能作参数，不能读。
- 列的类型在执行前检查，不匹配时抛错，错误里写明第几列、列名、服务端类型和期望的 C++ 类型。

### 先发后取：一个请求要查好几样东西

```cpp
using pg::sql;
auto user  = c.send<db::users::row>(sql{"SELECT * FROM users WHERE id = {}", id});
auto count = c.send<std::int64_t>(sql{"SELECT count(*) FROM users WHERE rank = {}", std::int16_t{3}});
auto page  = c.send<db::users::row>(sql{"SELECT * FROM users ORDER BY id DESC LIMIT 50"});
render(user.one(), count.one(), page.all());   // 三条语句一起发出，总共约一次往返的时间
```

- 有未读语句的 `conn` 独占一条线路连接，语句按发送顺序执行；`conn` 结束时先读完它发出的每条语句。
- 每个 `pending` 的读取（`all()`、`one()`、`opt()`、`affected()`）只能调用一次。
- 事务里没人读的语句由 `commit()` 检查，有错就回滚并抛出它的错误。事务外没人读的语句出错时，错误被丢弃。
- 不需要结果的写入可以写成 `(void)c.send(...)`。

## 6. 类型

| C++ | PostgreSQL | 说明 |
|---|---|---|
| `bool` | bool | |
| `std::int16_t`、`std::int32_t`、`std::int64_t` | int2、int4、int8 | `long`、`long long` 都按 int8 |
| `float`、`double` | float4、float8 | |
| `std::string` | text | 结果还接受 varchar、bpchar、name |
| `std::string_view`、`const char*` | text | 只能作参数 |
| `pg::decimal<S>` | numeric | S 为 0..18，定点小数；读到超过 S 位小数的值时抛错 |
| `pg::jsonb` | jsonb | `.text` 是 JSON 文本 |
| `pg::uuid` | uuid | `to_string()`、`uuid::parse()` |
| `pg::bytea`（`std::vector<std::byte>`） | bytea | |
| `pg::timestamptz` | timestamptz | 自 Unix 纪元起的微秒；±infinity 是 `max()`/`min()` |
| `pg::date` | date | 自 Unix 纪元起的天数；±infinity 是 `max()`/`min()` |
| `std::vector<T>` | T 的数组 | 只能作参数；元素可以是 `std::optional<T>`（NULL）；T 是上表中的类型 |
| `std::optional<T>` | 同 T | 空是 NULL；NULL 读进非 optional 抛错 |

## 7. 连接与事务

```cpp
pg::pool p({.dsn = "postgresql://app@db/app?options=-c%20statement_timeout%3D30s", .max_conns = 8});

// 闭包事务：遇到可以重试的错误时重跑 f，最多 transact_attempts 次，每次之前带抖动的指数退避
std::int64_t added = p.transact([&](pg::conn& c) {
    c.exec(pg::sql{"UPDATE users SET points = points + {} WHERE id = {}", std::int64_t{10}, uid});
    return pg::insert(c, row);
});

// 手动事务
pg::conn c(p);
pg::transaction tx(c);       // 嵌套时是保存点
c.exec(pg::sql{"DELETE FROM users WHERE active = {}", false});
tx.commit();                 // 不调用 commit()，析构时回滚
```

`pool_options`：

| 选项 | 默认 | 说明 |
|---|---|---|
| `dsn` | 空 | libpq 的连接串；会话设置写在 `options` 里，如 `options=-c statement_timeout=30s -c synchronous_commit=off` |
| `max_conns` | 8 | 线路连接数上限 |
| `acquire_timeout` | 30 s | 等线路连接的最长时间，超时抛 `pg::error` |
| `transact_attempts` | 3 | 闭包事务的尝试次数 |

- `pool::stats()` 给出 `open`、`idle`、`waiting`、`borrows`。
- `pg::conn` 很便宜。事务外的 `exec`/`query*` 在往返期间租用一条线路连接，和别的线程共用；
  事务、有未读的先发语句、`raw()`、`insert_all` 的 COPY 独占一条。借连接按先来先到排队。
- 事务外的两条语句可能落在不同的服务端会话上，和事务池模式的池化器一样。`SET`、advisory lock、`currval` 这类依赖会话状态的语句，
  放进事务里，或者用 `raw()`。
- `c.raw()` 返回 `PGconn*`：此时在途语句都已读完，连接已经在普通阻塞模式，可以调用 `PQexec` 之类的函数。它归这个 `conn` 独占，
  `conn` 结束时关闭。
- 同一线程对同一个池再借一个 `pg::conn`，会抛 `nested_borrow`。函数之间请把 `conn&` 传下去。
- `commit()` 抛 `commit_unknown`，表示提交结果未知：COMMIT 已经发出，但连接断开了。闭包事务不会重试它；
  需要幂等的写入请用写回缓存，它靠回执处理这种情况。
- 错误都是 `pg::error`，看 `e.kind` 和 `e.sqlstate`，语句文本在 `e.sql`，`what()` 里不含 SQL 文本。
  `error_kind`：`broken`、`commit_unknown`、`conflict`、`stale_statement`、`read_only`、`lock_or_cancel`、`connect_failed`、`data`、`other`；
  闭包事务重试其中的 `broken`、`conflict`、`stale_statement`、`read_only`。

## 8. 迁移

```cpp
#include <pg/migrate.hpp>

pg::migrate(p, {.migrations = {{"0001_init", init_sql}, {"0002_users_index", idx_sql}}});
```

迁移按名字识别。所有待执行的迁移在一个事务里按顺序执行，并用 advisory lock 防止多个副本同时迁移；已执行的迁移记在
`libpg.migrations` 里，附带规范化后文本（CRLF 转 LF、去行尾空白）的 SHA-256。已执行的迁移如果被改过，校验和对不上，会直接抛错。
脚本可以包含多条语句，但不能自己结束事务。任何失败整体回滚并抛出。

## 9. 性能要点

部署机上实测，数据库和应用共用 2 个 vCPU：

- **语句条数**比客户端效率更重要。每条语句在服务端要花 40 µs 以上，libpg 自身每条只花几 µs。
- 写操作走写回缓存或 `insert_all`，不要在循环里逐行 `insert`。
- 一个请求里互不依赖的查询用 `send` 一起发出。
- 事件循环线程里不要做同步的数据库调用。读用写回缓存，写交给 `submit`。
- 数据库在远端时，线程数可以多于 `max_conns`：事务外的语句共用线路连接，不会因为每条语句等一个往返而排队。
