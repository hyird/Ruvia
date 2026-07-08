#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <zlib.h>

#include "HeaderTokenUtils.h"
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
        deflate_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
        deflate_.avail_in = static_cast<uInt>(input.size());
        char buffer[4096];
        for (;;) {
            deflate_.next_out = reinterpret_cast<Bytef*>(buffer);
            deflate_.avail_out = sizeof(buffer);
            const int status = deflate(&deflate_, Z_SYNC_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR) {
                return false;
            }
            out.append(buffer, sizeof(buffer) - deflate_.avail_out);
            if (deflate_.avail_out != 0) {
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
    // the sender stripped, then raw-inflates into `out`, bounded by `maxBytes`
    // (0 = unbounded) to defuse decompression bombs (RFC 7692 §7.2.2).
    WebSocketInflateResult decompress(std::string_view input, std::pmr::string& out, std::size_t maxBytes) {
        if (!inflateOk_ || inflateReset(&inflate_) != Z_OK) {
            return WebSocketInflateResult::kError;
        }
        static constexpr unsigned char kFlushMarker[4] = {0x00, 0x00, 0xFF, 0xFF};
        if (const auto r = inflateChunk(input.data(), input.size(), out, maxBytes); r != WebSocketInflateResult::kOk) {
            return r;
        }
        return inflateChunk(reinterpret_cast<const char*>(kFlushMarker), sizeof(kFlushMarker), out, maxBytes);
    }

private:
    WebSocketInflateResult inflateChunk(const char* data, std::size_t size, std::pmr::string& out, std::size_t maxBytes) {
        inflate_.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
        inflate_.avail_in = static_cast<uInt>(size);
        char buffer[8192];
        for (;;) {
            inflate_.next_out = reinterpret_cast<Bytef*>(buffer);
            inflate_.avail_out = sizeof(buffer);
            const int status = inflate(&inflate_, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
                return WebSocketInflateResult::kError;
            }
            const auto produced = sizeof(buffer) - inflate_.avail_out;
            if (maxBytes != 0 && produced > maxBytes - out.size()) {
                return WebSocketInflateResult::kTooLarge;
            }
            out.append(buffer, produced);
            if (inflate_.avail_out != 0) {
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

struct WebSocketDeflateNegotiation final {
    bool enabled = false;
    // The client pinned server_max_window_bits=15, i.e. exactly our fixed window.
    // RFC 7692 §7.1.2.1 requires a server that accepts an offer carrying this
    // parameter to echo server_max_window_bits in the response, so record it.
    bool echoServerMaxWindowBits = false;
};

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
        if (asciiEqualsIgnoreCase(name, "permessage-deflate")) {
            const auto params = semicolon == std::string_view::npos ? std::string_view{} : offer.substr(semicolon + 1);
            const auto serverWindow = httpFindSemicolonParameterQuotedIgnoreCase(params, "server_max_window_bits");
            if (!serverWindow.has_value()) {
                return {.enabled = true, .echoServerMaxWindowBits = false};
            }
            if (httpTrimQuotes(*serverWindow) == "15") {
                return {.enabled = true, .echoServerMaxWindowBits = true};
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        offers.remove_prefix(comma + 1);
    }
    return {};
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

[[nodiscard]] inline WebSocketDeflateNegotiation webSocketNegotiatePermessageDeflate(
    const HttpRequest& request) noexcept {
    // RFC 6455 §9.1: extension declarations may be split across multiple
    // Sec-WebSocket-Extensions field lines, which RFC 9110 §5.3 makes equivalent to
    // one comma-joined list. request.header() returns only the last line, so scan
    // every line in order and honor the first acceptable offer. Offers are resolved
    // independently (first honorable wins), so first-honorable-across-lines is the
    // same result as scanning the joined list.
    for (const auto& header : request.headers()) {
        if (!asciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Extensions")) {
            continue;
        }
        const auto negotiation = webSocketScanDeflateOffers(header.value());
        if (negotiation.enabled) {
            return negotiation;
        }
    }
    return {};
}

}  // namespace ruvia::detail
