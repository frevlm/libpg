# 线路面默认按语句缓存 prepared statement，不支持 PgBouncer 事务池模式

每个连接有一张以 SQL 文本为键的 LRU 语句缓存（上限 256），用户面不暴露开关。只支持直连和 PgBouncer 会话池模式。

理由：只有先 Parse 加 Describe，才能在执行之前用 codec 核对结果列的类型，类型不对时语句根本不执行；
二进制格式（0006）也依赖 Describe 拿到的 OID。PgBouncer 事务池模式下启动参数、会话状态、prepared statement、
LISTEN 都不可靠，两个使用方也都不用它，明确排除之后线路面才能放心使用这些能力。

## Considered Options

- 默认关闭 prepared statement：放弃执行前的类型检查和二进制格式。
- 完全不用 prepared statement：同上，并且每次都要重新规划。

## Consequences

- 表结构变化导致 `0A000`（缓存的计划不能改变结果类型）或语句被外部 `DEALLOCATE` 导致 `26000` 时，
  引擎丢掉那条缓存；事务外重新 Parse 后重试一次，事务内交给闭包事务重试。
- 动态拼出的 SQL 形状很多时会被 LRU 淘汰，被淘汰语句的 Close 跟着下一次 Sync 发出，不额外往返。
