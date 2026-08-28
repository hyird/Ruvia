#pragma once

#include <memory_resource>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/StaticFiles.h"

namespace ruvia::detail {

struct StaticRootMimeTypeStorage final {
    explicit StaticRootMimeTypeStorage(std::pmr::memory_resource* resource)
        : extension(pmrResourceOrDefault(resource)),
          contentType(pmrResourceOrDefault(resource)) {}

    std::pmr::string extension;
    std::pmr::string contentType;
};

// Validated, normalized, owning static-root policy. App configuration keeps one
// process-level instance; snapshot rebuild jobs copy it onto the process PMR so
// they never borrow worker or request state while running off-thread.
struct StaticRootConfigStorage final {
    explicit StaticRootConfigStorage(std::pmr::memory_resource* resource)
        : cacheControl(pmrResourceOrDefault(resource)),
          indexFile(pmrResourceOrDefault(resource)),
          defaultContentType("application/octet-stream", pmrResourceOrDefault(resource)),
          mimeTypes(pmrResourceOrDefault(resource)),
          fileTypeExtensions(pmrResourceOrDefault(resource)) {}

    StaticRootConfigStorage(const StaticRootConfigStorage& source, std::pmr::memory_resource* resource);
    StaticRootConfigStorage(StaticRootConfigStorage&&) noexcept = default;
    StaticRootConfigStorage& operator=(const StaticRootConfigStorage&) = delete;
    StaticRootConfigStorage& operator=(StaticRootConfigStorage&&) noexcept = default;

    std::pmr::string cacheControl;
    std::pmr::string indexFile;
    std::pmr::string defaultContentType;
    std::pmr::vector<StaticRootMimeTypeStorage> mimeTypes;
    StaticFileTypePolicy::Kind fileTypeKind{StaticFileTypePolicy::Kind::kDefaults};
    std::pmr::vector<std::pmr::string> fileTypeExtensions;
    StaticRangeRequestPolicy rangeRequests{StaticRangeRequestPolicy::kHonor};
    StaticResponseValidatorPolicy responseValidators{StaticResponseValidatorPolicy::kEmit};
    StaticDotfilePolicy dotfiles{StaticDotfilePolicy::kDeny};
};

[[nodiscard]] StaticRootConfigStorage makeStaticRootConfigStorage(const StaticRootOptions& source, std::pmr::memory_resource* resource);

// The caller owns whole-config validation and invokes this only after every
// related policy has passed. This split lets App validate DocumentRootConfig
// atomically before the first owner-PMR allocation.
[[nodiscard]] StaticRootConfigStorage storeValidatedStaticRootConfig(const StaticRootOptions& source, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
