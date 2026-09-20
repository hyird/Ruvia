#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/request/RequestBodyDecoding.h"

#include "request_header_memory_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::HttpContentCoding;
using ruvia::HttpHeaderView;
using ruvia::HttpKnownMethod;
using ruvia::HttpProtocolVersion;
using ruvia::HttpRequest;
using ruvia::HttpRequestTargetForm;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::requestBodyBytes;
using ruvia::detail::requestContentCoding;
using ruvia::detail::RequestKnownHeader;
using ruvia::detail::requestKnownHeader;

using ruvia::test::HeaderMemory;

}  // namespace

RUVIA_TEST(request_header_blocks_reclaim_repeated_parses_and_preserve_retained_results) {
    HeaderMemory resource;
    ruvia::Http1RequestParser parser;
    {
        auto retained = parser.parse("GET /saved HTTP/1.1\r\nHost: example\r\nX-Data: saved\r\n\r\n", {.resource = &resource});
        RUVIA_CHECK(retained.parsed() != nullptr);
        const auto baseline = resource.liveBytes;
        RUVIA_CHECK(baseline >= 2 * sizeof(HttpHeaderView));
        RUVIA_CHECK(baseline < ruvia::kMaxHttpHeaderFields * sizeof(HttpHeaderView));
        for (int i = 0; i < 64; ++i) {
            {
                const auto before = resource.allocations;
                auto result = parser.parse("GET / HTTP/1.1\r\nHost: second\r\n\r\n", {.resource = &resource});
                RUVIA_CHECK(result.parsed() != nullptr);
                RUVIA_CHECK_EQ(resource.allocations, before + 1);
                auto moved = std::move(result);
                RUVIA_CHECK_EQ(moved.parsed()->request().header("Host").value(), std::string_view("second"));
                RUVIA_CHECK_EQ(retained.parsed()->request().header("X-Data").value(), std::string_view("saved"));
            }
            RUVIA_CHECK_EQ(resource.liveBytes, baseline);
            auto incomplete = parser.parse("POST / HTTP/1.1\r\nHost: example\r\nContent-Length: 3\r\n\r\nx", {.resource = &resource});
            RUVIA_CHECK(incomplete.needMore() != nullptr);
            RUVIA_CHECK_EQ(resource.liveBytes, baseline);
        }
        resource.reject = true;
        bool failed = false;
        try {
            (void)parser.parse("GET / HTTP/1.1\r\nHost: example\r\n\r\n", {.resource = &resource});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.liveBytes, baseline);
        RUVIA_CHECK_EQ(retained.parsed()->request().path(), std::string_view("/saved"));
    }
    RUVIA_CHECK_EQ(resource.liveBytes, std::size_t{0});
}

RUVIA_TEST(request_header_block_move_assignment_and_reset_release_storage) {
    HeaderMemory firstResource;
    HeaderMemory secondResource;
    auto first = HttpRequestAccess::make();
    auto second = HttpRequestAccess::make();
    HttpRequestAccess::setResource(first, &firstResource);
    HttpRequestAccess::setResource(second, &secondResource);
    HttpRequestAccess::reserveHeaders(first, 1);
    HttpRequestAccess::reserveHeaders(second, 1);
    const auto host = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost);
    RUVIA_CHECK(HttpRequestAccess::addHeader(first, {"Host", "first"}, host));
    RUVIA_CHECK(HttpRequestAccess::addHeader(second, {"Host", "second"}, host));
    first = std::move(second);
    RUVIA_CHECK_EQ(firstResource.liveBytes, std::size_t{0});
    RUVIA_CHECK_EQ(first.header("host").value(), std::string_view("second"));
    RUVIA_CHECK(!second.header("host").has_value());
    HttpRequestAccess::reset(first);
    RUVIA_CHECK_EQ(secondResource.liveBytes, std::size_t{0});
}

