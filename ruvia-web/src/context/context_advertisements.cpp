#include <algorithm>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_limits.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "context/http_connection_advertisement_output.h"

namespace ruvia {
scoped_operation<void> context::advertise_origins(std::span<const std::string_view> origins) {
    if (services().connection_advertisements() == nullptr || conn_info_.tls() == nullptr) {
        throw std::logic_error("ORIGIN requires an HTTPS connection with advertisement output");
    }
    return services().connection_advertisements()->advertise_origins(origins);
}
scoped_operation<void> context::advertise_alternative_service(std::string_view value) {
    if (services().connection_advertisements() == nullptr) {
        throw std::logic_error("this dispatch has no connection advertisement output");
    }
    return services().connection_advertisements()->advertise_alternative_service(value);
}
}  // namespace ruvia

namespace ruvia::detail {
scoped_operation<void> http_connection_advertisement_output::advertise_origins(std::span<const std::string_view> origins) {
    if (origins_ == nullptr) {
        throw std::logic_error("this HTTP version does not support ORIGIN frames");
    }
    if (scope_.has_pending_operations()) {
        throw std::logic_error("connection advertisement output is already active");
    }
    if (origins.size() > max_http_header_bytes / 2) {
        throw std::length_error("too many advertised origins");
    }
    auto bytes_value = origins.size() * 2;
    for (const auto origin : origins) {
        if (origin.size() > max_http_header_bytes - std::min(bytes_value, max_http_header_bytes)) {
            throw std::length_error("origin advertisement exceeds its byte bound");
        }
        bytes_value += origin.size();
    }
    std::pmr::vector<std::pmr::string> owned(resource_);
    owned.reserve(origins.size());
    for (const auto origin : origins) {
        owned.emplace_back(origin);
    }
    return ::ruvia::make_scoped_operation(scope_, write_origins(std::move(owned)));
}
scoped_operation<void> http_connection_advertisement_output::advertise_alternative_service(std::string_view value) {
    if (service_ == nullptr) {
        throw std::logic_error("ALTSVC frames require HTTP/2");
    }
    if (scope_.has_pending_operations()) {
        throw std::logic_error("connection advertisement output is already active");
    }
    if (value.size() > max_http_header_bytes) {
        throw std::length_error("alternative service advertisement exceeds its byte bound");
    }
    return ::ruvia::make_scoped_operation(scope_, write_service(std::pmr::string(value, resource_)));
}
task<void> http_connection_advertisement_output::write_origins(std::pmr::vector<std::pmr::string> origins) {
    std::pmr::vector<std::string_view> views(resource_);
    views.reserve(origins.size());
    for (const auto& origin : origins) {
        views.emplace_back(origin);
    }
    co_await origins_(target_, views);
}
task<void> http_connection_advertisement_output::write_service(std::pmr::string value) {
    co_await service_(target_, value);
}
}  // namespace ruvia::detail
