#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <zlib.h>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/WebSocketProtocol.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketHandshakeFields.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia::detail {

enum class WebSocketInflateResult : std::uint8_t {
    kOk,
    kError,
    kTooLarge,
};

// RFC 7692 permessage-deflate codec for one connection. The handshake negotiates
// no-context-takeover in both directions, so the deflate/inflate state is reset
// before every message and each message is an independent raw-DEFLATE block.
// zlib owns its working memory, so no custom resource plumbing is needed.
class WebSocketDeflate final {
public:
    WebSocketDeflate() {
        if (deflateInit2(
                &deflate_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            throw std::runtime_error("failed to initialize WebSocket deflate encoder");
        }
        if (inflateInit2(&inflate_, -15) != Z_OK) {
            (void)deflateEnd(&deflate_);
            throw std::runtime_error("failed to initialize WebSocket deflate decoder");
        }
    }

    ~WebSocketDeflate() {
        (void)inflateEnd(&inflate_);
        (void)deflateEnd(&deflate_);
    }

    WebSocketDeflate(const WebSocketDeflate&) = delete;
    WebSocketDeflate& operator=(const WebSocketDeflate&) = delete;

    // Compresses a whole message, appending the raw-DEFLATE block to `out` with
    // the trailing 0x00 0x00 0xFF 0xFF flush marker removed (RFC 7692 §7.2.1).
    bool compress(std::string_view input, std::pmr::string& out) {
        if (deflateReset(&deflate_) != Z_OK) {
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
    WebSocketInflateResult decompress(
        std::string_view input, std::pmr::string& out, ProtocolByteLimit messageLimit) {
        if (inflateReset(&inflate_) != Z_OK) {
            return WebSocketInflateResult::kError;
        }
        static constexpr unsigned char kFlushMarker[4] = {0x00, 0x00, 0xFF, 0xFF};
        if (const auto r = inflateChunk(input.data(), input.size(), out, messageLimit);
            r != WebSocketInflateResult::kOk) {
            return r;
        }
        return inflateChunk(
            reinterpret_cast<const char*>(kFlushMarker), sizeof(kFlushMarker), out, messageLimit);
    }

private:
    WebSocketInflateResult inflateChunk(
        const char* data, std::size_t size, std::pmr::string& out, ProtocolByteLimit messageLimit) {
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
                return WebSocketInflateResult::kError;
            }
            const auto produced = sizeof(buffer) - inflate_.avail_out;
            if (messageLimit.additionExceeds(out.size(), produced)) {
                return WebSocketInflateResult::kTooLarge;
            }
            out.append(buffer, produced);
            if (inflate_.avail_out != 0 && inflate_.avail_in == 0 && supplied == size) {
                break;
            }
        }
        return WebSocketInflateResult::kOk;
    }

    z_stream deflate_{};
    z_stream inflate_{};
};

[[nodiscard]] constexpr bool webSocketDeflateNegotiated(WebSocketCompression negotiation) noexcept {
    return negotiation == WebSocketCompression::kPermessageDeflate ||
           negotiation == WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits;
}

// Decide whether the client offered permessage-deflate in a form we can honor. We
// run a fixed 32 KiB (15-bit) server window with no context takeover, so a bare
// offer, client_max_window_bits (our 15-bit inflate handles any smaller client
// window), and the no-context-takeover hints are all fine. An offer that pins
// server_max_window_bits is honored only when it permits 15: a smaller bound would
// require shrinking our compressor, so those offers are skipped (fall back to the
// next offer / no compression). RFC 7692 §7.1.2.1.
// Scan one Sec-WebSocket-Extensions field-line value (a comma list of offers) and
// return the first honorable permessage-deflate offer, or a disabled negotiation
// if the line carries none we can accept.
[[nodiscard]] inline std::optional<int> webSocketDeflateWindowBits(
    std::string_view value) noexcept {
    value = httpTrimOws(value);
    bool quoted = false;
    if (!value.empty() && value.front() == '"') {
        if (value.size() < 2 || value.back() != '"') {
            return std::nullopt;
        }
        quoted = true;
        value.remove_prefix(1);
        value.remove_suffix(1);
    }

    int parsed = 0;
    std::size_t digits = 0;
    bool leadingZero = false;
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
            leadingZero = ch == '0';
        } else if (leadingZero) {
            // RFC 7692 section 7.1.2 defines both max-window-bits
            // parameters as decimal integers without leading zeroes. Apply
            // that grammar after quoted-pair decoding as well as to tokens.
            return std::nullopt;
        }
        parsed = parsed * 10 + (ch - '0');
    }
    return digits != 0 && parsed >= 8 && parsed <= 15 ? std::optional<int>(parsed) : std::nullopt;
}

