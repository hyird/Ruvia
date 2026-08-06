#include "http_client_response_fixture.h"

// HTTP/1 client responses: the exchange around it: interim responses, Expect, upgrades and tunnels.

RUVIA_TEST(http_client_informational_response_enforces_shared_field_contract) {
    for (const auto status : {std::string_view("HTTP/1.1 100 Continue"), std::string_view("HTTP/1.1 103 Early Hints")}) {
        const auto head = parseHead("GET", status);
        RUVIA_CHECK(head.plan().informational() != nullptr);
    }

    const auto valid = parseHead("GET",
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </style.css>; rel=preload\r\n"
        "Content-Type: text/html; charset=utf-8");
    RUVIA_CHECK(valid.plan().informational() != nullptr);

    constexpr std::array invalidFields{
        std::string_view("Content-Length: 0"),
        std::string_view("Transfer-Encoding: chunked"),
        std::string_view("Trailer: X-Checksum"),
        std::string_view("Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                         "date: Thu, 01 Jan 1970 00:00:01 GMT"),
    };
    for (const auto fields : invalidFields) {
        std::string head("HTTP/1.1 103 Early Hints\r\n");
        head.append(fields);
        const auto result = parseResult("GET", head);
        RUVIA_CHECK(result.parsed() == nullptr);
        RUVIA_CHECK(result.failure() != nullptr);
        if (result.failure() != nullptr) {
            RUVIA_CHECK(result.failure()->error() == Http1ClientResponseParseError::kInvalidHeader);
        }
    }
}

RUVIA_TEST(http_client_limits_informational_responses_per_exchange) {
    ruvia::HttpClientRequestView request;
    request.method = "GET";
    std::array<char, 512> requestHead;
    const auto prepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, requestHead);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser parser(*prepared.prepared());
    constexpr std::string_view earlyHints = "HTTP/1.1 103 Early Hints\r\n\r\n";
    for (std::size_t i = 0; i < ruvia::detail::kMaxHttpClientInterimResponses; ++i) {
        const auto interim = parser.parse(earlyHints);
        RUVIA_CHECK(interim.parsed() != nullptr);
        if (interim.parsed() != nullptr) {
            RUVIA_CHECK(interim.parsed()->plan().informational() != nullptr);
        }
    }

    const auto excessive = parser.parse(earlyHints);
    RUVIA_CHECK(excessive.failure() != nullptr);
    if (excessive.failure() != nullptr) {
        RUVIA_CHECK(excessive.failure()->error() == Http1ClientResponseParseError::kTooManyInformationalResponses);
    }
    const auto afterFailure = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(afterFailure.failure() != nullptr);
    if (afterFailure.failure() != nullptr) {
        RUVIA_CHECK(afterFailure.failure()->error() == Http1ClientResponseParseError::kExchangeFailed);
    }
}

RUVIA_TEST(http_client_expect_continue_is_one_stateful_exchange_contract) {
    ruvia::HttpClientRequestView request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContentView::bytes("payload");
    std::array<char, 512> requestHead;
    const auto preparedResult = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, requestHead, Http1ClientRequestWirePolicy::expectContinue());
    const auto* prepared = preparedResult.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(prepared->contentPlan().continueGated() != nullptr);

    Http1ClientResponseParser parser(*prepared);
    auto earlyHints = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(earlyHints.parsed() != nullptr);
    if (earlyHints.parsed() != nullptr) {
        RUVIA_CHECK(!earlyHints.parsed()->plan().requestContentSignal());
        RUVIA_CHECK(earlyHints.parsed()->plan().informational() != nullptr);
    }

    auto continueResponse = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    if (continueResponse.parsed() != nullptr) {
        RUVIA_CHECK(continueResponse.parsed()->plan().requestContentSignal() == Http1ClientRequestContentSignal::kContinue);
    }
    const auto duplicateContinue = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(duplicateContinue.parsed() != nullptr);
    if (duplicateContinue.parsed() != nullptr) {
        RUVIA_CHECK(!duplicateContinue.parsed()->plan().requestContentSignal());
    }
    RUVIA_CHECK(parser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kCompleted);
    RUVIA_CHECK(parser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kAlreadyComplete);

    auto finalResponse = parser.parse("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(finalResponse.parsed() != nullptr);
    if (finalResponse.parsed() != nullptr) {
        RUVIA_CHECK(!finalResponse.parsed()->plan().requestContentSignal());
    }

    const auto afterFinal = parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    RUVIA_CHECK(afterFinal.failure() != nullptr);
    if (afterFinal.failure() != nullptr) {
        RUVIA_CHECK(afterFinal.failure()->error() == Http1ClientResponseParseError::kExchangeComplete);
    }
    RUVIA_CHECK(parser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kExchangeTerminal);
}

