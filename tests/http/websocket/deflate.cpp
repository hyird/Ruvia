#include "test_harness.h"

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"

namespace {

using ruvia::ProtocolByteLimit;
using ruvia::WebSocketCompression;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::WebSocketDeflate;
using ruvia::detail::webSocketDeflateNegotiated;
using ruvia::detail::WebSocketInflateResult;
using ruvia::detail::webSocketNegotiatePermessageDeflate;

// Parses a WebSocket upgrade carrying `extensions` as its Sec-WebSocket-Extensions
// value and reports how the server would negotiate permessage-deflate for it.
// (parser/raw stay alive across the call: request headers view into raw.)
WebSocketCompression negotiateDeflate(std::string_view extensions) {
    std::string raw = "GET /ws HTTP/1.1\r\nHost: x\r\n";
    if (!extensions.empty()) {
        raw += "Sec-WebSocket-Extensions: ";
        raw.append(extensions.data(), extensions.size());
        raw += "\r\n";
    }
    raw += "\r\n";
    Http1ServerRequestParser parser;
    const auto result = parser.parseMessage(raw);
    return webSocketNegotiatePermessageDeflate(result.request);
}

bool offersDeflate(std::string_view extensions) {
    return webSocketDeflateNegotiated(negotiateDeflate(extensions));
}

// Compress then decompress on the same codec (separate deflate/inflate streams,
// each reset per message for no-context-takeover) must reproduce the input.
bool roundTrips(WebSocketDeflate& codec, std::string_view message) {
    std::pmr::string compressed(std::pmr::get_default_resource());
    if (!codec.compress(message, compressed)) {
        return false;
    }
    std::pmr::string restored(std::pmr::get_default_resource());
    if (codec.decompress(compressed, restored, ProtocolByteLimit::unlimited()) !=
        WebSocketInflateResult::kOk) {
        return false;
    }
    return std::string_view(restored.data(), restored.size()) == message;
}

std::string patterned(std::size_t n) {
    std::string s;
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(static_cast<char>('A' + (i * 7 + (i >> 3)) % 26));
    }
    return s;
}

}  // namespace

RUVIA_TEST(websocket_deflate_construction_yields_a_valid_codec) {
    WebSocketDeflate codec;
}

RUVIA_TEST(websocket_deflate_round_trips_various_sizes) {
    WebSocketDeflate codec;
    RUVIA_CHECK(roundTrips(codec, ""));
    RUVIA_CHECK(roundTrips(codec, "a"));
    RUVIA_CHECK(roundTrips(codec, "hello world"));
    RUVIA_CHECK(roundTrips(codec, std::string(4096, 'z')));  // highly compressible
    RUVIA_CHECK(roundTrips(codec, patterned(10000)));        // varied content
    // Reusing the same codec across messages must keep working (per-message reset).
    RUVIA_CHECK(roundTrips(codec, "second message on the same codec"));
}

RUVIA_TEST(websocket_deflate_inflate_respects_max_bytes) {
    WebSocketDeflate codec;
    std::pmr::string compressed(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.compress(std::string(10000, 'a'), compressed));  // tiny compressed form
    // Decompressing a bomb under a small cap must be refused, not expanded.
    std::pmr::string restored(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(compressed, restored, ProtocolByteLimit::limited(100)) ==
                WebSocketInflateResult::kTooLarge);
    // With a sufficient cap the same payload inflates fully.
    std::pmr::string ok(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(compressed, ok, ProtocolByteLimit::limited(10000)) ==
                WebSocketInflateResult::kOk);
    RUVIA_CHECK_EQ(ok.size(), std::size_t{10000});
}

RUVIA_TEST(websocket_deflate_offer_accepted_forms) {
    // A bare offer, and the common browser offer that only constrains the
    // client's window, are honored (we run a fixed 32 KiB server window).
    RUVIA_CHECK(offersDeflate("permessage-deflate"));
    RUVIA_CHECK(offersDeflate("permessage-deflate; client_max_window_bits"));
    RUVIA_CHECK(offersDeflate("permessage-deflate; client_max_window_bits=15"));
    // The extension name matches case-insensitively.
    RUVIA_CHECK(offersDeflate("PERMESSAGE-DEFLATE"));
    // Surrounding optional whitespace is trimmed before the name compare.
    RUVIA_CHECK(offersDeflate("  permessage-deflate  "));
}

