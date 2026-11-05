# 所有语句走同一个 pipeline 模式、非阻塞的引擎

连接建立后进入 libpq 的 pipeline 模式并设为非阻塞，此后所有语句——单条查询、事务、格子操作、写回批次——
都走同一条路径：把需要的协议消息一次排好，末尾一个 Sync，在一个 poll 循环里边发边收直到读到 Sync。
不再调用任何同步的 libpq 函数，因此要求 libpq ≥ 14。

理由：开事务的 BEGIN 可以和第一条语句同批发出，缓存未命中的 Prepare 和 Execute 也能同批，写回一批只要一次往返；
边发边收避免了阻塞模式下“发送缓冲区满、服务端等客户端读”的互锁；非阻塞 IO 让每次操作都能有截止时间。

## Consequences

- 使用方拿到的原始 `PGconn*` 处在 pipeline 模式，不能在上面调用 PQexec 这类同步函数。
- 多语句脚本（迁移）只能临时退出 pipeline 用 simple 协议执行，执行过的连接不再回池。
- libpq 17 起有 `PQsendPipelineSync` 和 `PQsendClosePrepared`，14–16 要退回 `PQpipelineSync` 和 `DEALLOCATE`；
  CMake 选项 `LIBPG_FORCE_LIBPQ16_PATH` 在新版 libpq 上强制走旧路径，用来测试它。
