#pragma once

#include <cstddef>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/Dotenv.h"
#include "ruvia/web/detail/app/AppResource.h"

namespace ruvia::detail {

struct EnvVariable final {
    std::pmr::string name{appResource()};
    std::pmr::string value{appResource()};
};

struct EnvState final {
    std::pmr::vector<EnvVariable> variables{appResource()};
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

struct DotenvResultAccess final {
    [[nodiscard]] static DotenvResult make(bool loaded) noexcept {
        return DotenvResult(loaded);
    }

    static void incrementVariablesSet(DotenvResult& result) noexcept {
        ++result.variablesSet_;
    }

    static void incrementVariablesSkipped(DotenvResult& result) noexcept {
        ++result.variablesSkipped_;
    }
};

struct DotenvEntry final {
    std::pmr::string name{appResource()};
    std::pmr::string value{appResource()};
};

[[nodiscard]] std::pmr::vector<DotenvEntry> readDotenvEntries(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path dotenvExecutableDirectory();
DotenvResult loadEnvFromExecutableDirectory(Env& env, DotenvOptions options);
DotenvResult loadEnvFromFile(Env& env, const std::filesystem::path& path, DotenvOptions options);

}  // namespace ruvia::detail
