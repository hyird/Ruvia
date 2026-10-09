#include <algorithm>
#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <variant>
#include <vector>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/http/HttpResponseStream.h"

#include "test_harness.h"

namespace {
void ignore_event(void*, const ruvia::Http3ConnectionEvent&) {}

std::vector<char> settings_wire(ruvia::Http3Settings settings) {
    std::array<char, 128> bytes{};
    const auto payload = ruvia::encodeHttp3Settings(std::span(bytes).subspan(16), settings);
    const auto frame = ruvia::encodeHttp3FrameHeader(bytes, 4, std::get<0>(payload));
    std::vector<char> wire{0};
    wire.insert(wire.end(), bytes.begin(), bytes.begin() + std::get<0>(frame));
    wire.insert(wire.end(), bytes.begin() + 16, bytes.begin() + 16 + std::get<0>(payload));
    return wire;
}

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes{};
    std::size_t allocations{};
    std::size_t returns{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        live_bytes += bytes;
        ++allocations;
        return value;
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        live_bytes -= bytes;
        ++returns;
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

enum class response_kind { buffered,
    streaming,
    interim,
    trailers,
    connect };

std::variant<std::size_t, ruvia::Http3ResponseHeadFailure> encode_response(
    ruvia::Http3Connection& connection, response_kind kind, ruvia::Http3FieldSectionLimits limits = {}) {
    if (kind == response_kind::trailers) {
        const std::array fields{ruvia::Http3FieldSectionFieldView{"x-repeated", "value"}};
        auto result = connection.encodeResponseTrailers(0, fields, limits);
        if ((result.index() != 0)) {
            return std::get<1>(result);
        }
        return std::get<0>(result).decodedFieldSectionSize();
    }
    if (kind == response_kind::interim) {
        const std::array fields{ruvia::HttpHeaderView{"x-repeated", "value"}};
        auto result = connection.encodeInterimResponseHead(0, ruvia::HttpInterimResponseHead(ruvia::http_status::kEarlyHints, fields), limits);
        if ((result.index() != 0)) {
            return std::get<1>(result);
        }
        return std::get<0>(result).field_section.decodedFieldSectionSize();
    }
    ruvia::HttpResponse response;
    response.header("date", "Thu, 01 Jan 1970 00:00:00 GMT");
    response.header("x-repeated", "value");
    if (kind == response_kind::streaming) {
        auto result = connection.encodeStreamingResponseHead(0, std::move(response), ruvia::HttpKnownMethod::kGet,
            ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none, limits);
        if ((result.index() != 0)) {
            return std::get<1>(result);
        }
        return std::get<0>(result).head.field_section.decodedFieldSectionSize();
    }
    auto result = kind == response_kind::connect
                      ? connection.encodeConnectResponseHead(0, response, limits)
                      : connection.encodeResponseHead(0, response, ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response), limits);
    if ((result.index() != 0)) {
        return std::get<1>(result);
    }
    return std::get<0>(result).field_section.decodedFieldSectionSize();
}
}  // namespace

