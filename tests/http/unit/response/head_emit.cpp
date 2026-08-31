#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;
using ruvia::detail::appendResponseHead;
using ruvia::detail::http1BufferedResponsePlan;
using ruvia::detail::http1ChunkedResponseStreamHeadPlan;
using ruvia::detail::http1CloseDelimitedResponseStreamHeadPlan;
using ruvia::detail::Http1FinalResponseCommitError;
using ruvia::detail::Http1FinalResponseCommitFailure;
using ruvia::detail::Http1FinalResponseCommitResult;
using ruvia::detail::http1KnownLengthResponseStreamHeadPlan;
using ruvia::detail::Http1ResponseHeadPlan;
using ruvia::detail::Http1ServerConnectionPlan;
using ruvia::detail::httpResponseBodyPlan;
using ruvia::detail::ResponseHeadBuffer;

static_assert(
    std::same_as<decltype(std::declval<const Http1FinalResponseCommitResult&>().committed()),
        const Http1ServerConnectionPlan*>);
static_assert(std::derived_from<Http1FinalResponseCommitError, std::exception>);
static_assert(std::is_trivially_copyable_v<Http1FinalResponseCommitResult>);
static_assert(sizeof(Http1FinalResponseCommitResult) <= 8);

template <typename T>
concept HasRawFinalCommitError = requires(const T& failure) { failure.error(); };

static_assert(!HasRawFinalCommitError<Http1FinalResponseCommitFailure>);

ruvia::detail::Http1ServerConnectionPlan connectionPlanFor(
    ruvia::HttpProtocolVersion protocolVersion) {
    const ruvia::detail::HttpConnectionOptions options;
    return protocolVersion == ruvia::HttpProtocolVersion::kHttp10
               ? ruvia::detail::http1PlanHttp10RequestConnection(options)
               : ruvia::detail::http1PlanHttp11RequestConnection(options);
}

std::string emitHead(HttpResponse& response, const Http1ResponseHeadPlan& plan) {
    ResponseHeadBuffer buffer(std::pmr::new_delete_resource());
    appendResponseHead(response, buffer, plan);
    const auto view = buffer.view();
    return std::string(view.data(), view.size());
}

std::string emitBufferedHead(HttpResponse& response,
    HttpKnownMethod requestMethod = HttpKnownMethod::kGet,
    ruvia::HttpProtocolVersion protocolVersion = ruvia::HttpProtocolVersion::kHttp11) {
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(requestMethod, response);
    const auto responsePlan =
        http1BufferedResponsePlan(writePlan, connectionPlanFor(protocolVersion));
    return emitHead(response, responsePlan.headPlan());
}

std::string emitChunkedStreamHead(HttpResponse& response,
    HttpKnownMethod requestMethod = HttpKnownMethod::kGet,
    ruvia::HttpProtocolVersion protocolVersion = ruvia::HttpProtocolVersion::kHttp11) {
    return emitHead(response,
        http1ChunkedResponseStreamHeadPlan(httpResponseBodyPlan(requestMethod, response.status()),
            connectionPlanFor(protocolVersion)));
}

std::string emitKnownLengthStreamHead(HttpResponse& response, std::uint64_t contentLength,
    HttpKnownMethod requestMethod = HttpKnownMethod::kGet,
    ruvia::HttpProtocolVersion protocolVersion = ruvia::HttpProtocolVersion::kHttp11) {
    return emitHead(response, http1KnownLengthResponseStreamHeadPlan(
                                  httpResponseBodyPlan(requestMethod, response.status()),
                                  connectionPlanFor(protocolVersion), contentLength));
}

std::string emitCloseDelimitedStreamHead(HttpResponse& response,
    HttpKnownMethod requestMethod = HttpKnownMethod::kGet,
    ruvia::HttpProtocolVersion protocolVersion = ruvia::HttpProtocolVersion::kHttp11) {
    return emitHead(response, http1CloseDelimitedResponseStreamHeadPlan(
                                  httpResponseBodyPlan(requestMethod, response.status()),
                                  connectionPlanFor(protocolVersion)));
}

