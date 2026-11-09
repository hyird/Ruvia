#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/http/http_limits.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "context/http_connection_advertisement_output.h"
#include "context/http_interim_response_output.h"
#include "context/http_push_output.h"
#include "http/inline_http_header_views.h"
#include "util/operation_lane_lease.h"
namespace ruvia {
void context::advertise_origins(std::span<const std::string_view> origins) {
    if (services().connection_advertisements() == nullptr || conn().tls() == nullptr) {
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
bool context::push(http_push_request_view request) {
    if (services().push_output() == nullptr) {
        throw std::logic_error("server push requires an HTTP/2 request");
    }
    return services().push_output()->push(request);
}
task<void> context::inform(const http_interim_response_head& response) {
    if (services().interim_output() == nullptr) {
        throw std::logic_error("this dispatch has no transport response output");
    }
    return services().interim_output()->inform(response);
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
task<void> http_interim_response_output::inform(const http_interim_response_head& response) {
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    std::pmr::vector<http_header> owned(resource_);
    owned.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        owned.emplace_back(field.name(), field.value(), resource_);
    }
    return ::ruvia::make_scoped_operation(scope_, write_owned(response.status(), std::move(owned)));
}
task<void> http_interim_response_output::write_owned(
    http_status_code status, std::pmr::vector<http_header> fields_value) {
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    const detail::inline_http_header_views<16> active_views(fields_value, resource_);
    const http_interim_response_head head(status, active_views.view());
    co_await write_(target_, head);
}
task<void> http_interim_response_output::send_continue() {
    if (final_committed_) {
        co_return;
    }
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    co_await write_(target_, http_interim_response_head(http_status::continue_value));
}
}  // namespace ruvia::detail
