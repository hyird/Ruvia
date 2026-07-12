#include "test_harness.h"

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseHeadBuffer.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"

namespace {

using ruvia::HttpResponse;
using ruvia::HttpKnownMethod;
using ruvia::detail::Http1ResponseHeadPlan;
using ruvia::detail::ResponseHeadBuffer;
using ruvia::detail::appendResponseHead;
using ruvia::detail::http1BufferedResponsePlan;
using ruvia::detail::http1ChunkedResponseStreamHeadPlan;
using ruvia::detail::http1CloseDelimitedResponseStreamHeadPlan;
using ruvia::detail::httpResponseBodyPlan;

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

std::string emitBufferedHead(
    HttpResponse& response,
    HttpKnownMethod requestMethod = HttpKnownMethod::kGet,
    ruvia::HttpProtocolVersion protocolVersion =
        ruvia::HttpProtocolVersion::kHttp11) {
    const auto writePlan = ruvia::detail::httpBufferedResponseWritePlan(
        requestMethod,
        response);
    const auto responsePlan = http1BufferedResponsePlan(
        writePlan,
        connectionPlanFor(protocolVersion));
    return emitHead(response, responsePlan.headPlan());
}

std::string emitChunkedStreamHead(
    HttpResponse& response,
    HttpKnownMethod requestMethod = HttpKnownMethod::kGet,
    ruvia::HttpProtocolVersion protocolVersion =
        ruvia::HttpProtocolVersion::kHttp11) {
    return emitHead(
        response,
        http1ChunkedResponseStreamHeadPlan(
            httpResponseBodyPlan(requestMethod, response.status()),
            connectionPlanFor(protocolVersion)));
}

std::string emitCloseDelimitedStreamHead(
    HttpResponse& response,
    HttpKnownMethod requestMethod = HttpKnownMethod::kGet,
    ruvia::HttpProtocolVersion protocolVersion =
        ruvia::HttpProtocolVersion::kHttp11) {
    return emitHead(
        response,
        http1CloseDelimitedResponseStreamHeadPlan(
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

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(finalize_buffered_response_preserves_request_version_and_persistence) {
    using ruvia::detail::finalizeBufferedRouteResponse;
    using ruvia::detail::Http1ConnectionDisposition;
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    const auto finalize = [&](std::string_view request) {
        HttpResponse response(std::pmr::new_delete_resource());
        response.status(200);
        std::size_t count = 0;
        const auto plan = finalizeBufferedRouteResponse(
            response,
            parser.parseMessage(request).connectionPlan,
            count,
            /*maxRequests=*/0);
        return std::pair(plan.disposition(), std::string(response.header("Connection")));
    };

    // RFC 9112 §9.3: a kept-alive HTTP/1.0 response MUST advertise keep-alive,
    // otherwise the client (which defaults to close) never reuses the connection.
    const auto http10Reuse = finalize(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    RUVIA_CHECK(http10Reuse.first == Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK_EQ(http10Reuse.second, std::string("keep-alive"));
    // HTTP/1.1 is persistent by default -> no Connection header needed.
    const auto http11Reuse = finalize(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(http11Reuse.first == Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK_EQ(http11Reuse.second, std::string(""));
    // A non-kept-alive response is closed regardless of version.
    RUVIA_CHECK_EQ(
        finalize("GET / HTTP/1.0\r\n\r\n").second,
        std::string("close"));
    RUVIA_CHECK_EQ(
        finalize("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").second,
        std::string("close"));
}

RUVIA_TEST(http1_buffered_response_plan_owns_request_version_and_length) {
    using ruvia::detail::Http1ServerRequestParser;
    using ruvia::detail::http1BufferedResponsePlan;
    using ruvia::detail::http1FinalizeResponseConnection;
    using ruvia::detail::httpBufferedResponseWritePlan;

    Http1ServerRequestParser parser;
    const auto emitFor = [&](std::string_view request) {
        HttpResponse response(std::pmr::new_delete_resource());
        response.setBodyCopy("hello");
        const auto connectionPlan = http1FinalizeResponseConnection(
            response,
            parser.parseMessage(request).connectionPlan);
        const auto responsePlan = http1BufferedResponsePlan(
            httpBufferedResponseWritePlan(HttpKnownMethod::kGet, response),
            connectionPlan);
        RUVIA_CHECK_EQ(
            responsePlan.headPlan().buffered()->contentLength(),
            responsePlan.writePlan().contentLength());
        return std::pair(
            emitHead(response, responsePlan.headPlan()),
            responsePlan.headPlan().protocolVersion());
    };

    const auto [http10Head, http10Version] = emitFor(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    RUVIA_CHECK(http10Version == ruvia::HttpProtocolVersion::kHttp10);
    RUVIA_CHECK(http10Head.starts_with("HTTP/1.0 200 OK\r\n"));
    RUVIA_CHECK(
        http10Head.find("Content-Length: 5\r\n") != std::string::npos);
    RUVIA_CHECK(
        http10Head.find("Connection: keep-alive\r\n") !=
        std::string::npos);

    const auto [http11Head, http11Version] = emitFor(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(http11Version == ruvia::HttpProtocolVersion::kHttp11);
    RUVIA_CHECK(http11Head.starts_with("HTTP/1.1 200 OK\r\n"));
    RUVIA_CHECK(http11Head.find("Connection:") == std::string::npos);
}

RUVIA_TEST(http1_response_head_rejects_status_plan_mismatch) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(207);
    response.setBodyCopy("planned");
    const auto plan = http1BufferedResponsePlan(
        ruvia::detail::httpBufferedResponseWritePlan(
            HttpKnownMethod::kGet,
            response),
        connectionPlanFor(ruvia::HttpProtocolVersion::kHttp11));

    response.status(208);
    RUVIA_CHECK(throwsInvalid([&] {
        (void)emitHead(response, plan.headPlan());
    }));
    RUVIA_CHECK_EQ(plan.writePlan().responseStatus(), std::uint16_t{207});
    RUVIA_CHECK_EQ(
        plan.headPlan().bodyPlan().responseStatus(),
        std::uint16_t{207});
}

RUVIA_TEST(http1_protocol_finalizer_returns_the_authoritative_reuse_verdict) {
    using ruvia::detail::Http1ConnectionDisposition;
    using ruvia::detail::Http1ServerRequestParser;
    using ruvia::detail::http1FinalizeResponseConnection;

    Http1ServerRequestParser parser;

    HttpResponse http10(std::pmr::new_delete_resource());
    const auto http10Plan = http1FinalizeResponseConnection(
        http10,
        parser.parseMessage(
            "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").connectionPlan);
    RUVIA_CHECK(http10Plan.disposition() == Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK_EQ(std::string(http10.header("Connection")), std::string("keep-alive"));

    HttpResponse http10Upgrade(std::pmr::new_delete_resource());
    http10Upgrade.header("Connection", "upgrade");
    const auto http10UpgradePlan = http1FinalizeResponseConnection(
        http10Upgrade,
        parser.parseMessage(
            "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").connectionPlan);
    RUVIA_CHECK(
        http10UpgradePlan.disposition() == Http1ConnectionDisposition::kReuse);
    const auto http10UpgradeHead = emitBufferedHead(http10Upgrade);
    RUVIA_CHECK(
        http10UpgradeHead.find("Connection: upgrade\r\n") != std::string::npos);
    RUVIA_CHECK(
        http10UpgradeHead.find("Connection: keep-alive\r\n") != std::string::npos);

    HttpResponse applicationClose(std::pmr::new_delete_resource());
    applicationClose.header("Connection", "upgrade");
    applicationClose.header(
        "Connection",
        "close",
        HttpResponse::HeaderOptions{.append = true});
    const auto applicationClosePlan = http1FinalizeResponseConnection(
        applicationClose,
        parser.parseMessage(
            "GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan);
    RUVIA_CHECK(
        applicationClosePlan.disposition() == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(std::string(applicationClose.header("Connection")), std::string("close"));
    const auto applicationCloseHead = emitBufferedHead(applicationClose);
    RUVIA_CHECK_EQ(
        countOccurrences(applicationCloseHead, "Connection: "),
        std::size_t{1});

    HttpResponse runtimeClose(std::pmr::new_delete_resource());
    const auto runtimeClosePlan = http1FinalizeResponseConnection(
        runtimeClose,
        parser.parseMessage(
            "GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan.requireClose());
    RUVIA_CHECK(runtimeClosePlan.disposition() == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(std::string(runtimeClose.header("Connection")), std::string("close"));
}

RUVIA_TEST(http1_protocol_finalizer_generates_upgrade_pairing) {
    using ruvia::detail::Http1ServerRequestParser;
    using ruvia::detail::http1FinalizeResponseConnection;

    Http1ServerRequestParser parser;
    const auto requestPlan = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan;
    HttpResponse unpaired(std::pmr::new_delete_resource());
    unpaired.status(426);
    unpaired.header("Upgrade", "websocket");
    RUVIA_CHECK(
        http1FinalizeResponseConnection(unpaired, requestPlan).disposition() ==
        ruvia::detail::Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK_EQ(
        std::string(unpaired.header("Connection")),
        std::string("Upgrade"));

    HttpResponse closing(std::pmr::new_delete_resource());
    closing.header("Upgrade", "websocket");
    RUVIA_CHECK(
        http1FinalizeResponseConnection(
            closing,
            requestPlan.requireClose()).disposition() ==
        ruvia::detail::Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(
        std::string(closing.header("Connection")),
        std::string("close, Upgrade"));
    RUVIA_CHECK_EQ(
        std::string(closing.header("Upgrade")),
        std::string("websocket"));
}

RUVIA_TEST(http1_protocol_finalizer_rejects_upgrade_required_without_protocol) {
    using ruvia::detail::Http1ServerRequestParser;
    using ruvia::detail::http1FinalizeResponseConnection;

    Http1ServerRequestParser parser;
    const auto requestPlan = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan;

    HttpResponse missingUpgrade(std::pmr::new_delete_resource());
    missingUpgrade.status(426);
    RUVIA_CHECK(throwsInvalid([&] {
        (void)http1FinalizeResponseConnection(missingUpgrade, requestPlan);
    }));
    RUVIA_CHECK(missingUpgrade.header("Connection").empty());
}

RUVIA_TEST(http1_buffered_request_limit_closes_the_typed_connection_plan) {
    using ruvia::detail::finalizeBufferedRouteResponse;
    using ruvia::detail::Http1ConnectionDisposition;
    using ruvia::detail::Http1ServerRequestParser;

    HttpResponse response(std::pmr::new_delete_resource());
    Http1ServerRequestParser parser;
    std::size_t requestCount = 4;
    const auto connectionPlan = finalizeBufferedRouteResponse(
        response,
        parser.parseMessage(
            "GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan,
        requestCount,
        /*maxRequests=*/5);
    RUVIA_CHECK_EQ(requestCount, std::size_t{5});
    RUVIA_CHECK(connectionPlan.disposition() == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(std::string(response.header("Connection")), std::string("close"));
}

RUVIA_TEST(http1_stream_plan_receives_the_next_response_close_policy) {
    using ruvia::detail::Http1ServerClosePolicy;
    using ruvia::detail::nextHttp1ResponseClosePolicy;

    RUVIA_CHECK(
        nextHttp1ResponseClosePolicy(0, 0) ==
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(
        nextHttp1ResponseClosePolicy(3, 5) ==
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(
        nextHttp1ResponseClosePolicy(4, 5) ==
        Http1ServerClosePolicy::kCloseAfterResponse);
}

RUVIA_TEST(http1_body_completion_tightens_without_losing_protocol_version) {
    using ruvia::detail::finalizeBodyRouteResponse;
    using ruvia::detail::Http1ConnectionDisposition;
    using ruvia::detail::Http1RequestBodyConsumption;
    using ruvia::detail::Http1ServerRequestParser;

    Http1ServerRequestParser parser;
    constexpr std::string_view request =
        "POST / HTTP/1.0\r\nConnection: keep-alive\r\nContent-Length: 1\r\n\r\nx";

    HttpResponse completeResponse(std::pmr::new_delete_resource());
    completeResponse.setBodyCopy("response");
    std::size_t completeRequestCount = 0;
    const auto completePlan = finalizeBodyRouteResponse(
        completeResponse,
        parser.parseMessage(request).connectionPlan,
        completeRequestCount,
        /*maxRequests=*/0,
        Http1RequestBodyConsumption::kComplete);
    RUVIA_CHECK_EQ(completeRequestCount, std::size_t{1});
    RUVIA_CHECK(completePlan.disposition() == Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(
        completePlan.protocolVersion() ==
        ruvia::HttpProtocolVersion::kHttp10);
    RUVIA_CHECK_EQ(
        std::string(completeResponse.header("Connection")),
        std::string("keep-alive"));

    HttpResponse incompleteResponse(std::pmr::new_delete_resource());
    incompleteResponse.setBodyCopy("response");
    std::size_t incompleteRequestCount = 0;
    const auto incompletePlan = finalizeBodyRouteResponse(
        incompleteResponse,
        parser.parseMessage(request).connectionPlan,
        incompleteRequestCount,
        /*maxRequests=*/0,
        Http1RequestBodyConsumption::kIncomplete);
    RUVIA_CHECK_EQ(incompleteRequestCount, std::size_t{1});
    RUVIA_CHECK(incompletePlan.disposition() == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK(
        incompletePlan.protocolVersion() ==
        ruvia::HttpProtocolVersion::kHttp10);
    RUVIA_CHECK_EQ(
        std::string(incompleteResponse.header("Connection")),
        std::string("close"));
}

RUVIA_TEST(response_head_emits_well_formed_normal) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(200);
    response.header("X-Foo", "bar");
    response.setBodyCopy("hello");
    const auto head = emitBufferedHead(response);

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    RUVIA_CHECK(head.find("X-Foo: bar\r\n") != std::string::npos);
    RUVIA_CHECK(head.find("Server:") == std::string::npos);                 // product policy is explicit
    RUVIA_CHECK(head.find("Date: ") != std::string::npos);                   // auto-injected
    RUVIA_CHECK(head.find("Content-Length: 5\r\n") != std::string::npos);    // auto, body size
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));                                  // blank-line terminator
}

RUVIA_TEST(response_head_extension_status_uses_an_empty_reason_phrase) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(299);
    const auto head = emitBufferedHead(response);

    // RFC 9112 section 4 keeps the SP before the optional reason-phrase.
    // An unregistered status must not be mislabeled as a generic client error.
    RUVIA_CHECK(head.starts_with("HTTP/1.1 299 \r\n"));
    RUVIA_CHECK(head.find("Bad Request") == std::string::npos);
}

RUVIA_TEST(response_head_preserves_explicit_server_and_does_not_duplicate_date) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.header("Server", "custom");
    response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.setBodyCopy("x");
    const auto head = emitBufferedHead(response);

    RUVIA_CHECK(head.find("Server: custom\r\n") != std::string::npos);
    RUVIA_CHECK_EQ(countOccurrences(head, "Server: "), std::size_t{1});
    RUVIA_CHECK_EQ(countOccurrences(head, "Date: "), std::size_t{1});   // exactly one Date
}

RUVIA_TEST(response_head_suppresses_auto_content_length) {
    // A streaming/chunked writer owns framing itself. Caller-provided framing is
    // replaced by one canonical chunked field and no Content-Length survives.
    HttpResponse response(std::pmr::new_delete_resource());
    response.setBodyCopy("hello");
    response.header("Transfer-Encoding", "gzip, chunked");
    response.header("Content-Length", "999");
    const auto head = emitChunkedStreamHead(response);
    RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);
    RUVIA_CHECK(head.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    RUVIA_CHECK(head.find("gzip") == std::string::npos);
    RUVIA_CHECK_EQ(
        countOccurrences(head, "Transfer-Encoding: "),
        std::size_t{1});
}

RUVIA_TEST(response_head_close_delimited_stream_rejects_declared_framing) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.setBodyCopy("streamed");
    response.header("Transfer-Encoding", "chunked");
    response.header("Content-Length", "8");

    const auto head = emitCloseDelimitedStreamHead(response);
    RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string::npos);
    RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);

    // A HEAD response has no payload and may retain representation length
    // metadata, but HTTP/1.0 still cannot carry Transfer-Encoding.
    const auto metadataHead = emitCloseDelimitedStreamHead(
        response, HttpKnownMethod::kHead);
    RUVIA_CHECK(metadataHead.find("Transfer-Encoding:") == std::string::npos);
    RUVIA_CHECK(metadataHead.find("Content-Length: 8\r\n") != std::string::npos);

    HttpResponse notModified(std::pmr::new_delete_resource());
    notModified.status(304);
    notModified.header("Transfer-Encoding", "chunked");
    notModified.header("Content-Length", "123");
    const auto notModifiedHead = emitCloseDelimitedStreamHead(notModified);
    RUVIA_CHECK(notModifiedHead.find("Transfer-Encoding:") == std::string::npos);
    RUVIA_CHECK(
        notModifiedHead.find("Content-Length: 123\r\n") !=
        std::string::npos);
}

RUVIA_TEST(response_head_bodyless_status_omits_auto_content_length) {
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(204);
    const auto head = emitBufferedHead(response);
    RUVIA_CHECK(head.starts_with("HTTP/1.1 204 No Content\r\n"));
    RUVIA_CHECK(head.find("Content-Length:") == std::string::npos);
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}

RUVIA_TEST(response_head_reset_content_canonicalizes_zero_length) {
    // RFC 9110 §15.3.6 forbids 205 content. Even if the application supplies a
    // body and contradictory framing, both buffered and streaming head emission
    // must suppress it and retain an unambiguous persistent HTTP/1 message.
    for (const bool streaming : {false, true}) {
        HttpResponse response(std::pmr::new_delete_resource());
        response.status(205);
        response.setBodyCopy("must-not-be-sent");
        response.header("Content-Length", "16");
        response.header("Transfer-Encoding", "chunked");
        const auto head = streaming
            ? emitChunkedStreamHead(response)
            : emitBufferedHead(response);
        RUVIA_CHECK(head.starts_with("HTTP/1.1 205 Reset Content\r\n"));
        RUVIA_CHECK_EQ(countOccurrences(head, "Content-Length: "), std::size_t{1});
        RUVIA_CHECK(head.find("Content-Length: 0\r\n") != std::string::npos);
        RUVIA_CHECK(head.find("Content-Length: 16\r\n") == std::string::npos);
        RUVIA_CHECK(head.find("Transfer-Encoding:") == std::string::npos);
        RUVIA_CHECK(head.ends_with("\r\n\r\n"));
    }
}

RUVIA_TEST(response_head_heap_spill_preserves_full_output) {
    // Force the emitted head well past the 512-byte stack buffer so the heap
    // (reserveAdditional) emit path runs. Every header must survive intact and
    // the precomputed size bound must not undercount -- an undercount would let
    // the unchecked raw stack sink overflow or the output truncate.
    HttpResponse response(std::pmr::new_delete_resource());
    response.status(200);
    const std::string big(200, 'v');
    for (int i = 0; i < 10; ++i) {
        response.header("X-Pad-" + std::to_string(i), big);
    }
    response.setBodyCopy("body");
    const auto head = emitBufferedHead(response);

    RUVIA_CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    for (int i = 0; i < 10; ++i) {
        RUVIA_CHECK(head.find("X-Pad-" + std::to_string(i) + ": " + big + "\r\n") != std::string::npos);
    }
    RUVIA_CHECK(head.find("Content-Length: 4\r\n") != std::string::npos);
    RUVIA_CHECK(head.ends_with("\r\n\r\n"));
}
