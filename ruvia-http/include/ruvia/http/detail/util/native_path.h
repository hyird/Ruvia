#pragma once

#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia::detail {

using http_native_path_char_type = std::filesystem::path::value_type;
using http_native_path_string_type = std::pmr::basic_string<http_native_path_char_type>;
using http_native_path_view_type = std::basic_string_view<http_native_path_char_type>;

[[nodiscard]] inline http_native_path_view_type http_native_path_view(
    const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return http_native_path_view_type(native);
}

inline void assign_http_native_path(http_native_path_string_type& output, const std::filesystem::path& path) {
    const auto native = http_native_path_view(path);
    output.assign(native.data(), native.size());
}

[[nodiscard]] inline std::filesystem::path make_path_from_http_native_path(
    const http_native_path_char_type* path) {
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
}

[[nodiscard]] inline std::filesystem::path make_path_from_http_native_path(
    const http_native_path_string_type& path) {
    return make_path_from_http_native_path(path.c_str());
}

}  // namespace ruvia::detail
