# 连接是唯一的执行器；嵌套借用立即抛异常

用户面只有一种执行器：借出的连接（`pg::conn&`）。不做模板 concept，不做虚基类，池也不能直接执行语句
（`pool::transact` 只是借连接、开事务、调用闭包）；在不在事务里是连接自己的状态。同一线程对同一个池嵌套借用时立即抛 `nested_borrow`。

理由：使用方大量 `f(Db&, ...)` 非模板函数的签名可以原样保留。helper 随手传池、再借连接会让同一线程嵌套借用，
池满时死锁；禁止嵌套借用后，HikariCP 的死锁公式 `Tn × (Cm − 1) + 1` 里 Cm = 1，任何池大小都不会死锁。
tokio-postgres 为了统一 Client 和 Transaction 只好另加 GenericClient trait，sqlx 维护者建议直接收 `&mut PgConnection`，
sqlpp11 本身也把事务当作连接上的状态。
