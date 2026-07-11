#include "test_harness.h"

#include <array>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpClientRedirect.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/detail/client/HttpClientContentEncoding.h"

namespace {

using ruvia::HttpClientResponse;
using ruvia::Http1ClientConnectionDisposition;
using ruvia::Http1ClientRequestClosePolicy;
using ruvia::Http1ClientRequestContentDisposition;
using ruvia::Http1ClientRequestContentCompletionStatus;
using ruvia::Http1ClientRequestContentSignal;
using ruvia::Http1ClientRequestWirePolicy;
using ruvia::Http1ClientResponseBodyMode;
using ruvia::Http1ClientResponseParseError;
using ruvia::Http1ClientResponseParseKind;
using ruvia::Http1ClientResponseParseResult;
using ruvia::Http1ClientResponseParser;
using ruvia::Http1ParsedClientResponseHead;
using ruvia::HttpProtocolVersion;
using ruvia::isValidHttpClientOriginTarget;

Http1ClientResponseParseResult parseWire(
    std::string_view method,
    std::string_view wire,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {},
    std::pmr::memory_resource* resource = nullptr) {
    std::array<char, 2048> requestHead;
    const auto origin = ruvia::HttpOrigin::https("example.test");
    ruvia::HttpClientRequest request;
    request.method = method;
    request.headers = requestHeaders;
    const auto preparedResult = method == "CONNECT"
        ? ruvia::Http1ClientRequestWriter().prepareConnect(
              origin,
              requestHeaders,
              requestHead,
              Http1ClientRequestWirePolicy::withoutExpectation(closePolicy))
        : ruvia::Http1ClientRequestWriter().prepare(
              origin,
              request,
              requestHead,
              Http1ClientRequestWirePolicy::withoutExpectation(closePolicy));
    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        throw std::runtime_error("test request could not be prepared");
    }
    auto parser = Http1ClientResponseParser(*prepared, resource);
    return parser.parse(wire);
}

Http1ClientResponseParseResult parseResult(
    std::string_view method,
    std::string_view headerSection,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {},
    std::pmr::memory_resource* resource = nullptr) {
    std::string wire(headerSection);
    wire.append("\r\n\r\n");
    return parseWire(method, wire, closePolicy, requestHeaders, resource);
}

Http1ParsedClientResponseHead parseHead(
    std::string_view method,
    std::string_view headerSection,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    auto result = parseResult(
        method, headerSection, closePolicy, requestHeaders);
    auto* parsed = result.parsed();
    if (parsed == nullptr) {
        throw std::runtime_error("test expected a parsed HTTP/1 response head");
    }
    return std::move(*parsed);
}

struct ParsedResponse final {
    HttpClientResponse response;
};

ParsedResponse parseResponse(
    std::string_view method,
    std::string_view headerSection) {
    auto head = parseHead(method, headerSection);
    return ParsedResponse{std::move(head).takeResponse()};
}

bool parseFails(
    std::string_view method,
    std::string_view headerSection,
    Http1ClientRequestClosePolicy closePolicy =
        Http1ClientRequestClosePolicy::kAllowReuse,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    return parseResult(
        method, headerSection, closePolicy, requestHeaders).failure() != nullptr;
}

Http1ClientResponseParseError parseFailureError(
    std::string_view method,
    std::string_view headerSection,
    std::span<const ruvia::HttpHeaderView> requestHeaders = {}) {
    const auto result = parseResult(
        method,
        headerSection,
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders);
    const auto* failure = result.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test expected an HTTP/1 response parse failure");
    }
    return failure->error();
}

class RejectingMemoryResource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t, std::size_t) override {
        throw std::bad_alloc();
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {}

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

}  // namespace

