# 默认异步提交

在部署机上实测，一笔事务约 1 ms，几乎全花在 COMMIT 等 WAL 落盘（fdatasync）上。行锁在这段时间里一直被持有，
所以热点行的串行吞吐上限大约是每秒 1000 笔，加连接也没用。

决定：`pool_options::synchronous_commit` 默认为 false。建连时在 startup options 里带上
`-c synchronous_commit=off`，不多花往返；它也是会话的 RESET 值，池回收会话时执行 RESET ALL 后依然生效。
需要持久化保证的池可以设为 true。

## Consequences

热点行事务（SELECT ... FOR UPDATE 加两条 INSERT），单线程从约 1.0k 提到 2.9k 笔每秒，配合先发后取到 3.8k；
4 个线程时从 2.8k 提到 6.4k。

- 服务端崩溃时，最近约 0.6 秒（wal_writer_delay 的 3 倍）内已经提交的事务可能丢失。数据不会损坏，也不会出现半个事务。
- 进程或连接断开不受影响，只有服务端机器或 postgres 进程崩溃才会丢。
