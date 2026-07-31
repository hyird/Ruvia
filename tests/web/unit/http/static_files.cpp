#include "test_harness.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"

namespace {

namespace fs = std::filesystem;

// A throwaway document root mixing a normal asset with hidden files and a
// hidden directory (the .git checkout an operator might accidentally deploy).
fs::path makeDotfileRoot() {
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

[[nodiscard]] bool served(const ruvia::StaticRoot& root, std::string_view path) {
    return ruvia::detail::StaticRootAccess::find(root, path).has_value();
}

}  // namespace

RUVIA_TEST(static_root_hides_dotfiles_even_under_all_policy) {
    const auto dir = makeDotfileRoot();
    ruvia::StaticRootOptions options;
    // all() would otherwise index and serve every file regardless of extension;
    // the hidden-path default-deny must still keep secrets out of the index.
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    ruvia::StaticRoot root(dir, std::move(options));

    RUVIA_CHECK(served(root, "app.js"));
    RUVIA_CHECK(!served(root, ".env"));
    RUVIA_CHECK(!served(root, ".htpasswd"));
    RUVIA_CHECK(!served(root, ".backup.json"));
    RUVIA_CHECK(!served(root, ".git/config"));

    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

RUVIA_TEST(static_root_serves_dotfiles_when_opted_in) {
    const auto dir = makeDotfileRoot();
    ruvia::StaticRootOptions options;
    options.fileTypes = ruvia::StaticFileTypePolicy::all();
    options.serveDotfiles = true;
    ruvia::StaticRoot root(dir, std::move(options));

    RUVIA_CHECK(served(root, ".env"));
    RUVIA_CHECK(served(root, ".git/config"));
    RUVIA_CHECK(served(root, "app.js"));

    std::error_code ignored;
    fs::remove_all(dir, ignored);
}
