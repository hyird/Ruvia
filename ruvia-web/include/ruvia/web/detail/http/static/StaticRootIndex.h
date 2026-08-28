#pragma once

#include "ruvia/core/detail/util/NativePath.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/detail/response/HttpResponseFileBody.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/static/StaticRootConfigStorage.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia::detail {

struct StaticRootPrecompressionOptions final {
    bool gzip{false};
    bool brotli{false};
    bool zstd{false};
    std::size_t minBytes{1024};
    std::size_t maxBytes{std::size_t{256} * 1024};

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return gzip || brotli || zstd;
    }
};

struct StaticRootMemoryVariant final {
    explicit StaticRootMemoryVariant(std::pmr::memory_resource* resource)
        : bytes(resource),
          etag(resource),
          lastModified(resource) {}

    HttpContentCoding contentCoding{HttpContentCoding::kIdentity};
    std::pmr::string bytes;
    std::uint64_t modifiedToken{0};
    std::time_t modifiedSeconds{0};
    std::pmr::string etag;
    std::pmr::string lastModified;
};

struct StaticRootEntry final {
    explicit StaticRootEntry(std::pmr::memory_resource* resource)
        : relativePath(resource),
          filePath(resource),
          contentType(resource),
          etag(resource),
          lastModified(resource),
          memoryVariants(resource) {}

    std::pmr::string relativePath;
    NativePathString filePath;
    std::pmr::string contentType;
    std::uint64_t size{0};
    ResponseFileIdentity identity{ResponseFileIdentity::unchecked()};
    std::uint64_t modifiedToken{0};
    std::time_t modifiedSeconds{0};
    std::pmr::string etag;
    std::pmr::string lastModified;
    bool directlyServable{true};
    std::pmr::vector<StaticRootMemoryVariant> memoryVariants;
};

struct StaticRootState final {
    NativePathString root;
    StaticRootConfigStorage config;
    std::pmr::vector<StaticRootEntry> entries;
    std::pmr::vector<std::pmr::string> directories;
    // Refresh request leases are charged to this worker-owned snapshot, not to
    // the server globally. The refresh loop can therefore reclaim unrelated
    // retired snapshots while a long request still holds an older one.
    // Application-owned immutable roots outlive all workers and never touch
    // this counter from their concurrent request paths.
    std::size_t activeBindings{0};
    std::uint64_t fingerprint{0};

    StaticRootState(std::pmr::memory_resource* resource, StaticRootConfigStorage configuredPolicy)
        : root(resource),
          config(std::move(configuredPolicy)),
          entries(resource),
          directories(resource) {}
};

class StaticRootMemoryVariantView final {
public:
    [[nodiscard]] HttpContentCoding contentCoding() const noexcept {
        return contentCoding_;
    }

    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    [[nodiscard]] std::uint64_t modifiedToken() const noexcept {
        return modifiedToken_;
    }

    [[nodiscard]] std::time_t modifiedSeconds() const noexcept {
        return modifiedSeconds_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view lastModified() const noexcept {
        return lastModified_;
    }

private:
    friend class StaticRootEntryView;

    StaticRootMemoryVariantView(HttpContentCoding contentCoding, std::string_view bytes, std::uint64_t modifiedToken, std::time_t modifiedSeconds, std::string_view etag, std::string_view lastModified) noexcept
        : contentCoding_(contentCoding),
          bytes_(bytes),
          modifiedToken_(modifiedToken),
          modifiedSeconds_(modifiedSeconds),
          etag_(etag),
          lastModified_(lastModified) {}

    HttpContentCoding contentCoding_;
    std::string_view bytes_;
    std::uint64_t modifiedToken_;
    std::time_t modifiedSeconds_;
    std::string_view etag_;
    std::string_view lastModified_;
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

    [[nodiscard]] ResponseFileIdentity identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] std::uint64_t modifiedToken() const noexcept {
        return modifiedToken_;
    }

    [[nodiscard]] std::time_t modifiedSeconds() const noexcept {
        return modifiedSeconds_;
    }

    [[nodiscard]] StaticRangeRequestPolicy rangeRequests() const noexcept {
        return rangeRequests_;
    }

