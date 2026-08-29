#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/http2/message/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/response/HttpResponseHeadersAccess.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpResponse.h"

namespace {

using ruvia::HttpInterimResponseHead;
using ruvia::HttpResponse;
using ruvia::detail::appendHttp2InterimResponseHeaders;
using ruvia::detail::appendHttp2ResponseHeaders;
using ruvia::detail::HpackDecoder;
using ruvia::detail::Http2ResponseHeadPlan;
using ruvia::detail::Http2ResponseHeadPlanResult;
using ruvia::detail::Http2StreamState;

static_assert(!std::is_default_constructible_v<Http2ResponseHeadPlan>);
static_assert(!std::is_default_constructible_v<Http2ResponseHeadPlanResult>);
static_assert(requires(const Http2ResponseHeadPlan& plan, const Http2ResponseHeadPlan&& temporary) {
    { plan.bodyPlan() } -> std::same_as<ruvia::detail::HttpResponseBodyPlan>;
    { temporary.bodyPlan() } -> std::same_as<ruvia::detail::HttpResponseBodyPlan>;
});

enum class ResponseHeadMode : std::uint8_t { kBuffered,
    kStreaming };

struct Collector final {
    std::vector<std::pair<std::string, std::string>> headers;
};

void addUncheckedHeader(HttpResponse& response, std::string_view name, std::string_view value) {
    auto& headers = const_cast<ruvia::HttpResponseHeaders&>(response.headers());
    (void)ruvia::detail::HttpResponseHeadersAccess::add(headers, name, value, 0);
}

bool collect(void* target, std::string_view name, std::string_view value) {
    static_cast<Collector*>(target)->headers.emplace_back(std::string(name), std::string(value));
    return true;
}

bool appendBufferedResponseHeaders(Http2StreamState& stream, const HttpResponse& response,
    ruvia::HttpKnownMethod method = ruvia::HttpKnownMethod::kGet) {
    const auto planResult = ruvia::detail::http2BufferedResponseHeadPlan(
        ruvia::detail::httpBufferedResponseWritePlan(method, response), response);
    const auto* plan = planResult.plan();
    const auto controlResult = ruvia::detail::http2FinalResponseControlPlan(response);
    const auto* http2Control = controlResult.control();
    if (plan == nullptr || http2Control == nullptr) {
        return false;
    }
    if (!appendHttp2ResponseHeaders(stream, response, *plan, *http2Control)) {
        return false;
    }
    return true;
}

bool decodeResponseHeaders(const HttpResponse& response, Collector& out,
    ResponseHeadMode mode = ResponseHeadMode::kBuffered,
    ruvia::HttpKnownMethod method = ruvia::HttpKnownMethod::kGet) {
    Http2StreamState stream(1, std::pmr::get_default_resource());
    if (mode == ResponseHeadMode::kBuffered) {
        if (!appendBufferedResponseHeaders(stream, response, method)) {
            return false;
        }
    } else {
        const auto bodyPlan = ruvia::detail::httpResponseBodyPlan(method, response.status());
        const auto planResult = ruvia::detail::http2StreamingResponseHeadPlan(bodyPlan, response);
        const auto* plan = planResult.plan();
        const auto controlResult = ruvia::detail::http2FinalResponseControlPlan(response);
        const auto* http2Control = controlResult.control();
        if (plan == nullptr || http2Control == nullptr) {
            return false;
        }
        if (!appendHttp2ResponseHeaders(stream, response, *plan, *http2Control)) {
            return false;
        }
    }

    HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
    const auto result = decoder.decode(stream.localHeaderBlock(), &out, &collect);
    return result.decoded() != nullptr;
}

bool decodeInterimResponseHeaders(const HttpInterimResponseHead& response, Collector& out) {
    Http2StreamState stream(1, std::pmr::get_default_resource());
    if (appendHttp2InterimResponseHeaders(stream, response) !=
        ruvia::detail::Http2InterimResponseHeaderEncodeStatus::kOk) {
        return false;
    }

    HpackDecoder decoder({.resource = std::pmr::get_default_resource()});
    const auto result = decoder.decode(stream.localHeaderBlock(), &out, &collect);
    return result.decoded() != nullptr;
}

bool hasHeader(const Collector& headers, std::string_view name, std::string_view value) {
    for (const auto& header : headers.headers) {
        if (header.first == name && header.second == value) {
            return true;
        }
    }
    return false;
}

bool hasHeaderName(const Collector& headers, std::string_view name) {
    for (const auto& header : headers.headers) {
        if (header.first == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

RUVIA_TEST(http2_response_head_content_length_plan_drives_execution) {
    HttpResponse buffered({.resource = std::pmr::get_default_resource()});
    buffered.body("hello");
    const auto bufferedPlanResult = ruvia::detail::http2BufferedResponseHeadPlan(
        ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kGet, buffered),
        buffered);
    const auto* bufferedPlan = bufferedPlanResult.plan();
    RUVIA_CHECK(bufferedPlan != nullptr);
    if (bufferedPlan == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(bufferedPlan->contentLength(), std::optional<std::uint64_t>{5});
    RUVIA_CHECK(!bufferedPlan->streamingContentLength().has_value());

    HttpResponse streaming({.resource = std::pmr::get_default_resource()});
    const auto streamingBodyPlan =
        ruvia::detail::httpResponseBodyPlan(ruvia::HttpKnownMethod::kGet, streaming.status());
    const auto streamingPlanResult =
        ruvia::detail::http2StreamingResponseHeadPlan(streamingBodyPlan, streaming);
    const auto* streamingPlan = streamingPlanResult.plan();
    RUVIA_CHECK(streamingPlan != nullptr);
    if (streamingPlan == nullptr) {
        return;
    }
    RUVIA_CHECK(!streamingPlan->contentLength().has_value());
    RUVIA_CHECK(!streamingPlan->streamingContentLength().has_value());

    streaming.header("Content-Length", "0005");
    const auto explicitPlanResult =
        ruvia::detail::http2StreamingResponseHeadPlan(streamingBodyPlan, streaming);
    const auto* explicitPlan = explicitPlanResult.plan();
    RUVIA_CHECK(explicitPlan != nullptr);
    if (explicitPlan == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(explicitPlan->contentLength(), std::optional<std::uint64_t>{5});
    RUVIA_CHECK_EQ(explicitPlan->streamingContentLength(), std::optional<std::uint64_t>{5});

    HttpResponse noContent({.resource = std::pmr::get_default_resource()});
    noContent.status(ruvia::http_status::kNoContent);
    noContent.header("Content-Length", "12");
    const auto forbiddenPlanResult = ruvia::detail::http2StreamingResponseHeadPlan(
        ruvia::detail::httpResponseBodyPlan(ruvia::HttpKnownMethod::kGet, noContent.status()),
        noContent);
    const auto* forbiddenPlan = forbiddenPlanResult.plan();
    RUVIA_CHECK(forbiddenPlan != nullptr);
    if (forbiddenPlan == nullptr) {
        return;
    }
    RUVIA_CHECK(!forbiddenPlan->contentLength().has_value());
    RUVIA_CHECK(!forbiddenPlan->streamingContentLength().has_value());

    const auto connectPlanResult =
        ruvia::detail::http2ConnectResponseHeadPlan(ruvia::detail::httpResponseBodyPlan(
            ruvia::HttpKnownMethod::kConnect, ruvia::http_status::kOk));
    const auto* connectPlan = connectPlanResult.plan();
    RUVIA_CHECK(connectPlan != nullptr);
    if (connectPlan == nullptr) {
        return;
    }
    RUVIA_CHECK(connectPlan->bodyPlan().contentSemantics() ==
                ruvia::detail::HttpResponseContentSemantics::kConnectTunnel);
    RUVIA_CHECK(!connectPlan->contentLength().has_value());
    RUVIA_CHECK(!connectPlan->streamingContentLength().has_value());

    const auto invalidConnectPlan = ruvia::detail::http2ConnectResponseHeadPlan(streamingBodyPlan);
    RUVIA_CHECK(invalidConnectPlan.plan() == nullptr);
    RUVIA_CHECK(invalidConnectPlan.failure() != nullptr);
    RUVIA_CHECK(invalidConnectPlan.failure()->error() ==
                ruvia::detail::Http2ResponseHeadPlanError::kConnectTunnelRequired);
}

RUVIA_TEST(http2_response_head_rejects_status_plan_mismatch) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kMultiStatus);
    response.body("planned");
    const auto bufferedWritePlan =
        ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kGet, response);
    const auto streamingBodyPlan =
        ruvia::detail::httpResponseBodyPlan(ruvia::HttpKnownMethod::kGet, response.status());

    response.status(ruvia::http_status::kAlreadyReported);
    const auto buffered = ruvia::detail::http2BufferedResponseHeadPlan(bufferedWritePlan, response);
    RUVIA_CHECK(buffered.plan() == nullptr);
    RUVIA_CHECK(buffered.failure() != nullptr);
    RUVIA_CHECK(buffered.failure()->error() ==
                ruvia::detail::Http2ResponseHeadPlanError::kResponseStatusMismatch);

    const auto streaming =
        ruvia::detail::http2StreamingResponseHeadPlan(streamingBodyPlan, response);
    RUVIA_CHECK(streaming.plan() == nullptr);
    RUVIA_CHECK(streaming.failure() != nullptr);
    RUVIA_CHECK(streaming.failure()->error() ==
                ruvia::detail::Http2ResponseHeadPlanError::kResponseStatusMismatch);
}

RUVIA_TEST(http2_response_head_rejects_representation_plan_mismatch) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kMultiStatus);
    response.body("old");
    const auto writePlan =
        ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kGet, response);

    response.body("longer");
    const auto result = ruvia::detail::http2BufferedResponseHeadPlan(writePlan, response);
    RUVIA_CHECK(result.plan() == nullptr);
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK(result.failure()->error() ==
                ruvia::detail::Http2ResponseHeadPlanError::kResponseRepresentationMismatch);
}

