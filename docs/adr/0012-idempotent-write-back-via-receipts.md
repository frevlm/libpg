# 写回批次靠回执表幂等重放；不用 pg_xact_status 判定提交结果

每个写回批次分配一个 id，在同一个隐式事务里先 `INSERT INTO libpg.flush_receipts(id)`（故意不写 ON CONFLICT），
再插入行、累加增量，末尾一个 Sync 提交，一次往返。结果未知或可重试时原样重放同一批；回执上的 23505 就表示
这一批之前已经提交过，按已提交结算。闭包事务遇到 COMMIT 期间断连则抛提交结果未知，永不重试。

理由：v1 在 COMMIT 前取 `pg_current_xact_id()`，断连后查 `pg_xact_status`。主库切换后新主库上的 xid 可能还没分配，
也可能已经属于别的事务，查出来的结果不可靠；而“只重放 set、不重放 delta”等于丢账。有了回执，“未知”这个状态不复存在。

## Considered Options

- 改用 `pg_current_xact_id_if_assigned()`：同样受主库切换影响。
- 查 xid，查不出就撤回再对齐：仍会在切换后误判。

## Consequences

- 行用 `unnest` 数组插入，增量先按键求和、排序再 `UPDATE … FROM unnest`：参数个数与行数无关，语句能命中缓存；
  同一键在 `UPDATE … FROM` 里出现两次时 PG 只用其中一行；固定加锁顺序减少跨副本死锁。
- 不用 COPY：COPY 不能放进 pipeline，也不能 RETURNING。
- 22xxx/23xxx 错误时换成显式事务、每笔一个保存点重做，找出出错的那一笔撤回。
- 写回线程定期清理一天以前的回执，时间一律用数据库的 `now()`。
