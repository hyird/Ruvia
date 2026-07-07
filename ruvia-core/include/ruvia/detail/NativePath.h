#pragma once

#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

using NativePathChar = std::filesystem::path::value_type;
using NativePathString = std::pmr::basic_string<NativePathChar>;
using NativePathView = std::basic_string_view<NativePathChar>;

[[nodiscard]] inline NativePathView nativePathView(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return NativePathView(native.data(), native.size());
}

inline void assignNativePath(NativePathString& output, const std::filesystem::path& path) {
    const auto native = nativePathView(path);
    output.assign(native.data(), native.size());
}

[[nodiscard]] inline std::filesystem::path makePathFromNativePath(const NativePathChar* path) {
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
}

[[nodiscard]] inline std::filesystem::path makePathFromNativePath(const NativePathString& path) {
    return makePathFromNativePath(path.c_str());
}

}  // namespace ruvia::detail