RUVIA_TEST(http2_interim_response_headers_are_bodyless_exact_and_normalized) {
    const ruvia::HttpHeaderView fields[] = {
        {"Link", "</style.css>; rel=preload"},
        {"Content-Type", "text/html; charset=utf-8"},
        {"X-Hint", "warm"},
    };
    const HttpInterimResponseHead response(ruvia::http_status::kEarlyHints, fields);

    Collector headers;
    RUVIA_CHECK(decodeInterimResponseHeaders(response, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "103"));
    RUVIA_CHECK(hasHeader(headers, "link", "</style.css>; rel=preload"));
    RUVIA_CHECK(hasHeader(headers, "content-type", "text/html; charset=utf-8"));
    RUVIA_CHECK(hasHeader(headers, "x-hint", "warm"));
    // Protocol encoding is exact: product policy must add optional Server/Date
    // explicitly instead of the generic HTTP core inventing fields.
    RUVIA_CHECK(!hasHeaderName(headers, "server"));
    RUVIA_CHECK(!hasHeaderName(headers, "date"));
    RUVIA_CHECK(!hasHeaderName(headers, "content-length"));
}

RUVIA_TEST(http2_interim_response_header_rejection_is_transactional) {
    Http2StreamState stream(1, std::pmr::get_default_resource());
    const auto rejects = [&](std::span<const ruvia::HttpHeaderView> fields) {
        stream.localHeaderBlock().assign("sentinel");
        const HttpInterimResponseHead response(ruvia::http_status::kEarlyHints, fields);
        const auto status = appendHttp2InterimResponseHeaders(stream, response);
        const bool unchanged = stream.localHeaderBlock() == "sentinel";
        stream.localHeaderBlock().clear();
        return status == ruvia::detail::Http2InterimResponseHeaderEncodeStatus::kInvalidHeader &&
               unchanged;
    };

    const ruvia::HttpHeaderView contentLength[] = {{"Content-Length", "0"}};
    const ruvia::HttpHeaderView transferEncoding[] = {
        {"Transfer-Encoding", "chunked"},
    };
    const ruvia::HttpHeaderView connection[] = {{"Connection", "close"}};
    const ruvia::HttpHeaderView te[] = {{"TE", "trailers"}};
    const ruvia::HttpHeaderView trailer[] = {{"Trailer", "X-Checksum"}};
    const ruvia::HttpHeaderView malformed[] = {{"Bad Name", "value"}};
    const ruvia::HttpHeaderView malformedContentEncoding[] = {
        {"Content-Encoding", "gzip;level=9"},
    };
    const ruvia::HttpHeaderView emptyContentEncoding[] = {
        {"Content-Encoding", ""},
    };
    const ruvia::HttpHeaderView malformedContentType[] = {
        {"Content-Type", "not a media type"},
    };
    const ruvia::HttpHeaderView emptyContentType[] = {
        {"Content-Type", ""},
    };
    const ruvia::HttpHeaderView leadingWhitespace[] = {
        {"Link", " </style.css>; rel=preload"},
    };
    const ruvia::HttpHeaderView trailingWhitespace[] = {
        {"Link", "</style.css>; rel=preload\t"},
    };
    const ruvia::HttpHeaderView duplicateServer[] = {
        {"Server", "one"},
        {"server", "two"},
    };
    RUVIA_CHECK(rejects(contentLength));
    RUVIA_CHECK(rejects(transferEncoding));
    RUVIA_CHECK(rejects(connection));
    RUVIA_CHECK(rejects(te));
    RUVIA_CHECK(rejects(trailer));
    RUVIA_CHECK(rejects(malformed));
    RUVIA_CHECK(rejects(malformedContentEncoding));
    RUVIA_CHECK(rejects(emptyContentEncoding));
    RUVIA_CHECK(rejects(malformedContentType));
    RUVIA_CHECK(rejects(emptyContentType));
    RUVIA_CHECK(rejects(leadingWhitespace));
    RUVIA_CHECK(rejects(trailingWhitespace));
    RUVIA_CHECK(rejects(duplicateServer));

    const std::string oversizedValue(ruvia::kMaxHttpHeaderBytes, 'x');
    const ruvia::HttpHeaderView oversized[] = {
        {"X-Oversized", oversizedValue},
    };
    RUVIA_CHECK(rejects(oversized));

    std::array<ruvia::HttpHeaderView, ruvia::kMaxHttpHeaderFields + 1> tooMany{};
    for (auto& header : tooMany) {
        header = {"X-Many", "value"};
    }
    RUVIA_CHECK(rejects(tooMany));
}

