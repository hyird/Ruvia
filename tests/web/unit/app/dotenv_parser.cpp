#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "app/env_state.h"
#include "test_harness.h"

namespace {

using ruvia::detail::read_dotenv_entries;

std::filesystem::path write_temp_env(std::string_view name, std::string_view contents) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

}  // namespace

RUVIA_TEST(dotenv_parses_entries) {
    const auto path = write_temp_env("ruvia_dotenv_ok.env",
        "# a comment line\n"
        "\n"
        "KEY=value\n"
        "export EXPORTED=exported_value\n"
        "SPACED = spaced value  \n"
        "QUOTED=\"quoted value\"\n"
        "WITHCOMMENT=val # inline note\n");
    const auto entries = read_dotenv_entries(path);
    std::filesystem::remove(path);

    RUVIA_CHECK_EQ(entries.size(), std::size_t{5});  // comment and blank lines skipped
    RUVIA_CHECK_EQ(std::string_view(entries[0].name_), std::string_view("KEY"));
    RUVIA_CHECK_EQ(std::string_view(entries[0].value_), std::string_view("value"));
    // The export prefix is stripped.
    RUVIA_CHECK_EQ(std::string_view(entries[1].name_), std::string_view("EXPORTED"));
    RUVIA_CHECK_EQ(std::string_view(entries[1].value_), std::string_view("exported_value"));
    // Whitespace around key and value is trimmed.
    RUVIA_CHECK_EQ(std::string_view(entries[2].name_), std::string_view("SPACED"));
    RUVIA_CHECK_EQ(std::string_view(entries[2].value_), std::string_view("spaced value"));
    // Surrounding double quotes are removed.
    RUVIA_CHECK_EQ(std::string_view(entries[3].name_), std::string_view("QUOTED"));
    RUVIA_CHECK_EQ(std::string_view(entries[3].value_), std::string_view("quoted value"));
    // A whitespace-preceded '#' starts an inline comment.
    RUVIA_CHECK_EQ(std::string_view(entries[4].name_), std::string_view("WITHCOMMENT"));
    RUVIA_CHECK_EQ(std::string_view(entries[4].value_), std::string_view("val"));
}

RUVIA_TEST(dotenv_rejects_line_without_equals) {
    const auto path = write_temp_env("ruvia_dotenv_bad.env", "VALID=1\nNO_EQUALS_HERE\n");
    bool threw = false;
    try {
        (void)read_dotenv_entries(path);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    std::filesystem::remove(path);
    RUVIA_CHECK(threw);
}

RUVIA_TEST(dotenv_existing_unreadable_file_is_not_treated_as_missing) {
    const auto path = write_temp_env("ruvia_dotenv_unreadable.env", "SECRET=value\n");
    std::error_code error;
    std::filesystem::permissions(
        path, std::filesystem::perms::none, std::filesystem::perm_options::replace, error);
    if (error) {
        std::filesystem::remove(path);
        return;  // Some platforms cannot enforce file permission bits.
    }
    const bool unreadable = !std::ifstream(path);
    bool parser_threw = false;
    bool loader_threw = false;
    if (unreadable) {
        try {
            (void)read_dotenv_entries(path);
        } catch (const std::runtime_error&) {
            parser_threw = true;
        }
        ruvia::env env;
        try {
            (void)ruvia::detail::load_env_from_file(env, path, {});
        } catch (const std::runtime_error&) {
            loader_threw = true;
        }
    }
    std::filesystem::permissions(path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    std::filesystem::remove(path);
    if (unreadable) {
        RUVIA_CHECK(parser_threw);
        RUVIA_CHECK(loader_threw);
    }
}

RUVIA_TEST(dotenv_missing_file_is_empty) {
    const auto path = std::filesystem::temp_directory_path() / "ruvia_dotenv_absent.env";
    std::filesystem::remove(path);  // ensure it does not exist
    RUVIA_CHECK(read_dotenv_entries(path).empty());
}

RUVIA_TEST(dotenv_double_quote_escapes) {
    const auto path = write_temp_env("ruvia_dotenv_dq.env",
        "NEWLINE=\"a\\nb\"\n"
        "TAB=\"a\\tb\"\n"
        "QUOTE=\"a\\\"b\"\n"
        "BACKSLASH=\"a\\\\b\"\n");
    const auto entries = read_dotenv_entries(path);
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{4});
    RUVIA_CHECK_EQ(std::string_view(entries[0].value_), std::string_view("a\nb"));
    RUVIA_CHECK_EQ(std::string_view(entries[1].value_), std::string_view("a\tb"));
    RUVIA_CHECK_EQ(std::string_view(entries[2].value_), std::string_view("a\"b"));
    RUVIA_CHECK_EQ(std::string_view(entries[3].value_), std::string_view("a\\b"));
}

RUVIA_TEST(dotenv_single_quote_is_literal) {
    const auto path = write_temp_env("ruvia_dotenv_sq.env", "LITERAL='a\\nb'\n");
    const auto entries = read_dotenv_entries(path);
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{1});
    // Single quotes are literal: a backslash-n is not an escape sequence.
    RUVIA_CHECK_EQ(std::string_view(entries[0].value_), std::string_view("a\\nb"));
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
        const auto path = write_temp_env("ruvia_dotenv_badquote.env", content);
        bool threw = false;
        try {
            (void)read_dotenv_entries(path);
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
    const auto path = write_temp_env("ruvia_dotenv_esc.env", "KEY=\"a\\zb\"\n");
    const auto entries = read_dotenv_entries(path);
    std::filesystem::remove(path);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{1});
    RUVIA_CHECK_EQ(std::string_view(entries[0].value_), std::string_view("azb"));
}

