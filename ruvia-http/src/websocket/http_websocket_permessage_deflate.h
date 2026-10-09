#pragma once

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol.h"

#include "request/http_request_access.h"
#include "websocket/http_websocket_handshake_fields.h"

namespace ruvia::detail {

enum class websocket_inflate_result : std::uint8_t {
    ok,
    error,
    too_large,
};

// RFC 7692 codec owned by one connection. context reuse must match the negotiated
// wire mode. zlib owns its working memory; destruction releases both dictionaries.
class websocket_deflate final {
public:
    explicit websocket_deflate(int compression_level = 6, bool context_takeover = false)
        : websocket_deflate(compression_level, context_takeover, context_takeover, 15, 15) {}

    websocket_deflate(int compression_level, bool send_takeover, bool receive_takeover, int send_window, int receive_window)
        : send_takeover_(send_takeover),
          receive_takeover_(receive_takeover) {
        if (send_window < 8 || send_window > 15 || receive_window < 8 || receive_window > 15) {
            throw std::invalid_argument("invalid WebSocket DEFLATE window");
        }
        if (compression_level < 0 || compression_level > 9) {
            throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
        }
        if (deflateInit2(
                &deflate_, compression_level, Z_DEFLATED, -std::max(send_window, 9), 8, send_window == 8 ? Z_HUFFMAN_ONLY : Z_DEFAULT_STRATEGY) != Z_OK) {
            throw std::runtime_error("failed to initialize WebSocket deflate encoder");
        }
        if (inflateInit2(&inflate_, -receive_window) != Z_OK) {
            (void)deflateEnd(&deflate_);
            throw std::runtime_error("failed to initialize WebSocket deflate decoder");
        }
    }

    ~websocket_deflate() {
        (void)inflateEnd(&inflate_);
        (void)deflateEnd(&deflate_);
    }

    websocket_deflate(const websocket_deflate&) = delete;
    websocket_deflate& operator=(const websocket_deflate&) = delete;

    // Compresses a whole message, appending the raw-DEFLATE block to `out` with
    // the trailing 0x00 0x00 0xFF 0xFF flush marker removed (RFC 7692 §7.2.1).
    bool compress(std::string_view input, std::pmr::string& out) {
        if (!send_takeover_ && deflateReset(&deflate_) != Z_OK) {
            return false;
        }
        // Messages can exceed zlib's 32-bit avail_in, so supply the input in
        // windows instead of truncating the size.
        deflate_.avail_in = 0;
        std::size_t supplied = 0;
        char buffer[4096];
        for (;;) {
            if (deflate_.avail_in == 0 && supplied < input.size()) {
                const auto count = static_cast<uInt>(std::min<std::size_t>(
                    input.size() - supplied, (std::numeric_limits<uInt>::max)()));
                deflate_.next_in =
                    reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + supplied));
                deflate_.avail_in = count;
                supplied += count;
            }
            deflate_.next_out = reinterpret_cast<Bytef*>(buffer);
            deflate_.avail_out = sizeof(buffer);
            const int status =
                deflate(&deflate_, supplied == input.size() ? Z_SYNC_FLUSH : Z_NO_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR) {
                return false;
            }
            out.append(buffer, sizeof(buffer) - deflate_.avail_out);
            if (deflate_.avail_out != 0 && deflate_.avail_in == 0 && supplied == input.size()) {
                break;
            }
        }
        if (out.size() >= 4) {
            out.resize(out.size() - 4);
        }
        if (out.empty()) {
            out.push_back('\0');
        }
        return true;
    }

    // Decompresses a whole message: appends the 0x00 0x00 0xFF 0xFF marker that
    // the sender stripped, then raw-inflates into `out`, bounded by one explicit
    // message limit to defuse decompression bombs (RFC 7692 §7.2.2).
    websocket_inflate_result decompress(
        std::string_view input, std::pmr::string& out, protocol_byte_limit message_limit) {
        if (!receive_takeover_ && inflateReset(&inflate_) != Z_OK) {
            return websocket_inflate_result::error;
        }
        static constexpr unsigned char flush_marker[4] = {0x00, 0x00, 0xFF, 0xFF};
        if (const auto r = inflate_chunk(input.data(), input.size(), out, message_limit);
            r != websocket_inflate_result::ok) {
            return r;
        }
        return inflate_chunk(
            reinterpret_cast<const char*>(flush_marker), sizeof(flush_marker), out, message_limit);
    }

    // A trial that is not sent must not enter the peer's future dictionary.
    // Starting the next compressed message with an empty sender dictionary is
    // legal even when the receiver retains its prior dictionary (RFC 7692).
    void discard_compression() {
        if (deflateReset(&deflate_) != Z_OK) {
            throw std::runtime_error("failed to reset discarded WebSocket compression");
        }
    }