RUVIA_TEST(http2_response_headers_keep_server_product_policy_explicit) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});

    Collector defaults;
    RUVIA_CHECK(decodeResponseHeaders(response, defaults));
    RUVIA_CHECK(!hasHeaderName(defaults, "server"));
    // Date remains protocol-generated for a final 2xx origin response.
    RUVIA_CHECK(hasHeaderName(defaults, "date"));

    response.header("Server", "custom/1");
    Collector explicitServer;
    RUVIA_CHECK(decodeResponseHeaders(response, explicitServer));
    RUVIA_CHECK(hasHeader(explicitServer, "server", "custom/1"));
}

RUVIA_TEST(http2_response_headers_omit_content_length_for_204) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kNoContent);
    response.header("Content-Length", "12");

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "204"));
    RUVIA_CHECK(!hasHeaderName(headers, "content-length"));
}

RUVIA_TEST(http2_response_headers_keep_explicit_content_length_for_304) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kNotModified);
    response.header("Content-Length", "12");

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "304"));
    RUVIA_CHECK(hasHeader(headers, "content-length", "12"));
}

RUVIA_TEST(http2_response_headers_do_not_auto_content_length_for_304) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kNotModified);

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "304"));
    RUVIA_CHECK(!hasHeaderName(headers, "content-length"));
}

