# 线路面的结果只用二进制格式；字符串参数仍用文本

结果一律用二进制格式，不保留文本这条路（libpq 不支持按列指定结果格式）。参数格式逐个指定：
`std::string` / `string_view` 用文本并让服务端推断类型，其他类型用二进制并显式声明 OID。

理由：解码不再依赖任何会话参数。`timestamptz` 就是 int64 微秒，`infinity` 是它的两个极值，v1 里解析 ISO 文本、
BC 年份、超过 4 位年份以及依赖 `DateStyle` 的代码全部删掉；`double` 往返精确，不再需要 `extra_float_digits`；
`numeric` 的 base-10000 数字组直接换算成定点小数，不经过字符串。字符串参数保持文本，所以 jsonb、citext、enum、
`COLLATE ci` 参数的行为和现在一模一样。

## Consequences

- v1 是全文本格式；这是一处有意的推翻。
- 没有内置 codec 的结果类型（例如 `inet`）在 Describe 时就报错，提示在 SQL 里写 `::text` 或提供 codec；
  生成器遇到这类列也报错，不再像 v1 那样静默生成 `std::string`。
- 自定义 codec 必须提供二进制的编码、解码和类型检查。
