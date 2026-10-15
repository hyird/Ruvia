#include "ruvia/http/http3_websocket_handshake.h"

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_response.h"

#include "http3/http3_field_section_encoder.h"
#include "server/http_date_cache.h"
#include "websocket/http_websocket_handshake_fields.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia {
namespace {

std::pmr::memory_resource* normalized_resource(std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
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
        // RFC 9220 / RFC 8441 section 5: extended CONNECT carries no content
        // and neither the HTTP/1.1 key exchange nor connection-specific fields.
        if (detail::http_field_name_in(name, detail::http_message_framing_fields) ||
            detail::http_field_name_in(name, detail::http_connection_specific_fields) ||
            detail::http_field_name_in(name, detail::http_websocket_key_exchange_fields)) {
            return http3_websocket_handshake_failure(
                http3_websocket_handshake_failure::kind_type::invalid_request);
        }
        if (detail::classify_request_header(name) == detail::request_header_kind::sec_websocket_version) {
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

    const auto subprotocol = negotiation.subprotocol();
    const auto compression = negotiation.compression();

    const auto application_headers = negotiation.response_headers();
    const bool application_date = std::ranges::any_of(application_headers,
        [](const http_header& header_value) { return http_ascii_equals_ignore_case(header_value.name(), "date"); });
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

    detail::http_header_section_size section_size(std::numeric_limits<std::size_t>::max());
    for (const auto& field : fields) {
        if (!section_size.add(field.name_, field.value_)) {
            return http3_websocket_handshake_failure(
                http3_websocket_handshake_failure::kind_type::invalid_request);
        }
    }
    constexpr auto unlimited = std::numeric_limits<std::size_t>::max();
    auto encoded = detail::http3_headers_frame_access::encode(fields, resource,
        {.max_encoded_bytes_ = unlimited, .max_decoded_bytes_ = unlimited, .max_fields_ = unlimited}, nullptr, 0,
        section_size.bytes());
    if ((encoded.index() != 0)) {
        return http3_websocket_handshake_failure(
            http3_websocket_handshake_failure::kind_type::invalid_request);
    }
    http3_websocket_handshake result(resource, std::move(std::get<0>(encoded)));
    result.subprotocol_.assign(subprotocol);
    result.compression_ = compression;
    return result;
}

}  // namespace ruvia
