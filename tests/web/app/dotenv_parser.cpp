#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/app.h"

#include "test_harness.h"

namespace {

std::filesystem::path write_temp_env(std::string_view name, std::string_view contents) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

}  // namespace

RUVIA_TEST(dotenv_typed_lookup_does_not_hide_invalid_values) {
    const auto path = write_temp_env("ruvia_dotenv_typed.env",
        "PORT=8080\nBAD_PORT=not-a-port\nENABLED=maybe\nBAD_DOUBLE=nan\nBAD_INF=inf\n");
    auto& app = ruvia::app();
    app.load_dotenv(path, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override});
    const auto& env = app.env();
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

    auto& app = ruvia::app();
    app.load_dotenv(first, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override});
    const auto& env = app.env();
    RUVIA_CHECK_EQ(env.get("KEY").value_or(""), std::string_view("first"));

    app.load_dotenv(second);
    RUVIA_CHECK_EQ(env.get("KEY").value_or(""), std::string_view("first"));
    RUVIA_CHECK_EQ(env.get("NEW").value_or(""), std::string_view("value"));

    app.load_dotenv(
        second, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override});
    RUVIA_CHECK_EQ(env.get("KEY").value_or(""), std::string_view("second"));

    app.load_dotenv(missing);

    bool required_missing_threw = false;
    try {
        app.load_dotenv(
            missing, {.missing_file_ = ruvia::dotenv_missing_file_policy::require});
    } catch (const std::runtime_error&) {
        required_missing_threw = true;
    }
    RUVIA_CHECK(required_missing_threw);

    bool invalid_existing_policy_threw = false;
    try {
        app.load_dotenv(first,
            {.existing_variables_ = static_cast<ruvia::dotenv_existing_variable_policy>(0xFF)});
    } catch (const std::invalid_argument&) {
        invalid_existing_policy_threw = true;
    }
    RUVIA_CHECK(invalid_existing_policy_threw);

    bool invalid_missing_policy_threw = false;
    try {
        app.load_dotenv(
            first, {.missing_file_ = static_cast<ruvia::dotenv_missing_file_policy>(0xFF)});
    } catch (const std::invalid_argument&) {
        invalid_missing_policy_threw = true;
    }
    RUVIA_CHECK(invalid_missing_policy_threw);

    std::filesystem::remove(first);
    std::filesystem::remove(second);
}
