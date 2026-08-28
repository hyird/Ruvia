#pragma once

#include "ruvia/web/ServerConfig.h"

#include <chrono>
#include <memory_resource>
#include <optional>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

struct CorsOptions final {
    explicit CorsOptions(std::pmr::memory_resource* resource = nullptr)
        : CorsOptions(ResolvedPmrResourceTag{}, pmrResourceOrDefault(resource)) {}

    CorsOriginMode originMode{CorsOriginMode::kAny};
    std::pmr::string origin;
    CorsRequestHeadersMode requestHeadersMode{CorsRequestHeadersMode::kReflect};
    std::pmr::string requestHeaders;
    std::pmr::string exposeHeaders;
    std::optional<std::chrono::seconds> maxAge;

private:
    CorsOptions(ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : origin(resource),
          requestHeaders(resource),
          exposeHeaders(resource) {}
};

[[nodiscard]] CorsOptions makeCorsOptions(const CorsConfig& config, std::pmr::memory_resource* resource);

}  // namespace ruvia::detail