RUVIA_TEST(http2_response_headers_canonicalize_valid_explicit_content_length_once) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.header("Content-Length", "0005");

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, headers, ResponseHeadMode::kStreaming));
    RUVIA_CHECK(hasHeader(headers, "content-length", "5"));
    RUVIA_CHECK(!hasHeader(headers, "content-length", "0005"));
}

RUVIA_TEST(http2_response_headers_canonicalize_205_to_zero_length) {
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kResetContent);
    response.header("Content-Length", "12");

    for (const auto mode : {ResponseHeadMode::kStreaming, ResponseHeadMode::kBuffered}) {
        Collector headers;
        RUVIA_CHECK(decodeResponseHeaders(response, headers, mode));
        RUVIA_CHECK(hasHeader(headers, ":status", "205"));
        RUVIA_CHECK(hasHeader(headers, "content-length", "0"));
        RUVIA_CHECK(!hasHeader(headers, "content-length", "12"));
        RUVIA_CHECK(!hasHeader(headers, "content-length", "99"));
    }
}

RUVIA_TEST(http2_response_headers_override_wrong_content_length_for_200) {
    // RFC 9113 8.1.1: a content-length that disagrees with the DATA payload length
    // is malformed and a conformant peer resets the stream. For a buffered 2xx the
    // writer owns the length, so a handler's wrong Content-Length must be dropped
    // and replaced with the real body size -- matching the HTTP/1.1 path -- rather
    // than HPACK-encoded verbatim.
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kOk);
    response.header("Content-Length", "1000");  // wrong: the real body is 5 bytes
    response.body("hello");

    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, headers));
    RUVIA_CHECK(hasHeader(headers, ":status", "200"));
    RUVIA_CHECK(hasHeader(headers, "content-length", "5"));      // corrected to the body size
    RUVIA_CHECK(!hasHeader(headers, "content-length", "1000"));  // the wrong user value is gone
}