RUVIA_TEST(http3_connection_response_encoding_applies_peer_decoded_limit_before_qpack) {
    for (const auto kind : {response_kind::buffered, response_kind::streaming, response_kind::interim, response_kind::trailers, response_kind::connect}) {
        ruvia::Http3Connection baseline(ruvia::Http3PeerRole::kServer, std::pmr::get_default_resource());
        const auto size = encode_response(baseline, kind);
        RUVIA_CHECK((size.index() == 0));
        if ((size.index() != 0)) {
            continue;
        }
        for (const auto peer_limit : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0},
                 std::optional<std::uint64_t>{std::get<0>(size) - 1}, std::optional<std::uint64_t>{std::get<0>(size)},
                 std::optional<std::uint64_t>{std::get<0>(size) + 1}, std::optional<std::uint64_t>{ruvia::kHttp3VarIntMax}}) {
            ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, std::pmr::get_default_resource());
            const auto settings = settings_wire({.qpackMaxTableCapacity = 512, .maxFieldSectionSize = peer_limit, .qpackBlockedStreams = 2});
            RUVIA_CHECK(connection.feed(2, settings, false, false, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
            const auto pending = connection.pendingQpackEncoderOutput();
            const std::vector<char> before(pending.begin(), pending.end());
            const auto result = encode_response(connection, kind);
            const bool allowed = !peer_limit || *peer_limit >= std::get<0>(size);
            RUVIA_CHECK_EQ((result.index() == 0), allowed);
            if (!allowed && (result.index() != 0)) {
                RUVIA_CHECK(std::get<1>(result).kind == ruvia::Http3ResponseHeadError::peer_field_section_limit);
                RUVIA_CHECK(std::get<1>(result).fieldSectionError == ruvia::Http3FieldSectionError::kFieldListTooLarge);
                const auto after = connection.pendingQpackEncoderOutput();
                RUVIA_CHECK(std::vector<char>(after.begin(), after.end()) == before);
                // A refused response must not poison the shared encoder.
                RUVIA_CHECK((connection.encodeFieldSection(4, {}).index() == 0));
            }
            if (allowed) {
                RUVIA_CHECK_EQ(std::get<0>(result), std::get<0>(size));
                ruvia::Http3FieldSectionLimits local;
                local.maxDecodedBytes = std::get<0>(size) - 1;
                const auto refused = encode_response(connection, kind, local);
                RUVIA_CHECK((refused.index() != 0));
                if ((refused.index() != 0)) {
                    RUVIA_CHECK(std::get<1>(refused).kind == ruvia::Http3ResponseHeadError::kFieldSectionError);
                    RUVIA_CHECK(std::get<1>(refused).fieldSectionError == ruvia::Http3FieldSectionError::kFieldListTooLarge);
                }
            }
        }
    }
}

RUVIA_TEST(http3_connection_response_encoding_applies_normalized_local_budgets) {
    const auto encode = [&ruvia_ctx](ruvia::Http3ConnectionConfig config, std::uint64_t peer_limit,
                            ruvia::Http3FieldSectionLimits requested) {
        ruvia::Http3Connection connection(ruvia::Http3PeerRole::kServer, std::pmr::get_default_resource(), config);
        // Keep dynamic compression disabled so the encoded-byte budget test
        // measures the emitted literal, rather than a valid compact reference.
        const auto settings = settings_wire({.maxFieldSectionSize = peer_limit});
        RUVIA_CHECK(connection.feed(2, settings, false, false, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
        ruvia::HttpResponse response;
        response.header("date", "Thu, 01 Jan 1970 00:00:00 GMT");
        response.header("x-pad", std::string(200, 'x'));
        return connection.encodeResponseHead(0, response,
            ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response), requested);
    };
    constexpr ruvia::Http3FieldSectionLimits requested{65536, 65536, 256};
    const auto baseline = encode({}, 512, requested);
    RUVIA_CHECK((baseline.index() == 0));
    if ((baseline.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(baseline).field_section.decodedFieldSectionSize(), std::size_t{391});
    }
    const ruvia::Http3ConnectionConfig decoded_limit{.maxFieldSectionSize = 256};
    for (const auto peer_limit : {512, 384}) {
        const auto result = encode(decoded_limit, peer_limit, requested);
        RUVIA_CHECK((result.index() != 0));
        if ((result.index() != 0)) {
            RUVIA_CHECK(std::get<1>(result).kind == ruvia::Http3ResponseHeadError::kFieldSectionError);
            RUVIA_CHECK(std::get<1>(result).fieldSectionError == ruvia::Http3FieldSectionError::kFieldListTooLarge);
        }
    }
    const auto peer_limited = encode(decoded_limit, 128, requested);
    RUVIA_CHECK((peer_limited.index() != 0));
    if ((peer_limited.index() != 0)) {
        RUVIA_CHECK(std::get<1>(peer_limited).kind == ruvia::Http3ResponseHeadError::peer_field_section_limit);
        RUVIA_CHECK(std::get<1>(peer_limited).fieldSectionError == ruvia::Http3FieldSectionError::kFieldListTooLarge);
    }

    const ruvia::Http3ConnectionConfig fields_limit{.maxFields = 1};
    const auto too_many_fields = encode(fields_limit, 65536, requested);
    RUVIA_CHECK((too_many_fields.index() != 0));
    if ((too_many_fields.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_many_fields).kind == ruvia::Http3ResponseHeadError::kFieldSectionError);
        RUVIA_CHECK(std::get<1>(too_many_fields).fieldSectionError == ruvia::Http3FieldSectionError::kTooManyFields);
    }
    const ruvia::Http3ConnectionConfig encoded_limit{.maxEncodedFieldSectionBytes = 100};
    const auto too_many_encoded_bytes = encode(encoded_limit, 65536, requested);
    RUVIA_CHECK((too_many_encoded_bytes.index() != 0));
    if ((too_many_encoded_bytes.index() != 0)) {
        RUVIA_CHECK(std::get<1>(too_many_encoded_bytes).kind == ruvia::Http3ResponseHeadError::kFieldSectionError);
        RUVIA_CHECK(std::get<1>(too_many_encoded_bytes).fieldSectionError == ruvia::Http3FieldSectionError::kFieldSectionTooLarge);
    }
}

