#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"
#include "ruvia/web/detail/server/HttpServerAutoHttps.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"

namespace {

using ruvia::detail::appendHttpsPort;
using ruvia::detail::contentLengthExceedsLimit;
using ruvia::detail::hostWithoutExplicitPort;
using ruvia::detail::Http1ConnectionDisposition;
using ruvia::detail::Http1RequestBodyPlan;
using ruvia::detail::Http1ResponseConnectionSignal;
using ruvia::detail::Http1ServerClosePolicy;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::http1PlanResponseStream;
using ruvia::detail::prepareHttp1ResponseStreamHead;
using ruvia::detail::ResponseTrailerIntent;
using ruvia::detail::ResponseStreamFraming;
using ruvia::detail::ResponseStreamHeadDisposition;
using ruvia::detail::ResponseStreamKind;
using ruvia::detail::ResponseStreamTrailerFraming;
using ruvia::detail::HttpServerExpectationAction;

std::string withHttpsPort(std::string_view base, std::uint16_t port) {
    std::pmr::string location(std::pmr::get_default_resource());
    location.assign(base.data(), base.size());
    appendHttpsPort(location, port);
    return std::string(location.data(), location.size());
}

}  // namespace

RUVIA_TEST(request_state_content_length_exceeds_limit) {
    const auto over = Http1RequestBodyPlan::knownLength(101);
    const auto exact = Http1RequestBodyPlan::knownLength(100);
    const auto unlimited = Http1RequestBodyPlan::knownLength(1'000'000);
    const auto chunked = Http1RequestBodyPlan::chunked();
    RUVIA_CHECK(contentLengthExceedsLimit(over, 100));
    RUVIA_CHECK(!contentLengthExceedsLimit(exact, 100));
    RUVIA_CHECK(!contentLengthExceedsLimit(unlimited, 0));
    RUVIA_CHECK(!contentLengthExceedsLimit(chunked, 1));
}

RUVIA_TEST(request_state_keep_alive_by_connection_header) {
    Http1ServerRequestParser parser;
    RUVIA_CHECK(parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").connectionPlan.disposition() ==
        Http1ConnectionDisposition::kClose);
    RUVIA_CHECK(parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n").connectionPlan.disposition() ==
        Http1ConnectionDisposition::kReuse);
}