RUVIA_TEST(http2_response_headers_reject_only_preserved_invalid_content_length) {
    HttpResponse streaming({.resource = std::pmr::get_default_resource()});
    streaming.status(ruvia::http_status::kOk);
    streaming.header("Content-Length", "not-a-number");
    streaming.body("hello");
    Http2StreamState stream(1, std::pmr::get_default_resource());
    const auto streamingBodyPlan =
        ruvia::detail::httpResponseBodyPlan(ruvia::HttpKnownMethod::kGet, streaming.status());
    const auto streamingPlan =
        ruvia::detail::http2StreamingResponseHeadPlan(streamingBodyPlan, streaming);
    RUVIA_CHECK(streamingPlan.plan() == nullptr);
    RUVIA_CHECK(streamingPlan.failure() != nullptr);
    RUVIA_CHECK(streamingPlan.failure()->error() ==
                ruvia::detail::Http2ResponseHeadPlanError::kInvalidContentLength);
    RUVIA_CHECK(stream.localHeaderBlock().empty());

    // A buffered writer owns and canonicalizes the field, so an invalid caller
    // value is filtered before it can make the message malformed.
    Collector bufferedHeaders;
    RUVIA_CHECK(decodeResponseHeaders(streaming, bufferedHeaders));
    RUVIA_CHECK(hasHeader(bufferedHeaders, "content-length", "5"));

    HttpResponse notModified({.resource = std::pmr::get_default_resource()});
    notModified.status(ruvia::http_status::kNotModified);
    notModified.header("Content-Length", "5, 5");
    const auto notModifiedPlan = ruvia::detail::http2BufferedResponseHeadPlan(
        ruvia::detail::httpBufferedResponseWritePlan(ruvia::HttpKnownMethod::kGet, notModified),
        notModified);
    RUVIA_CHECK(notModifiedPlan.plan() == nullptr);
    RUVIA_CHECK(notModifiedPlan.failure() != nullptr);
    RUVIA_CHECK(stream.localHeaderBlock().empty());
}

