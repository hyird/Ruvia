#pragma once

#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

namespace ruvia {

using native_path_char_type = std::filesystem::path::value_type;
using native_path_string_type = std::pmr::basic_string<native_path_char_type>;
using native_path_view_type = std::basic_string_view<native_path_char_type>;

[[nodiscard]] inline native_path_view_type native_path_view(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return native_path_view_type(native);
}
native_path_view_type native_path_view(const std::filesystem::path&&) = delete;

inline void assign_native_path(native_path_string_type& output, const std::filesystem::path& path) {
    const auto native = native_path_view(path);
    output.assign(native.data(), native.size());
}

[[nodiscard]] inline std::filesystem::path make_path_from_native_path(const native_path_char_type* path) {
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
}

[[nodiscard]] inline std::filesystem::path make_path_from_native_path(const native_path_string_type& path) {
    return make_path_from_native_path(path.c_str());
}

}  // namespace ruvia

namespace ruvia::detail {
using ::ruvia::assign_native_path;
using ::ruvia::make_path_from_native_path;
using ::ruvia::native_path_char_type;
using ::ruvia::native_path_string_type;
using ::ruvia::native_path_view;
using ::ruvia::native_path_view_type;
}  // namespace ruvia::detail
