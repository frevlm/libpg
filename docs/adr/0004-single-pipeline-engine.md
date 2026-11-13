# 所有语句走同一条 pipeline 路径，事务外的语句共用线路连接

每条线路连接建立后进入 libpq 的 pipeline 模式并设为非阻塞，此后所有语句——单条查询、事务、`insert`、写回批次——
都走同一条路径：排成一个 ticket（`PQsendQueryPrepared`，参数和结果都用二进制），末尾一个 `PQpipelineSync`，
读取时按队列顺序逐个填 ticket。`query`/`exec` 是“排队后立刻读”，`send` 是“排队、稍后读”，没有第二条路径。
要求 libpq ≥ 14。

理由：开事务的 BEGIN 可以和连接的下一条语句同批发出，写回一批只要一次往返；唯一的读循环（每条线路连接同一时刻
只有一个线程在 poll，其余等条件变量）避免了阻塞模式下“发送缓冲区满、服务端等客户端读”的互锁。

## Consequences

- 事务外的语句共用线路连接（0020）。
- `conn::raw()` 返回的 `PGconn*` 已读完所有在途语句、退出 pipeline 模式并恢复阻塞，可以直接调用 PQexec；
  这条线路连接归这个 `conn` 独占，`conn` 结束时关闭，不再回池。迁移用它执行多语句脚本。
- `insert_all` 的 COPY 同样先读完在途语句、退出 pipeline 模式，结束后下一条语句再把连接放回非阻塞的 pipeline 模式。