RUVIA_TEST(http_client_origin_target_validation) {
    RUVIA_CHECK(isValidHttpClientOriginTarget("/ok%2F?q=%7B%7D"));
    RUVIA_CHECK(isValidHttpClientOriginTarget("*"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget(""));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("relative"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad#fragment"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad\\path"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%zz"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%2"));
}

RUVIA_TEST(http_client_response_plan_owns_content_length_framing) {
    constexpr std::string_view header =
        "HTTP/1.1 200 OK\r\nContent-Length: 5";
    const auto head = parseHead("GET", header);
    const auto& plan = head.plan();
    RUVIA_CHECK(plan.mode() == Http1ClientResponseBodyMode::kContentLength);
    RUVIA_CHECK(plan.hasContentLength());
    RUVIA_CHECK_EQ(plan.contentLength(), std::size_t{5});
    RUVIA_CHECK(plan.requiresBodyConsumption());
    RUVIA_CHECK(plan.selfDelimited());
    RUVIA_CHECK(
        plan.connectionDisposition() == Http1ClientConnectionDisposition::kReuse);
    RUVIA_CHECK_EQ(head.consumedBytes(), header.size() + 4);

    const auto empty = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    RUVIA_CHECK(empty.plan().hasContentLength());
    RUVIA_CHECK(!empty.plan().requiresBodyConsumption());
}

RUVIA_TEST(http_client_content_length_combined_and_repeated_equal_values) {
    const auto combined = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 5");
    RUVIA_CHECK(combined.plan().hasContentLength());
    RUVIA_CHECK_EQ(combined.plan().contentLength(), std::size_t{5});

    const auto repeated = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    RUVIA_CHECK_EQ(repeated.plan().contentLength(), std::size_t{5});

    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 6"));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

RUVIA_TEST(http_client_response_plan_owns_chunked_framing_and_reuse) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked");
    const auto& plan = head.plan();
    RUVIA_CHECK(plan.mode() == Http1ClientResponseBodyMode::kChunked);
    RUVIA_CHECK(plan.isChunked());
    RUVIA_CHECK(plan.requiresBodyConsumption());
    RUVIA_CHECK(plan.selfDelimited());
    RUVIA_CHECK(plan.transferCodings().empty());
    RUVIA_CHECK(
        plan.connectionDisposition() == Http1ClientConnectionDisposition::kReuse);
}

RUVIA_TEST(http_client_transfer_coding_before_final_chunked_is_typed) {
    const auto combined = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    RUVIA_CHECK(combined.plan().isChunked());
    RUVIA_CHECK_EQ(
        combined.plan().transferCodings().count,
        std::size_t{1});
    RUVIA_CHECK(
        combined.plan().transferCodings().values[0] ==
        ruvia::detail::HttpTransferCoding::kGzip);
    RUVIA_CHECK(
        combined.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kReuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    RUVIA_CHECK(split.plan().isChunked());
    RUVIA_CHECK(
        split.plan().transferCodings().values[0] ==
        ruvia::detail::HttpTransferCoding::kDeflate);
}

RUVIA_TEST(http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parseHead(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& plan = head.plan();
    RUVIA_CHECK(plan.isCloseDelimited());
    RUVIA_CHECK(!plan.selfDelimited());
    RUVIA_CHECK_EQ(plan.transferCodings().count, std::size_t{1});
    RUVIA_CHECK(
        plan.connectionDisposition() == Http1ClientConnectionDisposition::kClose);
}

RUVIA_TEST(http_client_rejects_invalid_or_unsupported_transfer_coding) {
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , chunked"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked"));
}

RUVIA_TEST(http_client_content_length_and_transfer_encoding_rejected_for_body) {
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked"));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5"));
}