std::size_t countOccurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (auto pos = haystack.find(needle); pos != std::string_view::npos;
        pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

ruvia::detail::Http1ServerConnectionPlan commitResponse(
    HttpResponse& response, ruvia::detail::Http1ServerConnectionPlan plan) {
    const auto result = ruvia::detail::http1CommitFinalResponse(response, plan);
    if (result.failure() != nullptr || result.committed() == nullptr) {
        throw std::logic_error("expected successful HTTP/1 final response commit");
    }
    return *result.committed();
}

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

template <typename Fn>
bool throwsLength(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::length_error&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(http1_buffered_response_plan_owns_request_version_and_length) {
    using ruvia::detail::http1BufferedResponsePlan;
    using ruvia::detail::Http1ServerRequestParser;
    using ruvia::detail::httpBufferedResponseWritePlan;

    Http1ServerRequestParser parser;
    const auto emitFor = [&](std::string_view request) {
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        response.body("hello");
        const auto connectionPlan =
            commitResponse(response, parser.parseMessage(request).connectionPlan);
        const auto responsePlan = http1BufferedResponsePlan(
            httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response), connectionPlan);
        RUVIA_CHECK_EQ(
            responsePlan.headPlan().buffered()->contentLength(), responsePlan.contentLength());
        return std::pair(
            emitHead(response, responsePlan.headPlan()), responsePlan.headPlan().protocolVersion());
    };

    const auto [http10Head, http10Version] =
        emitFor("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    RUVIA_CHECK(http10Version == ruvia::HttpProtocolVersion::kHttp10);
    RUVIA_CHECK(http10Head.starts_with("HTTP/1.0 200 OK\r\n"));
    RUVIA_CHECK(http10Head.find("Content-Length: 5\r\n") != std::string_view::npos);
    RUVIA_CHECK(http10Head.find("Connection: keep-alive\r\n") != std::string_view::npos);

    const auto [http11Head, http11Version] = emitFor("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(http11Version == ruvia::HttpProtocolVersion::kHttp11);
    RUVIA_CHECK(http11Head.starts_with("HTTP/1.1 200 OK\r\n"));
    RUVIA_CHECK(http11Head.find("Connection:") == std::string_view::npos);
}

RUVIA_TEST(http1_response_head_rejects_status_plan_mismatch) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::kMultiStatus);
    response.body("planned");
    const auto plan = http1BufferedResponsePlan(
        ruvia::detail::httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response),
        connectionPlanFor(ruvia::HttpProtocolVersion::kHttp11));

    response.status(ruvia::http_status::kAlreadyReported);
    RUVIA_CHECK(throwsInvalid([&] { (void)emitHead(response, plan.headPlan()); }));
    RUVIA_CHECK_EQ(plan.responseStatus(), ruvia::http_status::kMultiStatus);
    RUVIA_CHECK_EQ(plan.headPlan().bodyPlan().responseStatus(), ruvia::http_status::kMultiStatus);
}

RUVIA_TEST(http1_response_head_rejects_representation_plan_mismatch) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::kMultiStatus);
    response.body("old");
    const auto plan = http1BufferedResponsePlan(
        ruvia::detail::httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response),
        connectionPlanFor(ruvia::HttpProtocolVersion::kHttp11));

    response.body("longer");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitHead(response, plan.headPlan()); }));
    RUVIA_CHECK_EQ(plan.contentLength(), std::uint64_t{3});
}

RUVIA_TEST(http1_response_head_validates_trailer_field_names) {
    const auto rejects = [&ruvia_ctx](std::string_view value) {
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        ruvia::detail::setResponseHeaderStableView(response, "Trailer", value);
        RUVIA_CHECK(throwsInvalid([&] { (void)emitBufferedHead(response); }));
    };

    rejects("Content-Length");
    rejects("X-Checksum, bad field");
    rejects(",");

    HttpResponse valid({.resource = std::pmr::new_delete_resource()});
    valid.header("Trailer", "ETag, X-Checksum");
    const auto validHead = emitChunkedStreamHead(valid);
    RUVIA_CHECK(validHead.find("Trailer: ETag, X-Checksum\r\n") != std::string_view::npos);

    HttpResponse empty({.resource = std::pmr::new_delete_resource()});
    empty.header("Trailer", "");
    RUVIA_CHECK(!throwsInvalid([&] { (void)emitBufferedHead(empty); }));
}

