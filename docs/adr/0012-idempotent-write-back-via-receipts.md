# 写回批次靠回执表幂等重放；不用 pg_xact_status 判定提交结果

每个写回批次分配一个 uuid，在同一个事务里先 `INSERT INTO libpg.flush_receipts(id)`（故意不写 ON CONFLICT），
再插入行、累加增量，然后 COMMIT；整批通过 pipeline 一次发出。结果未知或可重试时原样重放同一批，回执上的 23505
就表示这一批之前已经提交过，按已提交结算。`libpg.flush_receipts` 由写回缓存在构造时自己建（`CREATE ... IF NOT EXISTS`，
在 advisory lock 下）。

理由：COMMIT 前取 `pg_current_xact_id()`、断连后查 `pg_xact_status`，在主库切换后不可靠——新主库上的 xid 可能还没分配，
也可能已经属于别的事务；而“只重放 set、不重放 delta”等于丢账。有了回执，“未知”这个状态不复存在。

## Considered Options

- 改用 `pg_current_xact_id_if_assigned()`：同样受主库切换影响。
- 查 xid，查不出就撤回再对齐：仍会在切换后误判。

## Consequences

- 行按（表，列集合，各列参数类型）分组，用 `INSERT ... SELECT FROM unnest(...)` 插入；增量先按键求和，再每个计数器
  一条 `UPDATE ... FROM unnest(...)`，受影响行数必须等于键数，否则（行不存在）这一笔按数据错误处理。参数个数与行数无关，语句能命中缓存。
- 一个事务最多 10000 笔，多出的留在队列里下一轮写。
- 失败的处置只看错误种类（0015）：断线、COMMIT 结果未知、超时、冲突这类暂时性错误原样重放同一批
  （重放 3 次锁冲突仍不成功的批，改成对半拆开写，期间不阻塞读取）；数据错误和带 SQLSTATE 的其他错误把批对半拆开，
  递归到单笔，仍失败的那一笔交给 `on_lost`，它的增量在内存里撤回。
- 在 COMMIT 结果未知时遇到关停，用回执做一次最后的判断：回执在就按已提交结算，否则这一批以 `commit_unknown` 交给 `on_lost`。
- 回执表不自动清理；使用方按 `created_at` 自行清理。
