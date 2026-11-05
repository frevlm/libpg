# 查询文件里的 SQL 一个字节都不改：参数名写在文件头，动态片段是注释形式的洞

查询文件用 `-- name:` / `-- params:` 注释头给出函数名、注解和参数名，SQL 正文保持原样，拿到 psql 里照样能跑。
动态部分写成注释 `/*@name*/`，在生成的函数里变成一个语句参数。

理由：不需要自己写词法扫描；`@name` 写法会撞上 JSONB 的 `@>`、`<@`、`?` 运算符。洞是注释，PREPARE 和 Describe
时自然忽略，结果的列形状不受影响，所以动态 WHERE 的查询也能拿到生成的行类型——revlm 的 `RequestStore::query`
和 HHHOJ 的提交列表正需要这个，而 sqlc 讨论多年（#364、#2021、#2060）都没有落地。

## Considered Options

- `@name` 加词法扫描（sqlc 风格）：需要改写 SQL，并与 JSONB 运算符冲突。