RUVIA_TEST(http1_response_head_rejects_non_empty_trailer_without_chunked_framing) {
    HttpResponse buffered({.resource = std::pmr::new_delete_resource()});
    buffered.header("Trailer", "ETag");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitBufferedHead(buffered); }));

    HttpResponse knownLength({.resource = std::pmr::new_delete_resource()});
    knownLength.header("Trailer", "ETag");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitKnownLengthStreamHead(knownLength, 5); }));

    HttpResponse closeDelimited({.resource = std::pmr::new_delete_resource()});
    closeDelimited.header("Trailer", "ETag");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitCloseDelimitedStreamHead(closeDelimited); }));

    HttpResponse head({.resource = std::pmr::new_delete_resource()});
    head.header("Trailer", "ETag");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitChunkedStreamHead(head, HttpKnownMethod::kHead); }));

    HttpResponse chunked({.resource = std::pmr::new_delete_resource()});
    chunked.header("Trailer", "ETag");
    RUVIA_CHECK(!throwsInvalid([&] { (void)emitChunkedStreamHead(chunked); }));
}

RUVIA_TEST(http1_protocol_finalizer_returns_the_authoritative_reuse_verdict) {
    using ruvia::Http1ClosePolicy;
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;

    HttpResponse http10({.resource = std::pmr::new_delete_resource()});
    const auto http10Plan = commitResponse(http10,
        parser.parseMessage("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").connectionPlan);
    RUVIA_CHECK(http10Plan.disposition() == Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK_EQ(std::string(http10.header("Connection").value_or(std::string_view{})),
        std::string("keep-alive"));

    HttpResponse http10Upgrade({.resource = std::pmr::new_delete_resource()});
    http10Upgrade.header("Connection", "upgrade");
    const auto http10UpgradePlan = commitResponse(http10Upgrade,
        parser.parseMessage("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").connectionPlan);
    RUVIA_CHECK(http10UpgradePlan.disposition() == Http1ClosePolicy::kAllowReuse);
    const auto http10UpgradeHead = emitBufferedHead(http10Upgrade);
    RUVIA_CHECK(http10UpgradeHead.find("Connection: upgrade\r\n") != std::string_view::npos);
    RUVIA_CHECK(http10UpgradeHead.find("Connection: keep-alive\r\n") != std::string_view::npos);

    HttpResponse applicationClose({.resource = std::pmr::new_delete_resource()});
    applicationClose.header("Connection", "upgrade");
    applicationClose.header("Connection", "close",
        HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
    const auto applicationClosePlan = commitResponse(
        applicationClose, parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan);
    RUVIA_CHECK(applicationClosePlan.disposition() == Http1ClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK_EQ(std::string(applicationClose.header("Connection").value_or(std::string_view{})),
        std::string("close"));
    const auto applicationCloseHead = emitBufferedHead(applicationClose);
    RUVIA_CHECK_EQ(countOccurrences(applicationCloseHead, "Connection: "), std::size_t{1});

    HttpResponse runtimeClose({.resource = std::pmr::new_delete_resource()});
    const auto runtimeClosePlan = commitResponse(runtimeClose,
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan.requireClose());
    RUVIA_CHECK(runtimeClosePlan.disposition() == Http1ClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK_EQ(std::string(runtimeClose.header("Connection").value_or(std::string_view{})),
        std::string("close"));
}

RUVIA_TEST(http1_protocol_finalizer_generates_upgrade_pairing) {
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    const auto requestPlan =
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan;
    HttpResponse unpaired({.resource = std::pmr::new_delete_resource()});
    unpaired.status(ruvia::http_status::kUpgradeRequired);
    unpaired.header("Upgrade", "websocket");
    RUVIA_CHECK(commitResponse(unpaired, requestPlan).disposition() ==
                ruvia::Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK_EQ(std::string(unpaired.header("Connection").value_or(std::string_view{})),
        std::string("Upgrade"));

    HttpResponse closing({.resource = std::pmr::new_delete_resource()});
    closing.header("Upgrade", "websocket");
    RUVIA_CHECK(commitResponse(closing, requestPlan.requireClose()).disposition() ==
                ruvia::Http1ClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK_EQ(std::string(closing.header("Connection").value_or(std::string_view{})),
        std::string("close, Upgrade"));
    RUVIA_CHECK_EQ(std::string(closing.header("Upgrade").value_or(std::string_view{})),
        std::string("websocket"));
}

RUVIA_TEST(http1_protocol_finalizer_rejects_upgrade_required_without_protocol) {
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    const auto requestPlan =
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan;

    HttpResponse missingUpgrade({.resource = std::pmr::new_delete_resource()});
    missingUpgrade.status(ruvia::http_status::kUpgradeRequired);
    const auto result = ruvia::detail::http1CommitFinalResponse(missingUpgrade, requestPlan);
    RUVIA_CHECK(result.committed() == nullptr);
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(result.failure()->exception().what()),
        std::string_view("Upgrade Required response requires an Upgrade protocol"));
    RUVIA_CHECK(!missingUpgrade.header("Connection").has_value());
}

RUVIA_TEST(http1_stream_prepare_preserves_typed_final_commit_failure) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto streamPlan = ruvia::detail::http1PlanResponseStream(
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n"),
        ruvia::Http1ClosePolicy::kAllowReuse);
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::kUpgradeRequired);

    const auto result = ruvia::detail::prepareHttp1ResponseStreamHead(std::move(response),
        ruvia::detail::ResponseStreamKind::kGeneric, streamPlan,
        ruvia::detail::ResponseTrailerIntent::kNone);
    RUVIA_CHECK(result.prepared() == nullptr);
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK_EQ(std::string_view(result.failure()->exception().what()),
        std::string_view("Upgrade Required response requires an Upgrade protocol"));
}

RUVIA_TEST(response_head_emits_well_formed_normal) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::kOk);
    response.header("X-Foo", "bar");
    response.body("hello");
    const auto head = emitBufferedHead(response);

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    RUVIA_CHECK(head.find("X-Foo: bar\r\n") != std::string_view::npos);
    RUVIA_CHECK(head.find("Server:") == std::string_view::npos);                // product policy is explicit
    RUVIA_CHECK(head.find("Date: ") != std::string_view::npos);                 // auto-injected
    RUVIA_CHECK(head.find("Content-Length: 5\r\n") != std::string_view::npos);  // auto, body size
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));                                    // blank-line terminator
}