RUVIA_TEST(http3_connection_client_request_and_push_promise_share_peer_limit) {
    // :method GET (42), :scheme https (44), :authority example.test (54),
    // and :path / (38), including the RFC 9114 per-field overhead.
    constexpr std::uint64_t decoded_size = 178;
    for (const auto peer_limit : {decoded_size - 1, decoded_size, decoded_size + 1}) {
        ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, std::pmr::get_default_resource());
        auto settings = settings_wire({.qpackMaxTableCapacity = 512, .maxFieldSectionSize = peer_limit, .qpackBlockedStreams = 2});
        RUVIA_CHECK(client.feed(3, settings, false, false, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
        const auto output = client.pendingQpackEncoderOutput();
        const std::vector<char> before(output.begin(), output.end());
        const auto request = client.encodeClientRequestHead(0,
            {.method = "GET", .scheme = "https", .authority = "example.test", .path = "/"});
        RUVIA_CHECK_EQ((request.index() == 0), peer_limit >= decoded_size);
        if ((request.index() != 0)) {
            const auto after = client.pendingQpackEncoderOutput();
            RUVIA_CHECK(std::vector<char>(after.begin(), after.end()) == before);
        }

        ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, std::pmr::get_default_resource());
        // Authorize Push ID 0 on the same client control stream as SETTINGS.
        settings.push_back(static_cast<char>(0xd));
        settings.push_back(1);
        settings.push_back(0);
        RUVIA_CHECK(server.feed(2, settings, false, false, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
        const auto promise = server.preparePushPromise(0, 0, {.authority = "example.test"});
        RUVIA_CHECK_EQ((promise.index() == 0), peer_limit >= decoded_size);
        RUVIA_CHECK_EQ(server.promisedRequest(0) != nullptr, peer_limit >= decoded_size);
    }
}

RUVIA_TEST(http3_reset_before_headers_releases_unpublished_dynamic_references) {
    for (const bool goaway : {false, true}) {
        counting_resource memory;
        {
            ruvia::Http3Connection client(ruvia::Http3PeerRole::kClient, &memory);
            ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, &memory,
                {.qpackMaxTableCapacity = 64, .qpackBlockedStreams = 1});
            if (goaway) {
                RUVIA_CHECK((server.prepareGoaway(0).index() == 0));
            }
            const auto settings = settings_wire({.qpackMaxTableCapacity = 64, .qpackBlockedStreams = 1});
            RUVIA_CHECK(client.feed(3, settings, false, false, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
            std::size_t warmed_live_bytes{};
            std::optional<std::pmr::vector<char>> retained;
            std::vector<char> retained_bytes;
            for (unsigned round = 0; round != 128; ++round) {
                const auto stream_id = 4 * round;
                {
                    const std::array fields{ruvia::Http3FieldSectionFieldView{round % 2 == 0 ? "x-first" : "x-other", "value"}};
                    auto unsent = client.encodeFieldSection(stream_id, fields);
                    RUVIA_CHECK((unsent.index() == 0));
                    const auto instructions = client.pendingQpackEncoderOutput();
                    // This table holds one field. Every new insertion requires
                    // release of the preceding stream's dynamic reference pin.
                    RUVIA_CHECK(!instructions.empty());
                    std::vector<char> encoder_wire;
                    if (round == 0) {
                        encoder_wire.push_back(2);
                    }
                    encoder_wire.insert(encoder_wire.end(), instructions.begin(), instructions.end());
                    RUVIA_CHECK(server.feed(6, encoder_wire, false, false, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
                    RUVIA_CHECK(client.consumeQpackEncoderOutput(instructions.size()));
                    RUVIA_CHECK_EQ(server.activeRequestCount(), std::size_t{0});

                    // Reset arrives without any request HEADERS/parser state.
                    RUVIA_CHECK(server.feed(stream_id, {}, false, true, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
                    const auto cancellation = server.pendingQpackDecoderOutput();
                    std::vector<char> decoder_wire;
                    if (round == 0) {
                        decoder_wire.push_back(3);
                    }
                    decoder_wire.insert(decoder_wire.end(), cancellation.begin(), cancellation.end());
                    RUVIA_CHECK(client.feed(11, decoder_wire, false, false, ignore_event, nullptr).scope == ruvia::Http3ConnectionErrorScope::kNone);
                    RUVIA_CHECK(server.consumeQpackDecoderOutput(cancellation.size()));
                    RUVIA_CHECK_EQ(server.activeRequestCount(), std::size_t{0});
                    if (round == 0) {
                        retained.emplace(std::move(std::get<0>(unsent)));
                        retained_bytes.assign(retained->begin(), retained->end());
                    }
                    RUVIA_CHECK(std::ranges::equal(*retained, retained_bytes));
                }
                if (round < 64) {
                    // Observe the bounded deque/buffer high-water mark rather than
                    // mistaking cached blocks or longer QPACK integers for a leak.
                    warmed_live_bytes = std::max(warmed_live_bytes, memory.live_bytes);
                } else {
                    RUVIA_CHECK(memory.live_bytes <= warmed_live_bytes);
                }
            }
            if (goaway) {
                const auto rejected = server.feed(512, {}, false, false, ignore_event, nullptr);
                RUVIA_CHECK(rejected.status == ruvia::Http3ConnectionStatus::kStreamError);
                RUVIA_CHECK(rejected.code == ruvia::Http3ConnectionErrorCode::kRequestRejected);
            }
        }
        RUVIA_CHECK_EQ(memory.live_bytes, std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
    }
}

RUVIA_TEST(http3_connection_request_and_generic_encoding_apply_peer_decoded_limit) {
    for (const auto role : {ruvia::Http3PeerRole::kClient, ruvia::Http3PeerRole::kServer}) {
        ruvia::Http3Connection connection(role, std::pmr::get_default_resource());
        const auto settings = settings_wire({.qpackMaxTableCapacity = 512, .maxFieldSectionSize = 39, .qpackBlockedStreams = 2});
        RUVIA_CHECK(connection.feed(role == ruvia::Http3PeerRole::kClient ? 3 : 2,
                                  settings, false, false, ignore_event, nullptr)
                        .scope == ruvia::Http3ConnectionErrorScope::kNone);
        const std::array fields{ruvia::Http3FieldSectionFieldView{":status", "200"}};
        RUVIA_CHECK((connection.encodeFieldSection(0, fields).index() != 0));  // 32 + 7 + 3 = 42
        const std::array fitting{ruvia::Http3FieldSectionFieldView{"x", "value"}};
        RUVIA_CHECK((connection.encodeFieldSection(4, fitting).index() == 0));  // 38 decoded bytes
        if (role == ruvia::Http3PeerRole::kClient) {
            RUVIA_CHECK((connection.encodeClientRequestHead(8, {.method = "GET", .scheme = "https", .authority = "example.test", .path = "/"}).index() != 0));
        }
    }
}
