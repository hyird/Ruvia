#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ruvia::edge {

// Durable single-file operations the disk tier is built on: naming an entry
// file, reading one back with a byte ceiling, and replacing one so a crash
// leaves either the old record or the new one -- never a torn file. Every
// failure is reported, never thrown.

[[nodiscard]] bool isCommittedEntryName(std::string_view name) noexcept;
[[nodiscard]] bool isOwnedTempName(std::string_view name) noexcept;

[[nodiscard]] bool readEntryFile(const std::filesystem::path& path, std::size_t maxBytes, std::string& out);

void syncDirectoryBestEffort(const std::filesystem::path& directory) noexcept;
[[nodiscard]] bool flushFileToDisk(const std::filesystem::path& path) noexcept;

// Rename a fully flushed temporary over its final path, so a crash leaves either
// the old record or the new one -- never a torn file.
[[nodiscard]] bool commitReplacement(const std::filesystem::path& temporary, const std::filesystem::path& finalPath) noexcept;

void removeOwnedFileBestEffort(const std::filesystem::path& path) noexcept;

}  // namespace ruvia::edge
