#include "http2_connection_fixture.h"

// Http2Connection: submitting request and response heads.

RUVIA_TEST(http2_connection_request_head_requires_started_preface_without_consuming_id) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);

    const auto beforeStart = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(beforeStart.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(beforeStart) ==
        Http2RequestHeadSubmitError::kConnectionNotStarted);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK(client.stream(1) == nullptr);

    beginClient(client);
    const auto accepted = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
}

RUVIA_TEST(http2_connection_feed_extension_method_emits_request_event) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeRequest(block, "PROPFIND");
    const auto frame = headersFrame(
        &resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    const auto result = conn.feed(std::string_view(frame.data(), frame.size()));

    RUVIA_CHECK(result == ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    RUVIA_CHECK(!conn.nextEvent().has_value());

    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->requestMethod(), std::string_view("PROPFIND"));
        RUVIA_CHECK(
            stream->requestKnownMethod() == ruvia::HttpKnownMethod::kUnknown);
    }
}

RUVIA_TEST(http2_connection_feed_accepts_non_http_request_scheme) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeRequest(block, "GET", "gemini", "/", std::nullopt);
    const auto frame = headersFrame(
        &resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));

    RUVIA_CHECK(conn.feed(std::string_view(frame.data(), frame.size())) ==
        Http2FeedResult::kAccepted);
    RUVIA_CHECK(!conn.connectionError().has_value());
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().value().kind() == Http2EventKind::kMessageEnd);
    const auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->requestScheme(), std::string_view("gemini"));
        RUVIA_CHECK_EQ(stream->schemeDefaultPort(), std::uint16_t{0});
        RUVIA_CHECK(!stream->hasAuthority());
    }
}

RUVIA_TEST(http2_connection_rejects_http_request_without_authority) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr std::string_view schemes[] = {"http", "HTTPS"};
    for (const auto scheme : schemes) {
        Http2Connection server(&resource);
        handshake(server);
        std::pmr::string block(&resource);
        encodeRequest(block, "GET", scheme, "/resource", std::nullopt);
        // A retained Host field is not a substitute for mandatory HTTP/2
        // control data when the target URI itself has an authority.
        HpackEncoder::encodeHeader(block, "host", "example.com");
        const auto request = headersFrame(
            &resource, 1,
            ruvia::detail::kHttp2FlagEndHeaders |
                ruvia::detail::kHttp2FlagEndStream,
            std::string_view(block.data(), block.size()));

        RUVIA_CHECK(server.feed(
            std::string_view(request.data(), request.size())) ==
            Http2FeedResult::kAccepted);
        RUVIA_CHECK(!server.connectionError().has_value());
        RUVIA_CHECK(server.nextEvent().value().kind() ==
            Http2EventKind::kStreamClosed);
        RUVIA_CHECK(!server.nextEvent().has_value());
        const auto out = server.pendingOutput();
        const auto reset = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(
            reset.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2Read32(
                reinterpret_cast<const unsigned char*>(out.data() + 9)),
            static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    }
}

