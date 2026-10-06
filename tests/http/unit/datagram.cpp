#include <array>
#include <string>

#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/HttpDatagram.h"

#include "test_harness.h"

namespace {
struct Capture {
    std::string body;
    std::uint64_t type{0};
    std::size_t ends{0};
};
void capsule(void* context, ruvia::HttpCapsuleEvent event) {
    auto& capture = *static_cast<Capture*>(context);
    capture.type = event.type;
    if (!event.payload.empty()) {
        capture.body.append(event.payload.data(), event.payload.size());
    }
    capture.ends += event.endCapsule;
}
}  // namespace
RUVIA_TEST(http_capsule_protocol_byte_parameters_accept_optional_padding_and_validate_quartets) {
    for (const auto value : {"?1;bytes=:YQ=:", "?1;bytes=:YQ==:", "?1;bytes=:YQ:", "?1;bytes=:YWJj:",
             "?1;bytes=:YWJ:", "?1;bytes=:YWJ=:", "?1;bytes=:YR==:", "?1;bytes=::"}) {
        const auto parsed = ruvia::parseHttpCapsuleProtocol(value);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK(*parsed);
        }
    }
    const auto disabled = ruvia::parseHttpCapsuleProtocol("?0;bytes=:YQ=:");
    RUVIA_CHECK(disabled.has_value());
    if (disabled) {
        RUVIA_CHECK(!*disabled);
    }
    for (const auto value : {"?1;bytes=:Y:", "?1;bytes=:Y=:", "?1;bytes=:YWJj=:", "?1;bytes=:YWJ==:",
             "?1;bytes=:YQ===:", "?1;bytes=:Y=Q:", "?1;bytes=:YQ$:", "?1;bytes=:YQ\n:", "?1;bytes=:YQ"}) {
        RUVIA_CHECK(!ruvia::parseHttpCapsuleProtocol(value));
    }
}

RUVIA_TEST(http_datagram_quarter_stream_id_and_udp_context) {
    std::array<char, 16> prefix{};
    auto count = ruvia::encodeHttp3DatagramPrefix(prefix, 4096);
    RUVIA_CHECK(count.has_value());
    auto decoded = ruvia::decodeHttp3Datagram(std::span<const char>(prefix).first(*count));
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK_EQ(decoded->streamId, 4096u);
    RUVIA_CHECK(decoded->payload.empty());
    RUVIA_CHECK(!ruvia::encodeHttp3DatagramPrefix(prefix, 1).has_value());
    auto oversized = ruvia::encodeHttp3VarInt(prefix, std::uint64_t{1} << 60);
    RUVIA_CHECK(!ruvia::decodeHttp3Datagram(std::span<const char>(prefix).first(*oversized)).has_value());
    RUVIA_CHECK(!ruvia::decodeHttp3Datagram({}).has_value());
    auto context = ruvia::encodeHttpUdpDatagramPrefix(prefix);
    auto udp = ruvia::decodeHttpUdpDatagram(std::span<const char>(prefix).first(*context));
    RUVIA_CHECK(udp.has_value());
    RUVIA_CHECK_EQ(udp->contextId, 0u);
}
RUVIA_TEST(http_capsule_incremental_unknown_empty_and_datagram) {
    std::array<char, 16> prefix{};
    auto count = ruvia::encodeHttpCapsuleHeader(prefix, 0x1234, 5);
    std::string wire(prefix.data(), *count);
    wire += "hello";
    auto empty = ruvia::encodeHttpCapsuleHeader(prefix, 0, 0);
    wire.append(prefix.data(), *empty);
    ruvia::HttpCapsuleDecoder decoder;
    Capture capture;
    for (const char& byte : wire) {
        RUVIA_CHECK(decoder.feed({&byte, 1}, false, capsule, &capture) == ruvia::HttpCapsuleStatus::kNeedMoreData);
    }
    RUVIA_CHECK_EQ(capture.body, std::string("hello"));
    RUVIA_CHECK_EQ(capture.ends, 2u);
    RUVIA_CHECK(decoder.feed({}, true, capsule, &capture) == ruvia::HttpCapsuleStatus::kEnd);
}
RUVIA_TEST(http_capsule_truncated_and_length_limit_are_terminal) {
    const std::array prefix{char(0), char(3)};
    ruvia::HttpCapsuleDecoder limit({.maxCapsuleLength = 2});
    RUVIA_CHECK(limit.feed(prefix, false, nullptr, nullptr) == ruvia::HttpCapsuleStatus::kLimit);
    ruvia::HttpCapsuleDecoder truncated;
    RUVIA_CHECK(truncated.feed(prefix, true, nullptr, nullptr) == ruvia::HttpCapsuleStatus::kTruncated);
}
RUVIA_TEST(http3_datagram_settings_validate_boolean_and_round_trip) {
    std::array<char, 128> bytes{};
    auto size = ruvia::encodeHttp3Settings(bytes, {.h3Datagram = true});
    RUVIA_CHECK(size.has_value());
    auto decoded = ruvia::decodeHttp3Settings(std::span<const char>(bytes).first(*size));
    RUVIA_CHECK(decoded.has_value());
    RUVIA_CHECK(decoded->h3Datagram);
    const std::array invalid{char(0x33), char(2)};
    RUVIA_CHECK(!ruvia::decodeHttp3Settings(invalid).has_value());
}