RUVIA_TEST(request_access_reset_initializes_defaults) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(request.method().empty());
    RUVIA_CHECK(request.knownMethod() == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(request.target().empty());
    RUVIA_CHECK(request.scheme().empty());
    RUVIA_CHECK(request.authority().empty());
    RUVIA_CHECK(request.targetForm() == HttpRequestTargetForm::kOrigin);
    RUVIA_CHECK(request.protocolVersion() == HttpProtocolVersion::kHttp11);
    RUVIA_CHECK(request.headers().empty());
    RUVIA_CHECK(requestBodyBytes(request).empty());
}

RUVIA_TEST(request_access_preserves_target_components_and_form) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setTarget(request, "https://example.test/search?q=1");
    HttpRequestAccess::setScheme(request, "https");
    HttpRequestAccess::setAuthority(request, "example.test");
    HttpRequestAccess::setTargetForm(request, HttpRequestTargetForm::kAbsolute);

    RUVIA_CHECK_EQ(request.target(), std::string_view("https://example.test/search?q=1"));
    RUVIA_CHECK_EQ(request.scheme(), std::string_view("https"));
    RUVIA_CHECK_EQ(request.authority(), std::string_view("example.test"));
    RUVIA_CHECK(request.targetForm() == HttpRequestTargetForm::kAbsolute);
}

RUVIA_TEST(request_access_protocol_version_is_typed_control_data) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::setProtocolVersion(request, HttpProtocolVersion::kHttp2);
    RUVIA_CHECK(request.protocolVersion() == HttpProtocolVersion::kHttp2);
}

RUVIA_TEST(request_access_preserves_extension_method_token) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "PROPFIND");
    RUVIA_CHECK_EQ(request.method(), std::string_view("PROPFIND"));
    RUVIA_CHECK(request.knownMethod() == HttpKnownMethod::kUnknown);
}

RUVIA_TEST(request_access_known_header_slot_mapping) {
    RUVIA_CHECK_EQ(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAccept), std::size_t{0});
    RUVIA_CHECK_EQ(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost), std::size_t{11});
    RUVIA_CHECK_EQ(
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kUserAgent), std::size_t{24});
    // Every known header maps within the cache (25 slots), so the clamp never
    // fires for a valid enumerator.
    RUVIA_CHECK(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kUserAgent) <
                HttpRequestAccess::kCachedHeaderSlots);
}

RUVIA_TEST(request_access_known_header_last_write_wins) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", "first.example"}, slot));
    RUVIA_CHECK_EQ(
        requestKnownHeader(request, RequestKnownHeader::kHost), std::string_view("first.example"));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", "second.example"}, slot));
    RUVIA_CHECK_EQ(
        requestKnownHeader(request, RequestKnownHeader::kHost), std::string_view("second.example"));
    // An unpopulated known header reads back empty.
    RUVIA_CHECK(requestKnownHeader(request, RequestKnownHeader::kUserAgent).empty());
}

RUVIA_TEST(request_access_add_header_appends_and_caches) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"host", "example.com"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost)));
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(request.headers()[0].name(), std::string_view("host"));
    RUVIA_CHECK_EQ(request.headers()[0].value(), std::string_view("example.com"));
    // The two-argument overload also caches the value for fast known-header access.
    RUVIA_CHECK_EQ(
        requestKnownHeader(request, RequestKnownHeader::kHost), std::string_view("example.com"));
}

RUVIA_TEST(request_access_unknown_header_lookup_uses_last_match) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Trace", "first"}));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"x-trace", "second"}));

    RUVIA_CHECK_EQ(request.header("X-Trace"), std::string_view("second"));
}

RUVIA_TEST(request_header_distinguishes_missing_from_present_empty) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);

    RUVIA_CHECK(!request.header("X-Empty").has_value());
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Empty", ""}));
    const auto presentEmpty = request.header("x-empty");
    RUVIA_CHECK(presentEmpty.has_value());
    RUVIA_CHECK(presentEmpty.value_or("missing").empty());

    const auto hostSlot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", ""}, hostSlot));
    const auto knownPresentEmpty = request.header("HOST");
    RUVIA_CHECK(knownPresentEmpty.has_value());
    RUVIA_CHECK(knownPresentEmpty.value_or("missing").empty());
}