RUVIA_TEST(http_client_closing_informational_response_ends_exchange) {
    ruvia::HttpClientRequestView getRequest;
    getRequest.method = "GET";
    std::array<char, 512> getHead;
    const auto getPrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), getRequest, getHead);
    RUVIA_CHECK(getPrepared.prepared() != nullptr);
    if (getPrepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser getParser(*getPrepared.prepared());
    const auto earlyHints = getParser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(earlyHints.parsed() != nullptr);
    if (earlyHints.parsed() != nullptr) {
        const auto* const informational = earlyHints.parsed()->plan().informational();
        RUVIA_CHECK(informational != nullptr);
        if (informational != nullptr) {
            RUVIA_CHECK(informational->persistence() == Http1ClientResponsePersistence::kClose);
        }
    }
    const auto afterClose = getParser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(afterClose.failure() != nullptr);
    if (afterClose.failure() != nullptr) {
        RUVIA_CHECK(afterClose.failure()->error() == Http1ClientResponseParseError::kExchangeComplete);
    }

    ruvia::HttpClientRequestView postRequest;
    postRequest.method = "POST";
    postRequest.content = ruvia::HttpClientRequestContentView::bytes("payload");
    std::array<char, 512> postHead;
    const auto postPrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), postRequest, postHead, Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(postPrepared.prepared() != nullptr);
    if (postPrepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser postParser(*postPrepared.prepared());
    const auto closingContinue = postParser.parse("HTTP/1.1 100 Continue\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(closingContinue.parsed() != nullptr);
    if (closingContinue.parsed() != nullptr) {
        RUVIA_CHECK(closingContinue.parsed()->plan().requestContentSignal() == Http1ClientRequestContentSignal::kExchangeComplete);
    }

    Http1ClientResponseParser closingHintsParser(*postPrepared.prepared());
    const auto closingHints = closingHintsParser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    RUVIA_CHECK(closingHints.parsed() != nullptr);
    if (closingHints.parsed() != nullptr) {
        RUVIA_CHECK(closingHints.parsed()->plan().requestContentSignal() == Http1ClientRequestContentSignal::kExchangeComplete);
    }

    std::array<char, 512> requestCloseHead;
    const auto requestClosePrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), getRequest, requestCloseHead, Http1ClientRequestWirePolicy::withoutExpectation(Http1ClientRequestClosePolicy::kCloseAfterResponse));
    RUVIA_CHECK(requestClosePrepared.prepared() != nullptr);
    if (requestClosePrepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser requestCloseParser(*requestClosePrepared.prepared());
    const auto nonClosingHints = requestCloseParser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    RUVIA_CHECK(nonClosingHints.parsed() != nullptr);
    if (nonClosingHints.parsed() != nullptr) {
        const auto* const informational = nonClosingHints.parsed()->plan().informational();
        RUVIA_CHECK(informational != nullptr);
        if (informational != nullptr) {
            RUVIA_CHECK(informational->persistence() == Http1ClientResponsePersistence::kReuse);
        }
    }
    const auto requestCloseFinal = requestCloseParser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    RUVIA_CHECK(requestCloseFinal.parsed() != nullptr);
    if (requestCloseFinal.parsed() != nullptr) {
        RUVIA_CHECK(requireWithoutContent(requestCloseFinal.parsed()->plan()).persistence() == Http1ClientResponsePersistence::kClose);
    }
}