RUVIA_TEST(response_head_extension_status_uses_an_empty_reason_phrase) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.status(ruvia::HttpStatusCode::fromValue(299));
    const auto head = emitBufferedHead(response);

    // RFC 9112 section 4 keeps the SP before the optional reason-phrase.
    // An unregistered status must not be mislabeled as a generic client error.
    RUVIA_CHECK(head.starts_with("HTTP/1.1 299 \r\n"));
    RUVIA_CHECK(head.find("Bad Request") == std::string_view::npos);
}

RUVIA_TEST(response_head_preserves_explicit_server_and_does_not_duplicate_date) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.header("Server", "custom");
    response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.body("x");
    const auto head = emitBufferedHead(response);

    RUVIA_CHECK(head.find("Server: custom\r\n") != std::string_view::npos);
    RUVIA_CHECK_EQ(countOccurrences(head, "Server: "), std::size_t{1});
    RUVIA_CHECK_EQ(countOccurrences(head, "Date: "), std::size_t{1});  // exactly one Date
}

RUVIA_TEST(response_head_suppresses_auto_content_length) {
    // A streaming/chunked writer owns framing itself. Caller-provided framing is
    // replaced by one canonical chunked field and no Content-Length survives.
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.body("hello");
    response.header("Transfer-Encoding", "gzip, chunked");
    response.header("Content-Length", "999");
    const auto head = emitChunkedStreamHead(response);
    RUVIA_CHECK(head.find("Content-Length:") == std::string_view::npos);
    RUVIA_CHECK(head.find("Transfer-Encoding: chunked\r\n") != std::string_view::npos);
    RUVIA_CHECK(head.find("gzip") == std::string_view::npos);
    RUVIA_CHECK_EQ(countOccurrences(head, "Transfer-Encoding: "), std::size_t{1});
}