private:
    websocket_inflate_result inflate_chunk(
        const char* data, std::size_t size, std::pmr::string& out, protocol_byte_limit message_limit) {
        // Messages can exceed zlib's 32-bit avail_in, so supply the input in
        // windows instead of truncating the size.
        inflate_.avail_in = 0;
        std::size_t supplied = 0;
        char buffer[8192];
        for (;;) {
            if (inflate_.avail_in == 0 && supplied < size) {
                const auto count = static_cast<uInt>(
                    std::min<std::size_t>(size - supplied, (std::numeric_limits<uInt>::max)()));
                inflate_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data + supplied));
                inflate_.avail_in = count;
                supplied += count;
            }
            inflate_.next_out = reinterpret_cast<Bytef*>(buffer);
            inflate_.avail_out = sizeof(buffer);
            const int status = inflate(&inflate_, Z_NO_FLUSH);
            // RFC 7692 messages are Z_SYNC_FLUSH blocks with the four-byte
            // marker removed. After restoring that marker, the raw stream does
            // not terminate with BFINAL. Accepting Z_STREAM_END lets a peer
            // submit an independently terminated DEFLATE stream and makes zlib
            // silently ignore any bytes that follow it.
            if (status == Z_STREAM_END || (status != Z_OK && status != Z_BUF_ERROR)) {
                return websocket_inflate_result::error;
            }
            const auto produced = sizeof(buffer) - inflate_.avail_out;
            if (message_limit.addition_exceeds(out.size(), produced)) {
                return websocket_inflate_result::too_large;
            }
            out.append(buffer, produced);
            if (inflate_.avail_out != 0 && inflate_.avail_in == 0 && supplied == size) {
                break;
            }
        }
        return websocket_inflate_result::ok;
    }

    z_stream deflate_{};
    z_stream inflate_{};
    bool send_takeover_{false};
    bool receive_takeover_{false};
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

[[nodiscard]] inline std::optional<websocket_compression> websocket_parse_deflate_offer(
    std::string_view offer, bool context_takeover = false) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(offer, 0, ';');
    const auto name = http_trim_ows(offer.substr(0, first_semicolon));
    if (!http_ascii_equals_ignore_case(name, "permessage-deflate")) {
        return std::nullopt;
    }

    bool server_no_context_takeover = false;
    bool client_no_context_takeover = false;
    bool server_window_seen = false;
    bool client_window_seen = false;
    int server_window = 15;
    std::size_t start = first_semicolon;
    while (start < offer.size()) {
        ++start;
        const auto end = http_find_unquoted_delimiter(offer, start, ';');
        const auto parameter = http_trim_ows(offer.substr(start, end - start));
        if (parameter.empty()) {
            return std::nullopt;
        }
        const auto equals = parameter.find('=');
        const bool has_value = equals != std::string_view::npos;
        const auto parameter_name = http_trim_ows(has_value ? parameter.substr(0, equals) : parameter);
        const auto parameter_value =
            has_value ? http_trim_ows(parameter.substr(equals + 1)) : std::string_view{};

        if (http_ascii_equals_ignore_case(parameter_name, "server_no_context_takeover")) {
            if (server_no_context_takeover || has_value) {
                return std::nullopt;
            }
            server_no_context_takeover = true;
        } else if (http_ascii_equals_ignore_case(parameter_name, "client_no_context_takeover")) {
            if (client_no_context_takeover || has_value) {
                return std::nullopt;
            }
            client_no_context_takeover = true;
        } else if (http_ascii_equals_ignore_case(parameter_name, "server_max_window_bits")) {
            if (server_window_seen || !has_value) {
                return std::nullopt;
            }
            const auto parsed_value = websocket_deflate_window_bits(parameter_value);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            server_window_seen = true;
            server_window = *parsed_value;
        } else if (http_ascii_equals_ignore_case(parameter_name, "client_max_window_bits")) {
            if (client_window_seen) {
                return std::nullopt;
            }
            if (has_value && !websocket_deflate_window_bits(parameter_value).has_value()) {
                return std::nullopt;
            }
            client_window_seen = true;
        } else {
            // RFC 7692 section 7.1: an offer containing an undefined or
            // malformed permessage-deflate parameter cannot be negotiated.
            return std::nullopt;
        }

        start = end;
    }

    if (context_takeover && !server_no_context_takeover && !client_no_context_takeover) {
        return server_window_seen ? (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .server_max_window_bits_ = server_window})
                                  : (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false});
    }
    return server_window_seen ? (websocket_compression{.enabled_ = true, .server_max_window_bits_ = server_window})
                              : (websocket_compression{.enabled_ = true});
}

[[nodiscard]] inline websocket_compression websocket_scan_deflate_offers(
    std::string_view offers, bool context_takeover = false) noexcept {
    std::optional<websocket_compression> accepted;
    http_visit_comma_separated_quoted_items(offers, [&accepted, context_takeover](std::string_view offer) noexcept {
        if (offer.empty()) {
            return true;
        }
        accepted = websocket_parse_deflate_offer(offer, context_takeover);
        return !accepted.has_value();
    });
    return accepted.value_or((websocket_compression{}));
}

[[nodiscard]] inline websocket_compression websocket_negotiate_permessage_deflate(
    const http_request& request, websocket_deflate_config config = {}) {
    if (config.compression_level_ < 0 || config.compression_level_ > 9) {
        throw std::invalid_argument("WebSocket compression level must be between 0 and 9");
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
