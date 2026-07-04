#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "net/http2/Http2StreamRequestData.h"
#include "ruvia/http/HttpCommon.h"

namespace {

using ruvia::HttpMethod;
using ruvia::detail::Http2StreamRequestData;

Http2StreamRequestData makeData() {
    return Http2StreamRequestData(std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(stream_request_data_cookie_reassembly) {
    // HTTP/2 splits Cookie into separate header fields; they are re-joined with
    // "; " (RFC 7540 8.1.2.5).
    auto data = makeData();
    RUVIA_CHECK(data.cookie().empty());
    RUVIA_CHECK(data.appendCookieHeaderValue("a=1", false));
    RUVIA_CHECK_EQ(data.cookie(), std::string_view("a=1"));
    RUVIA_CHECK(data.appendCookieHeaderValue("b=2", true));
    RUVIA_CHECK_EQ(data.cookie(), std::string_view("a=1; b=2"));
    RUVIA_CHECK(data.appendCookieHeaderValue("c=3", true));
    RUVIA_CHECK_EQ(data.cookie(), std::string_view("a=1; b=2; c=3"));
}

RUVIA_TEST(stream_request_data_cookie_overflow_rejected) {
    auto data = makeData();
    const std::string big(64 * 1024 + 1, 'x');  // exceeds kMaxHttpHeaderBytes
    RUVIA_CHECK(!data.appendCookieHeaderValue(big, false));
    RUVIA_CHECK(data.cookie().empty());  // unchanged on rejection
}

RUVIA_TEST(stream_request_data_body_accumulation) {
    auto data = makeData();
    RUVIA_CHECK(data.bodyEmpty());
    data.appendBody("chunk1");
    data.appendBody("chunk2");
    RUVIA_CHECK_EQ(data.bodyView(), std::string_view("chunk1chunk2"));
    RUVIA_CHECK_EQ(data.bodySize(), std::size_t{12});
    RUVIA_CHECK(!data.bodyEmpty());
    data.clearBody();
    RUVIA_CHECK(data.bodyEmpty());
    data.assignBody("replaced");
    RUVIA_CHECK_EQ(data.bodyView(), std::string_view("replaced"));
}

RUVIA_TEST(stream_request_data_scalar_fields) {
    auto data = makeData();
    RUVIA_CHECK(data.method() == HttpMethod::kUnknown);
    data.setMethod(HttpMethod::kPost);
    RUVIA_CHECK(data.method() == HttpMethod::kPost);
    data.assignAuthority("example.com");
    RUVIA_CHECK_EQ(data.authority(), std::string_view("example.com"));
    data.assignPath("/api?x=1");
    RUVIA_CHECK_EQ(data.path(), std::string_view("/api?x=1"));
}
