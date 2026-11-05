# libpg

PostgreSQL 的 C++20 客户端库，直接基于 libpq：连接池、参数化 SQL、带类型的结果、事务，
以及把数据库格子当变量用的 `pg::sql<Col>` / `pg::cached<Col>`。设计见 [docs/design.md](docs/design.md)。

## 依赖

- CMake ≥ 3.25，C++20 编译器，标准库要支持浮点 `std::from_chars`（GCC ≥ 11；macOS 需 Xcode ≥ 26 或 LLVM ≥ 20）
- libpq ≥ 14（≥ 17 时使用新的 pipeline 接口）
- 测试：Catch2 3、PostgreSQL 服务端程序（`initdb`/`pg_ctl`）

## 构建与测试

```sh
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```

`pg_ctl` 不在 PATH 时用 `-DLIBPG_PG_CTL=/path/to/pg_ctl` 指定。

## 在项目里使用

```cmake
include(FetchContent)
FetchContent_Declare(libpg
  URL https://github.com/frevlm/libpg/releases/download/v0.1.1/libpg-0.1.1.tar.gz)
FetchContent_MakeAvailable(libpg)
target_link_libraries(app PRIVATE pg)
```

作为子项目引入时默认不构建测试。

## 许可证

MIT