[[nodiscard]] inline std::optional<WebSocketCompression> webSocketParseDeflateOffer(
    std::string_view offer) noexcept {
    const auto firstSemicolon = httpFindUnquotedDelimiter(offer, 0, ';');
    const auto name = httpTrimOws(offer.substr(0, firstSemicolon));
    if (!httpAsciiEqualsIgnoreCase(name, "permessage-deflate")) {
        return std::nullopt;
    }

    bool serverNoContextTakeover = false;
    bool clientNoContextTakeover = false;
    bool serverWindowSeen = false;
    bool clientWindowSeen = false;
    int serverWindow = 15;
    std::size_t start = firstSemicolon;
    while (start < offer.size()) {
        ++start;
        const auto end = httpFindUnquotedDelimiter(offer, start, ';');
        const auto parameter = httpTrimOws(offer.substr(start, end - start));
        if (parameter.empty()) {
            return std::nullopt;
        }
        const auto equals = parameter.find('=');
        const bool hasValue = equals != std::string_view::npos;
        const auto parameterName = httpTrimOws(hasValue ? parameter.substr(0, equals) : parameter);
        const auto parameterValue =
            hasValue ? httpTrimOws(parameter.substr(equals + 1)) : std::string_view{};

        if (httpAsciiEqualsIgnoreCase(parameterName, "server_no_context_takeover")) {
            if (serverNoContextTakeover || hasValue) {
                return std::nullopt;
            }
            serverNoContextTakeover = true;
        } else if (httpAsciiEqualsIgnoreCase(parameterName, "client_no_context_takeover")) {
            if (clientNoContextTakeover || hasValue) {
                return std::nullopt;
            }
            clientNoContextTakeover = true;
        } else if (httpAsciiEqualsIgnoreCase(parameterName, "server_max_window_bits")) {
            if (serverWindowSeen || !hasValue) {
                return std::nullopt;
            }
            const auto parsed = webSocketDeflateWindowBits(parameterValue);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            serverWindowSeen = true;
            serverWindow = *parsed;
        } else if (httpAsciiEqualsIgnoreCase(parameterName, "client_max_window_bits")) {
            if (clientWindowSeen) {
                return std::nullopt;
            }
            if (hasValue && !webSocketDeflateWindowBits(parameterValue).has_value()) {
                return std::nullopt;
            }
            clientWindowSeen = true;
        } else {
            // RFC 7692 section 7.1: an offer containing an undefined or
            // malformed permessage-deflate parameter cannot be negotiated.
            return std::nullopt;
        }

        start = end;
    }

    if (serverWindowSeen && serverWindow != 15) {
        return std::nullopt;
    }
    return serverWindowSeen ? WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits
                            : WebSocketCompression::kPermessageDeflate;
}

[[nodiscard]] inline WebSocketCompression webSocketScanDeflateOffers(
    std::string_view offers) noexcept {
    std::optional<WebSocketCompression> accepted;
    httpVisitCommaSeparatedQuotedItems(offers, [&accepted](std::string_view offer) noexcept {
        if (offer.empty()) {
            return true;
        }
        accepted = webSocketParseDeflateOffer(offer);
        return !accepted.has_value();
    });
    return accepted.value_or(WebSocketCompression::kDisabled);
}

[[nodiscard]] inline WebSocketCompression webSocketNegotiatePermessageDeflate(
    const HttpRequest& request) noexcept {
    if (!webSocketExtensionOffersValid(request)) {
        return WebSocketCompression::kDisabled;
    }
    // RFC 6455 §9.1: extension declarations may be split across multiple
    // Sec-WebSocket-Extensions field lines, which RFC 9110 §5.3 makes equivalent to
    // one comma-joined list. request.header() returns only the last line, so scan
    // every line in order and honor the first acceptable offer. Offers are resolved
    // independently (first honorable wins), so first-honorable-across-lines is the
    // same result as scanning the joined list.
    for (const auto& header : request.headers()) {
        if (!httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Extensions")) {
            continue;
        }
        const auto negotiation = webSocketScanDeflateOffers(header.value());
        if (webSocketDeflateNegotiated(negotiation)) {
            return negotiation;
        }
    }
    return WebSocketCompression::kDisabled;
}

}  // namespace ruvia::detail
