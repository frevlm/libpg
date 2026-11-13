# 线路面的参数和结果都只用二进制格式

每个参数以二进制发出并显式声明 OID，每个结果列以二进制读回，没有文本这条路。具体的类型对应见 0027。

理由：解码不再依赖任何会话参数。`timestamptz` 就是 int64 微秒，`infinity` 是它的两个极值，不需要解析 ISO 文本、
BC 年份或依赖 `DateStyle`；`double` 往返精确，不需要 `extra_float_digits`；`numeric` 的 base-10000 数字组直接换算成
定点小数 `pg::decimal<S>`，不经过字符串。字符串参数的 OID 是 text，所以服务端会拒绝要求别的类型的位置（见 0027）。

## Consequences

- 没有 codec 的类型（例如 `inet`、枚举、json、数组结果）在 Describe 时就报错，提示在 SQL 里写 `::text`；
  生成器遇到这类列也报错并指出表、列和类型。
- 一个 codec 提供四样东西：参数的 OID、二进制编码、能解码的结果列 OID 集合、二进制解码。