RUVIA_TEST(http_client_no_body_precedence_ignores_framing_fields) {
    const auto head = parseHead(
        "HEAD",
        "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(head.plan().mode() == Http1ClientResponseBodyMode::kNone);
    RUVIA_CHECK(!head.plan().requiresBodyConsumption());
    RUVIA_CHECK(
        head.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kReuse);

    const auto notModified = parseHead(
        "GET",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(
        notModified.plan().mode() == Http1ClientResponseBodyMode::kNone);

    const auto noContent = parseHead(
        "GET",
        "HTTP/1.1 204 No Content\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(noContent.plan().mode() == Http1ClientResponseBodyMode::kNone);
}

RUVIA_TEST(http_client_205_uses_normal_http1_message_framing) {
    const auto zeroLength = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    RUVIA_CHECK(
        zeroLength.plan().mode() ==
        Http1ClientResponseBodyMode::kContentLength);
    RUVIA_CHECK(!zeroLength.plan().requiresBodyConsumption());
    RUVIA_CHECK(zeroLength.plan().selfDelimited());
    RUVIA_CHECK(
        zeroLength.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kReuse);

    const auto nonzeroLength = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3");
    RUVIA_CHECK_EQ(nonzeroLength.plan().contentLength(), std::size_t{3});
    RUVIA_CHECK(nonzeroLength.plan().requiresBodyConsumption());

    const auto chunked = parseHead(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    RUVIA_CHECK(chunked.plan().isChunked());
    RUVIA_CHECK(chunked.plan().requiresBodyConsumption());

    const auto unframed = parseHead(
        "GET", "HTTP/1.1 205 Reset Content");
    RUVIA_CHECK(unframed.plan().isCloseDelimited());
    RUVIA_CHECK(
        unframed.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kClose);
}

RUVIA_TEST(http_client_informational_response_awaits_final_response) {
    for (const auto status : {
             std::string_view("HTTP/1.1 100 Continue"),
             std::string_view("HTTP/1.1 103 Early Hints")}) {
        const auto head = parseHead("GET", status);
        RUVIA_CHECK(head.plan().mode() == Http1ClientResponseBodyMode::kNone);
        RUVIA_CHECK(
            head.plan().connectionDisposition() ==
            Http1ClientConnectionDisposition::kAwaitFinalResponse);
    }

    const auto ignoredFraming = parseHead(
        "GET",
        "HTTP/1.1 103 Early Hints\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(
        ignoredFraming.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kAwaitFinalResponse);
}

RUVIA_TEST(http_client_expect_continue_is_one_stateful_exchange_contract) {
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    std::array<char, 512> requestHead;
    const auto preparedResult = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        requestHead,
        Http1ClientRequestWirePolicy::expectContinue());
    const auto* prepared = preparedResult.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(
        prepared->contentPlan().disposition() ==
        Http1ClientRequestContentDisposition::kContinueGated);

    Http1ClientResponseParser parser(*prepared);
    auto earlyHints = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(earlyHints.parsed() != nullptr);
    if (earlyHints.parsed() != nullptr) {
        RUVIA_CHECK(
            earlyHints.parsed()->plan().requestContentSignal() ==
            Http1ClientRequestContentSignal::kNone);
        RUVIA_CHECK(
            earlyHints.parsed()->plan().connectionDisposition() ==
            Http1ClientConnectionDisposition::kAwaitFinalResponse);
    }

    auto continueResponse = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    if (continueResponse.parsed() != nullptr) {
        RUVIA_CHECK(
            continueResponse.parsed()->plan().requestContentSignal() ==
            Http1ClientRequestContentSignal::kContinue);
    }
    RUVIA_CHECK(
        parser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    RUVIA_CHECK(
        parser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kAlreadyComplete);

    auto finalResponse = parser.parse(
        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(finalResponse.parsed() != nullptr);
    if (finalResponse.parsed() != nullptr) {
        RUVIA_CHECK(
            finalResponse.parsed()->plan().requestContentSignal() ==
            Http1ClientRequestContentSignal::kExchangeComplete);
    }

    const auto afterFinal = parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    RUVIA_CHECK(afterFinal.failure() != nullptr);
    if (afterFinal.failure() != nullptr) {
        RUVIA_CHECK(
            afterFinal.failure()->error() ==
            Http1ClientResponseParseError::kExchangeComplete);
    }
    RUVIA_CHECK(
        parser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kExchangeTerminal);
}

RUVIA_TEST(http_client_upgrade_after_expect_requires_prior_continue) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.headers = upgradeHeaders;
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> rejectedHead;
    const auto rejectedPrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        rejectedHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(rejectedPrepared.prepared() != nullptr);
    if (rejectedPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser rejectedParser(*rejectedPrepared.prepared());
    const auto rejected = rejectedParser.parse(switching);
    RUVIA_CHECK(rejected.failure() != nullptr);
    if (rejected.failure() != nullptr) {
        RUVIA_CHECK(
            rejected.failure()->error() ==
            Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }
    const auto afterFailure = rejectedParser.parse(switching);
    RUVIA_CHECK(afterFailure.failure() != nullptr);
    if (afterFailure.failure() != nullptr) {
        RUVIA_CHECK(
            afterFailure.failure()->error() ==
            Http1ClientResponseParseError::kExchangeFailed);
    }

    std::array<char, 512> pendingHead;
    const auto pendingPrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        pendingHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(pendingPrepared.prepared() != nullptr);
    if (pendingPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser pendingParser(*pendingPrepared.prepared());
    const auto pendingContinue = pendingParser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(pendingContinue.parsed() != nullptr);
    const auto pendingUpgrade = pendingParser.parse(switching);
    RUVIA_CHECK(pendingUpgrade.failure() != nullptr);
    if (pendingUpgrade.failure() != nullptr) {
        RUVIA_CHECK(
            pendingUpgrade.failure()->error() ==
            Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> acceptedHead;
    const auto acceptedPrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"),
        request,
        acceptedHead,
        Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(acceptedPrepared.prepared() != nullptr);
    if (acceptedPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser acceptedParser(*acceptedPrepared.prepared());
    const auto continueResponse = acceptedParser.parse(
        "HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    RUVIA_CHECK(
        acceptedParser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto accepted = acceptedParser.parse(switching);
    RUVIA_CHECK(accepted.parsed() != nullptr);
    if (accepted.parsed() != nullptr) {
        RUVIA_CHECK(accepted.parsed()->plan().isUpgrade());
    }
}

RUVIA_TEST(http_client_upgrade_requires_complete_request_content) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequest request;
    request.method = "POST";
    request.headers = upgradeHeaders;
    request.content = ruvia::HttpClientRequestContent::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> incompleteHead;
    const auto incompletePrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"), request, incompleteHead);
    RUVIA_CHECK(incompletePrepared.prepared() != nullptr);
    if (incompletePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser incompleteParser(*incompletePrepared.prepared());
    const auto incomplete = incompleteParser.parse(switching);
    RUVIA_CHECK(incomplete.failure() != nullptr);
    if (incomplete.failure() != nullptr) {
        RUVIA_CHECK(
            incomplete.failure()->error() ==
            Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> completeHead;
    const auto completePrepared = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"), request, completeHead);
    RUVIA_CHECK(completePrepared.prepared() != nullptr);
    if (completePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser completeParser(*completePrepared.prepared());
    RUVIA_CHECK(
        completeParser.completeRequestContent() ==
        Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto complete = completeParser.parse(switching);
    RUVIA_CHECK(complete.parsed() != nullptr);
    if (complete.parsed() != nullptr) {
        RUVIA_CHECK(complete.parsed()->plan().isUpgrade());
    }
}

RUVIA_TEST(http_client_switching_protocols_is_a_typed_opaque_transition) {
    const ruvia::HttpHeaderView requestHeaders[] = {
        {"Connection", "keep-alive, Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto upgraded = parseHead(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket",
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders);
    RUVIA_CHECK(upgraded.response().status() == std::uint16_t{101});
    RUVIA_CHECK(upgraded.plan().mode() == Http1ClientResponseBodyMode::kOpaque);
    RUVIA_CHECK(upgraded.plan().isOpaque());
    RUVIA_CHECK(upgraded.plan().isUpgrade());
    RUVIA_CHECK(!upgraded.plan().isConnectTunnel());
    RUVIA_CHECK(!upgraded.plan().requiresBodyConsumption());
    RUVIA_CHECK(!upgraded.plan().selfDelimited());
    RUVIA_CHECK(
        upgraded.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kUpgrade);

    // Protocol names compare case-insensitively; versions remain exact tokens.
    const auto versioned = parseHead(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: irc/6.9",
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders);
    RUVIA_CHECK(versioned.plan().isUpgrade());
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: IRC/6.10",
        Http1ClientRequestClosePolicy::kAllowReuse,
        requestHeaders));
}

RUVIA_TEST(http_client_connection_fields_use_recipient_list_semantics) {
    const auto reusable = parseHead(
        "GET",
        "HTTP/1.0 200 OK\r\n"
        "Connection: , keep-alive,\r\nContent-Length: 0");
    RUVIA_CHECK(
        reusable.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kReuse);
    RUVIA_CHECK(
        reusable.response().protocolVersion() ==
        HttpProtocolVersion::kHttp10);

    RUVIA_CHECK(
        parseFailureError(
            "GET",
            "HTTP/1.1 200 OK\r\nConnection: close;invalid\r\n"
            "Content-Length: 0") ==
        Http1ClientResponseParseError::kInvalidConnection);
    RUVIA_CHECK(
        parseFailureError(
            "GET",
            "HTTP/1.1 200 OK\r\nUpgrade: websocket/\r\n"
            "Content-Length: 0") ==
        Http1ClientResponseParseError::kInvalidUpgrade);

    const ruvia::HttpHeaderView offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto upgraded = parseHead(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , websocket,",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered);
    RUVIA_CHECK(upgraded.plan().isUpgrade());
}

RUVIA_TEST(http_client_response_preserves_typed_protocol_version) {
    const auto http10 = parseHead(
        "GET", "HTTP/1.0 204 No Content");
    const auto http11 = parseHead(
        "GET", "HTTP/1.1 204 No Content");

    RUVIA_CHECK(
        http10.response().protocolVersion() ==
        HttpProtocolVersion::kHttp10);
    RUVIA_CHECK(
        http11.response().protocolVersion() ==
        HttpProtocolVersion::kHttp11);
}

RUVIA_TEST(http_client_switching_protocols_requires_wire_agreement) {
    const ruvia::HttpHeaderView offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const ruvia::HttpHeaderView closingOffer[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "websocket"},
    };
    constexpr std::string_view validResponse =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket";

    RUVIA_CHECK(parseFails("GET", validResponse));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: IRC",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\nContent-Length: 0",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Transfer-Encoding: chunked",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        "HTTP/1.0 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        Http1ClientRequestClosePolicy::kAllowReuse,
        offered));
    RUVIA_CHECK(parseFails(
        "GET",
        validResponse,
        Http1ClientRequestClosePolicy::kAllowReuse,
        closingOffer));
}

RUVIA_TEST(http_client_unframed_body_response_is_close_delimited) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().isCloseDelimited());
    RUVIA_CHECK(head.plan().requiresBodyConsumption());
    RUVIA_CHECK(
        head.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kClose);
}

RUVIA_TEST(http_client_response_plan_owns_version_and_connection_persistence) {
    const auto http10 = parseHead(
        "GET", "HTTP/1.0 200 OK\r\nContent-Length: 3");
    RUVIA_CHECK(http10.plan().selfDelimited());
    RUVIA_CHECK(
        http10.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kClose);

    const auto http10KeepAlive = parseHead(
        "GET",
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 3");
    RUVIA_CHECK(
        http10KeepAlive.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kReuse);

    const auto responseClose = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3");
    RUVIA_CHECK(
        responseClose.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kClose);

    const auto requestClose = parseHead(
        "GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 3",
        Http1ClientRequestClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK(
        requestClose.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kClose);
}

RUVIA_TEST(http_client_http10_transfer_encoding_is_faulty_framing) {
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked"));
    RUVIA_CHECK(parseFails(
        "HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip"));
}

RUVIA_TEST(http_client_successful_connect_transitions_to_tunnel) {
    const auto tunnel = parseHead(
        "CONNECT",
        "HTTP/1.1 200 Connection Established\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: chunked;invalid=parameter");
    RUVIA_CHECK(tunnel.plan().isConnectTunnel());
    RUVIA_CHECK(
        tunnel.plan().connectionDisposition() ==
        Http1ClientConnectionDisposition::kConnectTunnel);
    RUVIA_CHECK(!tunnel.plan().requiresBodyConsumption());

    const auto rejected = parseHead(
        "CONNECT", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 3");
    RUVIA_CHECK(rejected.plan().hasContentLength());

    // Methods are case-sensitive. A custom lowercase token is not CONNECT.
    const auto lowercase = parseHead("connect", "HTTP/1.1 200 OK");
    RUVIA_CHECK(lowercase.plan().isCloseDelimited());
}

RUVIA_TEST(http_client_head_method_is_case_sensitive) {
    RUVIA_CHECK(
        parseHead("HEAD", "HTTP/1.1 200 OK").plan().mode() ==
        Http1ClientResponseBodyMode::kNone);
    RUVIA_CHECK(
        parseHead("head", "HTTP/1.1 200 OK").plan().isCloseDelimited());
}

RUVIA_TEST(http_client_content_encoding_has_one_authoritative_path) {
    using ruvia::detail::HttpContentCoding;
    using ruvia::detail::httpClientResponseContentCoding;

    struct Case final {
        std::string_view headers;
        HttpContentCoding expected;
    };
    const Case cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0",
         HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 0",
         HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP\r\nContent-Length: 0",
         HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 0",
         HttpContentCoding::kBrotli},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 0",
         HttpContentCoding::kZstd},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0",
         HttpContentCoding::kNone},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 0",
         HttpContentCoding::kNone},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 0",
         HttpContentCoding::kNone},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
         "Content-Encoding: br\r\nContent-Length: 0",
         HttpContentCoding::kNone},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", HttpContentCoding::kNone},
    };

    for (const auto& test : cases) {
        auto parsed = parseResponse("GET", test.headers);
        RUVIA_CHECK_EQ(parsed.response.status(), std::uint16_t{200});
        RUVIA_CHECK(
            httpClientResponseContentCoding(parsed.response) == test.expected);
    }
}

RUVIA_TEST(http_client_rejects_malformed_status_and_length_fields) {
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/2 200 OK") ==
        Http1ClientResponseParseError::kUnsupportedHttpVersion);
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 99 Too Small") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 abc Bad") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 200") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    std::string invalidReason("HTTP/1.1 200 ");
    invalidReason.push_back('\x01');
    RUVIA_CHECK(
        parseFailureError("GET", invalidReason) ==
        Http1ClientResponseParseError::kInvalidReasonPhrase);
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: notanumber"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5,"));
    RUVIA_CHECK(!ruvia::http1ClientResponseParseErrorMessage(
        Http1ClientResponseParseError::kInvalidStatusCode).empty());
}

RUVIA_TEST(http_client_response_parser_need_more_is_distinct) {
    const auto result = parseWire(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
    RUVIA_CHECK(result.kind() == Http1ClientResponseParseKind::kNeedMore);
    RUVIA_CHECK(result.needMore() != nullptr);
    RUVIA_CHECK(result.parsed() == nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
}

RUVIA_TEST(http_client_response_parser_owns_exact_head_boundary) {
    std::string wire =
        "HTTP/1.1 200 OK\r\nX-Owner: response\r\nContent-Length: 4\r\n\r\n"
        "bodyHTTP/1.1 500 ignored\r\n\r\n";
    const auto expectedConsumed = wire.find("\r\n\r\n") + 4;
    auto result = parseWire("GET", wire);
    auto* parsed = result.parsed();
    RUVIA_CHECK(result.kind() == Http1ClientResponseParseKind::kParsed);
    RUVIA_CHECK(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->consumedBytes(), expectedConsumed);
    RUVIA_CHECK_EQ(parsed->response().status(), std::uint16_t{200});
    RUVIA_CHECK(
        parsed->response().protocolVersion() ==
        HttpProtocolVersion::kHttp11);
    RUVIA_CHECK_EQ(parsed->response().headers().size(), std::size_t{2});

    wire.assign(wire.size(), 'x');
    const auto headers = parsed->response().headers();
    RUVIA_CHECK(headers[0].name() == "X-Owner");
    RUVIA_CHECK(headers[0].value() == "response");
    RUVIA_CHECK(headers[1].value() == "4");
}

RUVIA_TEST(http_client_response_parser_failure_is_typed_and_allocation_free) {
    RejectingMemoryResource rejecting;
    ruvia::HttpClientRequest request;
    request.method = "GET";
    std::array<char, 512> requestHead;
    const auto preparedResult = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"), request, requestHead);
    const auto* prepared = preparedResult.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }

    auto failureParser = Http1ClientResponseParser(*prepared, &rejecting);
    const auto failure = failureParser.parse("HTTP/2 200 OK\r\n\r\n");
    RUVIA_CHECK(failure.kind() == Http1ClientResponseParseKind::kFailure);
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(
        failure.failure()->error() ==
        Http1ClientResponseParseError::kUnsupportedHttpVersion);

    bool successAllocated = false;
    try {
        auto successParser = Http1ClientResponseParser(*prepared, &rejecting);
        (void)successParser.parse(
            "HTTP/1.1 200 OK\r\n"
            "X-Requires-Ownership: a-long-enough-value-to-require-storage\r\n"
            "Content-Length: 0\r\n\r\n");
    } catch (const std::bad_alloc&) {
        successAllocated = true;
    }
    RUVIA_CHECK(successAllocated);
}

RUVIA_TEST(http_client_response_parser_enforces_the_complete_head_limit) {
    std::string oversized(ruvia::kMaxHttpHeaderBytes, 'x');
    const auto result = parseWire("GET", oversized);
    RUVIA_CHECK(result.kind() == Http1ClientResponseParseKind::kFailure);
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK(
        result.failure()->error() ==
        Http1ClientResponseParseError::kHeaderTooLarge);
}
