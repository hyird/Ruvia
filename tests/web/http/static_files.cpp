#include "ruvia/web/static_files.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "context_request_fixture.h"
#include "test_harness.h"

namespace static_files_test {

namespace fs = std::filesystem;

// A throwaway document root mixing a normal asset with hidden files and a
// hidden directory (the .git checkout an operator might accidentally deploy).
fs::path make_dotfile_root() {
    const auto dir = fs::temp_directory_path() / "ruvia_static_dotfiles_test";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir / ".git");
    const auto write = [](const fs::path& path, std::string_view body) {
        std::ofstream file(path);
        file << body;
    };
    write(dir / "app.js", "console.log(1)");
    write(dir / ".env", "SECRET=1");
    write(dir / ".htpasswd", "user:hash");
    write(dir / ".backup.json", "{}");
    write(dir / ".git" / "config", "[core]");
    return dir;
}

[[nodiscard]] bool served(const ruvia::static_root& root, std::string_view path) {
    bool found = false;
    static_cast<void>(context_request_test::with_context(
        ruvia::test_request::get("/"),
        [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                found =
                    ctx.static_file(root, {.relative_path_ = path}).status() == ruvia::http_status::ok;
            } catch (const ruvia::http_error&) {
                found = false;
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    return found;
}

struct served_body final {
    ruvia::http_status_code status_{ruvia::http_status::internal_server_error};
    bool file_backed_{false};
    std::string bytes_;
};

// Serves `path` and reports whether the response names a file for the
// transport or carries its bytes in memory.
[[nodiscard]] served_body serve_body(const ruvia::static_root& root, std::string_view path) {
    served_body result;
    static_cast<void>(context_request_test::with_context(
        ruvia::test_request::get("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                const auto response = ctx.static_file(root, {.relative_path_ = path});
                result.status_ = response.status();
                result.file_backed_ = response.file_body().has_value();
                result.bytes_.assign(response.body_bytes());
            } catch (const ruvia::http_error& error) {
                result.status_ = error.info().status();
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    return result;
}

fs::path make_resident_root() {
    const auto dir = fs::temp_directory_path() / "ruvia_static_resident_test";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir / "nested");
    std::ofstream(dir / "small.txt", std::ios::binary) << "small-v1";
    std::ofstream(dir / "nested" / "page.txt", std::ios::binary) << "nested-v1";
    std::ofstream(dir / "large.txt", std::ios::binary) << std::string(64, 'L');
    return dir;
}

}  // namespace static_files_test

using static_files_test::make_dotfile_root;
using static_files_test::make_resident_root;
using static_files_test::serve_body;
using static_files_test::served;
namespace fs = std::filesystem;

RUVIA_TEST(static_root_hides_dotfiles_even_under_all_policy) {
    const auto dir = make_dotfile_root();
    ruvia::static_root_options options;
    // The hidden-path default-deny must still keep secrets out of responses.
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    ruvia::static_root root(dir, std::move(options));

    RUVIA_CHECK(served(root, "app.js"));
    RUVIA_CHECK(!served(root, ".env"));
    RUVIA_CHECK(!served(root, ".htpasswd"));
    RUVIA_CHECK(!served(root, ".backup.json"));
    RUVIA_CHECK(!served(root, ".git/config"));

    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

RUVIA_TEST(static_root_serves_dotfiles_when_opted_in) {
    const auto dir = make_dotfile_root();
    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
    options.dotfiles_ = ruvia::static_dotfile_policy::serve;
    ruvia::static_root root(dir, std::move(options));

    RUVIA_CHECK(served(root, ".env"));
    RUVIA_CHECK(served(root, ".git/config"));
    RUVIA_CHECK(served(root, "app.js"));

    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

RUVIA_TEST(static_root_without_resident_opt_in_stays_file_backed_and_fails_closed) {
    const auto dir = make_resident_root();
    ruvia::static_root root(dir, {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::all}});

    const auto before = serve_body(root, "nested/page.txt");
    RUVIA_CHECK_EQ(before.status_, ruvia::http_status::ok);
    RUVIA_CHECK(before.file_backed_);

    std::ofstream(dir / "nested" / "page.txt", std::ios::binary | std::ios::trunc) << "nested-v2-longer";
    // A HEAD has no body for the transport to check, so the identity check
    // runs while framing.
    bool head_failed = false;
    static_cast<void>(context_request_test::with_context(
        ruvia::test_request::head("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                static_cast<void>(ctx.static_file(root, {.relative_path_ = "nested/page.txt"}));
            } catch (const ruvia::http_error& error) {
                head_failed = error.info().status() == ruvia::http_status::internal_server_error;
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    RUVIA_CHECK(head_failed);

    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

RUVIA_TEST(static_root_resident_opt_in_serves_indexed_bytes_from_memory) {
    const auto dir = make_resident_root();
    ruvia::static_root root(dir,
        {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::all},
            .resident_max_bytes_ = 32,
            .resident_budget_bytes_ = 1024});

    // Rewrite and delete after indexing: resident files keep serving the
    // indexed bytes; the file over resident_max_bytes_ stays on disk.
    std::ofstream(dir / "small.txt", std::ios::binary | std::ios::trunc) << "small-v2-longer";
    fs::remove(dir / "nested" / "page.txt");

    const auto small = serve_body(root, "small.txt");
    RUVIA_CHECK_EQ(small.status_, ruvia::http_status::ok);
    RUVIA_CHECK(!small.file_backed_);
    RUVIA_CHECK_EQ(small.bytes_, std::string("small-v1"));

    const auto nested = serve_body(root, "nested/page.txt");
    RUVIA_CHECK_EQ(nested.status_, ruvia::http_status::ok);
    RUVIA_CHECK(!nested.file_backed_);
    RUVIA_CHECK_EQ(nested.bytes_, std::string("nested-v1"));

    const auto large = serve_body(root, "large.txt");
    RUVIA_CHECK_EQ(large.status_, ruvia::http_status::ok);
    RUVIA_CHECK(large.file_backed_);

    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

RUVIA_TEST(static_root_resident_budget_bounds_the_kept_bytes) {
    const auto dir = make_resident_root();
    // Index order is large.txt, nested/page.txt, small.txt; only the first
    // eligible files that fit the budget stay resident.
    ruvia::static_root root(dir,
        {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::all},
            .resident_max_bytes_ = 32,
            .resident_budget_bytes_ = 9});
    RUVIA_CHECK(!serve_body(root, "nested/page.txt").file_backed_);
    RUVIA_CHECK(serve_body(root, "small.txt").file_backed_);

    std::error_code ignored;
    fs::remove_all(dir, ignored);
}
