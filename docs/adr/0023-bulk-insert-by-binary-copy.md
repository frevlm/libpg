# 批量插入走二进制 COPY，目标列的类型先用 Describe 核对

revlm 的计费记录一行 34 列、4 个索引。在 2 vCPU 的部署机上（和 PostgreSQL 共用），服务端 CPU 每行：逐行文本 INSERT 160 µs，
预编译加 pipeline 34 µs，多行 VALUES 45 µs，COPY 13 µs。按 2 万行每秒算，逐行 INSERT 要 3.2 个 CPU，
COPY 只要 0.26 个。

决定：

- `pg::insert_all(conn&, rows)` 接收任意写形状的连续区间，用一条二进制 COPY 写完所有行，一条语句自己就是原子的，
  不另开事务；空区间返回 0，不发往返。
- 这是唯一的批量路径，没有逐行 INSERT 的回退。写入的列是成员的列：`pg::field` 列在第一行设置了就写，
  之后任何一行的设置情况与第一行不同则抛错。
- 发数据之前，先用语句缓存对 `SELECT "a", "b" FROM "t"` 做 Describe，读出目标列的 OID，逐列检查成员类型的 codec 是否接受它
  （0027）；不一致就先重新 Describe 一次，仍不一致，在发送任何行之前抛错。domain 列按 Describe 报告的基类型比较。
- COPY 前先读完 `conn` 上在途的语句、让出 pipeline 模式，`PQputCopyData` 每 1 MiB 一块；结束后下一条语句再回到 pipeline。
  调用方在事务里时，COPY 失败就直接抛出，事务已经中止，只能回滚。

## Consequences

每批 200 行：单线程从逐行 INSERT 的 6.7k 行每秒提到 56k；每行的总 CPU（服务端加客户端）从 150 µs 降到 18 µs。

- COPY 期间这条线路连接被 `conn` 独占。
- 第一次写某个表的某组列时多一个 Describe 往返。
