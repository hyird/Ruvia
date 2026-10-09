#pragma once

#include <filesystem>
#include <memory_resource>
#include <string_view>

#include "http/static_root_config_storage.h"

// Which files a static root may serve and what Content-Type each gets: the
// extension policy the root was configured with, and the MIME table looked up
// before falling back to the built-in guess.

namespace ruvia::detail {

[[nodiscard]] bool is_valid_static_file_extension(std::string_view extension) noexcept;

[[nodiscard]] bool file_type_allowed(
    std::string_view extension, const static_root_config_storage& config);

[[nodiscard]] std::pmr::string content_type_for(const std::filesystem::path& path,
    std::string_view extension, const static_root_config_storage& config,
    std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
