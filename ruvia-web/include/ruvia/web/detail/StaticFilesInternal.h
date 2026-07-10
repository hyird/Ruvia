#pragma once

#include "ruvia/core/detail/NativePath.h"
#include "ruvia/web/StaticFiles.h"

#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {

struct StaticRootEntry final {
    explicit StaticRootEntry(std::pmr::memory_resource* resource)
        : relativePath(resource),
          filePath(resource),
          contentType(resource),
          etag(resource),
          lastModified(resource) {}

    std::pmr::string relativePath;
    NativePathString filePath;
    std::pmr::string contentType;
    std::uint64_t size{0};
    std::filesystem::file_time_type modified{};
    std::pmr::string etag;
    std::pmr::string lastModified;
};

struct StaticRootState final {
    NativePathString root;
    std::pmr::string indexFile;
    std::pmr::string cacheControl;
    std::pmr::vector<StaticRootEntry> entries;
    std::pmr::vector<std::pmr::string> directories;
    bool enableRanges{true};
    bool enableValidators{true};

    explicit StaticRootState(std::pmr::memory_resource* resource)
        : root(resource),
          indexFile(resource),
          cacheControl(resource),
          entries(resource),
          directories(resource) {}
};

struct StaticRootEntryView final {
    const NativePathChar* filePath{nullptr};
    std::string_view contentType;
    std::string_view cacheControl;
    std::string_view etag;
    std::string_view lastModified;
    std::uint64_t size{0};
    std::filesystem::file_time_type modified{};
    bool enableRanges{true};
    bool enableValidators{true};

    [[nodiscard]] bool found() const noexcept {
        return filePath != nullptr;
    }
};

class StaticRootAccess final {
public:
    [[nodiscard]] static std::string_view indexFile(const StaticRoot& root) noexcept;
    [[nodiscard]] static bool hasDirectoryIndex(const StaticRoot& root) noexcept;
    [[nodiscard]] static StaticRootEntryView find(const StaticRoot& root, std::string_view relativePath) noexcept;
    [[nodiscard]] static bool isIndexedDirectory(const StaticRoot& root, std::string_view relativePath) noexcept;
};

}  // namespace ruvia::detail
