# 类型严格对齐：参数二进制加显式 OID，结果列按 OID 和列名精确核对，没有回退

一个 C++ 类型对应固定的 PostgreSQL 类型 OID，两个方向都不做宽容：

- **参数**：每个参数都以二进制发出，并显式声明它的 OID（`codec<T>::type`），从不用 OID 0、不用文本格式。
  类型对不上由服务端自己的类型检查在 Prepare 时拒绝：给 int8 列传 `int32_t` 的比较、给 text 位置传别的类型，都直接报错。
- **结果**：语句执行前，用 Describe 得到的列 OID 检查 `codec<T>::accepts`，读形状还要逐列检查列名与 `pg_names` 完全一致、
  列数相等。任何不一致都抛 `pg::error`，错误里写明第几列、列名、服务端类型 OID 和期望的 C++ 类型。
  `std::string` 接受 text、varchar、bpchar、name；其余类型只接受自己的 OID（`decimal<S>` 是 numeric，`jsonb` 是 jsonb，
  `timestamptz`、`date`、`uuid`、`bytea`、`bool`、`int16/32/64`、`float/double` 同理）。
- **NULL**：结果里的 NULL 读进非 `std::optional` 的成员时抛错。
- **没有回退**：没有“先试二进制再退文本”，没有 pg_type 查询来放宽一个类型接受什么，没有隐式的整数拓宽，
  数组只作为参数（元素类型有 codec 时对应固定的数组 OID），不能读。
- `insert_all` 的目标列同样先 Describe、再按 `accepts` 核对（0023）；写回缓存的计数器走同样的参数规则。

理由：库里每多一条宽容的转换路径，类型不对齐的错误就从“Prepare 时的明确报错”变成“生产数据里悄悄读偏”的隐患，
而这条路径还要有自己的测试。类型不对时在第一次执行前就失败，成本只是写代码时多加一个 `::int8` 或换一个 C++ 类型。
不对齐的错误全部发生在开发期：语句第一次执行就会暴露，不依赖数据。

## Consequences

- 表达式的结果类型要和读它的 C++ 类型对上：`count(*)` 是 `int64_t`，`sum(bigint)` 是 numeric，要读成 `decimal<S>`，
  不对就在 SQL 里写类型转换。
- 没有 codec 的类型（`inet`、枚举、json、数组结果）既不能读也不能写，需要 `::text` 之类的转换；
  `pg_tablegen` 遇到这类列或结果列（以及不带 (p, s) 或精度超过 18 的 numeric）直接报错，指出表、列和类型，不生成回退的 `std::string`。
- 换列类型的迁移会让旧的生成代码在第一次执行时报错，而不是继续跑出旧类型的数据；`--check` 在 CI 里提前发现。
