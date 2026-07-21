#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/RequestBodyDecoding.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequest.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpKnownMethod;
using ruvia::HttpProtocolVersion;
using ruvia::HttpRequest;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::HttpContentCoding;
using ruvia::detail::RequestKnownHeader;
using ruvia::detail::requestContentCoding;
using ruvia::detail::requestBodyBytes;
using ruvia::detail::requestKnownHeader;

template <typename T>
concept ExposesRvalueHttpRequestHeaders = requires(T&& request) {
    std::move(request).headers();
};

static_assert(!ExposesRvalueHttpRequestHeaders<HttpRequest>);
static_assert(std::same_as<
    decltype(std::declval<const HttpRequest&>().header(std::string_view{})),
    std::optional<std::string_view>>);
static_assert(std::is_constructible_v<
    HttpHeaderView,
    const std::string&,
    const std::string&>);
static_assert(!std::is_constructible_v<
    HttpHeaderView,
    std::string&&,
    std::string_view>);
static_assert(!std::is_constructible_v<
    HttpHeaderView,
    std::string_view,
    std::string&&>);
static_assert(!std::is_constructible_v<
    HttpHeaderView,
    const std::string&&,
    std::string_view>);
static_assert(!std::is_constructible_v<
    HttpHeaderView,
    std::string_view,
    const std::string&&>);
static_assert(!std::is_constructible_v<
    HttpHeaderView,
    std::pmr::string&&,
    std::string_view>);
static_assert(!std::is_constructible_v<
    HttpHeaderView,
    std::string_view,
    const std::pmr::string&&>);

}  // namespace

RUVIA_TEST(request_access_reset_initializes_defaults) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(request.method().empty());
    RUVIA_CHECK(request.knownMethod() == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(
        request.protocolVersion() == HttpProtocolVersion::kHttp11);
    RUVIA_CHECK(request.headers().empty());
    RUVIA_CHECK(requestBodyBytes(request).empty());
}

RUVIA_TEST(request_access_protocol_version_is_typed_control_data) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::setProtocolVersion(
        request, HttpProtocolVersion::kHttp2);
    RUVIA_CHECK(
        request.protocolVersion() == HttpProtocolVersion::kHttp2);
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
    RUVIA_CHECK_EQ(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kUserAgent), std::size_t{24});
    // Every known header maps within the cache (25 slots), so the clamp never
    // fires for a valid enumerator.
    RUVIA_CHECK(HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kUserAgent) <
                HttpRequestAccess::kCachedHeaderSlots);
}

RUVIA_TEST(request_access_known_header_last_write_wins) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost);
    HttpRequestAccess::setKnownHeaderSlot(request, slot, "first.example");
    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kHost),
                   std::string_view("first.example"));
    HttpRequestAccess::setKnownHeaderSlot(request, slot, "second.example");
    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kHost),
                   std::string_view("second.example"));
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
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", "first.example"}, slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"host", "second.example"}, slot));

    RUVIA_CHECK_EQ(request.header("Host"), std::string_view("second.example"));
    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kHost),
                   std::string_view("second.example"));
}

RUVIA_TEST(request_content_coding_rejects_repeated_header_fields) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kContentEncoding);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Encoding", "br"}, slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Content-Encoding", "gzip"}, slot));

    RUVIA_CHECK_EQ(requestKnownHeader(request, RequestKnownHeader::kContentEncoding), std::string_view("gzip"));
    const auto coding = requestContentCoding(request);
    RUVIA_CHECK(coding.coding() == nullptr);
    RUVIA_CHECK(coding.invalid() == nullptr);
    RUVIA_CHECK(coding.unsupported() != nullptr);
}

RUVIA_TEST(request_content_coding_combines_field_lines_with_list_semantics) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(
        RequestKnownHeader::kContentEncoding);
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Content-Encoding", ","},
        slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Content-Encoding", "gzip"},
        slot));

    const auto coding = requestContentCoding(request);
    RUVIA_CHECK(coding.invalid() == nullptr);
    RUVIA_CHECK(coding.unsupported() == nullptr);
    RUVIA_CHECK(coding.coding() != nullptr);
    if (coding.coding() != nullptr) {
        RUVIA_CHECK(*coding.coding() == HttpContentCoding::kGzip);
    }
}

RUVIA_TEST(request_access_query_lookup_uses_last_match) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "a=first&b=2&a=second");

    const auto value = request.query("a");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("second"));
}

RUVIA_TEST(request_access_cookie_lookup_uses_last_match) {
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request,
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
    HttpRequestAccess::setKnownHeaderSlot(
        request, HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kHost), "h");
    // reset wipes cached known headers and appended headers.
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(requestKnownHeader(request, RequestKnownHeader::kHost).empty());
    RUVIA_CHECK(request.headers().empty());
}