RUVIA_TEST(websocket_deflate_offer_declined_forms) {
    // Nothing offered at all.
    RUVIA_CHECK(!offersDeflate(""));
    // A different extension is not permessage-deflate.
    RUVIA_CHECK(!offersDeflate("permessage-foo"));
    // A superstring name must not match as a whole token.
    RUVIA_CHECK(!offersDeflate("xpermessage-deflate"));
    // An offer pinning a server window is declined: we never shrink our window,
    // so honoring a smaller server_max_window_bits would break the negotiated bound.
    RUVIA_CHECK(!offersDeflate("permessage-deflate; server_max_window_bits=10"));
    // Extension parameter names are case-insensitive.
    RUVIA_CHECK(!offersDeflate("permessage-deflate; Server_Max_Window_Bits=10"));
}

RUVIA_TEST(websocket_deflate_offer_accepts_server_max_window_bits_15) {
    // server_max_window_bits=15 pins exactly our fixed 32 KiB window, so we can
    // honor it. RFC 7692 §7.1.2.1 then requires echoing the accepted value, which
    // the handshake records as the distinct echoed-window alternative.
    const auto pinned = negotiateDeflate("permessage-deflate; server_max_window_bits=15");
    RUVIA_CHECK(pinned == WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits);
    // A quoted value is equivalent to the bare token.
    const auto quoted = negotiateDeflate("permessage-deflate; server_max_window_bits=\"15\"");
    RUVIA_CHECK(quoted == WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits);
    // A bare/browser offer is accepted without echoing server_max_window_bits.
    const auto bare = negotiateDeflate("permessage-deflate; client_max_window_bits");
    RUVIA_CHECK(bare == WebSocketCompression::kPermessageDeflate);
    // A smaller pinned window cannot be honored (we never shrink our compressor).
    RUVIA_CHECK(negotiateDeflate("permessage-deflate; server_max_window_bits=14") ==
                WebSocketCompression::kDisabled);
    // A later offer that permits 15 wins over an earlier too-small one.
    const auto second = negotiateDeflate(
        "permessage-deflate; server_max_window_bits=10, permessage-deflate; "
        "server_max_window_bits=15");
    RUVIA_CHECK(second == WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits);
}

RUVIA_TEST(websocket_deflate_offer_ignores_unrelated_parameters) {
    // RFC 7692 defines the complete parameter set. Unknown parameters make one
    // permessage-deflate offer invalid instead of being silently ignored.
    RUVIA_CHECK(!offersDeflate("permessage-deflate; xserver_max_window_bits=10"));
    // An invalid offer can still be followed by a separate valid offer.
    RUVIA_CHECK(offersDeflate("permessage-deflate; unknown=value, permessage-deflate"));

    // A syntactically malformed extension list invalidates the entire opening
    // handshake; negotiation must not skip it and honor a later offer.
    RUVIA_CHECK(!offersDeflate("x-test; value=\"bad value\", permessage-deflate"));
}

RUVIA_TEST(websocket_deflate_offer_rejects_malformed_parameters) {
    RUVIA_CHECK(!offersDeflate("permessage-deflate; server_max_window_bits"));
    RUVIA_CHECK(!offersDeflate("permessage-deflate; server_max_window_bits=16"));
    RUVIA_CHECK(!offersDeflate("permessage-deflate; client_max_window_bits=7"));
    RUVIA_CHECK(!offersDeflate("permessage-deflate; client_max_window_bits=08"));
    RUVIA_CHECK(!offersDeflate("permessage-deflate; client_max_window_bits=\"09\""));
    RUVIA_CHECK(!offersDeflate("permessage-deflate; server_no_context_takeover=true"));
    RUVIA_CHECK(!offersDeflate("permessage-deflate; client_no_context_takeover=1"));
    RUVIA_CHECK(
        !offersDeflate("permessage-deflate; client_max_window_bits=15; "
                       "client_max_window_bits=14"));
    RUVIA_CHECK(
        !offersDeflate("permessage-deflate; server_max_window_bits=15; "
                       "server_max_window_bits=15"));

    // Quoted-pairs are decoded before the numeric range check.
    RUVIA_CHECK(negotiateDeflate("permessage-deflate; server_max_window_bits=\"1\\5\"") ==
                WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits);

    // A malformed offer does not poison the comma list: a later conforming
    // offer remains independently negotiable.
    RUVIA_CHECK(
        offersDeflate("permessage-deflate; client_max_window_bits=08, "
                      "permessage-deflate; client_max_window_bits=8"));
}

