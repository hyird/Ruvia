#include "client/http_origin_view.h"

#include <stdexcept>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_number_format.h"

#include "parser/http_request_target.h"

namespace ruvia {
namespace {

void validate_origin_host(std::string_view host) {
    if (host.empty()) {
        throw std::invalid_argument("HTTP origin host must not be empty");
    }
    if (!detail::is_valid_http_host(host)) {
        throw std::invalid_argument("HTTP origin host is invalid");
    }
}

std::uint16_t resolved_origin_port(http_scheme scheme, const http_origin_options& options) noexcept {
    if (options.port_.has_value()) {
        return *options.port_;
    }
    return scheme == http_scheme::https ? std::uint16_t{443} : std::uint16_t{80};
}

}  // namespace

http_origin_view http_origin_view::http(http_origin_options options) {
    const auto host = options.host_.view();
    validate_origin_host(host);
    return http_origin_view(http_scheme::http, host, resolved_origin_port(http_scheme::http, options));
}

http_origin_view http_origin_view::https(http_origin_options options) {
    const auto host = options.host_.view();
    validate_origin_host(host);
    return http_origin_view(
        http_scheme::https, host, resolved_origin_port(http_scheme::https, options));
}

std::pmr::string make_http_origin_authority(const http_origin_view& origin,
    std::pmr::memory_resource* resource) {
    const auto host = origin.host();
    const bool include_port = !detail::http_origin_uses_default_port(origin);
    std::pmr::string authority(detail::http_pmr_resource_or_default(resource));
    authority.reserve(host.size() + (include_port ? 6 : 0));
    authority.append(host);
    if (include_port) {
        authority.push_back(':');
        http_append_unsigned(authority, origin.port());
    }
    return authority;
}

}  // namespace ruvia
