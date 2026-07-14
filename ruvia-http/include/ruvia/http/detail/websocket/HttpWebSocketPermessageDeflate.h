#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <zlib.h>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"
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
// zlib's working memory comes from the global allocator, which this build routes
// to mimalloc, so no custom resource plumbing is needed.
class WebSocketDeflate final {
public:
    WebSocketDeflate() noexcept {
        deflateOk_ = deflateInit2(&deflate_, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK;
        inflateOk_ = inflateInit2(&inflate_, -15) == Z_OK;
    }

    ~WebSocketDeflate() {
        if (deflateOk_) {
            (void)deflateEnd(&deflate_);
        }
        if (inflateOk_) {
            (void)inflateEnd(&inflate_);
        }
    }

    WebSocketDeflate(const WebSocketDeflate&) = delete;
    WebSocketDeflate& operator=(const WebSocketDeflate&) = delete;

    [[nodiscard]] bool ok() const noexcept {
        return deflateOk_ && inflateOk_;
    }

    // Compresses a whole message, appending the raw-DEFLATE block to `out` with
    // the trailing 0x00 0x00 0xFF 0xFF flush marker removed (RFC 7692 §7.2.1).
    bool compress(std::string_view input, std::pmr::string& out) {
        if (!deflateOk_ || deflateReset(&deflate_) != Z_OK) {
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
                    input.size() - supplied,
                    (std::numeric_limits<uInt>::max)()));
                deflate_.next_in = reinterpret_cast<Bytef*>(
                    const_cast<char*>(input.data() + supplied));
                deflate_.avail_in = count;
                supplied += count;
            }
            deflate_.next_out = reinterpret_cast<Bytef*>(buffer);
            deflate_.avail_out = sizeof(buffer);
            const int status = deflate(
                &deflate_,
                supplied == input.size() ? Z_SYNC_FLUSH : Z_NO_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR) {
                return false;
            }
            out.append(buffer, sizeof(buffer) - deflate_.avail_out);
            if (deflate_.avail_out != 0 &&
                deflate_.avail_in == 0 &&
                supplied == input.size()) {
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
        std::string_view input,
        std::pmr::string& out,
        ProtocolByteLimit messageLimit) {
        if (!inflateOk_ || inflateReset(&inflate_) != Z_OK) {
            return WebSocketInflateResult::kError;
        }
        static constexpr unsigned char kFlushMarker[4] = {0x00, 0x00, 0xFF, 0xFF};
        if (const auto r = inflateChunk(
                input.data(), input.size(), out, messageLimit);
            r != WebSocketInflateResult::kOk) {
            return r;
        }
        return inflateChunk(
            reinterpret_cast<const char*>(kFlushMarker),
            sizeof(kFlushMarker),
            out,
            messageLimit);
    }

private:
    WebSocketInflateResult inflateChunk(
        const char* data,
        std::size_t size,
        std::pmr::string& out,
        ProtocolByteLimit messageLimit) {
        // Messages can exceed zlib's 32-bit avail_in, so supply the input in
        // windows instead of truncating the size.
        inflate_.avail_in = 0;
        std::size_t supplied = 0;
        char buffer[8192];
        for (;;) {
            if (inflate_.avail_in == 0 && supplied < size) {
                const auto count = static_cast<uInt>(std::min<std::size_t>(
                    size - supplied,
                    (std::numeric_limits<uInt>::max)()));
                inflate_.next_in = reinterpret_cast<Bytef*>(
                    const_cast<char*>(data + supplied));
                inflate_.avail_in = count;
                supplied += count;
            }
            inflate_.next_out = reinterpret_cast<Bytef*>(buffer);
            inflate_.avail_out = sizeof(buffer);
            const int status = inflate(&inflate_, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
                return WebSocketInflateResult::kError;
            }
            const auto produced = sizeof(buffer) - inflate_.avail_out;
            if (messageLimit.additionExceeds(out.size(), produced)) {
                return WebSocketInflateResult::kTooLarge;
            }
            out.append(buffer, produced);
            if (status == Z_STREAM_END ||
                (inflate_.avail_out != 0 &&
                 inflate_.avail_in == 0 &&
                 supplied == size)) {
                break;
            }
        }
        return WebSocketInflateResult::kOk;
    }

