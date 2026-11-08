# 使用说明

按“要做什么”查该用哪个接口。先看前面的：越靠前，手写的 SQL 越少。术语见 [CONTEXT.md](../CONTEXT.md)。

| 要做什么 | 用什么 | 写 SQL 吗 |
|---|---|---|
| 余额、计数这类高频累加的数 | 写回缓存 `pg::writeback` + `pg::posting` | 不写 |
| 把计数器里的一格当变量读写 | 缓存格子 `pg::cached<Col>` | 不写 |
| 计费流水、日志这类跟着累加一起写的行 | `posting::insert` | 不写 |
| 读或改某一行的某一列 | 直写格子 `pg::cell<Col>` | 不写 |
| 插一行 | `pg::insert(c, row)` | 不写 |
| 一次插很多行 | `pg::insert_all(c, rows)` | 不写 |
| 固定的查询、报表、联表 | 查询文件，生成带类型的函数 | 写在 `.sql` 文件里 |
| 拼出来的动态查询 | 语句 `pg::sql{"... {} ...", v}` | 写 |

## 1. 生成代码

`pg_tablegen` 从 schema 生成表的类型：

```sh
pg_tablegen postgresql://user@host/postgres schema.sql db_tables.hpp --namespace db \
    [--queries queries.sql --queries-out db_queries.hpp] [--override table.column=Type] [--check]
```

它在服务器上建一个临时库，执行 schema，读出类型，然后删掉这个库。每张表 `t` 生成以下内容：

- `db::t::<列名>`：列标签，携带表名、列名和 C++ 类型，例如 `db::users::balance`。
- `db::t::row`：读形状，对应 `SELECT *` 读出的一行。
- `db::t::write`：写形状。有 DEFAULT 的列是 `pg::field<T>`，不设置就写 DEFAULT。可空的列是 `std::optional<T>`。
- 唯一约束对应的定位器。

`--check` 只检查生成的文件和 schema 是否一致，不写文件，适合放在 CI 里。

## 2. 写回缓存：余额、计数

钱、额度、token 用量这类“每个请求都要累加”的数，不要在请求里写库：

```cpp
pg::pool p({.dsn = "postgresql://revlm@postgres/revlm"});
pg::writeback wb(p, {.on_lost = [](const pg::lost_posting& l) { log(l.reason, l.description); }});
wb.watch<db::users::balance>();               // 别的副本改了余额时，本进程会收到通知并对齐

auto left = wb.get<db::users::balance>(user_id);   // 读内存，不往返
if (left < cost) return reject();

db::requests::write r;                         // 计费流水
r.user_id = user_id; r.usd = cost; /* ... */
wb.submit(pg::posting{}.insert(r).add<db::users::balance>(-cost, user_id));
```

把计数器里的一格当变量用，读写都在内存里：

```cpp
pg::cached<db::users::balance> bal(wb, user_id);   // 和 pg::cell 一样当变量用，但读内存
if (bal < cost) return reject();
bal -= cost;                                         // 提交只含这一个增量的一笔
```

需要和插入行或几个增量一起原子提交的写回仍然用 `pg::posting`。

- `submit` 立即改内存、排队，然后返回，不访问数据库。
- 后台线程每 200 ms 把积压的笔写一次库。一个写回批次是一次往返、一个事务，并带一条回执，所以断线后重放不会重复扣款。
- 一笔里的插入行和增量一起提交，或者一起放弃。被放弃的笔交给 `on_lost`。
- 同一张表、同一组列的插入行，在一批里合成一条语句：不少于 64 行并且每列都能用二进制写时走 COPY，否则走 `INSERT ... SELECT FROM unnest(...)`。不会逐行插入。
- 积压超过 `backlog_limit` 时，`submit` 抛 `backlog_full`，并且什么都不记录。
- 要监视的列需要在 `pg::migrate` 的 `watched` 里登记（见第 8 节）；没登记时 `watch` 在启动时抛错。

管理员充值这类低频、需要立刻生效的写入，直接在事务里改格子（第 3 节）。监视的通知照样会到达每个副本。

## 3. 直写格子：某一行的某一列

```cpp
pg::conn c(p);
pg::cell<db::users::balance> bal(c, user_id);       // 按主键定位
pg::cell<db::users::email> mail(c, db::users::by_username, "alice");  // 按唯一键定位

pg::decimal<6> b = bal;   // SELECT
bal += 10;                // UPDATE ... SET balance = balance + $1
mail = "a@x.com";         // UPDATE
```

每次读或写都是一次往返。行不存在时抛 `pg::not_found`，格子不会自动插入行。格子在连接所在的事务里执行。

## 4. 插入

```cpp
db::users::write u;
u.username = "alice";
u.email = std::nullopt;          // 写 NULL
// u.created_at 不设置：写 DEFAULT
pg::insert(c, u);

std::vector<db::requests::write> batch = ...;
pg::insert_all(c, batch);        // 一条二进制 COPY，整批原子
```