RUVIA_TEST(http2_connection_buffered_response_length_is_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kOk);
    response.body("hello");
    RUVIA_CHECK(responseHeadSubmitted(
        submitBufferedResponseHead(conn, 1, response)));
    conn.consumeOutput(conn.pendingOutput().size());

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(
        requireLocalKnownLength(*stream).declaredLength(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{0});

    // All mismatches are rejected before output, flow-window, counters, or phase
    // change. The caller can correct the submission and continue the same stream.
    RUVIA_CHECK(conn.finishResponse(1, validatedTrailers({})) ==
        Http2FinishSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(conn.submitData(1, "four", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(conn.submitData(1, "sixsix", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{0});
    RUVIA_CHECK(stream->localSend().responseContentOpen() != nullptr);

    RUVIA_CHECK(conn.submitData(1, "he", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{2});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{2});
    RUVIA_CHECK(conn.submitData(1, "llo", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
    RUVIA_CHECK(stream->localContent().lengthComplete());
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
}

RUVIA_TEST(http2_connection_response_head_submit_result_is_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kOk);
    response.body("ok");

    Http2Connection missingStream(&resource);
    const auto closed = submitBufferedResponseHead(missingStream, 1, response);
    RUVIA_CHECK(closed.submitted() == nullptr);
    RUVIA_CHECK(closed.failure() != nullptr);
    RUVIA_CHECK(closed.failure()->peerClosed());
    RUVIA_CHECK_EQ(
        std::string_view(closed.failure()->exception().what()),
        std::string_view("HTTP/2 response stream is closed"));
    RUVIA_CHECK(missingStream.pendingOutput().empty());

    Http2Connection buffered(&resource);
    handshake(buffered);
    driveGetRequest(buffered, &resource);
    const auto submitted = submitBufferedResponseHead(buffered, 1, response);
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK(submitted.failure() == nullptr);
    RUVIA_CHECK_EQ(
        submitted.submitted()->contentLength(),
        std::uint64_t{2});

    Http2Connection streaming(&resource);
    handshake(streaming);
    driveGetRequest(streaming, &resource);
    ruvia::HttpResponse streamingHead(&resource);
    streamingHead.status(ruvia::http_status::kOk);
    const auto streamingSubmitted = streaming.submitStreamingResponseHead(
        1,
        std::move(streamingHead),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(streamingSubmitted.submitted() != nullptr);
    RUVIA_CHECK(streamingSubmitted.failure() == nullptr);
}

RUVIA_TEST(http2_connection_buffered_response_requires_matching_prepared_plan) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection connection(&resource);
    handshake(connection);
    driveGetRequest(connection, &resource);

    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kMultiStatus);
    response.body("old");

    const auto wrongMethodPlan =
        ruvia::detail::httpBufferedResponseWritePlan(
            ruvia::HttpKnownMethod::kHead,
            response);
    const auto wrongMethod = connection.submitResponseHead(
        1,
        response,
        wrongMethodPlan);
    RUVIA_CHECK(
        responseHeadSubmitFailureMessage(wrongMethod) ==
        "HTTP/2 response head does not match its write plan");
    RUVIA_CHECK(connection.pendingOutput().empty());

    const auto staleRepresentationPlan =
        ruvia::detail::httpBufferedResponseWritePlan(
            ruvia::HttpKnownMethod::kGet,
            response);
    response.body("longer");
    const auto staleRepresentation = connection.submitResponseHead(
        1,
        response,
        staleRepresentationPlan);
    RUVIA_CHECK(
        responseHeadSubmitFailureMessage(staleRepresentation) ==
        "HTTP/2 response head does not match its write plan");
    RUVIA_CHECK(connection.pendingOutput().empty());

    const auto staleStatusPlan =
        ruvia::detail::httpBufferedResponseWritePlan(
            ruvia::HttpKnownMethod::kGet,
            response);
    response.status(ruvia::http_status::kAlreadyReported);
    const auto staleStatus = connection.submitResponseHead(
        1,
        response,
        staleStatusPlan);
    RUVIA_CHECK(
        responseHeadSubmitFailureMessage(staleStatus) ==
        "HTTP/2 response head does not match its write plan");
    RUVIA_CHECK(connection.pendingOutput().empty());

    const auto submitted = connection.submitResponseHead(
        1,
        response,
        ruvia::detail::httpBufferedResponseWritePlan(
            ruvia::HttpKnownMethod::kGet,
            response));
    RUVIA_CHECK(responseHeadSubmitted(submitted));
    const auto& committedPlan = submittedResponsePlan(submitted);
    RUVIA_CHECK(
        committedPlan.requestMethod() == ruvia::HttpKnownMethod::kGet);
    RUVIA_CHECK_EQ(
        committedPlan.responseStatus(), ruvia::http_status::kAlreadyReported);
    RUVIA_CHECK_EQ(committedPlan.contentLength(), std::uint64_t{6});
}

RUVIA_TEST(http2_connection_rejects_duplicate_response_head_without_output) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse first(&resource);
    first.status(ruvia::http_status::kOk);
    const auto firstResult = conn.submitStreamingResponseHead(
        1,
        std::move(first),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(responseHeadSubmitted(firstResult));
    conn.consumeOutput(conn.pendingOutput().size());

    ruvia::HttpResponse duplicate(&resource);
    duplicate.status(ruvia::http_status::kOk);
    const auto duplicateResult = submitBufferedResponseHead(conn, 1, duplicate);
    RUVIA_CHECK(
        responseHeadSubmitFailureMessage(duplicateResult) ==
        "invalid HTTP/2 response head submission state");
    RUVIA_CHECK(conn.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_rejects_head_api_for_wrong_role) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    handshake(server);
    driveGetRequest(server, &resource);
    RUVIA_CHECK(requestHeadSubmitError(server.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none())) ==
        Http2RequestHeadSubmitError::kInvalidState);
    RUVIA_CHECK(server.pendingOutput().empty());

    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());
    ruvia::HttpResponse response(&resource);
    response.status(ruvia::http_status::kOk);
    const auto result = submitBufferedResponseHead(client, streamId, response);
    RUVIA_CHECK(
        responseHeadSubmitFailureMessage(result) ==
        "invalid HTTP/2 response head submission state");
    RUVIA_CHECK(client.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_request_content_alternatives_own_wire_framing) {
    std::pmr::monotonic_buffer_resource resource;

    const auto withoutContent = Http2RequestContent::none();
    RUVIA_CHECK(withoutContent.withoutContent() != nullptr);
    RUVIA_CHECK(withoutContent.knownLengthContent() == nullptr);
    RUVIA_CHECK(withoutContent.streamingContent() == nullptr);

    const auto zeroLength = Http2RequestContent::knownLength(0);
    RUVIA_CHECK(zeroLength.withoutContent() == nullptr);
    RUVIA_CHECK(zeroLength.knownLengthContent() != nullptr);
    RUVIA_CHECK(zeroLength.streamingContent() == nullptr);
    if (const auto* knownLength = zeroLength.knownLengthContent()) {
        RUVIA_CHECK_EQ(knownLength->length(), std::uint64_t{0});
    }

    const auto streaming = Http2RequestContent::streaming();
    RUVIA_CHECK(streaming.withoutContent() == nullptr);
    RUVIA_CHECK(streaming.knownLengthContent() == nullptr);
    RUVIA_CHECK(streaming.streamingContent() != nullptr);

    const auto check = [&resource, &ruvia_ctx](
                           std::string_view method,
                           Http2RequestContent content,
                           bool expectEndStream,
                           std::string_view expectedContentLength,
                           auto&& verifyLocalContent) {
        Http2Connection client(
            &resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const auto submit = client.submitRegularRequestHead(
            method,
            "https",
            "example.test",
            "/upload",
            {},
            content);
        RUVIA_CHECK(submit.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(submit);
        RUVIA_CHECK_EQ(streamId, static_cast<std::uint32_t>(1));

        const auto out = client.pendingOutput();
        RUVIA_CHECK(out.size() >= 9);
        const auto frame = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        RUVIA_CHECK_EQ(frame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
        RUVIA_CHECK_EQ(frame.streamId, streamId);
        RUVIA_CHECK((frame.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
        RUVIA_CHECK(
            ((frame.flags & ruvia::detail::kHttp2FlagEndStream) != 0) ==
            expectEndStream);
        RUVIA_CHECK_EQ(
            out.size(), static_cast<std::size_t>(9 + frame.length));

        RequestContentLengthObservation observation;
        HpackDecoder decoder(&resource);
        const auto decodeResult = decoder.decode(
            out.substr(9, frame.length),
            &observation,
            &observeRequestContentLength);
        RUVIA_CHECK(decodeResult.decoded() != nullptr);
        if (expectedContentLength.empty()) {
            RUVIA_CHECK_EQ(observation.count, static_cast<std::size_t>(0));
        } else {
            RUVIA_CHECK_EQ(observation.count, static_cast<std::size_t>(1));
            RUVIA_CHECK_EQ(observation.value, std::string(expectedContentLength));
        }

        const auto* stream = client.stream(streamId);
        RUVIA_CHECK(stream != nullptr);
        verifyLocalContent(stream->localContent());
        RUVIA_CHECK_EQ(
            stream->localSend().endStreamCommitted() != nullptr,
            expectEndStream);
    };

    check(
        "GET",
        Http2RequestContent::none(),
        true,
        {},
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            RUVIA_CHECK(localContent.forbidden() != nullptr);
            RUVIA_CHECK(localContent.knownLength() == nullptr);
        });
    check(
        "POST",
        Http2RequestContent::knownLength(0),
        true,
        "0",
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            const auto* knownLength = localContent.knownLength();
            RUVIA_CHECK(knownLength != nullptr);
            if (knownLength != nullptr) {
                RUVIA_CHECK_EQ(
                    knownLength->declaredLength(), std::uint64_t{0});
            }
        });
    check(
        "POST",
        Http2RequestContent::knownLength(5),
        false,
        "5",
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            const auto* knownLength = localContent.knownLength();
            RUVIA_CHECK(knownLength != nullptr);
            if (knownLength != nullptr) {
                RUVIA_CHECK_EQ(
                    knownLength->declaredLength(), std::uint64_t{5});
            }
        });
    check(
        "POST",
        Http2RequestContent::streaming(),
        false,
        {},
        [&ruvia_ctx](const Http2LocalContentState& localContent) {
            RUVIA_CHECK(localContent.unbounded() != nullptr);
            RUVIA_CHECK(localContent.knownLength() == nullptr);
        });
}

RUVIA_TEST(http2_connection_enforces_request_method_content_semantics_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto checkRejected = [&resource, &ruvia_ctx](
                                   std::string_view method,
                                   std::span<const ruvia::HttpHeaderView> headers,
                                   Http2RequestContent content) {
        Http2Connection client(
            &resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);

        const auto rejected = client.submitRegularRequestHead(
            method,
            "https",
            "example.test",
            "/diagnostics",
            headers,
            content);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(rejected) ==
            Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);

        const auto accepted = client.submitRegularRequestHead(
            "GET",
            "https",
            "example.test",
            "/",
            {},
            Http2RequestContent::none());
        RUVIA_CHECK(accepted.submitted() != nullptr);
        RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
    };

    checkRejected("TRACE", {}, Http2RequestContent::knownLength(0));
    checkRejected("TRACE", {}, Http2RequestContent::knownLength(1));
    checkRejected("TRACE", {}, Http2RequestContent::streaming());
    checkRejected("OPTIONS", {}, Http2RequestContent::knownLength(0));
    checkRejected("OPTIONS", {}, Http2RequestContent::knownLength(1));
    checkRejected("OPTIONS", {}, Http2RequestContent::streaming());

    const ruvia::HttpHeaderView invalidContentType[] = {
        {"content-type", "not a media type"}};
    checkRejected(
        "OPTIONS",
        invalidContentType,
        Http2RequestContent::knownLength(1));

    Http2Connection traceClient(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(traceClient);
    const auto trace = traceClient.submitRegularRequestHead(
        "TRACE",
        "https",
        "example.test",
        "/diagnostics",
        {},
        Http2RequestContent::none());
    RUVIA_CHECK(trace.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(trace), std::uint32_t{1});

    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const ruvia::HttpHeaderView contentType[] = {
        {"content-type", "application/json"}};
    const auto accepted = client.submitRegularRequestHead(
        "OPTIONS",
        "https",
        "example.test",
        "/diagnostics",
        contentType,
        Http2RequestContent::knownLength(1));
    RUVIA_CHECK(accepted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
}

RUVIA_TEST(http2_connection_rejects_repeated_websocket_identity_and_user_agent_fields_transactionally) {
    struct Case final {
        std::string_view name;
        std::string_view first;
        std::string_view second;
    };
    const Case cases[] = {
        {"sec-websocket-key", "first", "second"},
        {"sec-websocket-version", "13", "12"},
        {"user-agent", "first/1", "second/2"},
    };
    std::pmr::monotonic_buffer_resource resource;
    for (const auto& test : cases) {
        Http2Connection client(
            &resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const ruvia::HttpHeaderView headers[] = {
            {test.name, test.first},
            {test.name, test.second},
        };
        const auto rejected = client.submitRegularRequestHead(
            "GET",
            "https",
            "example.test",
            "/resource",
            headers,
            Http2RequestContent::none());
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(rejected) ==
            Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    }
}

RUVIA_TEST(http2_connection_rejects_oversized_outbound_request_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto checkRejected = [&resource, &ruvia_ctx](
                                   std::string_view path,
                                   std::span<const ruvia::HttpHeaderView> headers,
                                   Http2RequestContent content) {
        Http2Connection client(
            &resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const auto rejected = client.submitRegularRequestHead(
            "GET",
            "https",
            "example.test",
            path,
            headers,
            content);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(rejected) ==
            Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);
    };

    const std::string oversizedValue(ruvia::kMaxHttpHeaderBytes, 'x');
    const ruvia::HttpHeaderView oversized[] = {
        {"x-oversized", oversizedValue},
    };
    checkRejected("/resource", oversized, Http2RequestContent::none());

    const std::string oversizedPath(
        ruvia::kMaxHttpHeaderBytes, 'p');
    checkRejected(oversizedPath, {}, Http2RequestContent::none());

    std::array<
        ruvia::HttpHeaderView,
        ruvia::kMaxHttpHeaderFields + 1> tooMany{};
    for (auto& header : tooMany) {
        header = {"x-many", "value"};
    }
    checkRejected("/resource", tooMany, Http2RequestContent::none());

    std::array<
        ruvia::HttpHeaderView,
        ruvia::kMaxHttpHeaderFields> generatedOverflow{};
    for (auto& header : generatedOverflow) {
        header = {"x-generated", "value"};
    }
    checkRejected(
        "/resource",
        generatedOverflow,
        Http2RequestContent::knownLength(0));

    Http2Connection connectClient(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(connectClient);
    const auto rejectedConnect = connectClient.submitConnectRequestHead(
        "example.test:443", oversized);
    RUVIA_CHECK(rejectedConnect.submitted() == nullptr);
    RUVIA_CHECK(requestHeadSubmitError(rejectedConnect) ==
        Http2RequestHeadSubmitError::kInvalidMessage);
    RUVIA_CHECK(connectClient.pendingOutput().empty());
    RUVIA_CHECK(connectClient.stream(1) == nullptr);
}

RUVIA_TEST(http2_connection_rejects_oversized_response_heads_transactionally) {
    std::pmr::monotonic_buffer_resource resource;

    const auto checkRejected = [&resource, &ruvia_ctx](ruvia::HttpResponse response) {
        Http2Connection connection(&resource);
        handshake(connection);
        driveGetRequest(connection, &resource);

        const auto rejected = submitBufferedResponseHead(
            connection, 1, response);
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(
            responseHeadSubmitFailureMessage(rejected) ==
            "invalid HTTP/2 response head message");
        RUVIA_CHECK(connection.pendingOutput().empty());
        RUVIA_CHECK(connection.stream(1) != nullptr);
        RUVIA_CHECK(
            connection.stream(1)->localSend().headPending() != nullptr);
    };

    ruvia::HttpResponse oversized(&resource);
    oversized.header(
        "X-Oversized",
        std::string(ruvia::kMaxHttpHeaderBytes, 'x'));
    checkRejected(std::move(oversized));

    ruvia::HttpResponse tooMany(&resource);
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        tooMany.header("X-Field-" + std::to_string(i), "value");
    }
    checkRejected(std::move(tooMany));

    ruvia::HttpResponse generatedOverflow(&resource);
    for (std::size_t i = 0; i < ruvia::kMaxHttpHeaderFields - 1; ++i) {
        generatedOverflow.header(
            "X-Generated-" + std::to_string(i), "value");
    }
    checkRejected(std::move(generatedOverflow));
}

RUVIA_TEST(http2_connection_rejects_raw_request_content_length_transactionally) {
    std::pmr::monotonic_buffer_resource resource;
    const auto checkRejected =
        [&resource, &ruvia_ctx](std::span<const ruvia::HttpHeaderView> headers) {
        Http2Connection client(
            &resource, ruvia::detail::Http2Role::kClient);
        beginClient(client);
        const auto rejected = client.submitRegularRequestHead(
            "POST",
            "https",
            "example.test",
            "/upload",
            headers,
            Http2RequestContent::knownLength(5));
        RUVIA_CHECK(rejected.submitted() == nullptr);
        RUVIA_CHECK(requestHeadSubmitError(rejected) ==
            Http2RequestHeadSubmitError::kInvalidMessage);
        RUVIA_CHECK(client.pendingOutput().empty());
        RUVIA_CHECK(client.stream(1) == nullptr);

        const auto accepted = client.submitRegularRequestHead(
            "POST",
            "https",
            "example.test",
            "/upload",
            {},
            Http2RequestContent::knownLength(5));
        RUVIA_CHECK(accepted.submitted() != nullptr);
        RUVIA_CHECK_EQ(submittedRequestStreamId(accepted), std::uint32_t{1});
        RUVIA_CHECK(!client.pendingOutput().empty());
    };

    const ruvia::HttpHeaderView matching[] = {{"content-length", "5"}};
    const ruvia::HttpHeaderView conflicting[] = {{"content-length", "4"}};
    const ruvia::HttpHeaderView duplicate[] = {
        {"content-length", "5"},
        {"content-length", "5"}};
    const ruvia::HttpHeaderView invalid[] = {{"content-length", "invalid"}};
    checkRejected(matching);
    checkRejected(conflicting);
    checkRejected(duplicate);
    checkRejected(invalid);
}

RUVIA_TEST(http2_connection_encodes_non_http_request_without_authority) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);

    const auto submitted = client.submitRegularRequestHead(
        "GET", "git+ssh", std::nullopt, "", {},
        Http2RequestContent::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(submitted), std::uint32_t{1});

    const auto out = client.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RequestContentLengthObservation observation;
    HpackDecoder decoder(&resource);
    const auto decoded = decoder.decode(
        out.substr(9, frame.length), &observation,
        &observeRequestContentLength);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    RUVIA_CHECK_EQ(observation.scheme, std::string("git+ssh"));
    RUVIA_CHECK_EQ(observation.authorityCount, std::size_t{0});
    RUVIA_CHECK_EQ(observation.pathCount, std::size_t{1});
    RUVIA_CHECK(observation.path.empty());
    const auto* stream = client.stream(1);
    RUVIA_CHECK(stream != nullptr);
    if (stream != nullptr) {
        RUVIA_CHECK_EQ(stream->requestScheme(), std::string_view("git+ssh"));
    }
}

RUVIA_TEST(http2_connection_encodes_non_http_userinfo_authority) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);

    const auto submitted = client.submitRegularRequestHead(
        "GET", "git+ssh", "deploy:secret@example.test:9418",
        "/repository", {}, Http2RequestContent::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(submitted), std::uint32_t{1});

    const auto out = client.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RequestContentLengthObservation observation;
    HpackDecoder decoder(&resource);
    const auto decoded = decoder.decode(
        out.substr(9, frame.length), &observation,
        &observeRequestContentLength);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    RUVIA_CHECK_EQ(observation.authorityCount, std::size_t{1});
    RUVIA_CHECK_EQ(
        observation.authority,
        std::string("deploy:secret@example.test:9418"));
}

RUVIA_TEST(http2_connection_encodes_options_asterisk_path) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);

    const auto submitted = client.submitRegularRequestHead(
        "OPTIONS", "https", std::nullopt, "*", {},
        Http2RequestContent::none());
    RUVIA_CHECK(submitted.submitted() != nullptr);
    RUVIA_CHECK_EQ(submittedRequestStreamId(submitted), std::uint32_t{1});
    const auto out = client.pendingOutput();
    const auto frame = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RequestContentLengthObservation observation;
    HpackDecoder decoder(&resource);
    const auto decoded = decoder.decode(
        out.substr(9, frame.length), &observation,
        &observeRequestContentLength);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    RUVIA_CHECK_EQ(observation.path, std::string("*"));
    RUVIA_CHECK_EQ(observation.authorityCount, std::size_t{0});
}

RUVIA_TEST(http2_connection_exposes_negotiated_extended_connect_capability) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    RUVIA_CHECK(!client.peerExtendedConnectEnabled());
    beginPeerInput(client);

    char settings[15];
    auto* out = ruvia::detail::http2WriteFrameHeader(
        settings, 6, Http2FrameType::kSettings, 0, 0);
    out = ruvia::detail::http2WriteSettingsEntry(
        out, ruvia::detail::Http2SettingId::kEnableConnectProtocol, 1);
    RUVIA_CHECK_EQ(out, settings + sizeof(settings));
    RUVIA_CHECK(client.feed(std::string_view(settings, sizeof(settings))) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(client.peerExtendedConnectEnabled());

    Http2Connection server(&resource);
    beginPeerInput(server);
    RUVIA_CHECK(server.feed(std::string_view(settings, sizeof(settings))) ==
        ruvia::detail::Http2FeedResult::kAccepted);
    RUVIA_CHECK(!server.peerExtendedConnectEnabled());
}

RUVIA_TEST(http2_connection_request_known_length_is_exact_and_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto request = client.submitRegularRequestHead(
        "POST",
        "https",
        "example.test",
        "/upload",
        {},
        Http2RequestContent::knownLength(5));
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(client.submitData(streamId, "four", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kContentLengthIncomplete);
    RUVIA_CHECK(client.submitData(streamId, "sixsix", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kContentLengthExceeded);
    RUVIA_CHECK(client.pendingOutput().empty());
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{0});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{0});
    RUVIA_CHECK(stream->localSend().requestContentOpen() != nullptr);

    RUVIA_CHECK(client.submitData(streamId, "he", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    auto out = client.pendingOutput();
    auto data = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(data.length, static_cast<std::uint32_t>(2));
    RUVIA_CHECK((data.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{2});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{2});
    client.consumeOutput(out.size());

    RUVIA_CHECK(client.submitData(streamId, "llo", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    out = client.pendingOutput();
    data = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.length, static_cast<std::uint32_t>(3));
    RUVIA_CHECK((data.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
    RUVIA_CHECK_EQ(stream->localContent().acceptedBytes(), std::uint64_t{5});
    RUVIA_CHECK_EQ(stream->localContent().committedBytes(), std::uint64_t{5});
    RUVIA_CHECK(stream->localContent().lengthComplete());
    RUVIA_CHECK(stream->localSend().requestContentOpen() == nullptr);
    RUVIA_CHECK(stream->localSend().endStreamCommitted() != nullptr);
    client.consumeOutput(out.size());

    RUVIA_CHECK(client.submitData(streamId, "again", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kInvalidState);
    RUVIA_CHECK(client.pendingOutput().empty());
}

RUVIA_TEST(http2_connection_request_streaming_content_has_no_length_contract) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(
        &resource, ruvia::detail::Http2Role::kClient);
    beginClient(client);
    const auto request = client.submitRegularRequestHead(
        "POST",
        "https",
        "example.test",
        "/upload",
        {},
        Http2RequestContent::streaming());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.consumeOutput(client.pendingOutput().size());

    RUVIA_CHECK(client.submitData(streamId, "chunk-a", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    client.consumeOutput(client.pendingOutput().size());
    RUVIA_CHECK(client.submitData(streamId, "chunk-b", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    const auto out = client.pendingOutput();
    const auto data = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(data.length, static_cast<std::uint32_t>(7));
    RUVIA_CHECK((data.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

RUVIA_TEST(http2_connection_interim_head_preserves_final_head_phase) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    const ruvia::HttpHeaderView invalidHeaders[] = {
        {"Content-Length", "0"},
    };
    const ruvia::HttpInterimResponseHead invalidEarlyHints(
        ruvia::http_status::kEarlyHints,
        invalidHeaders);
    RUVIA_CHECK(conn.submitInterimResponseHead(1, invalidEarlyHints) ==
        Http2SubmitStatus::kInvalidMessage);
    RUVIA_CHECK(conn.pendingOutput().empty());

    const ruvia::HttpHeaderView earlyHintHeaders[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::HttpInterimResponseHead earlyHints(
        ruvia::http_status::kEarlyHints, earlyHintHeaders);
    RUVIA_CHECK(conn.submitInterimResponseHead(1, earlyHints) ==
        Http2SubmitStatus::kAccepted);
    const auto informational = conn.pendingOutput();
    const auto infoFrame = ruvia::detail::http2ParseFrameHeader(informational.substr(0, 9));
    RUVIA_CHECK_EQ(infoFrame.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((infoFrame.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(informational.size());

    ruvia::HttpResponse finalResponse(&resource);
    finalResponse.status(ruvia::http_status::kOk);
    const auto finalResult = submitBufferedResponseHead(conn, 1, finalResponse);
    RUVIA_CHECK(responseHeadSubmitted(finalResult));
    const auto finalHead = ruvia::detail::http2ParseFrameHeader(
        conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(finalHead.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((finalHead.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

// submitStreamingResponseHead emits HEADERS with NO Content-Length and leaves the
// stream open; subsequent submitData chunks stream the body, the last with END_STREAM.

RUVIA_TEST(http2_connection_submit_streaming_response_head_and_chunks) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse resp(&resource);
    resp.status(ruvia::http_status::kOk);
    const auto headResult = conn.submitStreamingResponseHead(
        1,
        std::move(resp),
        ruvia::detail::ResponseStreamKind::kGeneric,
        ResponseTrailerIntent::kNone);
    RUVIA_CHECK(responseHeadSubmitted(headResult));

    const auto head = conn.pendingOutput();
    const auto hd = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // stays open
    conn.consumeOutput(head.size());

    RUVIA_CHECK(conn.submitData(1, "chunk1", Http2EndStream::kKeepOpen) ==
        Http2DataSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.submitData(1, "chunk2", Http2EndStream::kEndStream) ==
        Http2DataSubmitStatus::kAccepted);
    const auto body = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(body.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(6));
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    const auto d2 = ruvia::detail::http2ParseFrameHeader(body.substr(9 + 6, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

RUVIA_TEST(http2_connection_streaming_rejects_invalid_content_length_before_head) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    for (const std::string_view invalid : {
             std::string_view{"x"},
             std::string_view{"-1"},
             std::string_view{"5,5"},
             std::string_view{"18446744073709551616"}}) {
        ruvia::HttpResponse response(&resource);
        response.status(ruvia::http_status::kOk);
        response.header("Content-Length", invalid);
        const auto result = conn.submitStreamingResponseHead(
            1,
            std::move(response),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone);
        RUVIA_CHECK(
            responseHeadSubmitFailureMessage(result) ==
            "invalid HTTP/2 response head message");
        RUVIA_CHECK(conn.pendingOutput().empty());
        auto* stream = conn.stream(1);
        RUVIA_CHECK(stream != nullptr);
        RUVIA_CHECK(stream->localSend().headPending() != nullptr);
        RUVIA_CHECK(stream->localContent().unset() != nullptr);
    }

    // A valid retry still owns the initial-head transition.
    ruvia::HttpResponse valid(&resource);
    valid.status(ruvia::http_status::kOk);
    valid.header("Content-Length", "5");
    RUVIA_CHECK(responseHeadSubmitted(
        conn.submitStreamingResponseHead(
            1,
            std::move(valid),
            ruvia::detail::ResponseStreamKind::kGeneric,
            ResponseTrailerIntent::kNone)));
    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(
        requireLocalKnownLength(*stream).declaredLength(), std::uint64_t{5});
}

// A 1xx interim head is validated and skipped (no events, stream not decoded); the
// following 200 head is the one surfaced. Hand-encoded server bytes drive the client.

RUVIA_TEST(http2_connection_client_role_interim_response_skipped) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    client.beginConnection();
    client.consumeOutput(client.pendingOutput().size());

    const auto request = client.submitRegularRequestHead(
        "GET", "http", "example.com", "/", {}, Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    // Server bytes: SETTINGS, then HEADERS(103), then HEADERS(200) + DATA END_STREAM.
    std::pmr::string bytes(&resource);
    {
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        bytes.append(settings, sizeof(settings));
        std::pmr::string interim(&resource);
        HpackEncoder::encodeHeader(interim, ":status", "103");
        HpackEncoder::encodeHeader(interim, "link", "</style.css>; rel=preload");
        const auto interimFrame = headersFrame(
            &resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(interim.data(), interim.size()));
        bytes.append(interimFrame.data(), interimFrame.size());
        std::pmr::string final_(&resource);
        HpackEncoder::encodeHeader(final_, ":status", "200");
        HpackEncoder::encodeHeader(final_, "content-length", "2");
        const auto finalFrame = headersFrame(
            &resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(final_.data(), final_.size()));
        bytes.append(finalFrame.data(), finalFrame.size());
        char data[9 + 2];
        ruvia::detail::http2EncodeFrameHeader(data, 2, Http2FrameType::kData,
            ruvia::detail::kHttp2FlagEndStream, streamId);
        std::memcpy(data + 9, "ok", 2);
        bytes.append(data, sizeof(data));
    }
    (void)client.feed(std::string_view(bytes.data(), bytes.size()));

    int heads = 0;
    std::string body;
    bool end = false;
    while (const auto event = client.nextEvent()) {
        if (event->messageHead() != nullptr) {
            ++heads;
        }
        if (const auto* bodyChunk = event->messageBodyChunk()) {
            body.append(bodyChunk->bytes().data(), bodyChunk->bytes().size());
        }
        if (event->messageEnd() != nullptr) {
            end = true;
        }
    }
    RUVIA_CHECK_EQ(heads, 1);  // only the final head is surfaced
    RUVIA_CHECK(body == "ok");
    RUVIA_CHECK(end);
    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    const auto* responseStatus = stream->responseStatus();
    RUVIA_CHECK(responseStatus != nullptr);
    if (responseStatus != nullptr) {
        RUVIA_CHECK_EQ(*responseStatus, ruvia::http_status::kOk);
    }
    RUVIA_CHECK_EQ(static_cast<int>(stream->interimResponseCount()), 1);
    RUVIA_CHECK(!client.connectionError().has_value());
}

RUVIA_TEST(http2_connection_client_rejects_te_in_every_response_head) {
    constexpr std::array statuses{std::string_view("103"), std::string_view("200")};

    for (const auto status : statuses) {
        std::pmr::monotonic_buffer_resource resource;
        Http2Connection client(&resource, Http2Role::kClient);
        handshake(client);

        const auto request = client.submitRegularRequestHead(
            "GET", "https", "example.test", "/", {},
            Http2RequestContent::none());
        RUVIA_CHECK(request.submitted() != nullptr);
        const auto streamId = submittedRequestStreamId(request);
        client.consumeOutput(client.pendingOutput().size());

        std::pmr::string response(&resource);
        HpackEncoder::encodeHeader(response, ":status", status);
        HpackEncoder::encodeHeader(response, "te", "trailers");
        const auto responseHead = headersFrame(
            &resource,
            streamId,
            static_cast<std::uint8_t>(ruvia::detail::kHttp2FlagEndHeaders |
                (status == "200" ? ruvia::detail::kHttp2FlagEndStream : 0)),
            std::string_view(response.data(), response.size()));
        RUVIA_CHECK(client.feed(
            std::string_view(responseHead.data(), responseHead.size())) ==
            Http2FeedResult::kAccepted);

        bool sawClosed = false;
        while (const auto event = client.nextEvent()) {
            RUVIA_CHECK(event->messageHead() == nullptr);
            RUVIA_CHECK(event->messageEnd() == nullptr);
            if (const auto* closed = event->streamClosed()) {
                sawClosed = true;
                RUVIA_CHECK(closed->source() ==
                    ruvia::detail::Http2StreamCloseSource::kLocal);
                RUVIA_CHECK(closed->error() == Http2ErrorCode::kProtocolError);
            }
        }
        RUVIA_CHECK(sawClosed);
        RUVIA_CHECK(client.stream(streamId) == nullptr);
        RUVIA_CHECK(!client.connectionError().has_value());

        const auto resetBytes = client.pendingOutput();
        RUVIA_CHECK_EQ(resetBytes.size(), static_cast<std::size_t>(13));
        const auto reset = ruvia::detail::http2ParseFrameHeader(
            resetBytes.substr(0, 9));
        RUVIA_CHECK_EQ(
            reset.type,
            static_cast<std::uint8_t>(Http2FrameType::kRstStream));
        RUVIA_CHECK_EQ(
            ruvia::detail::http2Read32(
                reinterpret_cast<const unsigned char*>(
                    resetBytes.data() + 9)),
            static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
    }
}

RUVIA_TEST(http2_connection_client_accepts_repeated_websocket_version_response_fields) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, Http2Role::kClient);
    handshake(client);

    const auto request = client.submitRegularRequestHead(
        "GET", "https", "example.test", "/", {},
        Http2RequestContent::none());
    RUVIA_CHECK(request.submitted() != nullptr);
    const auto streamId = submittedRequestStreamId(request);
    client.pinStream(streamId);
    client.consumeOutput(client.pendingOutput().size());

    std::pmr::string response(&resource);
    HpackEncoder::encodeHeader(response, ":status", "400");
    HpackEncoder::encodeHeader(response, "sec-websocket-version", "13");
    HpackEncoder::encodeHeader(response, "sec-websocket-version", "8, 7");
    const auto responseHead = headersFrame(
        &resource,
        streamId,
        ruvia::detail::kHttp2FlagEndHeaders |
            ruvia::detail::kHttp2FlagEndStream,
        std::string_view(response.data(), response.size()));
    RUVIA_CHECK(client.feed(
        std::string_view(responseHead.data(), responseHead.size())) ==
        Http2FeedResult::kAccepted);

    bool sawHead = false;
    bool sawEnd = false;
    bool sawClosed = false;
    while (const auto event = client.nextEvent()) {
        sawHead = sawHead || event->messageHead() != nullptr;
        sawEnd = sawEnd || event->messageEnd() != nullptr;
        sawClosed = sawClosed || event->streamClosed() != nullptr;
    }
    RUVIA_CHECK(sawHead);
    RUVIA_CHECK(sawEnd);
    RUVIA_CHECK(!sawClosed);
    RUVIA_CHECK(!client.connectionError().has_value());
    client.unpinStream(streamId);
}
