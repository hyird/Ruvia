#pragma once

#include "ruvia/detail/NativePath.h"

#include <cstdint>
#include <filesystem>

namespace ruvia::detail {

struct ResponseFileBody final {
    const NativePathChar* nativePath{nullptr};
    std::uint64_t size{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};

    [[nodiscard]] const NativePathChar* nativePathCStr() const noexcept {
        return nativePath;
    }

    [[nodiscard]] std::filesystem::path toPath() const {
        return makePathFromNativePath(nativePath);
    }
};

}  // namespace ruvia::detail
