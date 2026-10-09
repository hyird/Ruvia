#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http1/http1_server_request_parser.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "test_harness.h"
#include "websocket/http_websocket_permessage_deflate.h"

namespace {

using ruvia::protocol_byte_limit;
using ruvia::websocket_compression;
using ruvia::detail::http1_server_request_parser;
using ruvia::detail::websocket_deflate;
using ruvia::detail::websocket_deflate_negotiated;
using ruvia::detail::websocket_inflate_result;
using ruvia::detail::websocket_negotiate_permessage_deflate;

// Parses a websocket upgrade carrying `extensions` as its Sec-websocket-Extensions
// value and reports how the server would negotiate permessage-deflate for it.
// (parser/raw stay alive across the call: request headers view into raw.)
websocket_compression negotiate_deflate(std::string_view extensions) {
    std::string raw = "GET /ws HTTP/1.1\r\nHost: x\r\n";
    if (!extensions.empty()) {
        raw += "Sec-WebSocket-Extensions: ";
        raw.append(extensions.data(), extensions.size());
        raw += "\r\n";
    }
    raw += "\r\n";
    http1_server_request_parser parser;
    const auto result_value = parser.parse_message(raw);
    return websocket_negotiate_permessage_deflate(result_value.request_);
}

bool offers_deflate(std::string_view extensions) {
    return websocket_deflate_negotiated(negotiate_deflate(extensions));
}

// Compress then decompress on the same codec (separate deflate/inflate streams,
// each reset per message for no-context-takeover) must reproduce the input.
bool round_trips(websocket_deflate& codec, std::string_view message) {
    std::pmr::string compressed(std::pmr::get_default_resource());
    if (!codec.compress(message, compressed)) {
        return false;
    }
    std::pmr::string restored(std::pmr::get_default_resource());
    if (codec.decompress(compressed, restored, protocol_byte_limit::unlimited()) !=
        websocket_inflate_result::ok) {
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

RUVIA_TEST(websocket_deflate_takeover_level_nine_and_discarded_trial) {
    websocket_deflate sender(9, true), receiver(9, true);
    const auto payload_value = patterned(12000);
    std::size_t first_size = 0;
    for (int i = 0; i < 100; ++i) {
        std::pmr::string compressed, restored;
        RUVIA_CHECK(sender.compress(payload_value, compressed));
        if (i == 0) {
            first_size = compressed.size();
        }
        if (i == 1) {
            RUVIA_CHECK(compressed.size() < first_size);
        }
        RUVIA_CHECK(receiver.decompress(compressed, restored, protocol_byte_limit::limited(payload_value.size())) == websocket_inflate_result::ok);
        RUVIA_CHECK_EQ(std::string_view(restored), std::string_view(payload_value));
        if (i == 50) {
            std::pmr::string discarded;
            RUVIA_CHECK(sender.compress("trial-never-transmitted", discarded));
            sender.discard_compression();
        }
    }
    for (const int invalid : {-1, 10}) {
        bool rejected = false;
        try {
            websocket_deflate codec(invalid, true);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
    websocket_deflate new_sender(9, true), new_receiver(9, true);
    RUVIA_CHECK(round_trips(new_sender, payload_value));
    std::pmr::string compressed, restored;
    RUVIA_CHECK(new_receiver.compress(std::string(8192, 'a'), compressed));
    RUVIA_CHECK(new_receiver.decompress(compressed, restored, protocol_byte_limit::limited(100)) == websocket_inflate_result::too_large);
}

RUVIA_TEST(websocket_deflate_takeover_respects_peer_offer) {
    using ruvia::detail::websocket_parse_deflate_offer;
    RUVIA_CHECK(websocket_parse_deflate_offer("permessage-deflate", true) == (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}));
    RUVIA_CHECK(websocket_parse_deflate_offer("permessage-deflate; server_max_window_bits=15", true) == (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .server_max_window_bits_ = 15}));
    for (const auto offer : {"permessage-deflate; server_no_context_takeover", "permessage-deflate; client_no_context_takeover"}) {
        RUVIA_CHECK(websocket_parse_deflate_offer(offer, true) == (websocket_compression{.enabled_ = true}));
    }
    RUVIA_CHECK(websocket_parse_deflate_offer("permessage-deflate; server_max_window_bits=14", true)->server_max_window_bits_ == 14);
    const std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
    http1_server_request_parser parser;
    const auto parsed_value = parser.parse_message(raw);
    RUVIA_CHECK(websocket_negotiate_permessage_deflate(parsed_value.request_, {.enabled_ = false}) == (websocket_compression{}));
    RUVIA_CHECK(websocket_negotiate_permessage_deflate(parsed_value.request_, {.compression_level_ = 9, .context_takeover_ = true}) == (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}));
}

RUVIA_TEST(websocket_deflate_construction_yields_a_valid_codec) {
    websocket_deflate codec;
}

RUVIA_TEST(websocket_deflate_round_trips_various_sizes) {
    websocket_deflate codec;
    RUVIA_CHECK(round_trips(codec, ""));
    RUVIA_CHECK(round_trips(codec, "a"));
    RUVIA_CHECK(round_trips(codec, "hello world"));
    RUVIA_CHECK(round_trips(codec, std::string(4096, 'z')));  // highly compressible
    RUVIA_CHECK(round_trips(codec, patterned(10000)));        // varied content
    // Reusing the same codec across messages must keep working (per-message reset).
    RUVIA_CHECK(round_trips(codec, "second message on the same codec"));
}

RUVIA_TEST(websocket_deflate_inflate_respects_max_bytes) {
    websocket_deflate codec;
    std::pmr::string compressed(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.compress(std::string(10000, 'a'), compressed));  // tiny compressed form
    // Decompressing a bomb under a small cap must be refused, not expanded.
    std::pmr::string restored(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(compressed, restored, protocol_byte_limit::limited(100)) ==
                websocket_inflate_result::too_large);
    // With a sufficient cap the same payload inflates fully.
    std::pmr::string ok(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(compressed, ok, protocol_byte_limit::limited(10000)) ==
                websocket_inflate_result::ok);
    RUVIA_CHECK_EQ(ok.size(), std::size_t{10000});
}

RUVIA_TEST(websocket_deflate_offer_accepted_forms) {
    // A bare offer, and the common browser offer that only constrains the
    // client's window, are honored.
    RUVIA_CHECK(offers_deflate("permessage-deflate"));
    RUVIA_CHECK(offers_deflate("permessage-deflate; client_max_window_bits"));
    RUVIA_CHECK(offers_deflate("permessage-deflate; client_max_window_bits=15"));
    // The extension name matches case-insensitively.
    RUVIA_CHECK(offers_deflate("PERMESSAGE-DEFLATE"));
    // Surrounding optional whitespace is trimmed before the name compare.
    RUVIA_CHECK(offers_deflate("  permessage-deflate  "));
}

RUVIA_TEST(websocket_deflate_offer_declined_forms) {
    // Nothing offered at all.
    RUVIA_CHECK(!offers_deflate(""));
    // A different extension is not permessage-deflate.
    RUVIA_CHECK(!offers_deflate("permessage-foo"));
    // A superstring name must not match as a whole token.
    RUVIA_CHECK(!offers_deflate("xpermessage-deflate"));
    // A smaller server window is honored by the compressor.
    RUVIA_CHECK(offers_deflate("permessage-deflate; server_max_window_bits=10"));
    // Extension parameter names are case-insensitive.
    RUVIA_CHECK(offers_deflate("permessage-deflate; Server_Max_Window_Bits=10"));
}

RUVIA_TEST(websocket_deflate_offer_accepts_server_max_window_bits_15) {
    // The accepted window is echoed per RFC 7692 section 7.1.2.1.
    const auto pinned = negotiate_deflate("permessage-deflate; server_max_window_bits=15");
    RUVIA_CHECK(pinned == (websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));
    // A quoted value is equivalent to the bare token.
    const auto quoted = negotiate_deflate("permessage-deflate; server_max_window_bits=\"15\"");
    RUVIA_CHECK(quoted == (websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));
    // A bare/browser offer is accepted without echoing server_max_window_bits.
    const auto bare = negotiate_deflate("permessage-deflate; client_max_window_bits");
    RUVIA_CHECK(bare == (websocket_compression{.enabled_ = true}));
    // Smaller windows are represented exactly.
    RUVIA_CHECK(negotiate_deflate("permessage-deflate; server_max_window_bits=14").server_max_window_bits_ == 14);
    // The first valid offer wins.
    const auto second = negotiate_deflate(
        "permessage-deflate; server_max_window_bits=10, permessage-deflate; "
        "server_max_window_bits=15");
    RUVIA_CHECK(second == (websocket_compression{.enabled_ = true, .server_max_window_bits_ = 10}));
}

RUVIA_TEST(websocket_deflate_offer_ignores_unrelated_parameters) {
    // RFC 7692 defines the complete parameter set. Unknown parameters make one
    // permessage-deflate offer invalid instead of being silently ignored.
    RUVIA_CHECK(!offers_deflate("permessage-deflate; xserver_max_window_bits=10"));
    // An invalid offer can still be followed by a separate valid offer.
    RUVIA_CHECK(offers_deflate("permessage-deflate; unknown=value, permessage-deflate"));

    // A syntactically malformed extension list invalidates the entire opening
    // handshake; negotiation must not skip it and honor a later offer.
    RUVIA_CHECK(!offers_deflate("x-test; value=\"bad value\", permessage-deflate"));
}

RUVIA_TEST(websocket_deflate_offer_rejects_malformed_parameters) {
    RUVIA_CHECK(!offers_deflate("permessage-deflate; server_max_window_bits"));
    RUVIA_CHECK(!offers_deflate("permessage-deflate; server_max_window_bits=16"));
    RUVIA_CHECK(!offers_deflate("permessage-deflate; client_max_window_bits=7"));
    RUVIA_CHECK(!offers_deflate("permessage-deflate; client_max_window_bits=08"));
    RUVIA_CHECK(!offers_deflate("permessage-deflate; client_max_window_bits=\"09\""));
    RUVIA_CHECK(!offers_deflate("permessage-deflate; server_no_context_takeover=true"));
    RUVIA_CHECK(!offers_deflate("permessage-deflate; client_no_context_takeover=1"));
    RUVIA_CHECK(
        !offers_deflate("permessage-deflate; client_max_window_bits=15; "
                        "client_max_window_bits=14"));
    RUVIA_CHECK(
        !offers_deflate("permessage-deflate; server_max_window_bits=15; "
                        "server_max_window_bits=15"));

    // Quoted-pairs are decoded before the numeric range check.
    RUVIA_CHECK(negotiate_deflate("permessage-deflate; server_max_window_bits=\"1\\5\"") ==
                (websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));

    // A malformed offer does not poison the comma list: a later conforming
    // offer remains independently negotiable.
    RUVIA_CHECK(
        offers_deflate("permessage-deflate; client_max_window_bits=08, "
                       "permessage-deflate; client_max_window_bits=8"));
}

RUVIA_TEST(websocket_deflate_offer_picks_first_honorable_offer) {
    // RFC 7692 permits multiple offers; the server takes the first it can honor.
    // A valid pinned window is independently negotiable.
    RUVIA_CHECK(offers_deflate("permessage-deflate; server_max_window_bits=10, permessage-deflate"));
    // An acceptable offer ahead of an unacceptable one still wins.
    RUVIA_CHECK(offers_deflate("permessage-deflate, permessage-deflate; server_max_window_bits=10"));
    // Every valid pinned window is supported.
    RUVIA_CHECK(
        offers_deflate("permessage-deflate; server_max_window_bits=8, permessage-deflate; "
                       "server_max_window_bits=10"));
}

RUVIA_TEST(websocket_deflate_offer_spans_multiple_extension_lines) {
    // RFC 6455 §9.1 / RFC 9110 §5.3: an offer may be split across several
    // Sec-websocket-Extensions field lines, which are one comma-joined list.
    // Reading only the last line missed permessage-deflate offered earlier.
    const auto negotiate_lines = [](std::initializer_list<std::string_view> lines) {
        std::string raw = "GET /ws HTTP/1.1\r\nHost: x\r\n";
        for (const auto line : lines) {
            raw += "Sec-WebSocket-Extensions: ";
            raw.append(line.data(), line.size());
            raw += "\r\n";
        }
        raw += "\r\n";
        http1_server_request_parser parser;
        const auto result_value = parser.parse_message(raw);
        return websocket_negotiate_permessage_deflate(result_value.request_);
    };

    // permessage-deflate on the FIRST line, an unrelated extension on the second:
    // previously the last line was the only one read, so this was missed.
    RUVIA_CHECK(
        websocket_deflate_negotiated(negotiate_lines({"permessage-deflate", "x-unknown; a=1"})));
    // On the second line it still works (the old last-line behavior is preserved).
    RUVIA_CHECK(websocket_deflate_negotiated(negotiate_lines({"x-unknown", "permessage-deflate"})));
    // A per-line server_max_window_bits=15 is honored wherever the line sits.
    RUVIA_CHECK(negotiate_lines({"x-unknown", "permessage-deflate; server_max_window_bits=15"}) ==
                (websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));
    // No permessage-deflate on any line -> not enabled.
    RUVIA_CHECK(negotiate_lines({"x-unknown", "y-unknown"}) == (websocket_compression{}));
}

RUVIA_TEST(websocket_deflate_rejects_corrupt_input) {
    websocket_deflate codec;
    std::pmr::string restored(std::pmr::get_default_resource());
    // Random bytes are not a valid raw-DEFLATE block; inflate must report an error.
    const auto result_value =
        codec.decompress("\xff\xff\xff\xff\xff\xff", restored, protocol_byte_limit::unlimited());
    RUVIA_CHECK(result_value == websocket_inflate_result::error);

    // A complete stream without the trailing empty-block header is truncated.
    std::pmr::string final_stream_output(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(std::string_view("\x03\x00", 2), final_stream_output,
                    protocol_byte_limit::unlimited()) == websocket_inflate_result::error);

    std::pmr::string trailing_output(std::pmr::get_default_resource());
    RUVIA_CHECK(codec.decompress(std::string_view("\x03\x00junk", 6), trailing_output,
                    protocol_byte_limit::unlimited()) == websocket_inflate_result::error);
}
