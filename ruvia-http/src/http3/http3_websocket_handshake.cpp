#include "ruvia/http/http3_websocket_handshake.h"

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_var_int.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_response.h"

#include "server/http_date_cache.h"
#include "websocket/http_websocket_handshake_fields.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia {
namespace {

std::pmr::memory_resource* normalized_resource(std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

bool header_name_equals(std::string_view name, std::string_view expected) noexcept {
    return http_ascii_equals_ignore_case(name, expected);
}

}  // namespace

http_protocol_error http3_websocket_handshake_failure::protocol_error() const noexcept {
    switch (kind_) {
        case kind_type::invalid_request:
            return http_protocol_error(http_status::bad_request, "invalid WebSocket handshake");
        case kind_type::unsupported_version:
            return http_protocol_error(http_status::bad_request, "unsupported WebSocket version");
    }
    return http_protocol_error(http_status::bad_request, "invalid WebSocket handshake");
}

void http3_websocket_handshake_failure::apply_required_response_headers(http_response& response) const {
    if (kind_ == kind_type::unsupported_version) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

std::variant<std::monostate, http3_websocket_handshake_failure> validate_http3_websocket_handshake(
    const http_request& request, std::string_view protocol, bool stream_open) noexcept {
    if (!stream_open || request.known_method() != http_known_method::connect ||
        request.protocol_version() != http_protocol_version::http3 ||
        !http_ascii_equals_ignore_case(protocol, "websocket")) {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::invalid_request);
    }

    std::size_t version_count = 0;
    std::string_view version;
    for (const auto& header : request.headers()) {
        const auto name = header.name();
        if (header_name_equals(name, "content-length") ||
            header_name_equals(name, "sec-websocket-key") ||
            header_name_equals(name, "sec-websocket-accept") ||
            header_name_equals(name, "connection") || header_name_equals(name, "upgrade")) {
            return http3_websocket_handshake_failure(
                http3_websocket_handshake_failure::kind_type::invalid_request);
        }
        if (header_name_equals(name, "sec-websocket-version")) {
            ++version_count;
            version = header.value();
        }
    }
    if (version_count != 1 || !detail::websocket_subprotocol_offers_valid(request) ||
        !detail::websocket_extension_offers_valid(request)) {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::invalid_request);
    }
    if (version != "13") {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::unsupported_version);
    }
    return {};
}

std::variant<http3_websocket_handshake, http3_websocket_handshake_failure>
make_http3_websocket_handshake(const http_request& request, std::string_view protocol,
    bool stream_open, http3_websocket_handshake_options options) {
    const auto validation = validate_http3_websocket_handshake(request, protocol, stream_open);
    if ((validation.index() != 0)) {
        return std::get<1>(validation);
    }

    auto* resource = normalized_resource(options.resource_);
    auto negotiation = detail::make_websocket_server_negotiation(request, {
                                                                              .supported_subprotocols_ = options.supported_subprotocols_,
                                                                              .response_headers_ = options.response_headers_,
                                                                              .resource_ = resource,
                                                                              .deflate_ = options.deflate_,
                                                                          });

    http3_websocket_handshake result(resource);
    result.subprotocol_.assign(negotiation.subprotocol());
    result.compression_ = negotiation.compression();

    const auto application_headers = negotiation.response_headers();
    const bool application_date = std::ranges::any_of(application_headers,
        [](const http_header& header_value) { return header_name_equals(header_value.name(), "date"); });
    const auto date = options.date_.empty() ? detail::cached_date_value() : options.date_;
    if (!date.empty() && !is_valid_http_header_value(date)) {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::invalid_request);
    }

    std::pmr::vector<http3_field_section_field_view> fields(resource);
    fields.reserve(3 + static_cast<std::size_t>(!negotiation.subprotocol().empty()) +
                   static_cast<std::size_t>(!negotiation.extensions().empty()) + application_headers.size());
    fields.emplace_back(":status", "200");
    if (!date.empty() && !application_date) {
        fields.emplace_back("date", date);
    }
    if (!negotiation.subprotocol().empty()) {
        fields.emplace_back("sec-websocket-protocol", negotiation.subprotocol());
    }
    if (!negotiation.extensions().empty()) {
        fields.emplace_back("sec-websocket-extensions", negotiation.extensions());
    }
    for (const auto& header : application_headers) {
        fields.emplace_back(header.name(), header.value());
    }

    auto section = encode_http3_field_section(fields, resource);
    if ((section.index() != 0)) {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::invalid_request);
    }
    constexpr auto header_capacity = 2 * http3_var_int_max_bytes;
    if (std::get<0>(section).size() > std::numeric_limits<std::size_t>::max() - header_capacity) {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::invalid_request);
    }
    result.headers_frame_.resize(header_capacity + std::get<0>(section).size());
    const auto encoded = encode_http3_frame(result.headers_frame_,
        static_cast<std::uint64_t>(http3_frame_type::headers), std::get<0>(section));
    if ((encoded.index() != 0)) {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::invalid_request);
    }
    result.headers_frame_.resize(std::get<0>(encoded));
    return result;
}

}  // namespace ruvia