RUVIA_TEST(dotenv_rejects_invalid_key) {
    // A key starting with a digit, or containing a non-[A-Za-z0-9_] byte -- including
    // a high, non-ASCII byte, since key validation is ASCII-only and locale-independent
    // -- is invalid.
    for (const std::string_view content : {"1KEY=x\n", "KE-Y=x\n", "K Y=x\n", "K\xC3\x89Y=x\n"}) {
        const auto path = write_temp_env("ruvia_dotenv_badkey.env", content);
        bool threw = false;
        try {
            (void)read_dotenv_entries(path);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        std::filesystem::remove(path);
        RUVIA_CHECK(threw);
    }
    // A leading underscore, digits, and underscores in the tail are all valid.
    const auto ok = write_temp_env("ruvia_dotenv_okkey.env", "_MY_KEY2=x\n");
    const auto entries = read_dotenv_entries(ok);
    std::filesystem::remove(ok);
    RUVIA_CHECK_EQ(entries.size(), std::size_t{1});
    RUVIA_CHECK_EQ(std::string_view(entries[0].name_), std::string_view("_MY_KEY2"));
}

RUVIA_TEST(dotenv_hash_is_literal_unless_space_preceded) {
    // A '#' only starts an inline comment when preceded by whitespace. A '#' in the
    // MIDDLE of an unquoted value (no preceding space) is a literal character, so a
    // URL fragment or a '#'-bearing secret is not silently truncated. The existing
    // test only covers the space-preceded (comment) case, so a regression dropping
    // the "preceded by space" guard would pass it while corrupting these values.
    const auto path = write_temp_env("ruvia_dotenv_hash.env",
        "MIDHASH=a#b\n"                            // '#' not space-preceded -> literal
        "URL=http://host/path#frag\n"              // a URL fragment must survive
        "HASHSTART=# rest\n"                       // '#' at value start -> whole value commented (empty)
        "QUOTEDNOTE=\"kept\" # trailing note\n");  // a comment after a quoted value is allowed
    const auto entries = read_dotenv_entries(path);
    std::filesystem::remove(path);

    RUVIA_CHECK_EQ(entries.size(), std::size_t{4});
    RUVIA_CHECK_EQ(std::string_view(entries[0].value_), std::string_view("a#b"));
    RUVIA_CHECK_EQ(std::string_view(entries[1].value_), std::string_view("http://host/path#frag"));
    RUVIA_CHECK(entries[2].value_.empty());  // '#' at the start comments out the whole value
    RUVIA_CHECK_EQ(std::string_view(entries[3].value_), std::string_view("kept"));
}

RUVIA_TEST(dotenv_typed_lookup_does_not_hide_invalid_values) {
    const auto path = write_temp_env("ruvia_dotenv_typed.env",
        "PORT=8080\nBAD_PORT=not-a-port\nENABLED=maybe\nBAD_DOUBLE=nan\nBAD_INF=inf\n");
    ruvia::env env;
    (void)ruvia::detail::load_env_from_file(env, path, {});
    std::filesystem::remove(path);

    RUVIA_CHECK(!env.get<std::uint16_t>("MISSING").has_value());
    RUVIA_CHECK_EQ(env.get<std::uint16_t>("PORT").value_or(0), std::uint16_t{8080});

    bool bad_port_threw = false;
    try {
        (void)env.get<std::uint16_t>("BAD_PORT");
    } catch (const std::invalid_argument& error) {
        bad_port_threw = (std::string_view(error.what()).find("BAD_PORT") != std::string_view::npos);
    }
    RUVIA_CHECK(bad_port_threw);

    bool bad_bool_threw = false;
    try {
        (void)env.get<bool>("ENABLED");
    } catch (const std::invalid_argument& error) {
        bad_bool_threw = (std::string_view(error.what()).find("ENABLED") != std::string_view::npos);
    }
    RUVIA_CHECK(bad_bool_threw);

    bool bad_double_threw = false;
    try {
        (void)env.get<double>("BAD_DOUBLE");
    } catch (const std::invalid_argument& error) {
        bad_double_threw =
            (std::string_view(error.what()).find("BAD_DOUBLE") != std::string_view::npos);
    }
    RUVIA_CHECK(bad_double_threw);

    bool bad_inf_threw = false;
    try {
        (void)env.get<double>("BAD_INF");
    } catch (const std::invalid_argument& error) {
        bad_inf_threw = (std::string_view(error.what()).find("BAD_INF") != std::string_view::npos);
    }
    RUVIA_CHECK(bad_inf_threw);
}

RUVIA_TEST(dotenv_options_use_explicit_policies) {
    const auto first = write_temp_env("ruvia_dotenv_policy_first.env", "KEY=first\n");
    const auto second = write_temp_env("ruvia_dotenv_policy_second.env", "KEY=second\nNEW=value\n");
    const auto missing = std::filesystem::temp_directory_path() / "ruvia_dotenv_policy_missing.env";
    std::filesystem::remove(missing);

    ruvia::env env;
    const auto initial_value = ruvia::detail::load_env_from_file(env, first, {});
    RUVIA_CHECK(initial_value.loaded());
    RUVIA_CHECK_EQ(initial_value.variables_set(), std::size_t{1});
    RUVIA_CHECK_EQ(env.get("KEY").value_or(""), std::string_view("first"));

    const auto preserved = ruvia::detail::load_env_from_file(env, second, {});
    RUVIA_CHECK_EQ(preserved.variables_set(), std::size_t{1});
    RUVIA_CHECK_EQ(preserved.variables_skipped(), std::size_t{1});
    RUVIA_CHECK_EQ(env.get("KEY").value_or(""), std::string_view("first"));
    RUVIA_CHECK_EQ(env.get("NEW").value_or(""), std::string_view("value"));

    const auto overridden = ruvia::detail::load_env_from_file(
        env, second, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override});
    RUVIA_CHECK_EQ(overridden.variables_set(), std::size_t{2});
    RUVIA_CHECK_EQ(overridden.variables_skipped(), std::size_t{0});
    RUVIA_CHECK_EQ(env.get("KEY").value_or(""), std::string_view("second"));

    const auto ignored_missing = ruvia::detail::load_env_from_file(env, missing, {});
    RUVIA_CHECK(!ignored_missing.loaded());

    bool required_missing_threw = false;
    try {
        (void)ruvia::detail::load_env_from_file(
            env, missing, {.missing_file_ = ruvia::dotenv_missing_file_policy::require});
    } catch (const std::runtime_error&) {
        required_missing_threw = true;
    }
    RUVIA_CHECK(required_missing_threw);

    bool invalid_existing_policy_threw = false;
    try {
        (void)ruvia::detail::load_env_from_file(env, first,
            {.existing_variables_ = static_cast<ruvia::dotenv_existing_variable_policy>(0xFF)});
    } catch (const std::invalid_argument&) {
        invalid_existing_policy_threw = true;
    }
    RUVIA_CHECK(invalid_existing_policy_threw);

    bool invalid_missing_policy_threw = false;
    try {
        (void)ruvia::detail::load_env_from_file(
            env, first, {.missing_file_ = static_cast<ruvia::dotenv_missing_file_policy>(0xFF)});
    } catch (const std::invalid_argument&) {
        invalid_missing_policy_threw = true;
    }
    RUVIA_CHECK(invalid_missing_policy_threw);

    std::filesystem::remove(first);
    std::filesystem::remove(second);
}
