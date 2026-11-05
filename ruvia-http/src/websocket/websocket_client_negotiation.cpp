#include "ruvia/http/websocket_client_negotiation.h"

#include <array>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/websocket_handshake.h"

#include "websocket/http_websocket_permessage_deflate.h"

namespace ruvia {
namespace {
using error_type = websocket_client_handshake_error;
std::optional<websocket_compression> parse_response_extension(std::string_view text, const websocket_client_deflate_offer& deflate) {
    const auto& offer = deflate.compression_;
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
            (!deflate.offer_client_max_window_bits_ && !offer.client_max_window_bits_))) {
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
[[nodiscard]] bool subprotocol_offered(
    std::string_view offered_list, std::string_view selected) noexcept {
    if (selected.empty()) {
        return false;
    }
    return !detail::http_find_header_token(offered_list, [selected](std::string_view token) noexcept {
        return token == selected;
    }).empty();
}

std::variant<websocket_client_negotiation_result_view, error_type> validate(std::span<const http_header> fields_value,
    std::string_view offered_subprotocols, const websocket_client_deflate_offer& offer, bool extended) {
    websocket_client_negotiation_result_view result;
    std::size_t selected = 0, extensions = 0;
    for (const auto& field : fields_value) {
        const auto kind = detail::classify_request_header(field.name());
        if (kind == detail::request_header_kind::sec_websocket_protocol) {
            ++selected;
            result.selected_subprotocol_ = http_trim_ows(field.value());
            if (selected > 1 || !subprotocol_offered(offered_subprotocols, result.selected_subprotocol_)) {
                return error_type::subprotocol;
            }
        } else if (kind == detail::request_header_kind::sec_websocket_extensions) {
            ++extensions;
            if (extensions > 1) {
                return error_type::extensions;
            }
            auto negotiation = parse_response_extension(http_trim_ows(field.value()), offer);
            if (!negotiation) {
                return error_type::extensions;
            }
            result.compression_ = *negotiation;
        } else if (extended && detail::is_forbidden_http_binary_response_field(field.name())) {
            // Connection-specific fields are malformed in HTTP/2 (RFC 9113
            // section 8.2.2). Sec-WebSocket-Accept is superseded by :protocol
            // and therefore not processed (RFC 8441 section 5).
            return error_type::forbidden_field;
        }
    }
    return result;
}

inline constexpr std::string_view version_header_name = "sec-websocket-version";
inline constexpr std::string_view version_storage = "sec-websocket-version13";
inline constexpr std::string_view protocol_header_name = "sec-websocket-protocol";
inline constexpr std::string_view extensions_header_name = "sec-websocket-extensions";
inline constexpr std::string_view client_window_bits_suffix = "; client_max_window_bits";

[[nodiscard]] const std::string_view* static_deflate_extension_storage(
    const websocket_client_deflate_offer& deflate) noexcept {
    const auto& offer = deflate.compression_;
    if (!offer.enabled_ || offer.server_max_window_bits_.has_value() || offer.client_max_window_bits_.has_value()) {
        return nullptr;
    }
    static constexpr std::array<std::string_view, 8> table{
        "sec-websocket-extensionspermessage-deflate",
        "sec-websocket-extensionspermessage-deflate; client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; client_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_max_window_bits",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover",
        "sec-websocket-extensionspermessage-deflate; server_no_context_takeover; "
        "client_no_context_takeover; client_max_window_bits",
    };
    const std::size_t index = (offer.server_no_context_takeover_ ? 4U : 0U) |
                              (offer.client_no_context_takeover_ ? 2U : 0U) |
                              (deflate.offer_client_max_window_bits_ ? 1U : 0U);
    return &table[index];
}

// The validated request-field plan of one client configuration.
struct client_offer_plan final {
    // Bytes of the comma-joined Sec-WebSocket-Protocol value; zero without subprotocols.
    std::size_t subprotocol_bytes_{0};
    // Name-prefixed static storage of a parameterless deflate offer.
    const std::string_view* static_extension_{nullptr};
    // A deflate offer carrying window parameters.
    std::optional<detail::websocket_compression_extension> encoded_extension_;
    // The offer lists client_max_window_bits without a value after encoded_extension_.
    bool client_window_bits_suffix_{false};
};

[[nodiscard]] client_offer_plan make_client_offer_plan(const websocket_client_negotiation_config_view& config) {
    const auto& offer = config.deflate_.compression_;
    auto valid_window = [](std::optional<int> window_bits) {
        return !window_bits || (*window_bits >= 8 && *window_bits <= 15);
    };
    if (!valid_window(offer.server_max_window_bits_) || !valid_window(offer.client_max_window_bits_) ||
        (offer.client_max_window_bits_ && !config.deflate_.offer_client_max_window_bits_)) {
        throw std::invalid_argument("invalid WebSocket client deflate offer");
    }
    validate_websocket_deflater_config(config.deflate_.deflater_);
    client_offer_plan plan;
    websocket_subprotocol_set seen;
    for (const auto protocol_view : config.subprotocols_) {
        if (!seen.append(protocol_view)) {
            throw std::invalid_argument("invalid WebSocket subprotocol");
        }
        if (plan.subprotocol_bytes_ != 0) {
            if (plan.subprotocol_bytes_ > max_http_header_bytes - 2) {
                throw std::invalid_argument("WebSocket subprotocol field too large");
            }
            plan.subprotocol_bytes_ += 2;
        }
        if (protocol_view.size() > max_http_header_bytes - plan.subprotocol_bytes_) {
            throw std::invalid_argument("WebSocket subprotocol field too large");
        }
        plan.subprotocol_bytes_ += protocol_view.size();
    }
    // The generated Sec-WebSocket-* fields alone must fit the field section;
    // the handshake owner budgets the application headers it accepts.
    http_field_section_budget budget;
    auto add = [&budget](std::string_view name, std::size_t value_bytes) {
        if (!budget.add_field(name.size(), value_bytes)) {
            throw std::invalid_argument("WebSocket client fields too large");
        }
    };
    add(version_header_name, 2);
    if (plan.subprotocol_bytes_ != 0) {
        add(protocol_header_name, plan.subprotocol_bytes_);
    }
    if (offer.enabled_) {
        plan.static_extension_ = static_deflate_extension_storage(config.deflate_);
        if (plan.static_extension_ != nullptr) {
            add(extensions_header_name, plan.static_extension_->size() - extensions_header_name.size());
        } else {
            const auto& encoded = plan.encoded_extension_.emplace(offer);
            plan.client_window_bits_suffix_ =
                config.deflate_.offer_client_max_window_bits_ && !offer.client_max_window_bits_;
            add(extensions_header_name,
                encoded.view().size() + (plan.client_window_bits_suffix_ ? client_window_bits_suffix.size() : 0));
        }
    }
    return plan;
}

[[nodiscard]] http_header make_subprotocol_field(
    std::span<const std::string_view> subprotocols, std::size_t joined_bytes, std::pmr::memory_resource* memory) {
    if (subprotocols.size() == 1U) {
        return http_header(protocol_header_name, subprotocols.front(), memory);
    }
    static constexpr std::size_t inline_joined_capacity = 256;
    if (joined_bytes <= inline_joined_capacity) {
        std::array<char, inline_joined_capacity> joined_buffer;
        std::size_t offset = 0;
        for (const auto protocol_view : subprotocols) {
            if (offset != 0) {
                joined_buffer[offset++] = ',';
                joined_buffer[offset++] = ' ';
            }
            std::memcpy(joined_buffer.data() + offset, protocol_view.data(), protocol_view.size());
            offset += protocol_view.size();
        }
        return http_header(protocol_header_name, std::string_view(joined_buffer.data(), offset), memory);
    }
    std::pmr::string joined(memory);
    joined.reserve(joined_bytes);
    for (const auto protocol_view : subprotocols) {
        if (!joined.empty()) {
            joined.append(", ");
        }
        joined.append(protocol_view);
    }
    return http_header(protocol_header_name, joined, memory);
}

// The static offer (a private, storage-borrowing http_header) is built by the
// constructor; this handles the encoded offers that need owned storage.
[[nodiscard]] http_header make_extension_field(const client_offer_plan& plan, std::pmr::memory_resource* memory) {
    const auto encoded = plan.encoded_extension_->view();
    if (!plan.client_window_bits_suffix_) {
        return http_header(extensions_header_name, encoded, memory);
    }
    std::array<char, 128> offer_buffer;
    std::memcpy(offer_buffer.data(), encoded.data(), encoded.size());
    std::memcpy(offer_buffer.data() + encoded.size(), client_window_bits_suffix.data(), client_window_bits_suffix.size());
    return http_header(extensions_header_name,
        std::string_view(offer_buffer.data(), encoded.size() + client_window_bits_suffix.size()), memory);
}
}  // namespace
void websocket_client_negotiation::validate_configuration(websocket_client_negotiation_config_view config) {
    (void)make_client_offer_plan(config);
}
websocket_client_negotiation::websocket_client_negotiation(
    websocket_client_negotiation_config_view config, std::pmr::memory_resource* resource)
    : resource_(detail::http_pmr_resource_or_default(resource)),
      deflate_(config.deflate_) {
    const auto plan = make_client_offer_plan(config);
    fields_[field_count_++] = http_header(version_storage.data(), version_header_name.size(), 2);
    if (plan.subprotocol_bytes_ != 0) {
        fields_[field_count_] = make_subprotocol_field(config.subprotocols_.view(), plan.subprotocol_bytes_, resource_);
        offered_subprotocols_ = fields_[field_count_++].value();
    }
    if (config.deflate_.compression_.enabled_) {
        fields_[field_count_++] = plan.static_extension_ != nullptr
                                      ? http_header(plan.static_extension_->data(), extensions_header_name.size(),
                                            plan.static_extension_->size() - extensions_header_name.size())
                                      : make_extension_field(plan, resource_);
    }
}
std::variant<websocket_client_negotiation_result_view, error_type>
websocket_client_negotiation::validate_fields(std::span<const http_header> fields_value) const {
    return detail::validate_websocket_client_selection(*this, fields_value, false);
}

namespace detail {
std::variant<websocket_client_negotiation_result_view, error_type> validate_websocket_client_selection(
    const websocket_client_negotiation& negotiation, std::span<const http_header> fields, bool extended) {
    return validate(fields, negotiation.offered_subprotocols_, negotiation.deflate_, extended);
}
}  // namespace detail
}  // namespace ruvia