RUVIA_TEST(response_head_canonicalizes_managed_framing_fields_across_case) {
    std::uint64_t state = 0x5d28'c4f1'9e73'ab06ULL;
    const auto next = [&state] {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    };
    const auto randomCase = [&next](std::string_view name) {
        std::string out;
        out.reserve(name.size());
        for (const char c : name) {
            if (c >= 'A' && c <= 'Z') {
                out.push_back((next() & 1U) != 0 ? static_cast<char>(c - 'A' + 'a') : c);
            } else if (c >= 'a' && c <= 'z') {
                out.push_back((next() & 1U) != 0 ? static_cast<char>(c - 'a' + 'A') : c);
            } else {
                out.push_back(c);
            }
        }
        return out;
    };

    for (std::size_t sample = 0; sample < 512; ++sample) {
        HttpResponse buffered({.resource = std::pmr::new_delete_resource()});
        buffered.body("payload");
        buffered.header(randomCase("Content-Length"), "999");
        buffered.header(randomCase("Transfer-Encoding"), "gzip, chunked");
        buffered.header("X-Sample", std::to_string(sample));
        const auto bufferedHead = emitBufferedHead(buffered);
        RUVIA_CHECK_EQ(countOccurrences(bufferedHead, "Content-Length: "), std::size_t{1});
        RUVIA_CHECK(bufferedHead.find("Content-Length: 7\r\n") != std::string_view::npos);
        RUVIA_CHECK(bufferedHead.find("Content-Length: 999\r\n") == std::string_view::npos);
        RUVIA_CHECK(bufferedHead.find("Transfer-Encoding:") == std::string_view::npos);
        RUVIA_CHECK(bufferedHead.find("gzip") == std::string_view::npos);
        RUVIA_CHECK(bufferedHead.ends_with("\r\n\r\n"));

        HttpResponse chunked({.resource = std::pmr::new_delete_resource()});
        chunked.header(randomCase("Content-Length"), "999");
        chunked.header(randomCase("Transfer-Encoding"), "gzip, chunked");
        chunked.header("X-Sample", std::to_string(sample));
        const auto chunkedHead = emitChunkedStreamHead(chunked);
        RUVIA_CHECK(chunkedHead.find("Content-Length:") == std::string_view::npos);
        RUVIA_CHECK_EQ(countOccurrences(chunkedHead, "Transfer-Encoding: "), std::size_t{1});
        RUVIA_CHECK(chunkedHead.find("Transfer-Encoding: chunked\r\n") != std::string_view::npos);
        RUVIA_CHECK(chunkedHead.find("gzip") == std::string_view::npos);
        RUVIA_CHECK(chunkedHead.ends_with("\r\n\r\n"));
    }
}

RUVIA_TEST(http1_response_head_rejects_http10_chunked_payload_plan) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.body("hello");
    const auto plan = http1ChunkedResponseStreamHeadPlan(
        httpResponseBodyPlan(HttpKnownMethod::kGet, response.status()),
        connectionPlanFor(ruvia::HttpProtocolVersion::kHttp10));

    RUVIA_CHECK(throwsInvalid([&] { (void)emitHead(response, plan); }));
}

RUVIA_TEST(http1_known_length_stream_owns_canonical_length_and_status_semantics) {
    HttpResponse get({.resource = std::pmr::new_delete_resource()});
    get.header("Content-Length", "999");
    get.header("Transfer-Encoding", "chunked");
    const auto getHead = emitKnownLengthStreamHead(get, 5);
    RUVIA_CHECK_EQ(countOccurrences(getHead, "Content-Length: "), std::size_t{1});
    RUVIA_CHECK(getHead.find("Content-Length: 5\r\n") != std::string_view::npos);
    RUVIA_CHECK(getHead.find("Transfer-Encoding:") == std::string_view::npos);

    HttpResponse head({.resource = std::pmr::new_delete_resource()});
    const auto headWire = emitKnownLengthStreamHead(head, 5, HttpKnownMethod::kHead);
    RUVIA_CHECK(headWire.find("Content-Length: 5\r\n") != std::string_view::npos);

    HttpResponse noContent({.resource = std::pmr::new_delete_resource()});
    noContent.status(ruvia::http_status::kNoContent);
    noContent.header("Content-Length", "5");
    const auto noContentWire = emitKnownLengthStreamHead(noContent, 5);
    RUVIA_CHECK(noContentWire.find("Content-Length:") == std::string_view::npos);
    RUVIA_CHECK(noContentWire.find("Transfer-Encoding:") == std::string_view::npos);
}

RUVIA_TEST(http1_chunked_stream_does_not_invent_framing_for_head) {
    HttpResponse metadata({.resource = std::pmr::new_delete_resource()});
    metadata.header("Content-Length", "5");
    metadata.header("Transfer-Encoding", "chunked");
    const auto metadataWire = emitChunkedStreamHead(metadata, HttpKnownMethod::kHead);
    RUVIA_CHECK(metadataWire.find("Content-Length: 5\r\n") != std::string_view::npos);
    RUVIA_CHECK(metadataWire.find("Transfer-Encoding:") == std::string_view::npos);

    HttpResponse unknown({.resource = std::pmr::new_delete_resource()});
    const auto unknownWire = emitChunkedStreamHead(unknown, HttpKnownMethod::kHead);
    RUVIA_CHECK(unknownWire.find("Content-Length:") == std::string_view::npos);
    RUVIA_CHECK(unknownWire.find("Transfer-Encoding:") == std::string_view::npos);
}

