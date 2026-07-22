#include "test_harness.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/frame/Http2FramePayload.h"

namespace {

using ruvia::detail::Http2FrameHeader;
using ruvia::detail::Http2FramePayloadStatus;
using ruvia::detail::http2DecodeDataPayload;
using ruvia::detail::http2DecodeHeadersPayload;
using ruvia::detail::http2HeadersPriorityDependency;
using ruvia::detail::kHttp2FlagPadded;
using ruvia::detail::kHttp2FlagPriority;

Http2FrameHeader headerWithFlags(std::uint8_t flags) {
    Http2FrameHeader header{};
    header.flags = flags;
    return header;
}

}  // namespace

RUVIA_TEST(http2_data_payload_unpadded) {
    const auto header = headerWithFlags(0);
    std::string_view data;
    RUVIA_CHECK(http2DecodeDataPayload(header, "hello world", data));
    RUVIA_CHECK_EQ(data, std::string_view("hello world"));
}

RUVIA_TEST(http2_data_payload_padded) {
    const auto header = headerWithFlags(kHttp2FlagPadded);
    // [pad length = 3]["data"][3 padding bytes]
    std::string payload;
    payload += static_cast<char>(3);
    payload += "data";
    payload += std::string(3, '\0');
    std::string_view data;
    RUVIA_CHECK(http2DecodeDataPayload(header, payload, data));
    RUVIA_CHECK_EQ(data, std::string_view("data"));

    // A pad length >= the payload length is a protocol error (RFC 7540 6.1).
    std::string tooMuch;
    tooMuch += static_cast<char>(5);  // claims 5 padding bytes...
    tooMuch += "ab";                  // ...but the whole payload is only 3 bytes
    std::string_view rejected;
    RUVIA_CHECK(!http2DecodeDataPayload(header, tooMuch, rejected));

    // The PADDED flag with an empty payload (no pad-length byte) is rejected.
    std::string_view empty;
    RUVIA_CHECK(!http2DecodeDataPayload(header, std::string_view(), empty));
}

RUVIA_TEST(http2_headers_payload_priority) {
    const auto header = headerWithFlags(kHttp2FlagPriority);
    // Priority field: 4-byte stream dependency (7) + 1-byte weight, then the block.
    std::string payload;
    payload += static_cast<char>(0);
    payload += static_cast<char>(0);
    payload += static_cast<char>(0);
    payload += static_cast<char>(7);
    payload += static_cast<char>(0x10);
    payload += "hpack-fragment";

    std::uint32_t dependency = 999;
    RUVIA_CHECK(
        http2HeadersPriorityDependency(header, payload, dependency) ==
        Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(dependency, std::uint32_t{7});

    std::string_view fragment;
    RUVIA_CHECK(
        http2DecodeHeadersPayload(header, payload, fragment) ==
        Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(fragment, std::string_view("hpack-fragment"));

    // The PRIORITY flag with fewer than 5 bytes is rejected.
    std::string_view rejected;
    RUVIA_CHECK(
        http2DecodeHeadersPayload(header, std::string_view("\0\0", 2), rejected) ==
        Http2FramePayloadStatus::kMissingPriorityFields);
}

RUVIA_TEST(http2_headers_payload_padded_and_priority) {
    const auto header = headerWithFlags(kHttp2FlagPadded | kHttp2FlagPriority);
    // [pad length = 2][priority 5 bytes][block][2 padding bytes]
    std::string payload;
    payload += static_cast<char>(2);
    payload += std::string(4, '\0');    // stream dependency 0
    payload += static_cast<char>(0x00); // weight
    payload += "blk";
    payload += std::string(2, '\0');
    std::string_view fragment;
    RUVIA_CHECK(
        http2DecodeHeadersPayload(header, payload, fragment) ==
        Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(fragment, std::string_view("blk"));
}
