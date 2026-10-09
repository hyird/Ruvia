#include "ruvia/http/websocket_client_negotiation.h"

#include <array>
#include <charconv>
#include <stdexcept>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_limits.h"

#include "http_header_access.h"
#include "websocket/http_websocket_permessage_deflate.h"
#include "websocket/websocket_subprotocol_set.h"

namespace ruvia {
namespace {
using error_type = websocket_client_negotiation_error;
bool equal(std::string_view a, std::string_view b) noexcept {
    return detail::http_ascii_equals_ignore_case(a, b);
}
bool reserved(std::string_view name) noexcept {
    return name.empty() || name.front() == ':' || equal(name, "host") || equal(name, "connection") || equal(name, "upgrade") || equal(name, "keep-alive") || equal(name, "proxy-connection") || equal(name, "transfer-encoding") || equal(name, "content-length") || equal(name, "trailer") || equal(name, "expect") || equal(name, "sec-websocket-key") || equal(name, "sec-websocket-accept") || equal(name, "sec-websocket-version") || equal(name, "sec-websocket-protocol") || equal(name, "sec-websocket-extensions");
}
std::optional<websocket_compression> parse_response_extension(std::string_view text, websocket_client_deflate_offer offer) {
    if (!offer.enabled_) {
        return {};
    }
    auto first = detail::http_find_unquoted_delimiter(text, 0, ';');
    if (!equal(detail::http_trim_ows(text.substr(0, first)), "permessage-deflate")) {
        return {};
    }
    websocket_compression result_value{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false};
    std::uint8_t seen = 0;
    for (auto start = first; start < text.size();) {
        ++start;
        auto end = detail::http_find_unquoted_delimiter(text, start, ';');
        auto part = detail::http_trim_ows(text.substr(start, end - start));
        auto equals = part.find('=');
        auto name = detail::http_trim_ows(part.substr(0, equals));
        auto value = equals == std::string_view::npos ? std::string_view{} : detail::http_trim_ows(part.substr(equals + 1));
        std::uint8_t bit = 0;
        if (equal(name, "server_no_context_takeover")) {
            bit = 1;
            if (equals != std::string_view::npos) {
                return {};
            }
            result_value.server_no_context_takeover_ = true;
        } else if (equal(name, "client_no_context_takeover")) {
            bit = 2;
            if (equals != std::string_view::npos) {
                return {};
            }
            result_value.client_no_context_takeover_ = true;
        } else if (equal(name, "server_max_window_bits")) {
            bit = 4;
            result_value.server_max_window_bits_ = detail::websocket_deflate_window_bits(value);
            if (!result_value.server_max_window_bits_) {
                return {};
            }
        } else if (equal(name, "client_max_window_bits")) {
            bit = 8;
            result_value.client_max_window_bits_ = detail::websocket_deflate_window_bits(value);
            if (!result_value.client_max_window_bits_ || (!offer.offer_client_max_window_bits_ && !offer.client_max_window_bits_)) {
                return {};
            }
        } else {
            return {};
        }
        if (seen & bit) {
            return {};
        }
        seen |= bit;
        start = end;
    }
    if (offer.server_no_context_takeover_ && !result_value.server_no_context_takeover_) {
        return {};
    }
    if (offer.server_max_window_bits_ && (!result_value.server_max_window_bits_ || *result_value.server_max_window_bits_ > *offer.server_max_window_bits_)) {
        return {};
    }

    return result_value;
}
template <class fields, class name_type, class value_type>
std::variant<websocket_client_negotiation_result_view, error_type> validate(const fields& fields_value, name_type name, value_type value,
    const std::pmr::vector<std::pmr::string>& protocols, websocket_client_deflate_offer offer, bool extended) {
    websocket_client_negotiation_result_view result;
    std::size_t selected = 0, extensions = 0;
    for (const auto& field : fields_value) {
        auto n = name(field);
        auto v = value(field);
        if (equal(n, "sec-websocket-protocol")) {
            ++selected;
            result.selected_subprotocol_ = detail::http_trim_ows(v);
            bool found = false;
            for (const auto& protocol : protocols) {
                found = found || protocol == result.selected_subprotocol_;
            }
            if (selected > 1 || !found) {
                return error_type::subprotocol;
            }
        } else if (equal(n, "sec-websocket-extensions")) {
            ++extensions;
            if (extensions > 1) {
                return error_type::extensions;
            }
            auto negotiation = parse_response_extension(detail::http_trim_ows(v), offer);
            if (!negotiation) {
                return error_type::extensions;
            }
            result.compression_ = *negotiation;
        } else if (extended && (equal(n, "sec-websocket-accept") || equal(n, "sec-websocket-key") || equal(n, "upgrade") || equal(n, "connection"))) {
            return error_type::forbidden_field;
        }
    }
    return result;
}
}  // namespace
namespace {
template <class protocol_type, class field_type>
void validate_and_visit_config(const websocket_client_negotiation_config_view& config,
    protocol_type&& protocol, field_type&& field) {
    auto valid_window = [](std::optional<int> window_bits) {
        return !window_bits || (*window_bits >= 8 && *window_bits <= 15);
    };
    if (!valid_window(config.deflate_.server_max_window_bits_) || !valid_window(config.deflate_.client_max_window_bits_) ||
        (config.deflate_.client_max_window_bits_ && !config.deflate_.offer_client_max_window_bits_)) {
        throw std::invalid_argument("invalid WebSocket client deflate offer");
    }
    detail::websocket_subprotocol_set seen;
    std::size_t joined_bytes = 0;
    for (const auto protocol_view : config.subprotocols_) {
        if (!seen.append(protocol_view)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
        if (joined_bytes != 0) {
            if (joined_bytes > max_http_header_bytes - 2) {
                throw std::invalid_argument("WebSocket subprotocol field too large");
            }
            joined_bytes += 2;
        }
        if (protocol_view.size() > max_http_header_bytes - joined_bytes) {
            throw std::invalid_argument("WebSocket subprotocol field too large");
        }
        joined_bytes += protocol_view.size();
        protocol(protocol_view);
    }
    std::size_t header_bytes = 0;
    auto append = [&](std::string_view name, std::string_view value, std::size_t value_bytes) {
        if (header_bytes > max_http_header_bytes || max_http_header_bytes - header_bytes < 4 ||
            name.size() > max_http_header_bytes - header_bytes - 4 ||
            value_bytes > max_http_header_bytes - header_bytes - 4 - name.size()) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
        header_bytes += name.size() + value_bytes + 4;
        field(name, value, value_bytes);
    };
    if (config.headers_.size() > max_http_header_fields - 3) {
        throw std::invalid_argument("too many WebSocket client headers");
    }
    for (const auto& header : config.headers_) {
        if (reserved(header.name()) || !is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid WebSocket client header");
        }
        append(header.name(), header.value(), header.value().size());
    }
    append("Sec-WebSocket-Version", "13", 2);
    if (!config.subprotocols_.empty()) {
        append("Sec-WebSocket-Protocol", {}, joined_bytes);
    }
    if (config.deflate_.enabled_) {
        websocket_compression parameters{.enabled_ = true,
            .server_no_context_takeover_ = config.deflate_.server_no_context_takeover_,
            .client_no_context_takeover_ = config.deflate_.client_no_context_takeover_,
            .server_max_window_bits_ = config.deflate_.server_max_window_bits_,
            .client_max_window_bits_ = config.deflate_.client_max_window_bits_};
        const auto encoded = detail::get_websocket_compression_extension(parameters);
        constexpr std::size_t k_client_window_bits_suffix_bytes = sizeof("; client_max_window_bits") - 1;
        const std::size_t offer_bytes = encoded.view().size() +
                                        ((config.deflate_.offer_client_max_window_bits_ && !config.deflate_.client_max_window_bits_)
                                                ? k_client_window_bits_suffix_bytes
                                                : 0);
        append("Sec-WebSocket-Extensions", encoded.view(), offer_bytes);
    }
}
}  // namespace
void websocket_client_negotiation::validate_configuration(websocket_client_negotiation_config_view config) {
    validate_and_visit_config(config, [](std::string_view) {}, [](std::string_view, std::string_view, std::size_t) {});
}
websocket_client_negotiation::websocket_client_negotiation(websocket_client_negotiation_config_view config, std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      deflate_(config.deflate_),
      protocols_(resource_),
      fields_(resource_) {
    std::pmr::string joined(resource_);
    validate_and_visit_config(config, [&](std::string_view protocol_view) {
            protocols_.emplace_back(protocol_view);
            if (!joined.empty()) {
                joined.append(", ");
            }
            joined.append(protocol_view); }, [&](std::string_view name, std::string_view value, std::size_t value_bytes) {
            std::pmr::string materialized(value, resource_);
            if (name == "Sec-WebSocket-Protocol") {
                materialized = std::move(joined);
            } else if (name == "Sec-WebSocket-Extensions" && value_bytes > value.size()) {
                materialized.append("; client_max_window_bits");
            }
            std::pmr::string normalized_name(name, resource_);
            for (char& character : normalized_name) {
                character = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(character)));
            }
            fields_.push_back(detail::http_header_access::make(
                std::move(normalized_name), std::move(materialized))); });
}
std::variant<websocket_client_negotiation_result_view, error_type> websocket_client_negotiation::validate_fields(std::span<const http_header> fields_value) const {
    return validate(fields_value, [](const auto& field) { return field.name(); }, [](const auto& field) { return field.value(); }, protocols_, deflate_, false);
}
http2_request_head_submit_result websocket_client_negotiation::submit_http2_request(http2_connection& connection, std::string_view scheme, std::string_view authority, std::string_view target) const {
    std::pmr::vector<http_header_view> headers(resource_);
    for (const auto& f : fields_) {
        headers.emplace_back(f.name(), f.value());
    }
    return connection.submit_request_head(http2_extended_connect_request_head_view{.protocol_ = "websocket", .scheme_ = scheme, .authority_ = authority, .target_ = target, .headers_ = headers});
}
std::variant<http3_client_request_head, http3_client_request_head_failure> websocket_client_negotiation::encode_http3_request(std::string_view scheme, std::string_view authority, std::string_view target, bool peer_enable_connect_protocol, http3_field_section_limits limits) const {
    if (scheme != "https" && scheme != "http") {
        return http3_client_request_head_failure{http3_client_request_head_error::invalid_target};
    }
    std::pmr::vector<http3_field_section_field_view> fields(resource_);
    for (const auto& field : fields_) {
        fields.push_back({field.name(), field.value()});
    }
    return encode_http3_client_request_head({.method_ = "CONNECT", .scheme_ = scheme, .authority_ = authority, .path_ = target, .fields_ = fields, .protocol_ = "websocket", .peer_enable_connect_protocol_ = peer_enable_connect_protocol}, limits, resource_);
}
std::variant<websocket_client_negotiation_result_view, error_type> websocket_client_negotiation::validate_response(const http_client_response_head& head, bool stream_open) const {
    if (head.status().value() < 200 || head.status().value() >= 300) {
        return error_type::response_status;
    }
    if (!stream_open) {
        return error_type::stream_closed;
    }
    return validate(head.headers(), [](const auto& field) { return field.name(); }, [](const auto& field) { return field.value(); }, protocols_, deflate_, true);
}
std::variant<websocket_client_negotiation_result_view, error_type> websocket_client_negotiation::validate_response(const http3_message_head& head, bool stream_open) const {
    if (head.status_ < 200 || head.status_ >= 300) {
        return error_type::response_status;
    }
    if (!stream_open) {
        return error_type::stream_closed;
    }
    return validate(head.headers_, [](const auto& field) -> std::string_view { return field.name_; }, [](const auto& field) -> std::string_view { return field.value_; }, protocols_, deflate_, true);
}
}  // namespace ruvia
