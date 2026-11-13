# libpg

PostgreSQL 的 C++20 客户端库，直接基于 libpq：连接池、参数化 SQL、严格对齐的二进制类型、事务、
`conn::send` 先发后取、写回缓存 `pg::writeback`、批量插入 `pg::insert_all`、迁移 `pg::migrate`，
以及从 schema 和查询文件生成类型的 `pg_tablegen`。
怎么用见 [docs/usage.md](docs/usage.md)，术语见 [CONTEXT.md](CONTEXT.md)，设计决定见 [docs/adr/](docs/adr/)。

## 依赖

- CMake ≥ 3.25，C++20 编译器
- libpq ≥ 14（pipeline 模式）
- 测试：Catch2 3、PostgreSQL 服务端程序（`initdb`/`pg_ctl`）

## 构建与测试

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

`pg_ctl` 不在 PATH 时用 `-DLIBPG_PG_CTL=/path/to/pg_ctl` 指定。给出 `-DLIBPG_REVLM_SCHEMA=...` / `-DLIBPG_HHHOJ_SCHEMA=...`（schema.sql 路径）时，额外测试 `pg_tablegen` 对这两个项目的输出能编译。

## 在项目里使用

```cmake
include(FetchContent)
FetchContent_Declare(libpg
  URL https://github.com/frevlm/libpg/releases/download/v0.3.0/libpg-0.3.0.tar.gz)
FetchContent_MakeAvailable(libpg)
target_link_libraries(app PRIVATE pg)
```

链接 `pg` 得到核心；写回缓存在单独的库里，需要时再链接 `pg_writeback` 并 `#include <pg/writeback.hpp>`。
作为子项目引入时默认不构建测试。

```cpp
#include <pg/pg.hpp>

pg::pool p({.dsn = "postgresql://app@db/app"});
pg::conn c(p);
auto name = c.query_opt<std::string>(pg::sql{"SELECT username FROM users WHERE id = {}", 7});
```

## 许可证

MIT
