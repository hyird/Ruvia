#pragma once

#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_deflate_codecs.h"
#include "ruvia/http/websocket_protocol.h"

#include "coding/http_zlib_stream.h"
#include "request/http_request_access.h"
#include "websocket/http_websocket_handshake_fields.h"
#include "websocket/websocket_deflate_message.h"

namespace ruvia::detail {

// RFC 7692 codec state of one connection. A direction negotiated without
// context takeover starts every message from an empty dictionary, so it uses
// the borrowed shared codec set when the owner supplies one and then holds no
// codec memory. Every other direction owns a private zlib stream from the
// connection's resource, created on its first compressed message and kept,
// with its dictionary, until the connection ends. The owner validates the
// level (0-9), memLevel (1-9) and windows (8-15).
class websocket_deflate final {
public:
    websocket_deflate(std::pmr::memory_resource* resource, websocket_deflate_codecs* shared, int compression_level,
        int mem_level, bool send_takeover, bool receive_takeover, int send_window, int receive_window) noexcept
        : resource_(resource),
          shared_(shared),
          compression_level_(compression_level),
          mem_level_(mem_level),
          send_window_(send_window),
          receive_window_(receive_window),
          send_takeover_(send_takeover),
          receive_takeover_(receive_takeover) {}

    websocket_deflate(const websocket_deflate&) = delete;
    websocket_deflate& operator=(const websocket_deflate&) = delete;

    // Appends the compressed message to `out`. false reports a codec failure;
    // allocation exceptions propagate.
    bool compress(std::string_view input, std::pmr::string& out) {
        if (!send_takeover_ && shared_ != nullptr) {
            // Reset after every call, including failures and exceptions.
            return shared_->deflate_message(input, out, compression_level_, send_window_);
        }
        if (deflater_ == nullptr) {
            deflater_ = make_http_pmr_object<zlib_deflate_stream>(resource_, resource_, compression_level_,
                websocket_deflate_zlib_window_bits(send_window_), mem_level_,
                websocket_deflate_zlib_strategy(send_window_));
            if (!deflater_->initialized()) {
                deflater_.reset();
                return false;
            }
        } else if (!send_takeover_ && deflateReset(&deflater_->native()) != Z_OK) {
            deflater_->rethrow_allocation_failure();
            return false;
        }
        return websocket_deflate_message(*deflater_, input, out);
    }

    websocket_inflate_result decompress(
        std::string_view input, std::pmr::string& out, protocol_byte_limit message_limit) {
        if (!receive_takeover_ && shared_ != nullptr) {
            // The shared inflater has a 15-bit window, which also inflates
            // every smaller negotiated window.
            return shared_->inflate_message(input, out, message_limit);
        }
        if (inflater_ == nullptr) {
            inflater_ = make_http_pmr_object<zlib_inflate_stream>(resource_, resource_, -receive_window_);
            if (!inflater_->initialized()) {
                inflater_.reset();
                return websocket_inflate_result::error;
            }
        } else if (!receive_takeover_ && inflateReset(&inflater_->native()) != Z_OK) {
            inflater_->rethrow_allocation_failure();
            return websocket_inflate_result::error;
        }
        return websocket_inflate_message(*inflater_, input, out, message_limit);
    }

    // A trial that is not sent must not enter the peer's future dictionary.
    // Starting the next compressed message with an empty sender dictionary is
    // legal even when the receiver retains its prior dictionary (RFC 7692).
    // The shared codecs already reset after every message.
    void discard_compression() {
        if (deflater_ != nullptr && deflateReset(&deflater_->native()) != Z_OK) {
            deflater_->rethrow_allocation_failure();
            throw std::runtime_error("failed to reset discarded WebSocket compression");
        }
    }

private:
    template <typename stream_type>
    using private_stream_type = std::unique_ptr<stream_type, http_pmr_object_deleter<stream_type>>;

    std::pmr::memory_resource* resource_;
    websocket_deflate_codecs* shared_;
    private_stream_type<zlib_deflate_stream> deflater_;
    private_stream_type<zlib_inflate_stream> inflater_;
    int compression_level_;
    int mem_level_;
    int send_window_;
    int receive_window_;
    bool send_takeover_;
    bool receive_takeover_;
};

[[nodiscard]] constexpr bool websocket_deflate_negotiated(websocket_compression negotiation) noexcept {
    return negotiation.enabled_;
}

// Decode RFC 7692 window parameters (8 through 15), including quoted tokens.
// The negotiated send and receive bounds are independent of takeover policy.
[[nodiscard]] inline std::optional<int> websocket_deflate_window_bits(
    std::string_view value) noexcept {
    value = http_trim_ows(value);
    bool quoted = false;
    if (!value.empty() && value.front() == '"') {
        if (value.size() < 2 || value.back() != '"') {
            return std::nullopt;
        }
        quoted = true;
        value.remove_prefix(1);
        value.remove_suffix(1);
    }

    int parsed_value = 0;
    std::size_t digits = 0;
    bool leading_zero = false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto ch = value[i];
        if (quoted && ch == '\\') {
            if (++i == value.size()) {
                return std::nullopt;
            }
            ch = value[i];
        } else if (ch == '"') {
            return std::nullopt;
        }
        if (ch < '0' || ch > '9' || ++digits > 2) {
            return std::nullopt;
        }
        if (digits == 1) {
            leading_zero = ch == '0';
        } else if (leading_zero) {
            // RFC 7692 section 7.1.2 defines both max-window-bits
            // parameters as decimal integers without leading zeroes. Apply
            // that grammar after quoted-pair decoding as well as to tokens.
            return std::nullopt;
        }
        parsed_value = parsed_value * 10 + (ch - '0');
    }
    return digits != 0 && parsed_value >= 8 && parsed_value <= 15 ? std::optional<int>(parsed_value) : std::nullopt;
}

