#include <algorithm>
#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/HttpConnectionAdvertisementOutput.h"

namespace ruvia {
ScopedOperation<void> Context::advertiseOrigins(std::span<const std::string_view> origins) {
    if (connectionAdvertisements_ == nullptr || connInfo_.tls() == nullptr) {
        throw std::logic_error("ORIGIN requires an HTTPS connection with advertisement output");
    }
    return connectionAdvertisements_->advertiseOrigins(origins);
}
ScopedOperation<void> Context::advertiseAlternativeService(std::string_view value) {
    if (connectionAdvertisements_ == nullptr) {
        throw std::logic_error("this dispatch has no connection advertisement output");
    }
    return connectionAdvertisements_->advertiseAlternativeService(value);
}
}  // namespace ruvia

namespace ruvia::detail {
ScopedOperation<void> HttpConnectionAdvertisementOutput::advertiseOrigins(std::span<const std::string_view> origins) {
    if (origins_ == nullptr) {
        throw std::logic_error("this HTTP version does not support ORIGIN frames");
    }
    if (scope_.has_pending_operations()) {
        throw std::logic_error("connection advertisement output is already active");
    }
    if (origins.size() > kMaxHttpHeaderBytes / 2) {
        throw std::length_error("too many advertised origins");
    }
    auto bytes = origins.size() * 2;
    for (const auto origin : origins) {
        if (origin.size() > kMaxHttpHeaderBytes - std::min(bytes, kMaxHttpHeaderBytes)) {
            throw std::length_error("origin advertisement exceeds its byte bound");
        }
        bytes += origin.size();
    }
    std::pmr::vector<std::pmr::string> owned(resource_);
    owned.reserve(origins.size());
    for (const auto origin : origins) {
        owned.emplace_back(origin);
    }
    return ::ruvia::make_scoped_operation(scope_, writeOrigins(std::move(owned)));
}
ScopedOperation<void> HttpConnectionAdvertisementOutput::advertiseAlternativeService(std::string_view value) {
    if (service_ == nullptr) {
        throw std::logic_error("ALTSVC frames require HTTP/2");
    }
    if (scope_.has_pending_operations()) {
        throw std::logic_error("connection advertisement output is already active");
    }
    if (value.size() > kMaxHttpHeaderBytes) {
        throw std::length_error("alternative service advertisement exceeds its byte bound");
    }
    return ::ruvia::make_scoped_operation(scope_, writeService(std::pmr::string(value, resource_)));
}
Task<void> HttpConnectionAdvertisementOutput::writeOrigins(std::pmr::vector<std::pmr::string> origins) {
    std::pmr::vector<std::string_view> views(resource_);
    views.reserve(origins.size());
    for (const auto& origin : origins) {
        views.emplace_back(origin);
    }
    co_await origins_(target_, views);
}
Task<void> HttpConnectionAdvertisementOutput::writeService(std::pmr::string value) {
    co_await service_(target_, value);
}
}  // namespace ruvia::detail
