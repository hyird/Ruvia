#include "test_harness.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include "app/DotenvInternal.h"

namespace {

using ruvia::detail::readDotenvEntries;

std::filesystem::path writeTempEnv(std::string_view name, std::string_view contents) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

}  // namespace

RUVIA_TEST(dotenv_parses_entries) {
    const auto path = writeTempEnv("ruvia_dotenv_ok.env",
        "# a comment line\n"
        "\n"
        "KEY=value\n"
        "export EXPORTED=exported_value\n"
        "SPACED = spaced value  \n"
        "QUOTED=\"quoted value\"\n"
        "WITHCOMMENT=val # inline note\n");
    const auto entries = readDotenvEntries(path);
    std::filesystem::remove(path);

    RUVIA_CHECK_EQ(entries.size(), std::size_t{5});  // comment and blank lines skipped
    RUVIA_CHECK_EQ(std::string_view(entries[0].name), std::string_view("KEY"));
    RUVIA_CHECK_EQ(std::string_view(entries[0].value), std::string_view("value"));
    // The export prefix is stripped.
    RUVIA_CHECK_EQ(std::string_view(entries[1].name), std::string_view("EXPORTED"));
    RUVIA_CHECK_EQ(std::string_view(entries[1].value), std::string_view("exported_value"));
    // Whitespace around key and value is trimmed.
    RUVIA_CHECK_EQ(std::string_view(entries[2].name), std::string_view("SPACED"));
    RUVIA_CHECK_EQ(std::string_view(entries[2].value), std::string_view("spaced value"));
    // Surrounding double quotes are removed.
    RUVIA_CHECK_EQ(std::string_view(entries[3].name), std::string_view("QUOTED"));
    RUVIA_CHECK_EQ(std::string_view(entries[3].value), std::string_view("quoted value"));
    // A whitespace-preceded '#' starts an inline comment.
    RUVIA_CHECK_EQ(std::string_view(entries[4].name), std::string_view("WITHCOMMENT"));
    RUVIA_CHECK_EQ(std::string_view(entries[4].value), std::string_view("val"));
}

RUVIA_TEST(dotenv_rejects_line_without_equals) {
    const auto path = writeTempEnv("ruvia_dotenv_bad.env", "VALID=1\nNO_EQUALS_HERE\n");
    bool threw = false;
    try {
        (void)readDotenvEntries(path);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    std::filesystem::remove(path);
    RUVIA_CHECK(threw);
}

RUVIA_TEST(dotenv_missing_file_is_empty) {
    const auto path = std::filesystem::temp_directory_path() / "ruvia_dotenv_absent.env";
    std::filesystem::remove(path);  // ensure it does not exist
    RUVIA_CHECK(readDotenvEntries(path).empty());
}
