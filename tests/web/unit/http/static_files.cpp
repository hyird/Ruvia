#include "ruvia/web/static_files.h"

#include <filesystem>
#include <fstream>
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

}  // namespace static_files_test

using static_files_test::make_dotfile_root;
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