    [[nodiscard]] StaticResponseValidatorPolicy responseValidators() const noexcept {
        return responseValidators_;
    }

    [[nodiscard]] std::optional<StaticRootMemoryVariantView> memoryVariant(HttpContentCoding coding) const noexcept {
        if (memoryVariants_ == nullptr) {
            return std::nullopt;
        }
        for (const auto& variant : *memoryVariants_) {
            if (variant.contentCoding == coding) {
                return StaticRootMemoryVariantView(variant.contentCoding, variant.bytes, variant.modifiedToken, variant.modifiedSeconds, variant.etag, variant.lastModified);
            }
        }
        return std::nullopt;
    }

private:
    friend class StaticRootAccess;

    StaticRootEntryView(const NativePathChar* filePath, std::string_view contentType, std::string_view cacheControl, std::string_view etag, std::string_view lastModified, std::uint64_t size, ResponseFileIdentity identity, std::uint64_t modifiedToken, std::time_t modifiedSeconds, StaticRangeRequestPolicy rangeRequests, StaticResponseValidatorPolicy responseValidators, bool directlyServable, const std::pmr::vector<StaticRootMemoryVariant>* memoryVariants) noexcept
        : filePath_(filePath),
          contentType_(contentType),
          cacheControl_(cacheControl),
          etag_(etag),
          lastModified_(lastModified),
          size_(size),
          identity_(identity),
          modifiedToken_(modifiedToken),
          modifiedSeconds_(modifiedSeconds),
          rangeRequests_(rangeRequests),
          responseValidators_(responseValidators),
          directlyServable_(directlyServable),
          memoryVariants_(memoryVariants) {}

    const NativePathChar* filePath_;
    std::string_view contentType_;
    std::string_view cacheControl_;
    std::string_view etag_;
    std::string_view lastModified_;
    std::uint64_t size_;
    ResponseFileIdentity identity_;
    std::uint64_t modifiedToken_;
    std::time_t modifiedSeconds_;
    StaticRangeRequestPolicy rangeRequests_;
    StaticResponseValidatorPolicy responseValidators_;
    bool directlyServable_;
    const std::pmr::vector<StaticRootMemoryVariant>* memoryVariants_;
};

class StaticRootAccess final {
public:
    [[nodiscard]] static std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>> make(std::pmr::memory_resource* objectResource, const std::filesystem::path& root, const StaticRootConfigStorage& config);
    [[nodiscard]] static std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>> make(std::pmr::memory_resource* objectResource, const std::filesystem::path& root, StaticRootConfigStorage&& config);
    [[nodiscard]] static std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>> clone(std::pmr::memory_resource* objectResource, const StaticRoot& source);
    [[nodiscard]] static StaticRootConfigStorage copyConfig(const StaticRoot& root, std::pmr::memory_resource* resource);
    [[nodiscard]] static std::string_view indexFile(const StaticRoot& root) noexcept;
    [[nodiscard]] static bool hasDirectoryIndex(const StaticRoot& root) noexcept;
    [[nodiscard]] static std::optional<StaticRootEntryView> find(const StaticRoot& root, std::string_view relativePath) noexcept;
    [[nodiscard]] static std::optional<StaticRootEntryView> findVariant(const StaticRoot& root, std::string_view relativePath) noexcept;
    [[nodiscard]] static bool isIndexedDirectory(const StaticRoot& root, std::string_view relativePath) noexcept;
    [[nodiscard]] static std::uint64_t fingerprint(const StaticRoot& root) noexcept;
    [[nodiscard]] static bool sameSnapshot(const StaticRoot& left, const StaticRoot& right) noexcept;
    static void acquireBinding(const StaticRoot& root) noexcept;
    static void releaseBinding(const StaticRoot& root) noexcept;
    [[nodiscard]] static bool hasActiveBindings(const StaticRoot& root) noexcept;
    static void installPrecompressedVariants(StaticRoot& root, const StaticRoot* previous, const StaticRootPrecompressionOptions& options);

private:
    [[nodiscard]] static std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>> construct(std::pmr::memory_resource* objectResource, StaticRoot::PreparedConstruction prepared);
};

}  // namespace ruvia::detail
