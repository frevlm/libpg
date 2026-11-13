# 迁移在一个事务里执行，记账放进 libpg 自己的 schema，校验和由服务端算

所有待执行迁移在一个事务里全部成功或全部回滚。`pg::migrate` 在 `conn::raw()` 的阻塞连接上执行：`BEGIN`，
`pg_advisory_xact_lock(0x6c69627067)`（"libpg"），建 `libpg` schema 和 `libpg.migrations (name, sha256, applied_at)`，
读出已应用的记录，逐个检查，执行待执行的脚本（`PQexec`，多语句），记录，`COMMIT`。已应用的迁移校验和变了就抛错；
任何失败整体回滚。迁移脚本不能自己结束事务。

校验和是规范化之后文本的 SHA-256：CRLF 转 LF、去行尾空白，由服务端算
（`SELECT encode(sha256(convert_to($1, 'UTF8')), 'hex')`），库里不带 SHA-256 实现。规范化让 Git 换行转换不会误报，
真正修改了已应用的迁移才会报错。

理由：两个使用方现在就是一事务语义，保持不变；记账放进 libpg 独占的 `libpg` schema（写回回执也在这里），不借用使用方的表。
advisory lock 让多个副本同时迁移时串行。

## Considered Options

- 沿用使用方现有的记账表：回执没有归属，而且要改使用方的表。
- 会话锁加 checksum、每个迁移一个事务：破坏一事务语义。

## Consequences

- 迁移只有一个 key：名字。使用方原有按版本号记账的表不导入，需要时自己把记录写进 `libpg.migrations`。
- `ALTER TABLE` 拿锁排队会让这张表的读写都排在它后面；对此用 DSN 里的 `options=-c lock_timeout=...` 限制，
  由使用方决定是否重跑。
