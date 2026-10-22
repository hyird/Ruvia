#include "ruvia/http/websocket_client_negotiation.h"

#include <stdexcept>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/websocket_subprotocol_set.h"

#include "websocket/http_websocket_permessage_deflate.h"

namespace ruvia {
namespace {
using error_type = websocket_client_negotiation_error;
std::optional<websocket_compression> parse_response_extension(std::string_view text, websocket_client_deflate_offer offer) {
    if (!offer.enabled_) {
        return {};
    }
    const auto params = detail::parse_permessage_deflate_params(text);
    if (!params.has_value()) {
        return {};
    }
    // A response must give client_max_window_bits a value, and may only send it
    // when the offer included it (RFC 7692 section 7.1.2.2).
    if (params->client_max_window_bits_present_ &&
        (!params->client_max_window_bits_.has_value() ||
            (!offer.offer_client_max_window_bits_ && !offer.client_max_window_bits_))) {
        return {};
    }
    websocket_compression result_value{.enabled_ = true,
        .server_no_context_takeover_ = params->server_no_context_takeover_,
        // The client's own takeover opt-out binds its deflater even when the server does not echo it.
        .client_no_context_takeover_ = offer.client_no_context_takeover_ || params->client_no_context_takeover_,
        .server_max_window_bits_ = params->server_max_window_bits_,
        .client_max_window_bits_ = params->client_max_window_bits_};
    if (offer.server_no_context_takeover_ && !result_value.server_no_context_takeover_) {
        return {};
    }
    if (offer.server_max_window_bits_ && (!result_value.server_max_window_bits_ || *result_value.server_max_window_bits_ > *offer.server_max_window_bits_)) {
        return {};
    }
    // An offered client window binds this client even when the response omits
    // it or selects a larger one (RFC 7692 section 7.1.2.2).
    if (offer.client_max_window_bits_ &&
        (!result_value.client_max_window_bits_ || *result_value.client_max_window_bits_ > *offer.client_max_window_bits_)) {
        result_value.client_max_window_bits_ = offer.client_max_window_bits_;
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
        const auto kind = detail::classify_request_header(n);
        if (kind == detail::request_header_kind::sec_websocket_protocol) {
            ++selected;
            result.selected_subprotocol_ = http_trim_ows(v);
            bool found = false;
            for (const auto& protocol : protocols) {
                found = found || protocol == result.selected_subprotocol_;
            }
            if (selected > 1 || !found) {
                return error_type::subprotocol;
            }
        } else if (kind == detail::request_header_kind::sec_websocket_extensions) {
            ++extensions;
            if (extensions > 1) {
                return error_type::extensions;
            }
            auto negotiation = parse_response_extension(http_trim_ows(v), offer);
            if (!negotiation) {
                return error_type::extensions;
            }
            result.compression_ = *negotiation;
        } else if (extended && (detail::http_field_name_in(n, detail::http_websocket_key_exchange_fields) ||
                                   kind == detail::request_header_kind::upgrade ||
                                   kind == detail::request_header_kind::connection)) {
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
    websocket_subprotocol_set seen;
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
    // The generated Sec-WebSocket-* fields alone must fit the field section;
    // the handshake owner budgets the application headers it accepts.
    http_field_section_budget budget;
    auto append = [&](std::string_view name, std::string_view value, std::size_t value_bytes) {
        if (!budget.add_field(name.size(), value_bytes)) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
        field(name, value, value_bytes);
    };
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
        const detail::websocket_compression_extension encoded(parameters);
        constexpr std::size_t client_window_bits_suffix_bytes = sizeof("; client_max_window_bits") - 1;
        const std::size_t offer_bytes = encoded.view().size() +
                                        ((config.deflate_.offer_client_max_window_bits_ && !config.deflate_.client_max_window_bits_)
                                                ? client_window_bits_suffix_bytes
                                                : 0);
        append("Sec-WebSocket-Extensions", encoded.view(), offer_bytes);
    }
}
}  // namespace
void websocket_client_negotiation::validate_configuration(websocket_client_negotiation_config_view config) {
    validate_and_visit_config(config, [](std::string_view) {}, [](std::string_view, std::string_view, std::size_t) {});
}
websocket_client_negotiation::websocket_client_negotiation(websocket_client_negotiation_config_view config, std::pmr::memory_resource* resource)
    : deflate_(config.deflate_),
      protocols_(resource ? resource : std::pmr::get_default_resource()),
      fields_(protocols_.get_allocator().resource()) {
    auto* const memory = protocols_.get_allocator().resource();
    std::pmr::string joined(memory);
    validate_and_visit_config(config, [&](std::string_view protocol_view) {
            protocols_.emplace_back(protocol_view);
            if (!joined.empty()) {
                joined.append(", ");
            }
            joined.append(protocol_view); }, [&](std::string_view name, std::string_view value, std::size_t value_bytes) {
            if (name == "Sec-WebSocket-Protocol") {
                fields_.push_back(http_header::copy_with_lowercase_name(name, joined, memory));
            } else if (name == "Sec-WebSocket-Extensions" && value_bytes > value.size()) {
                std::pmr::string offer(value, memory);
                offer.append("; client_max_window_bits");
                fields_.push_back(http_header::copy_with_lowercase_name(name, offer, memory));
            } else {
                fields_.push_back(http_header::copy_with_lowercase_name(name, value, memory));
            } });
}
std::variant<websocket_client_negotiation_result_view, error_type> websocket_client_negotiation::validate_fields(std::span<const http_header> fields_value) const {
    return validate(fields_value, [](const auto& field) { return field.name(); }, [](const auto& field) { return field.value(); }, protocols_, deflate_, false);
}
std::variant<std::uint32_t, http2_request_head_submit_error> websocket_client_negotiation::submit_http2_request(
    http2_connection& connection, std::string_view scheme, std::string_view authority,
    std::string_view target, std::span<const http_header_view> application_headers) const {
    std::pmr::vector<http_header_view> headers(protocols_.get_allocator().resource());
    headers.reserve(fields_.size() + application_headers.size());
    for (const auto& f : fields_) {
        headers.emplace_back(f.name(), f.value());
    }
    headers.insert(headers.end(), application_headers.begin(), application_headers.end());
    return connection.submit_request_head(http2_extended_connect_request_head_view{
        .protocol_ = "websocket",
        .scheme_ = scheme,
        .authority_ = authority,
        .target_ = target,
        .headers_ = headers,
    });
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
}  // namespace ruvia
