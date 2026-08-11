#pragma once

#include <filesystem>
#include <memory_resource>
#include <string_view>
#include <vector>

#include "ruvia/web/StaticFiles.h"

// Which files a static root may serve and what Content-Type each gets: the
// extension policy the root was configured with, and the MIME table looked up
// before falling back to the built-in guess.

namespace ruvia::detail {

// Sort and de-duplicate configured values so lookups can binary-search them.
[[nodiscard]] bool isValidStaticFileExtension(std::string_view extension) noexcept;
void normalizeMimeTypes(std::vector<StaticMimeType>& mimeTypes);
void normalizeFileTypes(std::vector<std::string>& fileTypes);

[[nodiscard]] bool fileTypeAllowed(std::string_view extension, const StaticRootOptions& options);

[[nodiscard]] std::pmr::string contentTypeFor(const std::filesystem::path& path, std::string_view extension, const StaticRootOptions& options, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
