#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory_resource>
#include <string>
#include <vector>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/web/dotenv.h"

namespace ruvia::detail {

struct env_variable final {
    std::pmr::string name_{process_resource()};
    std::pmr::string value_{process_resource()};
};

struct env_state final {
    std::pmr::vector<env_variable> variables_{process_resource()};
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

struct dotenv_entry final {
    std::pmr::string name_{process_resource()};
    std::pmr::string value_{process_resource()};
};

[[nodiscard]] std::pmr::vector<dotenv_entry> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenv_executable_directory();
// prefix followed by path in UTF-8. path::string() narrows a Windows wide path
// through the ANSI code page, which throws or yields non-UTF-8 error text.
[[nodiscard]] std::pmr::string dotenv_path_message(std::string_view prefix, const std::filesystem::path& path);
void load_env_from_executable_directory(env& env_value, dotenv_options options);
void load_env_from_file(env& env_value, const std::filesystem::path& path, dotenv_options options);

}  // namespace ruvia::detail
