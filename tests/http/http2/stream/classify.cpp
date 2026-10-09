#include <cstddef>
#include <memory_resource>
#include <string_view>

#include "http2/http2_header_continuation.h"
#include "http2/http2_header_decode.h"
#include "http2/http2_hpack.h"
#include "test_harness.h"

namespace {

using ruvia::detail::header_decode_status;
using ruvia::detail::hpack_decoder;
using ruvia::detail::http2_classify_header_decode_result;

bool reject_header(void*, std::string_view, std::string_view) {
    return false;
}

}  // namespace

RUVIA_TEST(classify_header_decode_result) {
    hpack_decoder decoder({.resource_ = std::pmr::get_default_resource()});
    // A clean decode is OK.
    RUVIA_CHECK(http2_classify_header_decode_result(decoder.decode({}, nullptr, nullptr)) ==
                header_decode_status::ok);
    // A header-validation callback rejection is a protocol error.
    RUVIA_CHECK(http2_classify_header_decode_result(decoder.decode(std::string_view("\x82", 1), nullptr,
                    &reject_header)) == header_decode_status::protocol_error);
    // Any HPACK decoding fault is a compression error (RFC 7541 4.1).
    RUVIA_CHECK(http2_classify_header_decode_result(decoder.decode(std::string_view("\x80", 1), nullptr,
                    nullptr)) == header_decode_status::compression_error);
}
