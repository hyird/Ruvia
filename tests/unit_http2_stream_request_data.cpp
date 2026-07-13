#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2StreamRequestData.h"
#include "ruvia/http/HttpKnownMethod.h"

namespace {

using ruvia::HttpKnownMethod;
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

RUVIA_TEST(stream_request_data_cookie_accumulation_overflow_rejected) {
    // Individually-small crumbs that SUM past the limit are rejected too: an
    // attacker can send many HTTP/2 Cookie fields (cheap to compress via HPACK, so
    // the raw block stays under its own cap) that decode into a huge reassembled
    // cookie. The crumb that would cross kMaxHttpHeaderBytes is refused, and the
    // accumulated value is left exactly as it was (no partial append).
    auto data = makeData();
    const std::string chunk(30000, 'a');
    RUVIA_CHECK(data.appendCookieHeaderValue(chunk, false));         // 30000
    RUVIA_CHECK(data.appendCookieHeaderValue(chunk, true));          // + "; " + 30000 = 60002
    const std::string before(data.cookie());
    RUVIA_CHECK(!data.appendCookieHeaderValue(chunk, true));         // would reach ~90004 > 64 KiB
    RUVIA_CHECK_EQ(std::string(data.cookie()), before);             // unchanged, not partially grown
}

RUVIA_TEST(stream_request_data_scalar_fields) {
    auto data = makeData();
    RUVIA_CHECK(data.method().empty());
    RUVIA_CHECK(data.knownMethod() == HttpKnownMethod::kUnknown);
    data.assignMethod("POST");
    RUVIA_CHECK_EQ(data.method(), std::string_view("POST"));
    RUVIA_CHECK(data.knownMethod() == HttpKnownMethod::kPost);
    data.assignMethod("PROPFIND");
    RUVIA_CHECK_EQ(data.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(data.knownMethod() == HttpKnownMethod::kUnknown);
    data.assignAuthority("example.com");
    RUVIA_CHECK_EQ(data.authority(), std::string_view("example.com"));
    data.assignPath("/api?x=1");
    RUVIA_CHECK_EQ(data.path(), std::string_view("/api?x=1"));
}
