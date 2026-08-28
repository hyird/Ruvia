#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/http2/hpack/Http2HeaderContinuation.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderDecode.h"

namespace {

using ruvia::detail::HeaderDecodeStatus;
using ruvia::detail::HpackDecoder;
using ruvia::detail::http2ClassifyHeaderDecodeResult;

bool rejectHeader(void*, std::string_view, std::string_view) {
    return false;
}

}  // namespace

RUVIA_TEST(classify_header_decode_result) {
    HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
    // A clean decode is OK.
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(decoder.decode({}, nullptr, nullptr)) == HeaderDecodeStatus::kOk);
    // A header-validation callback rejection is a protocol error.
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(decoder.decode(std::string_view("\x82", 1), nullptr, &rejectHeader)) == HeaderDecodeStatus::kProtocolError);
    // Any HPACK decoding fault is a compression error (RFC 7541 4.1).
    RUVIA_CHECK(http2ClassifyHeaderDecodeResult(decoder.decode(std::string_view("\x80", 1), nullptr, nullptr)) == HeaderDecodeStatus::kCompressionError);
}
