# 每条线路连接缓存 prepared statement；不支持事务池模式的池化器

每条线路连接有一张以“SQL 文本加参数类型 OID”为键的语句缓存，用户面不暴露开关。缓存达到 1000 条时整张清空，
并排一条 `DEALLOCATE ALL`。只支持直连和 PgBouncer 会话池模式。

理由：只有先 Prepare 加 Describe，才能在执行之前核对结果列的类型和列名（0027），类型不对时语句根本不执行；
命中缓存时检查也不花往返。事务池模式下启动参数、会话状态和 prepared statement 都不可靠，两个使用方也都不用它，
明确排除之后线路面才能放心使用这些能力。

## Considered Options

- 默认关闭 prepared statement：放弃执行前的类型检查。
- 完全不用 prepared statement：同上，并且每次都要重新规划。

## Consequences

- 表结构变化导致 `0A000`（缓存的计划不能改变结果类型）或语句被外部 `DEALLOCATE` 导致 `26000` 时，
  这两类错误都是 `stale_statement`：先丢掉那条缓存再抛出，由 `pool::transact` 重试。
- 一条缓存里的描述没通过检查时，先重新 Describe 一次，仍然不对才抛错，所以改过列类型的表不会因为旧缓存而误报。