RUVIA_TEST(http1_consumed_request_body_can_commit_a_reusable_known_length_stream) {
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage(
        "POST / HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 1\r\n"
        "\r\n"
        "x");
    const auto streamPlan = ruvia::detail::http1PlanConsumedResponseStream(
        parsed, ruvia::Http1ClosePolicy::kAllowReuse);
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    const auto preparedResult = ruvia::detail::prepareHttp1KnownLengthResponseStreamHead(
        std::move(response), 5, ruvia::detail::ResponseStreamKind::kGeneric, streamPlan);
    const auto* prepared = preparedResult.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(prepared->connectionPlan().disposition() == ruvia::Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK(prepared->responseHeadPlan().knownLengthStream() != nullptr);
    RUVIA_CHECK_EQ(
        prepared->responseHeadPlan().knownLengthStream()->contentLength(), std::uint64_t{5});
    RUVIA_CHECK(prepared->responseHeadPlan().chunkedStream() == nullptr);
    RUVIA_CHECK(prepared->commitPlan().framing() ==
                ruvia::detail::ResponseStreamFraming::kHttp1KnownLength);
}

RUVIA_TEST(response_head_close_delimited_stream_rejects_declared_framing) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.body("streamed");
    response.header("Transfer-Encoding", "chunked");
    response.header("Content-Length", "8");

    const auto head = emitCloseDelimitedStreamHead(response);
    RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string_view::npos);
    RUVIA_CHECK(head.find("Content-Length:") == std::string_view::npos);

    // A HEAD response has no payload and may retain representation length
    // metadata, but HTTP/1.0 still cannot carry Transfer-Encoding.
    const auto metadataHead = emitCloseDelimitedStreamHead(response, HttpKnownMethod::kHead);
    RUVIA_CHECK(metadataHead.find("Transfer-Encoding:") == std::string_view::npos);
    RUVIA_CHECK(metadataHead.find("Content-Length: 8\r\n") != std::string_view::npos);

    HttpResponse notModified({.resource = std::pmr::new_delete_resource()});
    notModified.status(ruvia::http_status::kNotModified);
    notModified.header("Transfer-Encoding", "chunked");
    notModified.header("Content-Length", "123");
    const auto notModifiedHead = emitCloseDelimitedStreamHead(notModified);
    RUVIA_CHECK(notModifiedHead.find("Transfer-Encoding:") == std::string_view::npos);
    RUVIA_CHECK(notModifiedHead.find("Content-Length: 123\r\n") != std::string_view::npos);
}

RUVIA_TEST(response_head_validates_explicit_content_length_metadata) {
    HttpResponse malformed({.resource = std::pmr::new_delete_resource()});
    malformed.status(ruvia::http_status::kNotModified);
    malformed.header("Content-Length", "invalid");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitBufferedHead(malformed); }));

    HttpResponse conflicting({.resource = std::pmr::new_delete_resource()});
    conflicting.status(ruvia::http_status::kNotModified);
    conflicting.header("Content-Length", "7, 8");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitBufferedHead(conflicting); }));

    HttpResponse equivalent({.resource = std::pmr::new_delete_resource()});
    equivalent.status(ruvia::http_status::kNotModified);
    equivalent.header("Content-Length", "0007, 7");
    const auto canonical = emitBufferedHead(equivalent);
    RUVIA_CHECK_EQ(countOccurrences(canonical, "Content-Length: "), std::size_t{1});
    RUVIA_CHECK(canonical.find("Content-Length: 7\r\n") != std::string_view::npos);

    HttpResponse headMetadata({.resource = std::pmr::new_delete_resource()});
    headMetadata.header("Content-Length", "bad");
    RUVIA_CHECK(throwsInvalid(
        [&] { (void)emitCloseDelimitedStreamHead(headMetadata, HttpKnownMethod::kHead); }));
}

