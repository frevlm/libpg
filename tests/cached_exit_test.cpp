#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <string>

#include "pg/pg.hpp"

namespace db::exit_cells {
struct table_ {
    static constexpr std::string_view name = "exit_cells";
    using key = std::tuple<std::int32_t>;
    static constexpr std::array<std::string_view, 1> key_columns{"id"};
};
struct n {
    using table = table_;
    using type = std::int64_t;
    static constexpr std::string_view name = "n";
};
}  // namespace db::exit_cells

namespace {

// Child process: writes a cell without calling pg::shutdown(), then exits normally.
[[noreturn]] void child_writes_and_exits() {
    pg::connect(std::getenv("LIBPG_TEST_DSN"), 2);
    pg::conn c;
    c.exec("DROP TABLE IF EXISTS exit_cells");
    c.exec("CREATE TABLE exit_cells (id int PRIMARY KEY, n bigint NOT NULL)");
    c.exec("INSERT INTO exit_cells VALUES (1, 70)");
    pg::configure_cache({std::chrono::hours(1), nullptr});
    pg::cached<db::exit_cells::n> x{1};
    x = 99;
    std::exit(0);
}

}  // namespace

TEST_CASE("exiting with dirty cells prints one line and writes nothing") {
    REQUIRE(std::getenv("LIBPG_TEST_DSN") != nullptr);
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    const pid_t pid = fork();  // before any connection or thread exists in this process
    REQUIRE(pid >= 0);
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        child_writes_and_exits();
    }
    close(fds[1]);
    std::string err;
    char buf[256];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) err.append(buf, static_cast<std::size_t>(n));
    close(fds[0]);
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    const std::string line = "libpg: 1 cached cells were not written back; call pg::shutdown() before exit\n";
    REQUIRE(err.find(line) != std::string::npos);
    REQUIRE(err.find(line) == err.rfind(line));

    pg::connect(std::getenv("LIBPG_TEST_DSN"), 2);
    pg::conn c;
    REQUIRE(c.query_one<std::int64_t>("SELECT n FROM exit_cells WHERE id = 1").value() == 70);
}