    z_stream deflate_{};
    z_stream inflate_{};
    bool deflateOk_{false};
    bool inflateOk_{false};
};

// One negotiated permessage-deflate outcome. The former enabled/echo booleans
// admitted an impossible "disabled but echo server_max_window_bits" product.
enum class WebSocketDeflateNegotiation : std::uint8_t {
    kDisabled,
    kAccepted,
    // The client pinned server_max_window_bits=15, i.e. exactly our fixed window.
    // RFC 7692 §7.1.2.1 requires the response to echo that parameter.
    kAcceptedWithServerMaxWindowBits,
};

[[nodiscard]] constexpr bool webSocketDeflateNegotiated(
    WebSocketDeflateNegotiation negotiation) noexcept {
    return negotiation == WebSocketDeflateNegotiation::kAccepted ||
        negotiation == WebSocketDeflateNegotiation::
            kAcceptedWithServerMaxWindowBits;
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
[[nodiscard]] inline WebSocketDeflateNegotiation webSocketScanDeflateOffers(std::string_view offers) noexcept {
    while (!offers.empty()) {
        const auto comma = offers.find(',');
        const auto offer = httpTrimOws(comma == std::string_view::npos ? offers : offers.substr(0, comma));
        const auto semicolon = offer.find(';');
        const auto name = httpTrimOws(semicolon == std::string_view::npos ? offer : offer.substr(0, semicolon));
        if (httpAsciiEqualsIgnoreCase(name, "permessage-deflate")) {
            const auto params = semicolon == std::string_view::npos ? std::string_view{} : offer.substr(semicolon + 1);
            const auto serverWindow = httpFindSemicolonParameterQuotedIgnoreCase(params, "server_max_window_bits");
            if (!serverWindow.has_value()) {
                return WebSocketDeflateNegotiation::kAccepted;
            }
            if (httpTrimQuotes(*serverWindow) == "15") {
                return WebSocketDeflateNegotiation::
                    kAcceptedWithServerMaxWindowBits;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        offers.remove_prefix(comma + 1);
    }
    return WebSocketDeflateNegotiation::kDisabled;
}

// Response Sec-WebSocket-Extensions VALUES for an accepted permessage-deflate offer
// (no-context-takeover both directions; the MaxWindow variant echoes a client-pinned
// server_max_window_bits=15 per RFC 7692 §7.1.2.1). Shared by the h1 header line and
// the h2 HPACK handshake.
inline constexpr std::string_view kWebSocketDeflateResponseExtensions =
    "permessage-deflate; server_no_context_takeover; client_no_context_takeover";
inline constexpr std::string_view kWebSocketDeflateResponseExtensionsMaxWindow =
    "permessage-deflate; server_no_context_takeover; client_no_context_takeover; "
    "server_max_window_bits=15";

// The response Sec-WebSocket-Extensions VALUE for a negotiation result (empty when
// deflate was not accepted). Single-sources the h1 handshake and the h2 tunnel path.
[[nodiscard]] inline std::string_view webSocketDeflateResponseExtensions(
    WebSocketDeflateNegotiation negotiation) noexcept {
    switch (negotiation) {
        case WebSocketDeflateNegotiation::kDisabled:
            return {};
        case WebSocketDeflateNegotiation::kAccepted:
            return kWebSocketDeflateResponseExtensions;
        case WebSocketDeflateNegotiation::kAcceptedWithServerMaxWindowBits:
            return kWebSocketDeflateResponseExtensionsMaxWindow;
    }
    return {};
}

[[nodiscard]] inline WebSocketDeflateNegotiation webSocketNegotiatePermessageDeflate(
    const HttpRequest& request) noexcept {
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
    return WebSocketDeflateNegotiation::kDisabled;
}

}  // namespace ruvia::detail
