# 直接基于 libpq，写 SQL 文本，不做查询 DSL、ORM 或反射

libpg 用来取代 revlm、HHHOJ 现在用的 sqlpp11。HHHOJ 已经在至少 18 处用 verbatim 绕开 sqlpp11 的 DSL，
所以集合查询（列表、排行、JOIN）直接写 SQL 文本，类型安全由从 schema 和查询文件生成的代码提供，
而不是由 DSL 提供。反射被使用方否决，生成的代码是普通 C++，不依赖反射。

## Considered Options

- 保留查询 DSL：表达力永远落后于 SQL，使用方会继续绕开它。
- ORM：两个使用方都没有对象图需求，而且会把每次往返藏起来。