RUVIA_TEST(http_capsule_pull_decoder_stops_at_each_capsule_and_commits_fin_after_the_suffix) {
    std::array<char, 16> header;
    std::string wire;
    for (const auto& item : std::array<std::pair<std::uint64_t, std::string_view>, 3>{{{0x123456789ULL, "first"}, {0, ""}, {7, "last"}}}) {
        const auto encoded = ruvia::encodeHttpCapsuleHeader(header, item.first, item.second.size());
        wire.append(header.data(), *encoded);
        wire.append(item.second);
    }
    for (std::size_t block : {std::size_t{1}, std::size_t{3}, wire.size()}) {
        ruvia::HttpCapsuleDecoder decoder;
        Capture captured;
        std::size_t offset{}, complete{};
        std::string allPayload;
        while (offset != wire.size()) {
            const auto count = std::min(block, wire.size() - offset);
            const auto result = decoder.feedOne(std::span<const char>(wire).subspan(offset, count), offset + count == wire.size(), capsule, &captured);
            RUVIA_CHECK(result.consumedBytes > 0 && result.consumedBytes <= count);
            offset += result.consumedBytes;
            if (result.capsuleComplete) {
                const std::array<std::uint64_t, 3> expectedTypes{0x123456789ULL, 0, 7};
                const std::array<std::string_view, 3> expectedPayloads{"first", "", "last"};
                RUVIA_CHECK(captured.type == expectedTypes[complete]);
                RUVIA_CHECK(captured.body == expectedPayloads[complete]);
                captured.body.clear();
                ++complete;
            }
            RUVIA_CHECK(result.status == (offset == wire.size() ? ruvia::HttpCapsuleStatus::kEnd : ruvia::HttpCapsuleStatus::kNeedMoreData));
        }
        RUVIA_CHECK_EQ(complete, std::size_t{3});
        RUVIA_CHECK_EQ(captured.ends, std::size_t{3});
        const auto ended = decoder.feedOne({}, true, capsule, &captured);
        RUVIA_CHECK(ended.status == ruvia::HttpCapsuleStatus::kEnd && !ended.capsuleComplete && ended.consumedBytes == 0);
    }
    ruvia::HttpCapsuleDecoder truncated;
    const std::array partial{char(0), char(3), char('a')};
    const auto result = truncated.feedOne(partial, true, nullptr, nullptr);
    RUVIA_CHECK(result.status == ruvia::HttpCapsuleStatus::kTruncated && !result.capsuleComplete && result.consumedBytes == partial.size());
}

RUVIA_TEST(http_datagram_generic_channel_frames_opaque_payloads_and_plans_receive_failures) {
    using namespace ruvia;
    HttpDatagramSession session({.http3StreamId = 12, .localH3Datagram = true, .peerH3Datagram = true, .quicDatagram = true, .maxQuicPayloadBytes = 8});
    const std::string_view opaque("\1\0\xff", 3);
    const auto native = session.prepareDatagram(std::span(opaque.data(), opaque.size()), HttpDatagramTransport::kQuic);
    RUVIA_CHECK(native && native->prefixSize == 1 && native->prefix[0] == 3);
    const std::string packet = std::string(native->prefix.data(), native->prefixSize) + std::string(opaque);
    const auto received = session.receiveDatagram(std::span(packet.data(), packet.size()), HttpDatagramTransport::kQuic);
    RUVIA_CHECK(received && *received && std::string_view((**received).data(), (**received).size()) == opaque);
    const auto capsule = session.prepareDatagram({}, HttpDatagramTransport::kCapsule);
    RUVIA_CHECK(capsule && capsule->prefixSize == 2 && capsule->prefix[0] == 0 && capsule->prefix[1] == 0);
    RUVIA_CHECK(!session.prepareDatagram(std::span<const char>("12345678", 8), HttpDatagramTransport::kQuic));
    session.closeReceive();
    RUVIA_CHECK(!*session.receiveDatagram(std::span(packet.data(), packet.size()), HttpDatagramTransport::kQuic));
    session.closeSend();
    RUVIA_CHECK(!session.prepareDatagram({}, HttpDatagramTransport::kCapsule));
    const Http3DatagramView view{12, {}};
    RUVIA_CHECK(planHttp3DatagramReceive(view, {true, true, true, true}) == Http3DatagramReceiveStatus::kDeliver);
    RUVIA_CHECK(planHttp3DatagramReceive(view, {true, false, true, false}) == Http3DatagramReceiveStatus::kDrop);
    RUVIA_CHECK(planHttp3DatagramReceive(view, {true, true, false, false}) == Http3DatagramReceiveStatus::kDrop);
    RUVIA_CHECK(planHttp3DatagramReceive(view, {true, true, true, false}) == Http3DatagramReceiveStatus::kStreamError);
    RUVIA_CHECK(planHttp3DatagramReceive(view, {false, true, true, true}) == Http3DatagramReceiveStatus::kConnectionError);
}
