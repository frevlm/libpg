# 一个错误分类同时决定池、闭包事务和写回缓存的行为

每个错误都落在一个 `error_kind` 里：`broken`（COMMIT 之外断连）、`commit_unknown`（COMMIT 期间断连）、
`conflict`（40001/40P01）、`stale_statement`（26000，以及缓存计划结果类型变了的 0A000）、`read_only`（25006）、
`lock_or_cancel`（55P03/57014）、`connect_failed`、`data`（22xxx/23xxx）、`other`。`retryable(kind)` 是唯一的重试判据：
`pool::transact` 对 broken、conflict、stale_statement、read_only 重试，写回缓存对同一批做同样的重放或拆分判断。
没有第二套分类。

理由：同一个 SQLSTATE 在不同地方被不同对待，正是生产事故的来源。错误的 `what()` 只含 SQLSTATE 和主消息，
SQL 文本单独放在 `error::sql`，使用方把 `what()` 返回给客户端时不会泄漏 SQL。
`commit_unknown` 永不重试：事务可能已经提交，重跑闭包会重复执行；需要幂等的写入用写回缓存（0012）。
