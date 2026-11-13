#pragma once

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http_field_list_grammar.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_deflate_codecs.h"
#include "ruvia/http/websocket_protocol.h"

#include "coding/http_zlib_stream.h"
#include "field/http_field_kind.h"
#include "memory/http_address_overlap.h"
#include "request/http_request_access.h"
#include "websocket/http_websocket_handshake_fields.h"

// The one RFC 7692 whole-message deflate and inflate loop, shared by
// per-connection codecs and the shared no-context-takeover codec set. Context
// takeover is the owner's decision: these functions neither reset nor keep the
// state between messages.

namespace ruvia::detail {

// zlib cannot emit an 8-bit raw window, so an 8-bit sender window uses zlib's
// 9-bit window restricted to Huffman coding, which never refers back at all.
[[nodiscard]] constexpr int websocket_deflate_zlib_window_bits(int window_bits) noexcept {
    return -std::max(window_bits, 9);
}

[[nodiscard]] constexpr int websocket_deflate_zlib_strategy(int window_bits) noexcept {
    return window_bits == 8 ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY;
}

// Compresses a message, or one fragment of it, up to a sync flush and appends
// the raw-DEFLATE blocks to `out`. At the message end the trailing
// 0x00 0x00 0xFF 0xFF flush marker is removed (RFC 7692 §7.2.1); earlier
// fragments keep it, which the receiver inflates as an empty stored block.
[[nodiscard]] inline bool websocket_deflate_message(zlib_deflate_stream& deflater, std::string_view input,
    std::string& out, bool message_end = true) {
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    auto& stream = deflater.native();
    const zlib_borrow_scope borrow(stream);
    const auto start = out.size();
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[4096];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = deflate(&stream, supplied == input.size() ? Z_SYNC_FLUSH : Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR) {
            deflater.rethrow_allocation_failure();
            return false;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        out.append(buffer, produced);
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == input.size()) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input &&
            !(stream.avail_in == 0 && supplied < input.size())) {
            return false;
        }
    }
    if (!message_end) {
        return true;
    }
    if (out.size() - start >= 4) {
        out.resize(out.size() - 4);
    }
    if (out.size() == start) {
        out.push_back('\0');
    }
    return true;
}