RUVIA_TEST(websocket_deflate_offer_picks_first_honorable_offer) {
    // RFC 7692 permits multiple offers; the server takes the first it can honor.
    // First offer pins a server window (declined), the second is acceptable.
    RUVIA_CHECK(offersDeflate("permessage-deflate; server_max_window_bits=10, permessage-deflate"));
    // An acceptable offer ahead of an unacceptable one still wins.
    RUVIA_CHECK(offersDeflate("permessage-deflate, permessage-deflate; server_max_window_bits=10"));
    // Every offer pins a server window -> declined outright.
    RUVIA_CHECK(
        !offersDeflate("permessage-deflate; server_max_window_bits=8, permessage-deflate; "
                       "server_max_window_bits=10"));
}

RUVIA_TEST(websocket_deflate_offer_spans_multiple_extension_lines) {
    // RFC 6455 §9.1 / RFC 9110 §5.3: an offer may be split across several
    // Sec-WebSocket-Extensions field lines, which are one comma-joined list.
    // Reading only the last line missed permessage-deflate offered earlier.
    const auto negotiateLines = [](std::initializer_list<std::string_view> lines) {
        std::string raw = "GET /ws HTTP/1.1\r\nHost: x\r\n";
        for (const auto line : lines) {
            raw += "Sec-WebSocket-Extensions: ";
            raw.append(line.data(), line.size());
            raw += "\r\n";
        }
        raw += "\r\n";
        Http1ServerRequestParser parser;
        const auto result = parser.parseMessage(raw);
        return webSocketNegotiatePermessageDeflate(result.request);
    };

    // permessage-deflate on the FIRST line, an unrelated extension on the second:
    // previously the last line was the only one read, so this was missed.
    RUVIA_CHECK(
        webSocketDeflateNegotiated(negotiateLines({"permessage-deflate", "x-unknown; a=1"})));
    // On the second line it still works (the old last-line behavior is preserved).
    RUVIA_CHECK(webSocketDeflateNegotiated(negotiateLines({"x-unknown", "permessage-deflate"})));
    // A per-line server_max_window_bits=15 is honored wherever the line sits.
    RUVIA_CHECK(negotiateLines({"x-unknown", "permessage-deflate; server_max_window_bits=15"}) ==
                WebSocketCompression::kPermessageDeflateWithServerMaxWindowBits);
    // No permessage-deflate on any line -> not enabled.
    RUVIA_CHECK(negotiateLines({"x-unknown", "y-unknown"}) == WebSocketCompression::kDisabled);
}

RUVIA_TEST(websocket_deflate_rejects_corrupt_input) {
    WebSocketDeflate codec;
    std::pmr::string restored(std::pmr::get_default_resource());
    // Random bytes are not a valid raw-DEFLATE block; inflate must report an error.
    const auto result =
        codec.decompress("\xff\xff\xff\xff\xff\xff", restored, ProtocolByteLimit::unlimited());
    RUVIA_CHECK(result == WebSocketInflateResult::kError);

    // 03 00 is a complete raw-DEFLATE empty stream (BFINAL=1). A WebSocket
    // permessage-deflate payload must instead be the non-terminating prefix of
    // a Z_SYNC_FLUSH stream; accepting this would also let zlib ignore bytes
    // appended after the early stream end.
    std::pmr::string finalStreamOutput(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(std::string_view("\x03\x00", 2), finalStreamOutput,
                    ProtocolByteLimit::unlimited()) == WebSocketInflateResult::kError);

    std::pmr::string trailingOutput(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(std::string_view("\x03\x00junk", 6), trailingOutput,
                    ProtocolByteLimit::unlimited()) == WebSocketInflateResult::kError);
}
