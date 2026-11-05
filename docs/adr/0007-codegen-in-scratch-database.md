# 代码生成让 PostgreSQL 在临时库里解释 schema，不写 SQL 解析器

生成器连上服务器，`CREATE DATABASE … TEMPLATE template0` 建一个临时库，在里面执行使用方的 schema.sql 和查询文件，
从 `pg_catalog`、Describe 和 `EXPLAIN` 读出表、列、类型、约束和结果形状，最后无论成败都 `DROP DATABASE … WITH (FORCE)`。

理由：SQL 的语义只有 PG 自己说得准，domain、COLLATE、`SET search_path` 这些写法换成自写解析器都会出错。
代价是生成时需要一个有 CREATEDB 权限的服务器；生成的文件提交进仓库，平常构建不需要数据库，只有 CI 的漂移检查需要。