// One inflate pass over `data`; the message limit covers everything appended
// to `out` since `start`.
[[nodiscard]] inline websocket_inflate_result websocket_inflate_chunk(zlib_inflate_stream& inflater, const char* data,
    std::size_t size, std::string& out, std::size_t start, protocol_byte_limit message_limit) {
    auto& stream = inflater.native();
    const zlib_borrow_scope borrow(stream);
    stream.avail_in = 0;
    std::size_t supplied = 0;
    char buffer[8192];
    for (;;) {
        zlib_refill_input(stream, std::string_view(data, size), supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_STREAM_END && status != Z_OK && status != Z_BUF_ERROR) {
            inflater.rethrow_allocation_failure();
            return websocket_inflate_result::error;
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (message_limit.addition_exceeds(out.size() - start, produced)) {
            return websocket_inflate_result::too_large;
        }
        out.append(buffer, produced);
        // RFC 7692 permits BFINAL=1 followed by another DEFLATE stream.
        // Preserve the LZ77 window across streams, including takeover
        // history, while discarding the terminated stream's padding bits.
        if (status == Z_STREAM_END && inflateResetKeep(&stream) != Z_OK) {
            return websocket_inflate_result::error;
        }
        if (stream.avail_out != 0 && stream.avail_in == 0 && supplied == size) {
            break;
        }
        if (produced == 0 && stream.avail_in == before_input &&
            !(stream.avail_in == 0 && supplied < size)) {
            return websocket_inflate_result::error;
        }
    }
    return websocket_inflate_result::ok;
}

// Decompresses a whole message: appends the 0x00 0x00 0xFF 0xFF marker that
// the sender stripped, then raw-inflates into `out`, bounded by one explicit
// message limit across the appended bytes to defuse decompression bombs
// (RFC 7692 §7.2.2).
[[nodiscard]] inline websocket_inflate_result websocket_inflate_message(zlib_inflate_stream& inflater,
    std::string_view input, std::string& out, protocol_byte_limit message_limit) {
    static constexpr unsigned char flush_marker[4] = {0x00, 0x00, 0xFF, 0xFF};
    std::optional<std::string> owned_input;
    if (http_text_aliases_string(input, out)) {
        input = owned_input.emplace(input);
    }
    const auto start = out.size();
    if (const auto result_value = websocket_inflate_chunk(
            inflater, input.data(), input.size(), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    if (const auto result_value = websocket_inflate_chunk(inflater,
            reinterpret_cast<const char*>(flush_marker), sizeof(flush_marker), out, start, message_limit);
        result_value != websocket_inflate_result::ok) {
        return result_value;
    }
    // zlib reports bit 128 only at a complete DEFLATE block boundary.
    // Consuming every input byte alone does not detect a truncated block.
    return (inflater.native().data_type & 128) != 0 ? websocket_inflate_result::ok : websocket_inflate_result::error;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// RFC 7692 codec state of one connection. A direction negotiated without
// context takeover starts every message from an empty dictionary, so it uses
// the borrowed shared codec set when the owner supplies one and then holds no
// codec memory. Every other direction owns a private zlib stream,
// created on its first compressed message and kept,
// with its dictionary, until the connection ends. The owner validates the
// deflater (validate_websocket_deflater_config) and windows (8-15).
class websocket_deflate final {
public:
    websocket_deflate(websocket_deflate_codecs* shared,
        websocket_deflater_config deflater, bool send_takeover, bool receive_takeover, int send_window,
        int receive_window) noexcept
        : shared_(shared),
          deflater_config_(deflater),
          send_window_(send_window),
          receive_window_(receive_window),
          send_takeover_(send_takeover),
          receive_takeover_(receive_takeover) {}

    websocket_deflate(const websocket_deflate&) = delete;
    websocket_deflate& operator=(const websocket_deflate&) = delete;

    // Appends the compressed message to `out`. false reports a codec failure;
    // allocation exceptions propagate.
    bool compress(std::string_view input, std::string& out) {
        if (!send_takeover_ && shared_ != nullptr) {
            // Reset after every call, including failures and exceptions.
            return shared_->deflate_message(input, out, deflater_config_.compression_level_, send_window_);
        }
        if (deflater_ == nullptr) {
            deflater_ = std::make_unique<zlib_deflate_stream>(
                deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_),
                deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
            if (!deflater_->initialized()) {
                deflater_.reset();
                return false;
            }
        } else if (!send_takeover_ && !deflater_->reset()) {
            return false;
        }
        return websocket_deflate_message(*deflater_, input, out);
    }

    // Compresses one fragment of a message sent as several frames. The whole
    // message is one DEFLATE stream, so it uses a private codec even where
    // whole messages borrow the shared one, and starts from an empty
    // dictionary at its first fragment unless context takeover applies.
    bool compress_fragment(std::string_view input, bool first, bool final, std::string& out) {
        if (deflater_ == nullptr) {
            deflater_ = std::make_unique<zlib_deflate_stream>(
                deflater_config_.compression_level_, websocket_deflate_zlib_window_bits(send_window_),
                deflater_config_.mem_level_, websocket_deflate_zlib_strategy(send_window_));
            if (!deflater_->initialized()) {
                deflater_.reset();
                return false;
            }
        } else if (first && !send_takeover_ && !deflater_->reset()) {
            return false;
        }
        return websocket_deflate_message(*deflater_, input, out, final);
    }

    websocket_inflate_result decompress(
        std::string_view input, std::string& out, protocol_byte_limit message_limit) {
        if (!receive_takeover_ && shared_ != nullptr) {
            // The shared inflater has a 15-bit window, which also inflates
            // every smaller negotiated window.
            return shared_->inflate_message(input, out, message_limit);
        }
        if (inflater_ == nullptr) {
            inflater_ = std::make_unique<zlib_inflate_stream>(-receive_window_);
            if (!inflater_->initialized()) {
                inflater_.reset();
                return websocket_inflate_result::error;
            }
        } else if (!receive_takeover_ && !inflater_->reset()) {
            return websocket_inflate_result::error;
        }
        return websocket_inflate_message(*inflater_, input, out, message_limit);
    }

    // A trial that is not sent must not enter the peer's future dictionary.
    // Starting the next compressed message with an empty sender dictionary is
    // legal even when the receiver retains its prior dictionary (RFC 7692).
    // The shared codecs already reset after every message.
    void discard_compression() {
        if (deflater_ != nullptr && !deflater_->reset()) {
            throw std::runtime_error("failed to reset discarded WebSocket compression");
        }
    }

private:
    websocket_deflate_codecs* shared_;
    std::unique_ptr<zlib_deflate_stream> deflater_;
    std::unique_ptr<zlib_inflate_stream> inflater_;
    websocket_deflater_config deflater_config_;
    int send_window_;
    int receive_window_;
    bool send_takeover_;
    bool receive_takeover_;
};

// Decode RFC 7692 window parameters (8 through 15), including quoted tokens.
// The negotiated send and receive bounds are independent of takeover policy.
[[nodiscard]] inline std::optional<int> websocket_deflate_window_bits(
    std::string_view value) noexcept {
    value = http_trim_ows(value);
    int parsed_value = 0;
    std::size_t digits = 0;
    bool leading_zero = false;
    // RFC 7692 section 7.1.2 defines both max-window-bits parameters as
    // decimal integers without leading zeroes. Apply that grammar after
    // quoted-pair decoding as well as to tokens.
    const auto append_digit = [&](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9' || ++digits > 2) {
            return false;
        }
        if (digits == 1) {
            leading_zero = ch == '0';
        } else if (leading_zero) {
            return false;
        }
        parsed_value = parsed_value * 10 + (ch - '0');
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor quoted(value);
        unsigned char byte = 0;
        while (quoted.next(byte)) {
            if (!append_digit(byte)) {
                return std::nullopt;
            }
        }
        if (!quoted.closed() || quoted.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const char ch : value) {
            if (!append_digit(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
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
    const auto name = http_header_token_before_parameters(element);
    if (!http_ascii_equals_ignore_case(name, "permessage-deflate")) {
        return std::nullopt;
    }

    permessage_deflate_params params;
    const bool valid = http_all_parameters(element, [&params](std::string_view parameter) noexcept {
        const auto equals = parameter.find('=');
        const bool has_value = equals != std::string_view::npos;
        const auto parameter_name = http_trim_ows(parameter.substr(0, equals));
        const auto parameter_value =
            has_value ? http_trim_ows(parameter.substr(equals + 1)) : std::string_view{};

        switch (parameter_name.size()) {
            case 22:
                if (http_ascii_equals_ignore_case(parameter_name, "server_max_window_bits")) {
                    // A missing or empty value fails window decoding.
                    if (params.server_max_window_bits_.has_value()) {
                        return false;
                    }
                    params.server_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                    return params.server_max_window_bits_.has_value();
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_max_window_bits")) {
                    if (params.client_max_window_bits_present_) {
                        return false;
                    }
                    params.client_max_window_bits_present_ = true;
                    if (has_value) {
                        params.client_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                        return params.client_max_window_bits_.has_value();
                    }
                    return true;
                }
                return false;
            case 26:
                if (http_ascii_equals_ignore_case(parameter_name, "server_no_context_takeover")) {
                    if (params.server_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.server_no_context_takeover_ = true;
                    return true;
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_no_context_takeover")) {
                    if (params.client_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.client_no_context_takeover_ = true;
                    return true;
                }
                return false;
            default:
                // Includes an empty parameter, such as a trailing ';'.
                return false;
        }
    });
    return valid ? std::optional(params) : std::nullopt;
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
    if (context_takeover) {
        accepted.server_no_context_takeover_ = params->server_no_context_takeover_;
        accepted.client_no_context_takeover_ = params->client_no_context_takeover_;
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
    validate_websocket_deflater_config(config.deflater_);
    if (!config.enabled_) {
        return (websocket_compression{});
    }
    // RFC 6455 §9.1: extension declarations may be split across multiple
    // Sec-WebSocket-Extensions field lines, which RFC 9110 §5.3 makes equivalent to
    // one comma-joined list. Scan every line in order, validating the whole
    // extension list while honoring the first acceptable permessage-deflate offer.
    bool has_extension = false;
    websocket_compression negotiated{};
    const bool valid = http_request_access::visit_known_headers(request,
        http_field_kind::sec_websocket_extensions,
        [&has_extension, &negotiated, &config](std::string_view value) noexcept {
            if (!append_websocket_extension_list(value, has_extension)) {
                return false;
            }
            if (!negotiated.enabled_) {
                negotiated = websocket_scan_deflate_offers(value, config.context_takeover_);
            }
            return true;
        });
    return valid && has_extension ? negotiated : (websocket_compression{});
}

}  // namespace ruvia::detail
