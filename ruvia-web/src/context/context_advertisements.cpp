#include <algorithm>
#include <stdexcept>

#include "ruvia/http/http_limits.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "context/http_connection_advertisement_output.h"

namespace ruvia {
void context::advertise_origins(std::span<const std::string_view> origins) {
    if (services().connection_advertisements() == nullptr || conn_info_.tls() == nullptr) {
        throw std::logic_error("ORIGIN requires an HTTPS connection with advertisement output");
    }
    services().connection_advertisements()->advertise_origins(origins);
}
void context::advertise_alternative_service(std::string_view value) {
    if (services().connection_advertisements() == nullptr) {
        throw std::logic_error("this dispatch has no connection advertisement output");
    }
    services().connection_advertisements()->advertise_alternative_service(value);
}
}  // namespace ruvia

namespace ruvia::detail {
void http_connection_advertisement_output::advertise_origins(std::span<const std::string_view> origins) {
    if (origins_ == nullptr) {
        throw std::logic_error("this HTTP version does not support ORIGIN frames");
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
    origins_(target_, origins);
}
void http_connection_advertisement_output::advertise_alternative_service(std::string_view value) {
    if (service_ == nullptr) {
        throw std::logic_error("ALTSVC frames require HTTP/2");
    }
    if (value.size() > max_http_header_bytes) {
        throw std::length_error("alternative service advertisement exceeds its byte bound");
    }
    service_(target_, value);
}
}  // namespace ruvia::detail