RUVIA_TEST(http2_response_headers_set_cookie_uses_never_indexed_literal) {
    // RFC 7541 §7.1.3: Set-Cookie carries session credentials and must be emitted
    // as a never-indexed literal (0x10 prefix) so an intermediary never places it
    // in a shared HPACK dynamic table.
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kOk);
    response.header("Set-Cookie", "sid=secret; HttpOnly");

    Http2StreamState stream(1, std::pmr::get_default_resource());
    RUVIA_CHECK(appendBufferedResponseHeaders(stream, response));
    const auto& block = stream.localHeaderBlock();

    // Byte 0 is the indexed :status 200 (0x88); Set-Cookie is the next field and
    // its representation prefix must be the never-indexed literal nibble (0x10).
    RUVIA_CHECK(block.size() >= 2);
    RUVIA_CHECK(static_cast<unsigned char>(block[0]) == 0x88);
    RUVIA_CHECK((static_cast<unsigned char>(block[1]) & 0xF0U) == 0x10U);

    // The never-indexed hint must not corrupt the round-trip value.
    Collector headers;
    RUVIA_CHECK(decodeResponseHeaders(response, headers));
    RUVIA_CHECK(hasHeader(headers, "set-cookie", "sid=secret; HttpOnly"));
}

RUVIA_TEST(http2_response_headers_non_sensitive_uses_without_indexing) {
    // A non-credential field (content-type) must stay a plain without-indexing
    // literal (0x00 nibble) -- confirms the never-indexed choice discriminates by
    // header name rather than marking everything.
    HttpResponse response({.resource = std::pmr::get_default_resource()});
    response.status(ruvia::http_status::kOk);
    response.header("Content-Type", "text/plain");

    Http2StreamState stream(1, std::pmr::get_default_resource());
    RUVIA_CHECK(appendBufferedResponseHeaders(stream, response));
    const auto& block = stream.localHeaderBlock();

    RUVIA_CHECK(block.size() >= 2);
    RUVIA_CHECK(static_cast<unsigned char>(block[0]) == 0x88);
    RUVIA_CHECK((static_cast<unsigned char>(block[1]) & 0xF0U) == 0x00U);
}

RUVIA_TEST(http2_response_headers_reject_connection_specific_fields_before_hpack) {
    constexpr std::pair<std::string_view, std::string_view> fields[] = {
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Transfer-Encoding", "chunked"},
        {"Upgrade", "websocket"},
    };
    for (const auto& [name, value] : fields) {
        HttpResponse response({.resource = std::pmr::get_default_resource()});
        if (name == "TE") {
            addUncheckedHeader(response, name, value);
        } else {
            response.header(name, value);
        }
        Collector headers;
        RUVIA_CHECK(!decodeResponseHeaders(response, headers));
        RUVIA_CHECK(headers.headers.empty());
    }
}

RUVIA_TEST(http2_response_headers_reject_leading_and_trailing_value_whitespace_before_hpack) {
    for (const auto value : {" value", "value ", "\tvalue", "value\t"}) {
        HttpResponse response({.resource = std::pmr::get_default_resource()});
        ruvia::detail::setResponseHeaderStableView(response, "X-Test", value);

        Collector headers;
        RUVIA_CHECK(!decodeResponseHeaders(response, headers));
        RUVIA_CHECK(headers.headers.empty());
    }
}

RUVIA_TEST(http2_response_headers_reject_malformed_name_and_value_before_hpack) {
    constexpr std::pair<std::string_view, std::string_view> fields[] = {
        {"Bad Name", "value"},
        {"X-Test", std::string_view("bad\r\nvalue", 10)},
    };

    for (const auto& [name, value] : fields) {
        HttpResponse response({.resource = std::pmr::get_default_resource()});
        ruvia::detail::setResponseHeaderStableView(response, name, value);

        Collector headers;
        RUVIA_CHECK(!decodeResponseHeaders(response, headers));
        RUVIA_CHECK(headers.headers.empty());
    }
}
