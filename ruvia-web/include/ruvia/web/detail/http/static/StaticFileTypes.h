#pragma once

#include <filesystem>
#include <memory_resource>
#include <string_view>
#include "ruvia/web/detail/http/static/StaticRootConfigStorage.h"

// Which files a static root may serve and what Content-Type each gets: the
// extension policy the root was configured with, and the MIME table looked up
// before falling back to the built-in guess.

namespace ruvia::detail {

[[nodiscard]] bool isValidStaticFileExtension(std::string_view extension) noexcept;

[[nodiscard]] bool fileTypeAllowed(std::string_view extension, const StaticRootConfigStorage& config);

[[nodiscard]] std::pmr::string contentTypeFor(const std::filesystem::path& path, std::string_view extension, const StaticRootConfigStorage& config, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
