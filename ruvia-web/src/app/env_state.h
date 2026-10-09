#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/dotenv.h"

#include "app/app_resource.h"

namespace ruvia::detail {

struct env_variable final {
    std::pmr::string name_{app_resource()};
    std::pmr::string value_{app_resource()};
};

struct env_state final {
    std::pmr::vector<env_variable> variables_{app_resource()};
    bool loaded_{false};
};

struct env_access final {
    [[nodiscard]] static env_state& state(env& env_value) noexcept {
        return *env_value.state_;
    }

    [[nodiscard]] static const env_state& state(const env& env_value) noexcept {
        return *env_value.state_;
    }
};

struct dotenv_result_access final {
    [[nodiscard]] static dotenv_result make(bool loaded) noexcept {
        return dotenv_result(loaded);
    }

    static void increment_variables_set(dotenv_result& result_value) noexcept {
        ++result_value.variables_set_;
    }

    static void increment_variables_skipped(dotenv_result& result_value) noexcept {
        ++result_value.variables_skipped_;
    }
};

struct dotenv_entry final {
    std::pmr::string name_{app_resource()};
    std::pmr::string value_{app_resource()};
};

[[nodiscard]] std::pmr::vector<dotenv_entry> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path);
[[nodiscard]] std::pmr::vector<dotenv_entry> read_dotenv_entries(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenv_executable_directory();
dotenv_result load_env_from_executable_directory(env& env_value, dotenv_options options);
dotenv_result load_env_from_file(env& env_value, const std::filesystem::path& path, dotenv_options options);

}  // namespace ruvia::detail
