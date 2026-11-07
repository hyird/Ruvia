#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"

#include "http2/http2_request_headers.h"
#include "http2/http2_stream_state.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"

namespace ruvia::detail {

struct http2_request_target_parts final {
    std::string_view path_;
    std::string_view query_string_;
};

[[nodiscard]] inline http2_request_target_parts split_http2_request_target(std::string_view target) noexcept {
    if (target == "*") {
        return http2_request_target_parts{.path_ = "*", .query_string_ = {}};
    }
    const auto query = target.find('?');
    if (query == std::string_view::npos) {
        return http2_request_target_parts{.path_ = target, .query_string_ = {}};
    }
    return http2_request_target_parts{.path_ = target.substr(0, query), .query_string_ = target.substr(query + 1)};
}

// Materializes the validated request head of `stream`; its views borrow stream storage.
[[nodiscard]] inline http_request build_http2_request(
    http2_stream_state& stream, std::pmr::memory_resource* resource, std::string_view body) {
    auto request = http_request_access::make();
    http_request_access::set_resource(request, resource);
    const auto method = stream.request_method();
    if (method.empty()) {
        throw std::logic_error("validated HTTP/2 request cannot be materialized");
    }
    const auto* pending = stream.tunnel().pending();
    const bool standard_connect = pending != nullptr && pending->form() == http2_connect_form::standard;
    const auto target = standard_connect ? stream.request_authority() : stream.request_path();
    if (target.empty() && (standard_connect || !stream.has_path())) {
        throw std::logic_error("validated HTTP/2 request cannot be materialized");
    }
    const auto target_parts = split_http2_request_target(target);

    http_request_access::set_method(request, method);
    http_request_access::set_protocol_version(request, http_protocol_version::http2);
    http_request_access::set_target(request, target);
    http_request_access::set_scheme(request, stream.request_scheme());
    http_request_access::set_authority(request, http2_effective_request_authority(stream));
    http_request_access::set_protocol(request, stream.request_protocol());
    http_request_access::set_target_form(request, ::ruvia::http_request_target_form::http2);
    http_request_access::set_path(request, target_parts.path_);
    http_request_access::set_query_string(request, target_parts.query_string_);
    http_request_access::set_body(request, std::as_bytes(std::span(body.data(), body.size())));

    const auto authority = stream.request_authority();
    const bool synthesize_host = http2_request_synthesizes_host(stream);
    const auto header_count = stream.remote_header_count() + static_cast<std::size_t>(synthesize_host) +
                              static_cast<std::size_t>(stream.has_cookie());
    if (header_count > (std::numeric_limits<std::uint16_t>::max)()) {
        throw std::logic_error("validated HTTP/2 request cannot be materialized");
    }
    http_request_access::reserve_headers(request, header_count);
    for (std::size_t i = 0; i < stream.remote_header_count(); ++i) {
        const auto header_value = stream.remote_header_at(i);
        if (!http_request_access::add_header(request,
                http_header_view{header_value.name_, header_value.value_}, header_value.kind_)) {
            throw std::logic_error("validated HTTP/2 request cannot be materialized");
        }
    }
    // A non-HTTP target can carry RFC 3986 userinfo or another authority
    // value that is not legal Host syntax. Never manufacture an invalid
    // regular field from that distinct pseudo-header grammar.
    if (synthesize_host) {
        if (!http_request_access::add_header(
                request, http_header_view{"host", authority}, request_header_kind::host)) {
            throw std::logic_error("validated HTTP/2 request cannot be materialized");
        }
    }
    if (stream.has_cookie()) {
        if (!http_request_access::add_header(request,
                http_header_view{"cookie", stream.request_cookie()}, request_header_kind::cookie)) {
            throw std::logic_error("validated HTTP/2 request cannot be materialized");
        }
    }
    return request;
}

}  // namespace ruvia::detail
