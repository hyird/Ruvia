#include "test_harness.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/app/EnvState.h"

namespace {

using ruvia::detail::readDotenvEntries;

template <typename T>
concept ExposesAnyRvalueEnvBorrow = requires { std::declval<const T&&>().get("NAME"); } || requires { std::declval<const T&&>().template get<std::string_view>("NAME"); };

static_assert(!ExposesAnyRvalueEnvBorrow<ruvia::Env>);

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

RUVIA_TEST(dotenv_double_quote_escapes) {
    const auto path = writeTempEnv("ruvia_dotenv_dq.env",
        "NEWLINE=\"a\\nb\"\n"
        "TAB=\"a\\tb\"\n"
        "QUOTE=\"a\\\"b\"\n"
        "BACKSLASH=\"a\\\\b\"\n");
    const auto entries = readDotenvEntries(path);
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{4});
    RUVIA_CHECK_EQ(std::string_view(entries[0].value), std::string_view("a\nb"));
    RUVIA_CHECK_EQ(std::string_view(entries[1].value), std::string_view("a\tb"));
    RUVIA_CHECK_EQ(std::string_view(entries[2].value), std::string_view("a\"b"));
    RUVIA_CHECK_EQ(std::string_view(entries[3].value), std::string_view("a\\b"));
}

RUVIA_TEST(dotenv_single_quote_is_literal) {
    const auto path = writeTempEnv("ruvia_dotenv_sq.env", "LITERAL='a\\nb'\n");
    const auto entries = readDotenvEntries(path);
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{1});
    // Single quotes are literal: a backslash-n is not an escape sequence.
    RUVIA_CHECK_EQ(std::string_view(entries[0].value), std::string_view("a\\nb"));
}

RUVIA_TEST(dotenv_rejects_malformed_quoted_values) {
    // Malformed quoting must be rejected outright, never silently truncated into
    // a partial config value (a truncated secret or URL would be dangerous).
    const std::string_view bad[] = {
        "KEY=\"unterminated\n",  // no closing double quote
        "KEY='unterminated\n",   // no closing single quote
        "KEY=\"value\"junk\n",   // stray characters after a quoted value
        "KEY=\"a\\\n",           // a trailing backslash with nothing to escape
    };
    for (const auto content : bad) {
        const auto path = writeTempEnv("ruvia_dotenv_badquote.env", content);
        bool threw = false;
        try {
            (void)readDotenvEntries(path);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        std::filesystem::remove(path);
        RUVIA_CHECK(threw);
    }
}

RUVIA_TEST(dotenv_double_quote_unknown_escape_drops_backslash) {
    // An unrecognized escape keeps the escaped character and drops the backslash
    // (so "\z" becomes "z"), distinct from the named escapes and the literal
    // single-quote behavior.
    const auto path = writeTempEnv("ruvia_dotenv_esc.env", "KEY=\"a\\zb\"\n");
    const auto entries = readDotenvEntries(path);
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{1});
    RUVIA_CHECK_EQ(std::string_view(entries[0].value), std::string_view("azb"));
}

RUVIA_TEST(dotenv_rejects_invalid_key) {
    // A key starting with a digit, or containing a non-[A-Za-z0-9_] byte -- including
    // a high, non-ASCII byte, since key validation is ASCII-only and locale-independent
    // -- is invalid.
    for (const std::string_view content : {"1KEY=x\n", "KE-Y=x\n", "K Y=x\n", "K\xC3\x89Y=x\n"}) {
        const auto path = writeTempEnv("ruvia_dotenv_badkey.env", content);
        bool threw = false;
        try {
            (void)readDotenvEntries(path);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        std::filesystem::remove(path);
        RUVIA_CHECK(threw);
    }
    // A leading underscore, digits, and underscores in the tail are all valid.
    const auto ok = writeTempEnv("ruvia_dotenv_okkey.env", "_MY_KEY2=x\n");
    const auto entries = readDotenvEntries(ok);
    std::filesystem::remove(ok);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{1});
    RUVIA_CHECK_EQ(std::string_view(entries[0].name), std::string_view("_MY_KEY2"));
}

RUVIA_TEST(dotenv_hash_is_literal_unless_space_preceded) {
    // A '#' only starts an inline comment when preceded by whitespace. A '#' in the
    // MIDDLE of an unquoted value (no preceding space) is a literal character, so a
    // URL fragment or a '#'-bearing secret is not silently truncated. The existing
    // test only covers the space-preceded (comment) case, so a regression dropping
    // the "preceded by space" guard would pass it while corrupting these values.
    const auto path = writeTempEnv("ruvia_dotenv_hash.env",
        "MIDHASH=a#b\n"                            // '#' not space-preceded -> literal
        "URL=http://host/path#frag\n"              // a URL fragment must survive
        "HASHSTART=# rest\n"                       // '#' at value start -> whole value commented (empty)
        "QUOTEDNOTE=\"kept\" # trailing note\n");  // a comment after a quoted value is allowed
    const auto entries = readDotenvEntries(path);
    std::filesystem::remove(path);

    RUVIA_CHECK_EQ(entries.size(), std::size_t{4});
    RUVIA_CHECK_EQ(std::string_view(entries[0].value), std::string_view("a#b"));
    RUVIA_CHECK_EQ(std::string_view(entries[1].value), std::string_view("http://host/path#frag"));
    RUVIA_CHECK(entries[2].value.empty());  // '#' at the start comments out the whole value
    RUVIA_CHECK_EQ(std::string_view(entries[3].value), std::string_view("kept"));
}

RUVIA_TEST(dotenv_typed_lookup_does_not_hide_invalid_values) {
    const auto path = writeTempEnv("ruvia_dotenv_typed.env", "PORT=8080\nBAD_PORT=not-a-port\nENABLED=maybe\n");
    ruvia::Env env;
    (void)ruvia::detail::loadEnvFromFile(env, path, {});
    std::filesystem::remove(path);

    RUVIA_CHECK(!env.get<std::uint16_t>("MISSING").has_value());
    RUVIA_CHECK_EQ(env.get<std::uint16_t>("PORT").value_or(0), std::uint16_t{8080});

    bool badPortThrew = false;
    try {
        (void)env.get<std::uint16_t>("BAD_PORT");
    } catch (const std::invalid_argument& error) {
        badPortThrew = std::string_view(error.what()).find("BAD_PORT") != std::string_view::npos;
    }
    RUVIA_CHECK(badPortThrew);

    bool badBoolThrew = false;
    try {
        (void)env.get<bool>("ENABLED");
    } catch (const std::invalid_argument& error) {
        badBoolThrew = std::string_view(error.what()).find("ENABLED") != std::string_view::npos;
    }
    RUVIA_CHECK(badBoolThrew);
}