RUVIA_TEST(http_client_expect_final_cancels_only_pending_request_content) {
    ruvia::HttpClientRequestView request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContentView::bytes("payload");
    std::array<char, 512> requestHead;
    const auto prepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, requestHead, Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser parser(*prepared.prepared());
    const auto finalResponse = parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(finalResponse.parsed() != nullptr);
    if (finalResponse.parsed() != nullptr) {
        RUVIA_CHECK(finalResponse.parsed()->plan().requestContentSignal() == Http1ClientRequestContentSignal::kExchangeComplete);
    }

    Http1ClientResponseParser completedParser(*prepared.prepared());
    RUVIA_CHECK(completedParser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto completedFinal = completedParser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(completedFinal.parsed() != nullptr);
    if (completedFinal.parsed() != nullptr) {
        RUVIA_CHECK(!completedFinal.parsed()->plan().requestContentSignal());
    }
}

RUVIA_TEST(http_client_upgrade_after_expect_requires_prior_continue) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequestView request;
    request.method = "POST";
    request.headers = upgradeHeaders;
    request.content = ruvia::HttpClientRequestContentView::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> rejectedHead;
    const auto rejectedPrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, rejectedHead, Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(rejectedPrepared.prepared() != nullptr);
    if (rejectedPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser rejectedParser(*rejectedPrepared.prepared());
    const auto rejected = rejectedParser.parse(switching);
    RUVIA_CHECK(rejected.failure() != nullptr);
    if (rejected.failure() != nullptr) {
        RUVIA_CHECK(rejected.failure()->error() == Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }
    const auto afterFailure = rejectedParser.parse(switching);
    RUVIA_CHECK(afterFailure.failure() != nullptr);
    if (afterFailure.failure() != nullptr) {
        RUVIA_CHECK(afterFailure.failure()->error() == Http1ClientResponseParseError::kExchangeFailed);
    }

    // RFC 9110 section 7.8 still requires the server to acknowledge Expect
    // with 100 before 101, even when the client released and completed content
    // after its own finite wait expired.
    std::array<char, 512> completedWithoutContinueHead;
    const auto completedWithoutContinuePrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, completedWithoutContinueHead, Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(completedWithoutContinuePrepared.prepared() != nullptr);
    if (completedWithoutContinuePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser completedWithoutContinueParser(*completedWithoutContinuePrepared.prepared());
    RUVIA_CHECK(completedWithoutContinueParser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto completedWithoutContinue = completedWithoutContinueParser.parse(switching);
    RUVIA_CHECK(completedWithoutContinue.failure() != nullptr);
    if (completedWithoutContinue.failure() != nullptr) {
        RUVIA_CHECK(completedWithoutContinue.failure()->error() == Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> lateContinueHead;
    const auto lateContinuePrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, lateContinueHead, Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(lateContinuePrepared.prepared() != nullptr);
    if (lateContinuePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser lateContinueParser(*lateContinuePrepared.prepared());
    RUVIA_CHECK(lateContinueParser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto lateContinue = lateContinueParser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(lateContinue.parsed() != nullptr);
    if (lateContinue.parsed() != nullptr) {
        RUVIA_CHECK(!lateContinue.parsed()->plan().requestContentSignal());
    }
    const auto acceptedAfterLateContinue = lateContinueParser.parse(switching);
    RUVIA_CHECK(acceptedAfterLateContinue.parsed() != nullptr);
    if (acceptedAfterLateContinue.parsed() != nullptr) {
        RUVIA_CHECK(acceptedAfterLateContinue.parsed()->plan().protocolUpgrade() != nullptr);
    }

    std::array<char, 512> pendingHead;
    const auto pendingPrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, pendingHead, Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(pendingPrepared.prepared() != nullptr);
    if (pendingPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser pendingParser(*pendingPrepared.prepared());
    const auto pendingContinue = pendingParser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(pendingContinue.parsed() != nullptr);
    const auto pendingUpgrade = pendingParser.parse(switching);
    RUVIA_CHECK(pendingUpgrade.failure() != nullptr);
    if (pendingUpgrade.failure() != nullptr) {
        RUVIA_CHECK(pendingUpgrade.failure()->error() == Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> acceptedHead;
    const auto acceptedPrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, acceptedHead, Http1ClientRequestWirePolicy::expectContinue());
    RUVIA_CHECK(acceptedPrepared.prepared() != nullptr);
    if (acceptedPrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser acceptedParser(*acceptedPrepared.prepared());
    const auto continueResponse = acceptedParser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    RUVIA_CHECK(acceptedParser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto accepted = acceptedParser.parse(switching);
    RUVIA_CHECK(accepted.parsed() != nullptr);
    if (accepted.parsed() != nullptr) {
        RUVIA_CHECK(accepted.parsed()->plan().protocolUpgrade() != nullptr);
    }
}

RUVIA_TEST(http_client_upgrade_requires_complete_request_content) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::HttpClientRequestView request;
    request.method = "POST";
    request.headers = upgradeHeaders;
    request.content = ruvia::HttpClientRequestContentView::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    std::array<char, 512> incompleteHead;
    const auto incompletePrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, incompleteHead);
    RUVIA_CHECK(incompletePrepared.prepared() != nullptr);
    if (incompletePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser incompleteParser(*incompletePrepared.prepared());
    const auto incomplete = incompleteParser.parse(switching);
    RUVIA_CHECK(incomplete.failure() != nullptr);
    if (incomplete.failure() != nullptr) {
        RUVIA_CHECK(incomplete.failure()->error() == Http1ClientResponseParseError::kInvalidProtocolSwitch);
    }

    std::array<char, 512> completeHead;
    const auto completePrepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, completeHead);
    RUVIA_CHECK(completePrepared.prepared() != nullptr);
    if (completePrepared.prepared() == nullptr) {
        return;
    }
    Http1ClientResponseParser completeParser(*completePrepared.prepared());
    RUVIA_CHECK(completeParser.completeRequestContent() == Http1ClientRequestContentCompletionStatus::kCompleted);
    const auto complete = completeParser.parse(switching);
    RUVIA_CHECK(complete.parsed() != nullptr);
    if (complete.parsed() != nullptr) {
        RUVIA_CHECK(complete.parsed()->plan().protocolUpgrade() != nullptr);
    }
}

RUVIA_TEST(http_client_switching_protocols_is_an_exclusive_upgrade_transition) {
    const ruvia::HttpHeaderView requestHeaders[] = {
        {"Connection", "keep-alive, Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto upgraded = parseHead("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket",
        Http1ClientRequestClosePolicy::kAllowReuse, requestHeaders);
    RUVIA_CHECK(upgraded.head().status() == ruvia::http_status::kSwitchingProtocols);
    RUVIA_CHECK(upgraded.plan().protocolUpgrade() != nullptr);
    RUVIA_CHECK(upgraded.plan().connectTunnel() == nullptr);

    // Protocol names compare case-insensitively; versions remain exact tokens.
    const auto versioned = parseHead("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: irc/6.9",
        Http1ClientRequestClosePolicy::kAllowReuse, requestHeaders);
    RUVIA_CHECK(versioned.plan().protocolUpgrade() != nullptr);
    RUVIA_CHECK(parseFails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: IRC/6.10",
        Http1ClientRequestClosePolicy::kAllowReuse, requestHeaders));
}

RUVIA_TEST(http_client_connection_fields_use_recipient_list_semantics) {
    const auto reusable = parseHead("GET",
        "HTTP/1.0 200 OK\r\n"
        "Connection: , keep-alive,\r\nContent-Length: 0");
    RUVIA_CHECK(requireKnownLength(reusable.plan()).persistence() == Http1ClientResponsePersistence::kReuse);
    RUVIA_CHECK(reusable.head().protocolVersion() == HttpProtocolVersion::kHttp10);

    RUVIA_CHECK(parseFailureError("GET",
                    "HTTP/1.1 200 OK\r\nConnection: close;invalid\r\n"
                    "Content-Length: 0") == Http1ClientResponseParseError::kInvalidConnection);
    RUVIA_CHECK(parseFailureError("GET",
                    "HTTP/1.1 200 OK\r\nUpgrade: websocket/\r\n"
                    "Content-Length: 0") == Http1ClientResponseParseError::kInvalidUpgrade);

    const ruvia::HttpHeaderView offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto upgraded = parseHead("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , websocket,",
        Http1ClientRequestClosePolicy::kAllowReuse, offered);
    RUVIA_CHECK(upgraded.plan().protocolUpgrade() != nullptr);
}

RUVIA_TEST(http_client_rejects_end_to_end_connection_options) {
    for (const std::string_view option : {"Content-Length", "DATE", "Set-Cookie"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nContent-Length: 0");
        RUVIA_CHECK(parseFailureError("GET", response) == Http1ClientResponseParseError::kInvalidConnection);
    }

    const auto extension = parseHead("GET",
        "HTTP/1.1 200 OK\r\nConnection: X-Hop\r\n"
        "X-Hop: local\r\nContent-Length: 0");
    RUVIA_CHECK(extension.plan().knownLength() != nullptr);
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
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket", Http1ClientRequestClosePolicy::kAllowReuse, offered));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade", Http1ClientRequestClosePolicy::kAllowReuse, offered));
    RUVIA_CHECK(parseFails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: IRC",
        Http1ClientRequestClosePolicy::kAllowReuse, offered));
    RUVIA_CHECK(parseFails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\nContent-Length: 0",
        Http1ClientRequestClosePolicy::kAllowReuse, offered));
    RUVIA_CHECK(parseFails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n"
        "Transfer-Encoding: chunked",
        Http1ClientRequestClosePolicy::kAllowReuse, offered));
    RUVIA_CHECK(parseFails("GET",
        "HTTP/1.0 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        Http1ClientRequestClosePolicy::kAllowReuse, offered));
    RUVIA_CHECK(parseFails("GET", validResponse, Http1ClientRequestClosePolicy::kAllowReuse, closingOffer));
}

RUVIA_TEST(http_client_response_plan_owns_version_and_connection_persistence) {
    const auto http10 = parseHead("GET", "HTTP/1.0 200 OK\r\nContent-Length: 3");
    const auto& http10Body = requireKnownLength(http10.plan());
    RUVIA_CHECK(http10Body.persistence() == Http1ClientResponsePersistence::kClose);

    const auto http10KeepAlive = parseHead("GET", "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 3");
    RUVIA_CHECK(requireKnownLength(http10KeepAlive.plan()).persistence() == Http1ClientResponsePersistence::kReuse);

    const auto responseClose = parseHead("GET", "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3");
    RUVIA_CHECK(requireKnownLength(responseClose.plan()).persistence() == Http1ClientResponsePersistence::kClose);

    const auto requestClose = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3", Http1ClientRequestClosePolicy::kCloseAfterResponse);
    RUVIA_CHECK(requireKnownLength(requestClose.plan()).persistence() == Http1ClientResponsePersistence::kClose);
}

RUVIA_TEST(http_client_successful_connect_transitions_to_tunnel) {
    const auto tunnel = parseHead("CONNECT",
        "HTTP/1.1 200 Connection Established\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: chunked;invalid=parameter");
    RUVIA_CHECK(tunnel.plan().connectTunnel() != nullptr);

    const auto rejected = parseHead("CONNECT", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 3");
    RUVIA_CHECK(rejected.plan().knownLength() != nullptr);

    // Methods are case-sensitive. A custom lowercase token is not CONNECT.
    const auto lowercase = parseHead("connect", "HTTP/1.1 200 OK");
    RUVIA_CHECK(lowercase.plan().closeDelimited() != nullptr);
}
