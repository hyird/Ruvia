#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_known_method.h"

#include "http2/http2_stream_request_data.h"
#include "test_harness.h"

namespace {

using ruvia::http_known_method;
using ruvia::detail::http2_stream_request_data;

http2_stream_request_data make_data() {
    return http2_stream_request_data(std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(stream_request_data_cookie_reassembly) {
    // HTTP/2 splits Cookie into separate header fields; they are re-joined with
    // "; " (RFC 7540 8.1.2.5).
    auto data = make_data();
    RUVIA_CHECK(data.cookie().empty());
    RUVIA_CHECK(data.append_cookie_header_value("a=1", false));
    RUVIA_CHECK_EQ(data.cookie(), std::string_view("a=1"));
    RUVIA_CHECK(data.append_cookie_header_value("b=2", true));
    RUVIA_CHECK_EQ(data.cookie(), std::string_view("a=1; b=2"));
    RUVIA_CHECK(data.append_cookie_header_value("c=3", true));
    RUVIA_CHECK_EQ(data.cookie(), std::string_view("a=1; b=2; c=3"));
}

RUVIA_TEST(stream_request_data_cookie_overflow_rejected) {
    auto data = make_data();
    const std::string big(64 * 1024 + 1, 'x');  // exceeds max_http_header_bytes
    RUVIA_CHECK(!data.append_cookie_header_value(big, false));
    RUVIA_CHECK(data.cookie().empty());  // unchanged on rejection
}

RUVIA_TEST(stream_request_data_cookie_accumulation_overflow_rejected) {
    // Individually-small crumbs that SUM past the limit are rejected too: an
    // attacker can send many HTTP/2 Cookie fields (cheap to compress via HPACK, so
    // the raw block stays under its own cap) that decode into a huge reassembled
    // cookie. The crumb that would cross max_http_header_bytes is refused, and the
    // accumulated value is left exactly as it was (no partial append).
    auto data = make_data();
    const std::string chunk(30000, 'a');
    RUVIA_CHECK(data.append_cookie_header_value(chunk, false));  // 30000
    RUVIA_CHECK(data.append_cookie_header_value(chunk, true));   // + "; " + 30000 = 60002
    const std::string before(data.cookie());
    RUVIA_CHECK(!data.append_cookie_header_value(chunk, true));  // would reach ~90004 > 64 KiB
    RUVIA_CHECK_EQ(std::string(data.cookie()), before);          // unchanged, not partially grown
}

RUVIA_TEST(stream_request_data_scalar_fields) {
    auto data = make_data();
    RUVIA_CHECK(data.method().empty());
    RUVIA_CHECK(data.known_method() == http_known_method::unknown);
    data.assign_method("POST");
    RUVIA_CHECK_EQ(data.method(), std::string_view("POST"));
    RUVIA_CHECK(data.known_method() == http_known_method::post);
    data.assign_method("PROPFIND");
    RUVIA_CHECK_EQ(data.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(data.known_method() == http_known_method::unknown);
    data.assign_authority("example.com");
    RUVIA_CHECK_EQ(data.authority(), std::string_view("example.com"));
    data.assign_path("/api?x=1");
    RUVIA_CHECK_EQ(data.path(), std::string_view("/api?x=1"));
}