// RFC 7692 section 7.1 parameters of one permessage-deflate extension element.
// client_max_window_bits may appear without a value; whether that is
// acceptable depends on the role, so its presence is kept apart from its value.
struct permessage_deflate_params final {
    bool server_no_context_takeover_{false};
    bool client_no_context_takeover_{false};
    std::optional<int> server_max_window_bits_;
    bool client_max_window_bits_present_{false};
    std::optional<int> client_max_window_bits_;
};

// Grammar shared by offers and responses: every parameter is defined, appears
// at most once and carries a value exactly where RFC 7692 defines one (an
// undefined or malformed parameter cannot be negotiated). Returns nullopt for
// another extension or a malformed element; role policy is left to callers.
[[nodiscard]] inline std::optional<permessage_deflate_params> parse_permessage_deflate_params(
    std::string_view element) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(element, 0, ';');
    const auto name = http_trim_ows(element.substr(0, first_semicolon));
    if (!http_ascii_equals_ignore_case(name, "permessage-deflate")) {
        return std::nullopt;
    }

    permessage_deflate_params params;
    std::size_t start = first_semicolon;
    while (start < element.size()) {
        ++start;
        const auto end = start + http_find_unquoted_delimiter(element.substr(start), 0, ';');
        const auto parameter = http_trim_ows(element.substr(start, end - start));
        const auto equals = parameter.find('=');
        const bool has_value = equals != std::string_view::npos;
        const auto parameter_name = http_trim_ows(parameter.substr(0, equals));
        const auto parameter_value =
            has_value ? http_trim_ows(parameter.substr(equals + 1)) : std::string_view{};

        if (http_ascii_equals_ignore_case(parameter_name, "server_no_context_takeover")) {
            if (params.server_no_context_takeover_ || has_value) {
                return std::nullopt;
            }
            params.server_no_context_takeover_ = true;
        } else if (http_ascii_equals_ignore_case(parameter_name, "client_no_context_takeover")) {
            if (params.client_no_context_takeover_ || has_value) {
                return std::nullopt;
            }
            params.client_no_context_takeover_ = true;
        } else if (http_ascii_equals_ignore_case(parameter_name, "server_max_window_bits")) {
            // A missing or empty value fails window decoding.
            if (params.server_max_window_bits_.has_value()) {
                return std::nullopt;
            }
            params.server_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
            if (!params.server_max_window_bits_.has_value()) {
                return std::nullopt;
            }
        } else if (http_ascii_equals_ignore_case(parameter_name, "client_max_window_bits")) {
            if (params.client_max_window_bits_present_) {
                return std::nullopt;
            }
            params.client_max_window_bits_present_ = true;
            if (has_value) {
                params.client_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                if (!params.client_max_window_bits_.has_value()) {
                    return std::nullopt;
                }
            }
        } else {
            // Includes an empty parameter, such as a trailing ';'.
            return std::nullopt;
        }

        start = end;
    }
    return params;
}

// Server role: an offered client_max_window_bits, with or without a value, is
// accepted but never echoed in the response.
[[nodiscard]] inline std::optional<websocket_compression> websocket_parse_deflate_offer(
    std::string_view offer, bool context_takeover) noexcept {
    const auto params = parse_permessage_deflate_params(offer);
    if (!params.has_value()) {
        return std::nullopt;
    }
    websocket_compression accepted{.enabled_ = true, .server_max_window_bits_ = params->server_max_window_bits_};
    if (context_takeover && !params->server_no_context_takeover_ && !params->client_no_context_takeover_) {
        accepted.server_no_context_takeover_ = false;
        accepted.client_no_context_takeover_ = false;
    }
    return accepted;
}

[[nodiscard]] inline websocket_compression websocket_scan_deflate_offers(
    std::string_view offers, bool context_takeover) noexcept {
    std::optional<websocket_compression> accepted;
    http_visit_comma_separated_quoted_field_items(offers, [&accepted, context_takeover](std::string_view offer) noexcept {
        if (offer.empty()) {
            return true;
        }
        accepted = websocket_parse_deflate_offer(offer, context_takeover);
        return !accepted.has_value();
    });
    return accepted.value_or((websocket_compression{}));
}

[[nodiscard]] inline websocket_compression websocket_negotiate_permessage_deflate(
    const http_request& request, websocket_deflate_config config) {
    if (config.compression_level_ < 0 || config.compression_level_ > 9) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
    }
    if (config.mem_level_ < 1 || config.mem_level_ > 9) {
        throw std::invalid_argument("WebSocket deflate memory level must be between 1 and 9");
    }
    if (!config.enabled_ || !websocket_extension_offers_valid(request)) {
        return (websocket_compression{});
    }
    // RFC 6455 §9.1: extension declarations may be split across multiple
    // Sec-websocket-Extensions field lines, which RFC 9110 §5.3 makes equivalent to
    // one comma-joined list. request.header() returns only the last line, so scan
    // every line in order and honor the first acceptable offer. Offers are resolved
    // independently (first honorable wins), so first-honorable-across-lines is the
    // same result as scanning the joined list.
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (http_request_access::header_kind(request, i) !=
            static_cast<std::uint8_t>(request_header_kind::sec_websocket_extensions)) {
            continue;
        }
        const auto negotiation = websocket_scan_deflate_offers(headers[i].value(), config.context_takeover_);
        if (websocket_deflate_negotiated(negotiation)) {
            return negotiation;
        }
    }
    return (websocket_compression{});
}

}  // namespace ruvia::detail