`insert_all` 比逐行 `insert` 快约 8 倍。在部署机上测 34 列、4 个索引的表：每行总 CPU 从 150 µs 降到 18 µs，单线程 56k 行每秒。

需要注意：

- 调用方不在事务里时，`insert_all` 自己开一个事务。
- 有一列不能用二进制写（例如用 `std::string` 写 inet 列），或者各行设置的 `pg::field` 不一致时，整批自动改走逐行 INSERT。结果相同，只是慢。
- 在调用方的事务里，COPY 失败就直接抛出。事务这时已经中止，只能回滚。

## 5. 查询文件：固定的查询

```sql
-- name: user_requests :many
SELECT id, time, model, usd FROM requests
WHERE user_id = :user_id AND time >= :since /*@filter*/
ORDER BY id DESC LIMIT 50;
```

参数在 SQL 里就地命名：`:name`，或 psql 的 `:'name'`；`::` 是类型转换，不是占位符。两个以上输入（参数加洞）时生成
`db::user_requests_params`，调用方按名字填：

```cpp
auto rows = db::user_requests(c, {.user_id = id, .since = t, .filter = pg::sql{}});
```

只有 0 或 1 个输入时仍是位置参数。

| 注解 | 返回 | 行数不对时 |
|---|---|---|
| `:one` | `T` | 没有行抛 `not_found` |
| `:opt` | `std::optional<T>` | |
| `:many` | `std::vector<T>` | |
| `:exec` | 受影响的行数 | |

- 参数名按首次出现编号成 `$1..$n`，同一个名字复用一个参数。字符串、E 字符串、双引号标识符、美元引号和
  `--`、`/* */` 注释里的冒号不算。
- 2 个以上输入生成 `<函数名>_params` 结构体，成员按首次出现顺序：参数是 `std::string_view`、标量或其他类型，
  洞是 `pg::sql`；调用方用指定初始化器。0 或 1 个输入直接按位置传。
- 每列的可空性按通用执行计划推断。列别名写 `"x!"` 表示强制非空，写 `"x?"` 表示强制可空。
- `/*@名字*/` 是洞，生成函数里会多一个 `pg::sql` 参数，用来放动态的 WHERE 片段。
- SQL 文件保持 psql 写法，`\set name value` 之后可以照原样执行。

## 6. 语句：动态 SQL

前面几节覆盖不到的情况才手写语句：

```cpp
using pg::sql;
sql q{"SELECT id, name FROM users WHERE team = {}", team};
if (only_active) q += sql{" AND active"};
auto rows = c.query<std::int64_t, std::string>(q);                         // vector<tuple>
auto one  = c.query_one<db::users::row>(sql{"SELECT * FROM users WHERE id = {}", id});
auto opt  = c.query_opt<std::string>(sql{"SELECT email FROM users WHERE id = {}", id});
auto n    = c.exec(sql{"DELETE FROM sessions WHERE expires < now()"});
```

- `{}` 位置一律变成参数，值不会拼进 SQL 文本，所以不存在注入。
- 语句可以嵌套：`sql{"... WHERE {}", other_sql}`。
- 表名、列名这类标识符用 `pg::ident(name)` 插入。
- 任意字符串当 SQL 文本只有 `pg::unsafe_sql(text)` 这一个入口，专门留给审计时搜索。
- 列的类型在执行前检查，不匹配时抛错，不会读出乱码。
- 二进制类型的 `std::vector<T>` 绑定为数组参数，如 `sql{"id = ANY({})", ids}`；`long`、`long long` 都按 int8 绑定。

### 先发后取：一个请求要查好几样东西

```cpp
auto user  = c.send<db::users::row>(sql{"SELECT * FROM users WHERE id = {}", id});
auto count = c.send<std::int64_t>(sql{"SELECT count(*) FROM requests WHERE user_id = {}", id});
auto page  = c.send<db::requests::row>(sql{"SELECT * FROM requests WHERE user_id = {} ORDER BY id DESC LIMIT 50", id});
render(user.one(), count.one(), page.all());   // 三条语句一起发出，总共约一次往返的时间
```

- 事务外，发出去的语句之间没有先后顺序。
- 事务里，语句按顺序执行。没人读取结果的语句，由 `commit()` 检查，有错就抛出。
- 不需要结果的写入可以写成 `(void)c.send(...)`。

## 7. 连接与事务

```cpp
pg::pool p({.dsn = "...", .max_conns = 8});

// 闭包事务：遇到可以重试的错误（断线、序列化冲突等）时换连接重跑 f，最多 transact_attempts 次
auto id = p.transact([&](pg::conn& c) {
    pg::cell<db::users::balance>(c, uid) -= cost;
    return pg::insert(c, row);
});

// 手动事务
pg::conn c(p);
pg::transaction tx(c);       // 嵌套时是保存点
...
tx.commit();                 // 不调用 commit()，析构时回滚
```