RUVIA_TEST(request_access_known_header_lookup_uses_last_match) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost);
    RUVIA_CHECK(
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", "first.example"}, slot));
    RUVIA_CHECK(
        HttpRequestAccess::addHeader(request, HttpHeaderView{"host", "second.example"}, slot));

    RUVIA_CHECK_EQ(request.header("Host"), std::string_view("second.example"));
    RUVIA_CHECK_EQ(
        requestKnownHeader(request, RequestKnownHeader::kHost), std::string_view("second.example"));
}

RUVIA_TEST(request_content_coding_rejects_repeated_header_fields) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentEncoding);
    RUVIA_CHECK(
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Encoding", "br"}, slot));
    RUVIA_CHECK(
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Encoding", "gzip"}, slot));

    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kContentEncoding),
        std::string_view("gzip"));
    const auto coding = requestContentCoding(request);
    RUVIA_CHECK(coding.coding() == nullptr);
    RUVIA_CHECK(coding.invalid() == nullptr);
    RUVIA_CHECK(coding.unsupported() != nullptr);
}

RUVIA_TEST(request_content_coding_combines_field_lines_with_list_semantics) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentEncoding);
    RUVIA_CHECK(
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Encoding", ","}, slot));
    RUVIA_CHECK(
        HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Encoding", "gzip"}, slot));

    const auto coding = requestContentCoding(request);
    RUVIA_CHECK(coding.invalid() == nullptr);
    RUVIA_CHECK(coding.unsupported() == nullptr);
    RUVIA_CHECK(coding.coding() != nullptr);
    if (coding.coding() != nullptr) {
        RUVIA_CHECK(*coding.coding() == HttpContentCoding::kGzip);
    }
}

RUVIA_TEST(request_access_raw_query_lookup_preserves_encoding_and_uses_last_match) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(
        request, "a=first&b=2&a=second+value&encoded%20key=raw%2Fvalue");

    const auto value = request.lastRawQueryValue("a");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second+value"));
    RUVIA_CHECK(!request.lastRawQueryValue("encoded key").has_value());
    RUVIA_CHECK_EQ(*request.lastRawQueryValue("encoded%20key"), std::string_view("raw%2Fvalue"));
}

RUVIA_TEST(request_access_cookie_lookup_uses_last_match) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request,
        HttpHeaderView{"Cookie", "sid=first; theme=dark; sid=second"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));

    const auto value = request.cookie("sid");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second"));
}

RUVIA_TEST(request_access_cookie_lookup_scans_repeated_cookie_fields) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "a=1"}, slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "b=2"}, slot));

    const auto first = request.cookie("a");
    RUVIA_CHECK(first.has_value());
    RUVIA_CHECK_EQ(*first, std::string_view("1"));
    const auto second = request.cookie("b");
    RUVIA_CHECK(second.has_value());
    RUVIA_CHECK_EQ(*second, std::string_view("2"));
}

RUVIA_TEST(request_access_add_header_rejects_when_full) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    for (int i = 0; i < 64; ++i) {  // kMaxHttpHeaderFields == 64
        RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"x", "y"}));
    }
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{64});
    RUVIA_CHECK(!HttpRequestAccess::addHeader(request, HttpHeaderView{"over", "flow"}));
    RUVIA_CHECK_EQ(request.headers().size(), std::size_t{64});
}

RUVIA_TEST(request_access_reset_clears_cached_headers) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", "h"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost)));
    // reset wipes cached known headers and appended headers.
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(requestKnownHeader(request, RequestKnownHeader::kHost).empty());
    RUVIA_CHECK(request.headers().empty());
}
