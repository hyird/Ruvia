#include "ruvia/web/static_files.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

#include "http/static_root_index.h"
#include "test_harness.h"

namespace {

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
    return ruvia::detail::static_root_access::find(root, path).has_value();
}

}  // namespace

RUVIA_TEST(static_root_hides_dotfiles_even_under_all_policy) {
    const auto dir = make_dotfile_root();
    ruvia::static_root_options options;
    // all() would otherwise index and serve every file regardless of extension;
    // the hidden-path default-deny must still keep secrets out of the index.
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
