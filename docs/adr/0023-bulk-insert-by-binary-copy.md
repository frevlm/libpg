# 批量插入走二进制 COPY

revlm 的计费记录由 BatchWriter 每 200 ms 写一批，每条记录一条 INSERT。部署机（2 vCPU，和 PostgreSQL 共用）上，
一行 34 列、4 个索引的数据按四种方式写，服务端 CPU 每行分别是：逐行文本 INSERT 160 µs，预编译加 pipeline 34 µs，
多行 VALUES 45 µs，COPY 13 µs。按 2 万行每秒算，逐行 INSERT 要 3.2 个 CPU，这台机器只有 2 个，
revlm 的性能文档里记录的计费记录丢弃就是这么来的；改用 COPY 只要 0.26 个 CPU。

决定：

- `pg::insert_all(conn&, rows)` 接收任意写形状（write shape）的连续区间，用一条二进制 COPY 写完所有行。
- 每列的目标类型（domain 换成基类型）按表和列清单查一次 pg_attribute 和 pg_type，缓存在池里。二进制编码的列要求
  codec 的 OID 和目标类型一致；文本编码的列只能写字符串类、枚举、json 和 jsonb（jsonb 前面补版本字节）。
  有一列不满足，整批改走 pipeline 的逐行 INSERT。
- 所有行都没设置的 pg::field 列不出现在 COPY 里，用它的 DEFAULT；各行设置情况不一致时走逐行 INSERT。
- 调用方不在事务里时，自己开一个事务，保证要么全部写入，要么一行都不写；在调用方的事务里，COPY 失败就直接抛出，
  不回退（事务已经中止）。
- libpq 在 pipeline 模式下不允许 COPY：先等完在途的往返，退出 pipeline 模式执行 COPY，再回到 pipeline 模式。

## Consequences

部署机实测，每批 200 行：单线程从逐行 INSERT 的 6.7k 行每秒提到 56k，4 个线程时到 65k；每行的总 CPU
（服务端加客户端）从 150 µs 降到 18 µs。

- COPY 期间这条线路连接被独占，不能和别的语句共用。
- 第一次写某个表的某组列时要多一次查询类型的往返。
- 写回线程用同样的编码规则：一批里不少于 64 行、每列都能用二进制写的插入分组走 COPY。这时一批不再是一次往返的隐式事务，
  而是 BEGIN 加回执、每组一条 COPY、其余语句加 COMMIT，多两到三次往返；回执照旧保证重放不重复。出错后隔离坏笔的重做仍走
  unnest。部署机上 revlm 的写入形态（一笔一行 34 列加一次扣余额，每批 200 笔），每笔服务端 CPU 从 26 µs 降到 22 µs。