- `pg::conn` 很便宜。事务外的语句和别的连接共用线路连接，不独占。
- 同一线程对同一个池再借一个 `pg::conn`，会抛 `nested_borrow`。函数之间请把 `conn&` 传下去。
- `commit()` 抛 `error_kind::commit_unknown`，表示提交结果未知：COMMIT 已经发出，但连接断开了。需要幂等的写入请用写回缓存，它靠回执处理这种情况。
- 错误都是 `pg::error`，看 `e.kind` 和 `e.sqlstate` 即可。`what()` 里不含 SQL 文本。

`pool_options` 中常调的几项：

| 选项 | 默认 | 说明 |
|---|---|---|
| `max_conns` | 8 | 线路连接数 |
| `statement_timeout` | 30 s | 建连时设置的服务端超时 |
| `deadline` | 35 s | 客户端每次往返的等待上限，用来对付网络黑洞 |
| `synchronous_commit` | false | 关闭时，服务器崩溃最多丢失约 0.6 s 内的提交，但不会损坏数据；单线程事务吞吐约为开启时的 3 倍 |
| `transact_attempts` | 3 | 闭包事务的尝试次数 |

## 8. 迁移

```cpp
pg::migrate(p, {
    .migrations = {{"0001_init", init_sql}, {"0002_requests_index", idx_sql}},
    .watched = {pg::watch<db::users::balance>()},
    .legacy = pg::legacy_import{"schema_migrations"},   // 只在第一次运行时导入旧的记账表
});
```

迁移按名字识别。所有待执行的迁移在一个事务里执行，并用 advisory lock 防止多个副本同时迁移。已执行的迁移如果被改过，校验和对不上，会直接报错。

## 9. 性能要点

部署机上实测，数据库和应用共用 2 个 vCPU：

- **语句条数**比客户端效率更重要。每条语句在服务端要花 40 µs 以上，libpg 自身每条只花几 µs。
- 写操作走写回缓存或 `insert_all`，不要在循环里逐行 `insert`。
- 一个请求里互不依赖的查询用 `send` 一起发出。
- 事件循环线程里不要做同步的数据库调用。读用写回缓存，写交给 `submit`。

## 10. 多副本、多数据库（k8s）

数据分到多台数据库服务器上时，每台服务器一个池。按 key 选分片：

```cpp
std::vector<pg::pool_options> shards;
for (const std::string& dsn : config.shard_dsns) {   // 配置里的顺序就是分片编号，新分片只能加在末尾
    pg::pool_options o;
    o.dsn = dsn;
    shards.push_back(o);
}
pg::sharded<pg::pool> pools(shards);
pg::sharded<pg::writeback> wbs(pools.size(), [&](std::size_t i) {
    return std::make_unique<pg::writeback>(pools[i]);
});

pg::conn c(pools.of(user_id));               // 这个用户的数据所在的库
pg::cached<db::users::balance> b(wbs.of(user_id), user_id);
b -= cost;
```

两个分片集合的大小相同时，同一个 key 选到的下标相同，所以 `pools.of(id)` 和 `wbs.of(id)` 指向同一个库。
一个事务、一笔写回只能落在同一个分片上，不支持跨分片。

副本很多时，在库前面放一个 transaction 模式的 PgBouncer 或 PgCat，用来收拢连接：

```cpp
pg::pool_options o;
o.dsn = "postgresql://app@pgbouncer:6432/app";
o.pooler = true;                             // 不发送启动参数；statement_timeout 等设置在角色上：
                                             // ALTER ROLE app SET statement_timeout = '30s';
                                             // ALTER ROLE app SET synchronous_commit = off;
pg::pool p(o);

pg::writeback_options w;
w.direct_dsn = "postgresql://app@db-0:5432/app";   // 写入线程和 LISTEN 需要直连服务器
pg::writeback wb(p, w);
```

- PgBouncer 要设置 `max_prepared_statements`（例如 200），这样预编译语句才能在池化器后面使用。
- 迁移要用直连的池执行，因为它依赖 advisory lock。迁移完成后在 PgBouncer 上执行 `RECONNECT`。
- 经过池化器时，`SET` 和 advisory lock 这类会话状态只在一个事务内有效。
- 就绪检查：`pool::ready()`。它不会在繁忙的池上排队，可以直接接到 `/readyz`。
- 停机：收到 SIGTERM 后析构 `writeback`，它会在 `shutdown_timeout`（默认 10 s）内把积压写完。
  k8s 的 `terminationGracePeriodSeconds` 要比这个时长更长。
- 服务端连接数的核算：
  - 经过池化器：池化器的服务端连接数 + 副本数 × 分片数 × 2（writeback 直连）；
  - 直连：副本数 × 分片数 × `max_conns`。
