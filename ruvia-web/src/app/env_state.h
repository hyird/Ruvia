#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory_resource>
#include <string>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
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

[[nodiscard]] std::pmr::vector<env_variable> read_dotenv_entries(
    std::istream& input, const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenv_executable_directory();
// prefix followed by path in UTF-8. path::string() narrows a Windows wide path
// through the ANSI code page, which throws or yields non-UTF-8 error text.
[[nodiscard]] std::pmr::string dotenv_path_message(std::string_view prefix, const std::filesystem::path& path);

}  // namespace ruvia::detail
