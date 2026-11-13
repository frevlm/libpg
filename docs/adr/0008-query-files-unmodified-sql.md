# 查询文件保持 psql 写法：动态片段是注释形式的洞

查询文件用 `-- name:` 注释头给出函数名和注解，SQL 正文保持 psql 写法（参数写法见 0026），拿到 psql 里照样能跑。
动态部分写成注释 `/*@name*/`，在生成的函数里变成一个语句参数。

理由：洞写成 `/*@name*/` 而不是 `@name`，是因为后者会撞上 JSONB 的 `@>`、`<@`、`?` 运算符。洞是注释，Prepare 和 Describe
时自然忽略，结果的列形状不受影响，所以动态 WHERE 的查询也能拿到生成的行类型——revlm 的 `RequestStore::query`
和 HHHOJ 的提交列表正需要这个，而 sqlc 讨论多年（#364、#2021、#2060）都没有落地。

## Considered Options

- `@name` 加词法扫描（sqlc 风格）：与 JSONB 运算符冲突。
