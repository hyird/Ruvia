#pragma once

#include "ruvia/core/detail/NativePath.h"
#include "ruvia/web/StaticFiles.h"

#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <optional>
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

class StaticRootEntryView final {
public:
    [[nodiscard]] const NativePathChar* filePath() const noexcept {
        return filePath_;
    }

    [[nodiscard]] std::string_view contentType() const noexcept {
        return contentType_;
    }

    [[nodiscard]] std::string_view cacheControl() const noexcept {
        return cacheControl_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view lastModified() const noexcept {
        return lastModified_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::filesystem::file_time_type modified() const noexcept {
        return modified_;
    }

    [[nodiscard]] bool rangesEnabled() const noexcept {
        return rangesEnabled_;
    }

    [[nodiscard]] bool validatorsEnabled() const noexcept {
        return validatorsEnabled_;
    }

private:
    friend class StaticRootAccess;

    StaticRootEntryView(
        const NativePathChar* filePath,
        std::string_view contentType,
        std::string_view cacheControl,
        std::string_view etag,
        std::string_view lastModified,
        std::uint64_t size,
        std::filesystem::file_time_type modified,
        bool rangesEnabled,
        bool validatorsEnabled) noexcept
        : filePath_(filePath),
          contentType_(contentType),
          cacheControl_(cacheControl),
          etag_(etag),
          lastModified_(lastModified),
          size_(size),
          modified_(modified),
          rangesEnabled_(rangesEnabled),
          validatorsEnabled_(validatorsEnabled) {}

    const NativePathChar* filePath_;
    std::string_view contentType_;
    std::string_view cacheControl_;
    std::string_view etag_;
    std::string_view lastModified_;
    std::uint64_t size_;
    std::filesystem::file_time_type modified_;
    bool rangesEnabled_;
    bool validatorsEnabled_;
};

class StaticRootAccess final {
public:
    [[nodiscard]] static std::string_view indexFile(const StaticRoot& root) noexcept;
    [[nodiscard]] static bool hasDirectoryIndex(const StaticRoot& root) noexcept;
    [[nodiscard]] static std::optional<StaticRootEntryView> find(
        const StaticRoot& root,
        std::string_view relativePath) noexcept;
    [[nodiscard]] static bool isIndexedDirectory(const StaticRoot& root, std::string_view relativePath) noexcept;
};

}  // namespace ruvia::detail
