#pragma once

#include <cstddef>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/app/Dotenv.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

struct EnvVariable final {
    std::pmr::string name{ProcessMemory::instance().upstreamResource()};
    std::pmr::string value{ProcessMemory::instance().upstreamResource()};
};

struct EnvState final {
    std::pmr::vector<EnvVariable> variables{ProcessMemory::instance().upstreamResource()};
    bool loaded{false};
};

struct EnvAccess final {
    [[nodiscard]] static EnvState& state(Env& env) noexcept {
        return *env.state_;
    }

    [[nodiscard]] static const EnvState& state(const Env& env) noexcept {
        return *env.state_;
    }
};

struct DotenvEntry final {
    std::pmr::string name{ProcessMemory::instance().upstreamResource()};
    std::pmr::string value{ProcessMemory::instance().upstreamResource()};
};

[[nodiscard]] std::pmr::vector<DotenvEntry> readDotenvEntries(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenvExecutableDirectory();
DotenvResult loadEnvFromExecutableDirectory(Env& env, DotenvOptions options);
DotenvResult loadEnvFromFile(Env& env, const std::filesystem::path& path, DotenvOptions options);

}  // namespace ruvia::detail
