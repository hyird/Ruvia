#pragma once

#include "ruvia/web/ServerConfig.h"

#include <chrono>
#include <memory_resource>
#include <optional>

namespace ruvia::detail {

struct CorsOptions final {
    explicit CorsOptions(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : origin(resource), requestHeaders(resource), exposeHeaders(resource) {}

    CorsOriginPolicy::Kind originKind{CorsOriginPolicy::Kind::kAny};
    std::pmr::string origin;
    CorsRequestHeadersPolicy::Kind requestHeadersKind{CorsRequestHeadersPolicy::Kind::kReflect};
    std::pmr::string requestHeaders;
    std::pmr::string exposeHeaders;
    std::optional<std::chrono::seconds> maxAge;
};

[[nodiscard]] inline CorsOptions makeCorsOptions(const CorsConfig& config, std::pmr::memory_resource* resource) {
    CorsOptions stored(resource);
    stored.originKind = config.origin.kind();
    stored.origin.assign(config.origin.origin());
    stored.requestHeadersKind = config.requestHeaders.kind();
    stored.requestHeaders.assign(config.requestHeaders.headers());
    stored.exposeHeaders.assign(config.exposeHeaders.value());
    stored.maxAge = config.maxAge.has_value() ? std::optional<std::chrono::seconds>(config.maxAge->value()) : std::nullopt;
    return stored;
}

}  // namespace ruvia::detail
