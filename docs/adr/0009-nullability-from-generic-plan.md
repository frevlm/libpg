# 结果列的可空性用 GENERIC_PLAN 推断，三条同时满足才算非空

一个结果列被推断为非空，必须同时满足：能经 `PQftable`/`PQftablecol` 定位到一个 `attnotnull` 的表列；
在 `EXPLAIN (VERBOSE, FORMAT JSON, GENERIC_PLAN)` 的计划里这张表不出现在任何外连接的可空侧（自连接保守按可空）；
不是 PG 18 `RETURNING old.*` / `new.*` 的列。其余一律可空。列别名后缀 `!` / `?` 可以强制，和 sqlx、PgTyped 一致。

不用 sqlx 那种“参数全填 NULL 再 EXPLAIN”：`WHERE x = NULL` 会被常量折叠，外连接节点可能整个从计划里消失，
推断结果悄悄出错。推断错了的代价只是运行期抛解码错误，不会读出脏值。生成器因此要求 PG ≥ 16，只影响 CI。
