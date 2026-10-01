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
    capture.body.append(event.payload.data(), event.payload.size());
    capture.ends += event.endCapsule;
}
}  // namespace
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