RUVIA_TEST(response_head_bodyless_status_omits_auto_content_length) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::kNoContent);
    const auto head = emitBufferedHead(response);
    RUVIA_CHECK(head.starts_with("HTTP/1.1 204 No Content\r\n"));
    RUVIA_CHECK(head.find("Content-Length:") == std::string_view::npos);
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}

RUVIA_TEST(response_head_reset_content_canonicalizes_zero_length) {
    // RFC 9110 §15.3.6 forbids 205 content. Even if the application supplies a
    // body and contradictory framing, both buffered and streaming head emission
    // must suppress it and retain an unambiguous persistent HTTP/1 message.
    for (const bool streaming : {false, true}) {
        HttpResponse response({.resource = std::pmr::new_delete_resource()});
        response.status(ruvia::http_status::kResetContent);
        response.body("must-not-be-sent");
        response.header("Content-Length", "16");
        response.header("Transfer-Encoding", "chunked");
        const auto head = streaming ? emitChunkedStreamHead(response) : emitBufferedHead(response);
        RUVIA_CHECK(head.starts_with("HTTP/1.1 205 Reset Content\r\n"));
        RUVIA_CHECK_EQ(countOccurrences(head, "Content-Length: "), std::size_t{1});
        RUVIA_CHECK(head.find("Content-Length: 0\r\n") != std::string_view::npos);
        RUVIA_CHECK(head.find("Content-Length: 16\r\n") == std::string_view::npos);
        RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string_view::npos);
        RUVIA_CHECK(head.ends_with("\r\n\r\n"));
    }
}

RUVIA_TEST(response_head_heap_spill_preserves_full_output) {
    // Force the emitted head well past the 512-byte stack buffer so the heap
    // (reserveAdditional) emit path runs. Every header must survive intact and
    // the precomputed size bound must not undercount -- an undercount would let
    // the unchecked raw stack sink overflow or the output truncate.
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    response.status(ruvia::http_status::kOk);
    const std::string big(200, 'v');
    for (int i = 0; i < 10; ++i) {
        response.header("X-Pad-" + std::to_string(i), big);
    }
    response.body("body");
    const auto head = emitBufferedHead(response);

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    for (int i = 0; i < 10; ++i) {
        RUVIA_CHECK(head.find("X-Pad-" + std::to_string(i) + ": " + big + "\r\n") !=
                    std::string_view::npos);
    }
    RUVIA_CHECK(head.find("Content-Length: 4\r\n") != std::string_view::npos);
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}

RUVIA_TEST(response_head_rejects_oversized_field_section) {
    HttpResponse oversized({.resource = std::pmr::new_delete_resource()});
    oversized.header("X-Oversized", std::string(ruvia::kMaxHttpHeaderBytes, 'v'));
    RUVIA_CHECK(throwsLength([&] { (void)emitBufferedHead(oversized); }));

    HttpResponse tooMany({.resource = std::pmr::new_delete_resource()});
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        tooMany.header("X-Field-" + std::to_string(i), "value");
    }
    RUVIA_CHECK(throwsLength([&] { (void)emitBufferedHead(tooMany); }));

    HttpResponse generatedOverflow({.resource = std::pmr::new_delete_resource()});
    for (std::size_t i = 0; i < ruvia::kMaxHttpHeaderFields - 1; ++i) {
        generatedOverflow.header("X-Generated-" + std::to_string(i), "value");
    }
    RUVIA_CHECK(throwsLength([&] {
        // The generated Date and Content-Length fields also consume slots.
        (void)emitBufferedHead(generatedOverflow);
    }));
}

RUVIA_TEST(response_head_rejects_malformed_header_name_and_value) {
    HttpResponse badName({.resource = std::pmr::new_delete_resource()});
    ruvia::detail::setResponseHeaderStableView(badName, "Bad Name", "value");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitBufferedHead(badName); }));

    HttpResponse badValue({.resource = std::pmr::new_delete_resource()});
    ruvia::detail::setResponseHeaderStableView(
        badValue, "X-Test", std::string_view("bad\r\nvalue", 10));
    RUVIA_CHECK(throwsInvalid([&] { (void)emitBufferedHead(badValue); }));
}

RUVIA_TEST(response_head_rejects_request_only_te_field) {
    HttpResponse response({.resource = std::pmr::new_delete_resource()});
    ruvia::detail::setResponseHeaderStableView(response, "TE", "trailers");
    RUVIA_CHECK(throwsInvalid([&] { (void)emitBufferedHead(response); }));
}
