#include "test_harness.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/frame/Http2FramePayload.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"

namespace {

using ruvia::detail::http2DecodeDataPayload;
using ruvia::detail::http2DecodeHeadersPayload;
using ruvia::detail::Http2FrameHeader;
using ruvia::detail::Http2FramePayloadStatus;
using ruvia::detail::http2HeadersPriorityDependency;
using ruvia::detail::http2StripPadAndPriority;
using ruvia::detail::kHttp2FlagPadded;
using ruvia::detail::kHttp2FlagPriority;

Http2FrameHeader headerWithFlags(std::uint8_t flags) noexcept {
    Http2FrameHeader header;
    header.flags = flags;
    return header;
}

std::string withByte(int prefix, std::string_view rest) {
    std::string out;
    out.push_back(static_cast<char>(prefix));
    out.append(rest.data(), rest.size());
    return out;
}

}  // namespace

RUVIA_TEST(frame_data_no_padding) {
    auto header = headerWithFlags(0);
    std::string_view data;
    RUVIA_CHECK(http2DecodeDataPayload(header, "hello", data));
    RUVIA_CHECK_EQ(data, std::string_view("hello"));
}

RUVIA_TEST(frame_data_with_padding_strips_prefix_and_trailer) {
    // pad length 3, data "DD", then 3 padding bytes.
    auto header = headerWithFlags(kHttp2FlagPadded);
    std::string payload = withByte(0x03, "DDPPP");
    std::string_view data;
    RUVIA_CHECK(http2DecodeDataPayload(header, payload, data));
    RUVIA_CHECK_EQ(data, std::string_view("DD"));
}

RUVIA_TEST(frame_data_pad_length_zero_keeps_all) {
    auto header = headerWithFlags(kHttp2FlagPadded);
    std::string payload = withByte(0x00, "hello");
    std::string_view data;
    RUVIA_CHECK(http2DecodeDataPayload(header, payload, data));
    RUVIA_CHECK_EQ(data, std::string_view("hello"));
}

RUVIA_TEST(frame_data_padding_consumes_all_yields_empty) {
    // pad length 2, no data, two padding bytes.
    auto header = headerWithFlags(kHttp2FlagPadded);
    std::string payload = withByte(0x02, "PP");
    std::string_view data;
    RUVIA_CHECK(http2DecodeDataPayload(header, payload, data));
    RUVIA_CHECK(data.empty());
}

RUVIA_TEST(frame_data_padding_underflow_rejected) {
    // pad length 255 but only two bytes follow: must be rejected, no underflow.
    auto header = headerWithFlags(kHttp2FlagPadded);
    std::string payload = withByte(0xFF, "ab");
    std::string_view data;
    RUVIA_CHECK(!http2DecodeDataPayload(header, payload, data));
}

RUVIA_TEST(frame_padded_flag_but_empty_payload_rejected) {
    auto header = headerWithFlags(kHttp2FlagPadded);
    std::string_view data;
    RUVIA_CHECK(!http2DecodeDataPayload(header, "", data));
}

RUVIA_TEST(frame_headers_priority_skipped_and_dependency_masked) {
    // 5-byte priority field: stream dependency with the E bit set (must be
    // masked off), weight byte, then the header-block fragment.
    auto header = headerWithFlags(kHttp2FlagPriority);
    std::string payload;
    payload.push_back(static_cast<char>(0x80));  // E bit + high dependency byte
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x05);  // dependency low byte -> 5
    payload.push_back(0x00);  // weight
    payload += "frag";

    std::string_view fragment;
    RUVIA_CHECK(http2DecodeHeadersPayload(header, payload, fragment) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(fragment, std::string_view("frag"));

    std::uint32_t dependency = 0xffffffffU;
    RUVIA_CHECK(http2HeadersPriorityDependency(header, payload, dependency) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(dependency, std::uint32_t{5});  // E bit stripped
}

RUVIA_TEST(frame_headers_priority_too_short_rejected) {
    auto header = headerWithFlags(kHttp2FlagPriority);
    std::string_view fragment;
    RUVIA_CHECK(http2DecodeHeadersPayload(header, "abc", fragment) == Http2FramePayloadStatus::kMissingPriorityFields);
}

RUVIA_TEST(frame_headers_padded_and_priority_combined) {
    // pad length 2, 5-byte priority (dependency 7), fragment "hdr", 2 padding.
    auto header = headerWithFlags(static_cast<std::uint8_t>(kHttp2FlagPadded | kHttp2FlagPriority));
    std::string payload;
    payload.push_back(0x02);  // pad length
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x00);
    payload.push_back(0x07);  // dependency 7
    payload.push_back(0x00);  // weight
    payload += "hdr";
    payload += "PP";  // padding

    std::string_view content;
    std::uint32_t dependency = 0;
    RUVIA_CHECK(http2StripPadAndPriority(header, payload, true, content, &dependency) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(content, std::string_view("hdr"));
    RUVIA_CHECK_EQ(dependency, std::uint32_t{7});
}

RUVIA_TEST(frame_headers_plain_keeps_all_and_default_dependency) {
    auto header = headerWithFlags(0);
    std::string_view fragment;
    RUVIA_CHECK(http2DecodeHeadersPayload(header, "block", fragment) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(fragment, std::string_view("block"));

    // No PRIORITY flag -> dependency defaults to 0.
    std::uint32_t dependency = 0xffffffffU;
    RUVIA_CHECK(http2HeadersPriorityDependency(header, "block", dependency) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(dependency, std::uint32_t{0});
}

RUVIA_TEST(frame_data_ignores_priority_flag) {
    // A DATA frame never interprets the priority bit (allowPriority=false), so
    // the whole payload after any padding is data.
    auto header = headerWithFlags(kHttp2FlagPriority);
    std::string_view data;
    RUVIA_CHECK(http2DecodeDataPayload(header, "raw", data));
    RUVIA_CHECK_EQ(data, std::string_view("raw"));
}

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
    RUVIA_CHECK(http2HeadersPriorityDependency(header, payload, dependency) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(dependency, std::uint32_t{7});

    std::string_view fragment;
    RUVIA_CHECK(http2DecodeHeadersPayload(header, payload, fragment) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(fragment, std::string_view("hpack-fragment"));

    // The PRIORITY flag with fewer than 5 bytes is rejected.
    std::string_view rejected;
    RUVIA_CHECK(http2DecodeHeadersPayload(header, std::string_view("\0\0", 2), rejected) == Http2FramePayloadStatus::kMissingPriorityFields);
}

RUVIA_TEST(http2_headers_payload_padded_and_priority) {
    const auto header = headerWithFlags(kHttp2FlagPadded | kHttp2FlagPriority);
    // [pad length = 2][priority 5 bytes][block][2 padding bytes]
    std::string payload;
    payload += static_cast<char>(2);
    payload += std::string(4, '\0');     // stream dependency 0
    payload += static_cast<char>(0x00);  // weight
    payload += "blk";
    payload += std::string(2, '\0');
    std::string_view fragment;
    RUVIA_CHECK(http2DecodeHeadersPayload(header, payload, fragment) == Http2FramePayloadStatus::kDecoded);
    RUVIA_CHECK_EQ(fragment, std::string_view("blk"));
}