RUVIA_TEST(request_state_keep_alive_default_by_version) {
    Http1ServerRequestParser parser;
    // HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close.
    const auto http11 = parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n").connectionPlan;
    RUVIA_CHECK(http11.disposition() ==
        Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(
        http11.responseSignal() == Http1ResponseConnectionSignal::kImplicitPersistence);
    const auto http10Default = parser.parseMessage(
        "GET / HTTP/1.0\r\n\r\n").connectionPlan;
    RUVIA_CHECK(http10Default.disposition() ==
        Http1ConnectionDisposition::kClose);
    // HTTP/1.0 can still opt in with an explicit keep-alive.
    const auto http10KeepAlive = parser.parseMessage(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").connectionPlan;
    RUVIA_CHECK(http10KeepAlive.disposition() ==
        Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(
        http10KeepAlive.responseSignal() ==
        Http1ResponseConnectionSignal::kExplicitKeepAlive);

    // The full-message convenience parser clears request views while waiting
    // for missing body bytes, but the header-derived connection contract must
    // survive so a runtime never has to reconstruct it from a cleared version.
    const auto incompleteHttp10 = parser.parseMessage(
        "POST / HTTP/1.0\r\nConnection: keep-alive\r\nContent-Length: 1\r\n\r\n");
    RUVIA_CHECK(
        incompleteHttp10.phase() ==
        ruvia::detail::Http1ServerRequestParsePhase::kNeedRequestBody);
    RUVIA_CHECK(
        incompleteHttp10.connectionPlan.disposition() ==
        Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(
        incompleteHttp10.connectionPlan.responseSignal() ==
        Http1ResponseConnectionSignal::kExplicitKeepAlive);
}

RUVIA_TEST(request_state_wants_continue) {
    Http1ServerRequestParser parser;
    const auto empty = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(empty.bodyPlan.expectations().has100Continue());
    RUVIA_CHECK(
        empty.bodyPlan.expectationAction() ==
        HttpServerExpectationAction::kNone);

    const auto body = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 1\r\n\r\nx");
    RUVIA_CHECK(body.bodyPlan.expectations().has100Continue());
    RUVIA_CHECK(
        body.bodyPlan.expectationAction() ==
        HttpServerExpectationAction::kSend100Continue);

    RUVIA_CHECK(!parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        .bodyPlan.expectations().has100Continue());
    // A 100-continue expectation from an HTTP/1.0 client MUST be ignored: RFC 9110
    // §15.2 forbids sending any 1xx response to an HTTP/1.0 client, which would
    // misread the interim 100 as the final response.
    RUVIA_CHECK(parser.parseMessage(
        "POST / HTTP/1.0\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n")
        .bodyPlan.expectationAction() ==
        HttpServerExpectationAction::kNone);

    const auto extension = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue, custom-feature\r\n"
        "Content-Length: 1\r\n\r\nx");
    RUVIA_CHECK(extension.messageReady());
    RUVIA_CHECK(
        extension.bodyPlan.expectationAction() ==
        HttpServerExpectationAction::kUnsupported);
}

RUVIA_TEST(http1_response_stream_plan_owns_version_body_and_persistence_semantics) {
    Http1ServerRequestParser parser;

    const auto http11 = http1PlanResponseStream(
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n"),
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(http11.framing() == ResponseStreamFraming::kHttp1Chunked);
    RUVIA_CHECK(
        http11.requestConnectionPlan().disposition() == Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(http11.closePolicy() == Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(
        http11.requestConnectionPlan().responseSignal() ==
        Http1ResponseConnectionSignal::kImplicitPersistence);

    const auto limitedHttp11 = http1PlanResponseStream(
        parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n"),
        Http1ServerClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK(limitedHttp11.framing() == ResponseStreamFraming::kHttp1Chunked);
    RUVIA_CHECK(
        limitedHttp11.requestConnectionPlan().disposition() ==
        Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(limitedHttp11.closePolicy() == Http1ServerClosePolicy::kCloseAfterResponse);

    const auto requestBodyPending = http1PlanResponseStream(
        parser.parseMessage("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n"),
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(requestBodyPending.framing() == ResponseStreamFraming::kHttp1Chunked);
    RUVIA_CHECK(
        requestBodyPending.requestConnectionPlan().disposition() ==
        Http1ConnectionDisposition::kClose);

    const auto http10KeepAlive = http1PlanResponseStream(
        parser.parseMessage("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"),
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(http10KeepAlive.framing() == ResponseStreamFraming::kHttp1CloseDelimited);
    RUVIA_CHECK(
        http10KeepAlive.requestConnectionPlan().disposition() ==
        Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(http10KeepAlive.closePolicy() == Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(
        http10KeepAlive.requestConnectionPlan().responseSignal() ==
        Http1ResponseConnectionSignal::kExplicitKeepAlive);
}

RUVIA_TEST(http1_prepared_stream_head_binds_wire_signal_to_final_connection_disposition) {
    Http1ServerRequestParser parser;
    const auto prepare = [&](
                             std::string_view request,
                             Http1ServerClosePolicy closePolicy,
                             std::string_view responseConnection) {
        const auto plan = http1PlanResponseStream(parser.parseMessage(request), closePolicy);
        ruvia::HttpResponse response(std::pmr::get_default_resource());
        response.status(200);
        if (!responseConnection.empty()) {
            response.header("Connection", responseConnection);
        }
        auto prepared = prepareHttp1ResponseStreamHead(
            std::move(response),
            ResponseStreamKind::kGeneric,
            plan,
            ResponseTrailerIntent::kNone);
        return std::tuple(
            prepared.connectionPlan().disposition(),
            std::string(prepared.response().header("Connection")),
            std::string(prepared.response().header("Transfer-Encoding")));
    };

    constexpr std::string_view http11 = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";

    // The normal HTTP/1.1 stream is reusable and self-delimited by chunked
    // framing, so no connection-specific signal is needed.
    const auto reusable = prepare(
        http11, Http1ServerClosePolicy::kAllowReuse, {});
    RUVIA_CHECK(std::get<0>(reusable) == Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(std::get<1>(reusable).empty());
    RUVIA_CHECK_EQ(std::get<2>(reusable), std::string("chunked"));

    // RFC 9112 9.6: once the response sends "close", the server MUST close and
    // MUST NOT process another request on this connection. The commit-time
    // result therefore tightens a reusable pre-commit plan to kClose.
    const auto applicationClose = prepare(
        http11, Http1ServerClosePolicy::kAllowReuse, "close");
    RUVIA_CHECK(std::get<0>(applicationClose) == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(std::get<1>(applicationClose), std::string("close"));

    // A runtime close policy is authoritative in the opposite direction: a
    // handler cannot announce keep-alive on a socket the session will close.
    const auto runtimeClose = prepare(
        http11, Http1ServerClosePolicy::kCloseAfterResponse, "keep-alive");
    RUVIA_CHECK(std::get<0>(runtimeClose) == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(std::get<1>(runtimeClose), std::string("close"));

    // HTTP/1.0 response streams are close-delimited even when the request opted
    // into keep-alive, so the prepared result canonicalizes the final signal.
    const auto closeDelimited = prepare(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        Http1ServerClosePolicy::kAllowReuse,
        "keep-alive");
    RUVIA_CHECK(std::get<0>(closeDelimited) == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(std::get<1>(closeDelimited), std::string("close"));
    RUVIA_CHECK(std::get<2>(closeDelimited).empty());
}

RUVIA_TEST(http1_prepared_body_suppressed_stream_is_self_delimited) {
    Http1ServerRequestParser parser;
    const auto prepare = [&](
                             std::string_view request,
                             std::uint16_t status,
                             Http1ServerClosePolicy closePolicy) {
        const auto plan = http1PlanResponseStream(
            parser.parseMessage(request), closePolicy);
        ruvia::HttpResponse response(std::pmr::get_default_resource());
        response.status(status);
        return prepareHttp1ResponseStreamHead(
            std::move(response),
            ResponseStreamKind::kGeneric,
            plan,
            ResponseTrailerIntent::kNone);
    };

    auto http11 = prepare(
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
        205,
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(http11.commitPlan().bodyPlan().bodySuppressed());
    RUVIA_CHECK(
        http11.commitPlan().headDisposition() ==
        ResponseStreamHeadDisposition::kMessageEnded);
    RUVIA_CHECK(
        http11.commitPlan().trailerFraming() ==
        ResponseStreamTrailerFraming::kUnavailable);
    RUVIA_CHECK(http11.response().header("Transfer-Encoding").empty());
    RUVIA_CHECK(
        http11.connectionPlan().disposition() == Http1ConnectionDisposition::kReuse);

    auto http10 = prepare(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        205,
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(http10.commitPlan().bodyPlan().bodySuppressed());
    RUVIA_CHECK(http10.response().header("Transfer-Encoding").empty());
    RUVIA_CHECK(
        http10.connectionPlan().disposition() == Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK_EQ(
        std::string(http10.response().header("Connection")),
        std::string("keep-alive"));

    auto http10Head = prepare(
        "HEAD / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        200,
        Http1ServerClosePolicy::kAllowReuse);
    RUVIA_CHECK(http10Head.commitPlan().bodyPlan().bodySuppressed());
    RUVIA_CHECK(
        http10Head.connectionPlan().disposition() == Http1ConnectionDisposition::kReuse);

    auto limitedHttp10 = prepare(
        "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
        205,
        Http1ServerClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK(limitedHttp10.commitPlan().bodyPlan().bodySuppressed());
    RUVIA_CHECK(
        limitedHttp10.connectionPlan().disposition() == Http1ConnectionDisposition::kClose);
    RUVIA_CHECK_EQ(
        std::string(limitedHttp10.response().header("Connection")),
        std::string("close"));
}

RUVIA_TEST(http1_stream_commit_plan_exposes_exact_trailer_capability) {
    Http1ServerRequestParser parser;
    const auto prepare = [&parser](std::string_view request) {
        const auto plan = http1PlanResponseStream(
            parser.parseMessage(request),
            Http1ServerClosePolicy::kAllowReuse);
        ruvia::HttpResponse response(std::pmr::get_default_resource());
        response.status(200);
        return prepareHttp1ResponseStreamHead(
            std::move(response),
            ResponseStreamKind::kGeneric,
            plan,
            ResponseTrailerIntent::kPresent);
    };

    const auto http11 = prepare("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(
        http11.commitPlan().headDisposition() ==
        ResponseStreamHeadDisposition::kBodyOpen);
    RUVIA_CHECK(
        http11.commitPlan().trailerFraming() ==
        ResponseStreamTrailerFraming::kHttp1Chunked);

    // HTTP/1.0 response content is close-delimited, so a trailer section has no
    // legal framing. The Web sink consumes this typed verdict and fails before
    // writing the response head instead of accepting and later dropping fields.
    const auto http10 = prepare("GET / HTTP/1.0\r\n\r\n");
    RUVIA_CHECK(
        http10.commitPlan().headDisposition() ==
        ResponseStreamHeadDisposition::kBodyOpen);
    RUVIA_CHECK(
        http10.commitPlan().trailerFraming() ==
        ResponseStreamTrailerFraming::kUnavailable);

    // RFC 9112 §6.3 terminates HEAD at the initial field section. Even though
    // Transfer-Encoding can describe the hypothetical GET representation, it
    // cannot make a trailer section legal on this response.
    const auto head = prepare("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n");
    RUVIA_CHECK(
        head.commitPlan().headDisposition() ==
        ResponseStreamHeadDisposition::kMessageEnded);
    RUVIA_CHECK(
        head.commitPlan().trailerFraming() ==
        ResponseStreamTrailerFraming::kUnavailable);
}

RUVIA_TEST(auto_https_host_without_explicit_port_strips_port_bracket_aware) {
    // The HTTP->HTTPS redirect reuses the request Host but drops any explicit port
    // (the HTTPS port is appended separately). A reg-name loses its ":port".
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("example.com:80"), std::string_view("example.com"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("example.com:8080"), std::string_view("example.com"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("example.com"), std::string_view("example.com"));

    // IPv6 must be handled bracket-aware: only the ":port" AFTER "]" is stripped, the
    // colons inside the literal are kept (a naive find(':') would corrupt it).
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[::1]:80"), std::string_view("[::1]"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[::1]"), std::string_view("[::1]"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[2001:db8::1]:443"), std::string_view("[2001:db8::1]"));
    // An unterminated bracket is returned unchanged rather than mangled.
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[::1"), std::string_view("[::1"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort(""), std::string_view(""));
}

RUVIA_TEST(auto_https_append_port_omits_default) {
    // 443 is the default HTTPS port: it must NOT appear in the redirect URL.
    RUVIA_CHECK_EQ(withHttpsPort("https://example.com", 443), std::string("https://example.com"));
    // Any other port is appended explicitly.
    RUVIA_CHECK_EQ(withHttpsPort("https://example.com", 8443), std::string("https://example.com:8443"));
    RUVIA_CHECK_EQ(withHttpsPort("https://example.com", 80), std::string("https://example.com:80"));
}

RUVIA_TEST(auto_https_redirect_response_is_private_and_well_formed) {
    Http1ServerRequestParser parser;
    // Host carries the cleartext port, which must be dropped and replaced by the
    // (default, so omitted) HTTPS port; the path and query are preserved.
    const std::string request = "GET /a/b?x=1 HTTP/1.1\r\nHost: example.com:80\r\n\r\n";
    const auto parsed = parser.parseMessage(request);
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    const auto response = ruvia::detail::makeAutoHttpsRedirectResponse(parsed.request, memory, 443);

    RUVIA_CHECK_EQ(response.status(), std::uint16_t{308});
    RUVIA_CHECK_EQ(std::string(response.header("Location")), std::string("https://example.com/a/b?x=1"));
    // The Location is Host-derived, so the redirect must be private: a shared cache
    // must not store one Host's redirect and replay it for another.
    RUVIA_CHECK_EQ(std::string(response.header("Cache-Control")), std::string("private"));
    RUVIA_CHECK_EQ(std::string(response.header("Connection")), std::string("close"));
}
