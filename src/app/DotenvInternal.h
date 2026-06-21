#pragma once

#include <cstddef>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

struct DotenvEntry final {
    std::pmr::string name{ProcessMemory::instance().upstreamResource()};
    std::pmr::string value{ProcessMemory::instance().upstreamResource()};
};

[[nodiscard]] std::pmr::vector<DotenvEntry> readDotenvEntries(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenvExecutableDirectory();

}  // namespace ruvia::detail
