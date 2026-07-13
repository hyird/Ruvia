#pragma once

#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

using HttpNativePathChar = std::filesystem::path::value_type;
using HttpNativePathString = std::pmr::basic_string<HttpNativePathChar>;
using HttpNativePathView = std::basic_string_view<HttpNativePathChar>;

[[nodiscard]] inline HttpNativePathView httpNativePathView(
    const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return HttpNativePathView(native.data(), native.size());
}

inline void assignHttpNativePath(HttpNativePathString& output, const std::filesystem::path& path) {
    const auto native = httpNativePathView(path);
    output.assign(native.data(), native.size());
}

[[nodiscard]] inline std::filesystem::path makePathFromHttpNativePath(
    const HttpNativePathChar* path) {
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
}

[[nodiscard]] inline std::filesystem::path makePathFromHttpNativePath(
    const HttpNativePathString& path) {
    return makePathFromHttpNativePath(path.c_str());
}

}  // namespace ruvia::detail
