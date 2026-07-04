#include "test_harness.h"

#include <cstddef>
#include <string_view>

#include "http/HttpRequestInternal.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpMethod;
using ruvia::HttpRequest;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RequestKnownHeader;
using ruvia::detail::requestBodyBytes;
using ruvia::detail::requestKnownHeader;

}  // namespace

RUVIA_TEST(request_access_reset_initializes_defaults) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(request.method() == HttpMethod::kUnknown);
    RUVIA_CHECK_EQ(request.httpVersion(), std::string_view("HTTP/1.1"));
    RUVIA_CHECK(request.headers().empty());
    RUVIA_CHECK(!request.isSecure());
    RUVIA_CHECK(requestBodyBytes(request).empty());
}

RUVIA_TEST(request_access_known_header_slot_mapping) {
    RUVIA_CHECK_EQ(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAccept), std::size_t{0});
    RUVIA_CHECK_EQ(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost), std::size_t{11});
    RUVIA_CHECK_EQ(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kUserAgent), std::size_t{24});
    // Every known header maps within the cache (25 slots), so the clamp never
    // fires for a valid enumerator.
    RUVIA_CHECK(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kUserAgent) <
                HttpRequestAccess::kCachedHeaderSlots);
}

RUVIA_TEST(request_access_known_header_first_write_wins) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost);
    HttpRequestAccess::setKnownHeaderSlot(request, slot, "first.example");
    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kHost),
                   std::string_view("first.example"));
    // A second write to a populated slot is ignored: a duplicate known header
    // keeps the first value.
    HttpRequestAccess::setKnownHeaderSlot(request, slot, "second.example");
    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kHost),
                   std::string_view("first.example"));
    // An unpopulated known header reads back empty.
    RUVIA_CHECK(requestKnownHeader(request, RequestKnownHeader::kUserAgent).empty());
}

RUVIA_TEST(request_access_add_header_appends_and_caches) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request, HttpHeaderView{"host", "example.com"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost)));
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(request.headers()[0].name(), std::string_view("host"));
    RUVIA_CHECK_EQ(request.headers()[0].value(), std::string_view("example.com"));
    // The two-argument overload also caches the value for fast known-header access.
    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kHost),
                   std::string_view("example.com"));
}

RUVIA_TEST(request_access_unknown_header_lookup_uses_last_match) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Trace", "first"}));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"x-trace", "second"}));

    RUVIA_CHECK_EQ(request.header("X-Trace"), std::string_view("second"));
}

RUVIA_TEST(request_access_add_header_rejects_when_full) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    for (int i = 0; i < 64; ++i) {  // kMaxRequestHeaders == 64
        RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"x", "y"}));
    }
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{64});
    RUVIA_CHECK(!HttpRequestAccess::addHeader(request, HttpHeaderView{"over", "flow"}));
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{64});
}

RUVIA_TEST(request_access_reset_clears_cached_headers_and_transport) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setKnownHeaderSlot(
        request, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost), "h");
    HttpRequestAccess::setTransport(request, "1.2.3.4", "subject-dn", true);
    RUVIA_CHECK(request.isSecure());
    RUVIA_CHECK_EQ(request.remoteAddress(), std::string_view("1.2.3.4"));
    RUVIA_CHECK_EQ(request.clientCertificate(), std::string_view("subject-dn"));

    // reset wipes cached known headers, appended headers, and transport metadata.
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(requestKnownHeader(request, RequestKnownHeader::kHost).empty());
    RUVIA_CHECK(request.headers().empty());
    RUVIA_CHECK(!request.isSecure());
    RUVIA_CHECK(request.remoteAddress().empty());
}
