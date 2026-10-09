#include "http3_client_connection_fixture.h"

namespace {

ruvia::Task<void> exerciseRealResponse(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, local_quic_response_peer& peer,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientResponseState response(worker, &memory);
            response.bufferedLimit = 6;
            ruvia::detail::HttpClientRequestStorage request("GET", "/incremental", &memory);
            const auto deadline = std::chrono::steady_clock::now() + 12s;
            const auto submitted = connection.submit(std::move(request), response, deadline);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(response.references == 2);
            connection.start();

            std::exception_ptr failure;
            try {
                const bool gotHead = co_await wait_for_peer(worker, peer, [&] { return peer.first_part_ready() && response.headReady; }, 8s);
                RUVIA_CHECK(gotHead);
                if (gotHead) {
                    RUVIA_CHECK(response.responseBodyPlan.has_value());
                    RUVIA_CHECK_EQ(response.status.value(), 200);
                    RUVIA_CHECK_EQ(response.headers.size(), std::size_t{1});
                    if (!response.headers.empty()) {
                        RUVIA_CHECK_EQ(response.headers.front().name(), std::string_view("content-length"));
                        RUVIA_CHECK_EQ(response.headers.front().value(), std::string_view("6"));
                    }

                    auto first = co_await response.consume_body<std::string_view>();
                    RUVIA_CHECK(first.has_value());
                    if (first) {
                        RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                        peer.allow_final_part();
                        const bool gotFinal = co_await wait_for_peer(worker, peer, [&] { return peer.final_part_sent() && response.pending.size() == 3; }, 3s);
                        RUVIA_CHECK(gotFinal);
                        RUVIA_CHECK_EQ(*first, std::string_view("abc"));
                        if (gotFinal) {
                            auto second = co_await response.consume_body<std::string_view>();
                            RUVIA_CHECK(second.has_value());
                            if (second) {
                                RUVIA_CHECK_EQ(*second, std::string_view("def"));
                            }
                            co_await connection.wait(submitted.id);
                            RUVIA_CHECK(response.complete);
                            const auto completed = connection.result(submitted.id);
                            RUVIA_CHECK(completed != nullptr);
                            if (completed != nullptr) {
                                RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
                                RUVIA_CHECK(completed->responseBodyPlan.has_value());
                                RUVIA_CHECK_EQ(completed->status, std::uint16_t{200});
                            }
                        }
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }

            connection.requestStop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            connection.consumerReleased(submitted.id);
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            RUVIA_CHECK(response.references == 1);
            RUVIA_CHECK(response.http3Connection == nullptr && response.http3RequestId == 0);
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseResponseReleaseAcrossGenerations(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& firstPeer, local_quic_response_peer& secondPeer,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource firstMemory;
        CountingResource secondMemory;
        {
            ruvia::detail::Http3ClientBodyBudget bodyBudget(7);
            {
                ruvia::detail::HttpClientResponseState firstState(worker, &firstMemory);
                ruvia::detail::HttpClientResponseState secondState(worker, &secondMemory);
                std::string_view bodyView;
                std::string_view headerView;
                std::string_view trailerView;
                {
                    ruvia::detail::ClientTransportConfigView config{
                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
                    ruvia::detail::http3_quic_client_tls_context tls(config);
                    ruvia::TaskScope tasks(worker, {.resource = &firstMemory});
                    {
                        Connection connection(io, worker, tasks, tls,
                            ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = firstPeer.port()}),
                            10s, &firstMemory, 4, 16 * 1024 * 1024, 30s, &bodyBudget);
                        ConnectionWatchdog watchdog(io, connection);
                        firstState.bufferedLimit = 7;
                        const auto submitted = connection.submit(
                            ruvia::detail::HttpClientRequestStorage("GET", "/retain-response", &firstMemory),
                            firstState);
                        RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
                        connection.start();

                        std::exception_ptr failure;
                        try {
                            const bool gotHead = co_await wait_for_peer(worker, firstPeer, [&] { return firstPeer.first_part_ready() && firstState.headReady; }, 8s);
                            RUVIA_CHECK(gotHead);
                            firstPeer.allow_final_part();
                            const bool completed = co_await wait_for_peer(worker, firstPeer, [&] { return firstPeer.final_part_sent() && firstState.complete; }, 5s);
                            RUVIA_CHECK(completed);
                        } catch (...) {
                            failure = std::current_exception();
                        }

                        connection.requestStop();
                        try {
                            co_await connection.wait(submitted.id);
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        try {
                            co_await tasks.join();
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        watchdog.disarm();
                        RUVIA_CHECK(!watchdog.expired());
                        RUVIA_CHECK(connection.result(submitted.id) != nullptr);
                        RUVIA_CHECK(firstState.complete);
                        RUVIA_CHECK_EQ(firstState.status.value(), 200);
                        RUVIA_CHECK_EQ(firstState.protocolVersion, ruvia::HttpProtocolVersion::kHttp3);
                        RUVIA_CHECK_EQ(firstState.headers.size(), std::size_t{1});
                        RUVIA_CHECK_EQ(firstState.pending, std::string_view("abcdef"));
                        firstState.trailers.push_back(
                            ruvia::HttpHeader::copyOf("x-retained", "trailer", &firstMemory));
                        headerView = firstState.headers.front().value();
                        trailerView = firstState.trailers.front().value();
                        RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{6});
                        RUVIA_CHECK(connection.releaseResponseRequest(submitted.id));
                        RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{6});
                        RUVIA_CHECK(!firstState.errorCode && firstState.failure == nullptr);
                        RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
                        RUVIA_CHECK_EQ(firstState.references, std::size_t{1});
                        RUVIA_CHECK(firstState.http3Connection == nullptr);
                        if (failure != nullptr) {
                            std::rethrow_exception(failure);
                        }
                    }
                }

                RUVIA_CHECK(firstState.complete);
                RUVIA_CHECK_EQ(firstState.status.value(), 200);
                RUVIA_CHECK_EQ(firstState.protocolVersion, ruvia::HttpProtocolVersion::kHttp3);
                RUVIA_CHECK_EQ(firstState.headers.front().value(), headerView);
                RUVIA_CHECK_EQ(firstState.trailers.front().value(), trailerView);
                {
                    auto firstBodyRead = co_await firstState.consume_body<std::string_view>();
                    RUVIA_CHECK(firstBodyRead.has_value());
                    if (firstBodyRead) {
                        bodyView = *firstBodyRead;
                        RUVIA_CHECK_EQ(bodyView, std::string_view("abcdef"));
                    }
                }
                RUVIA_CHECK_EQ(firstState.offset, std::size_t{6});
                RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{6});
                RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{6});

                {
                    ruvia::detail::ClientTransportConfigView config{
                        .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
                    ruvia::detail::http3_quic_client_tls_context tls(config);
                    ruvia::TaskScope tasks(worker, {.resource = &secondMemory});
                    {
                        Connection connection(io, worker, tasks, tls,
                            ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = secondPeer.port()}),
                            10s, &secondMemory, 4, 16 * 1024 * 1024, 30s, &bodyBudget);
                        ConnectionWatchdog watchdog(io, connection);
                        const auto submitted = connection.submit(
                            ruvia::detail::HttpClientRequestStorage("GET", "/next-generation", &secondMemory),
                            secondState);
                        RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
                        connection.start();

                        std::exception_ptr failure;
                        try {
                            const bool blockedAtBudget = co_await wait_for_peer(worker, secondPeer, [&] { return secondPeer.first_part_ready() && secondState.headReady &&
                                                                                                                 secondState.producerBodyBytes() == 1; }, 8s);
                            RUVIA_CHECK(blockedAtBudget);
                            RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{7});
                            RUVIA_CHECK_EQ(bodyView, std::string_view("abcdef"));
                            RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{6});
                            bodyView = {};
                            firstState.releaseConsumedBodyPrefix();
                            RUVIA_CHECK(firstState.buffered.empty());
                            RUVIA_CHECK_EQ(firstState.offset, std::size_t{0});
                            RUVIA_CHECK_EQ(firstState.http3BodyBudget.retainedBytes(), std::size_t{0});
                            RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{1});
                            secondPeer.allow_final_part();
                            const bool completed = co_await wait_for_peer(worker, secondPeer, [&] { return secondPeer.final_part_sent() && secondState.complete; }, 5s);
                            RUVIA_CHECK(completed);
                        } catch (...) {
                            failure = std::current_exception();
                        }

                        connection.requestStop();
                        try {
                            co_await connection.wait(submitted.id);
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        try {
                            co_await tasks.join();
                        } catch (...) {
                            if (failure == nullptr) {
                                failure = std::current_exception();
                            }
                        }
                        watchdog.disarm();
                        RUVIA_CHECK(!watchdog.expired());
                        RUVIA_CHECK(connection.result(submitted.id) != nullptr);
                        RUVIA_CHECK(connection.releaseResponseRequest(submitted.id));
                        RUVIA_CHECK_EQ(secondState.http3BodyBudget.retainedBytes(), std::size_t{6});
                        RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
                        RUVIA_CHECK_EQ(secondState.references, std::size_t{1});
                        if (failure != nullptr) {
                            std::rethrow_exception(failure);
                        }
                    }
                }
                RUVIA_CHECK(secondState.complete);
                RUVIA_CHECK_EQ(secondState.pending, std::string_view("abcdef"));
                RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{6});
                secondState.discardResponseBody();
                RUVIA_CHECK_EQ(bodyBudget.used(), std::size_t{0});
            }
        }
        RUVIA_CHECK_EQ(firstMemory.allocations, firstMemory.returns);
        RUVIA_CHECK_EQ(firstMemory.liveBytes, std::size_t{0});
        RUVIA_CHECK_EQ(secondMemory.allocations, secondMemory.returns);
        RUVIA_CHECK_EQ(secondMemory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseConnectionErrorPriority(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        CountingResource response_memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientResponseState response(worker, &response_memory);
            ruvia::detail::HttpClientRequestStorage request("GET", "/protocol-priority", &memory);
            const auto deadline = std::chrono::steady_clock::now() + 12s;
            const auto submitted = connection.submit(std::move(request), response, deadline);
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();

            std::exception_ptr failure;
            try {
                const bool gotHead = co_await wait_for_peer(worker, peer, [&] { return peer.first_part_ready() && response.headReady; }, 8s);
                RUVIA_CHECK(gotHead);
                if (gotHead) {
                    RUVIA_CHECK_EQ(response.pending, std::string_view("abc"));
                    response_memory.rejectAllocations();
                    peer.allow_final_part();
                    const bool terminal = co_await wait_for_peer(
                        worker, peer, [&] { return response.complete; }, 4s);
                    RUVIA_CHECK(terminal);
                    if (terminal) {
                        RUVIA_CHECK(response_memory.rejectedAllocations != 0);
                        RUVIA_CHECK(response.errorCode.has_value());
                        RUVIA_CHECK_EQ(*response.errorCode,
                            static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kProtocolError));
                        RUVIA_CHECK(response.failure == nullptr);
                        const auto result = connection.result(submitted.id);
                        RUVIA_CHECK(result != nullptr);
                        if (result != nullptr) {
                            RUVIA_CHECK(result->outcome == Connection::Outcome::kProtocolError);
                        }
                    }
                }
            } catch (...) {
                failure = std::current_exception();
            }

            response_memory.rejectAllocations(false);
            connection.requestStop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            response.discardResponseBody();
            RUVIA_CHECK(response.errorCode.has_value());
            RUVIA_CHECK_EQ(*response.errorCode,
                static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kProtocolError));
            RUVIA_CHECK(connection.releaseResponseRequest(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            RUVIA_CHECK(response.references == 1);
            RUVIA_CHECK(response.failure == nullptr);
            RUVIA_CHECK_EQ(response.protocolVersion, ruvia::HttpProtocolVersion::kHttp3);
            RUVIA_CHECK_EQ(response.status.value(), 200);
            RUVIA_CHECK_EQ(response.headers.size(), std::size_t{1});
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
        RUVIA_CHECK_EQ(response_memory.allocations, response_memory.returns);
        RUVIA_CHECK_EQ(response_memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseParserRegistrationAllocationFailure(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource registrationMemory;
        CountingResource::Allocation registrationAllocation{};
        {
            ruvia::detail::Http3ClientSansIoSessionEngine parser(&registrationMemory);
            registrationMemory.beginTrace();
            const auto registered = parser.registerRequest(0, ruvia::HttpKnownMethod::kGet);
            RUVIA_CHECK(registered.scope == ruvia::Http3ConnectionErrorScope::kNone);
            const auto allocation = registrationMemory.firstAllocation();
            RUVIA_CHECK(allocation.has_value());
            if (!allocation) {
                throw std::runtime_error("HTTP/3 parser registration made no PMR allocation");
            }
            registrationAllocation = *allocation;
            (void)parser.stop();
        }
        RUVIA_CHECK_EQ(registrationMemory.allocations, registrationMemory.returns);
        RUVIA_CHECK_EQ(registrationMemory.liveBytes, std::size_t{0});

        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
                &memory, 4, 16 * 1024 * 1024);
            ConnectionWatchdog watchdog(io, connection);
            const auto first = connection.submit(
                ruvia::detail::HttpClientRequestStorage("GET", "/parser-allocation", &memory));
            const auto sibling = connection.submit(
                ruvia::detail::HttpClientRequestStorage("GET", "/sibling-must-not-write", &memory));
            RUVIA_CHECK(first.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(sibling.outcome == Connection::Outcome::kPending);
            // registerRequest()'s first allocation is inside responses_.try_emplace().
            // The request driver opens its QUIC stream before invoking this callback,
            // and emits HEADERS only after it returns.
            memory.failFirstAllocation(registrationAllocation);
            connection.start();

            std::exception_ptr failure;
            try {
                co_await connection.wait(first.id);
                co_await connection.wait(sibling.id);
            } catch (...) {
                failure = std::current_exception();
            }
            if (failure != nullptr) {
                connection.requestStop();
            }
            try {
                co_await tasks.join();
            } catch (...) {
                if (failure == nullptr) {
                    failure = std::current_exception();
                }
            }
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            RUVIA_CHECK(memory.rejectedAllocations == 1);
            RUVIA_CHECK(memory.lastRejectedAllocation() == registrationAllocation);
            RUVIA_CHECK(memory.matchingAllocationAttempts() == 1);
            RUVIA_CHECK(!connection.running());
            RUVIA_CHECK(peer.synchronize());
            // Registration failure can close the client before the peer has
            // confirmed the TLS handshake. Only the no-request-payload contract
            // is synchronized here; peer handshake readiness is not guaranteed.
            RUVIA_CHECK_EQ(peer.request_payload_bytes(), std::size_t{0});

            const auto firstResult = connection.result(first.id);
            const auto siblingResult = connection.result(sibling.id);
            RUVIA_CHECK(firstResult != nullptr &&
                        firstResult->outcome == Connection::Outcome::kTransportError);
            RUVIA_CHECK(siblingResult != nullptr &&
                        siblingResult->outcome == Connection::Outcome::kTransportError);
            RUVIA_CHECK(firstResult != nullptr && !firstResult->responseBodyPlan);
            RUVIA_CHECK(siblingResult != nullptr && !siblingResult->responseBodyPlan);
            RUVIA_CHECK(connection.submit(ruvia::detail::HttpClientRequestStorage(
                                              "GET", "/must-not-reuse", &memory))
                            .outcome == Connection::Outcome::kConnectionDraining);
            RUVIA_CHECK(connection.release(first.id));
            RUVIA_CHECK(connection.release(sibling.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            if (failure != nullptr) {
                std::rethrow_exception(failure);
            }
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

}  // namespace

RUVIA_TEST(http3ClientConnectionResponsePlanMovesAcrossResourceHandoffAsAnOwnedValue) {
    CountingResource source;
    CountingResource target;
    {
        Connection::Response original(&source);
        original.outcome = Connection::Outcome::kComplete;
        original.status = 200;
        original.responseBodyPlan = ruvia::planHttpResponseBody(
            ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk);
        original.headers.push_back(ruvia::HttpHeader::copyOf("x-result", "owned", &source));
        original.trailers.push_back(ruvia::HttpHeader::copyOf("x-trailer", "owned", &source));
        original.body.assign("representation");

        Connection::Response moved(std::move(original));
        Connection::Response handedOff(&target);
        handedOff = std::move(moved);
        RUVIA_CHECK(handedOff.outcome == Connection::Outcome::kComplete);
        RUVIA_CHECK(handedOff.responseBodyPlan &&
                    handedOff.responseBodyPlan->requestMethod() == ruvia::HttpKnownMethod::kHead &&
                    handedOff.responseBodyPlan->responseStatus() == ruvia::http_status::kOk &&
                    handedOff.responseBodyPlan->bodySuppressed());
        RUVIA_CHECK(handedOff.headers.size() == 1 && handedOff.headers.front().value() == "owned");
        RUVIA_CHECK(handedOff.trailers.size() == 1 && handedOff.trailers.front().value() == "owned");
        RUVIA_CHECK(handedOff.body == "representation");
    }
    RUVIA_CHECK(source.allocations == source.returns && source.liveBytes == 0);
    RUVIA_CHECK(target.allocations == target.returns && target.liveBytes == 0);
}

RUVIA_TEST(http3ClientConnectionPublishesRealQuicResponseIncrementallyAndReclaimsRequestState) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseRealResponse(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3ClientPoolPublishesIncrementalResponseThroughPublicResponseApi) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercisePublicHttp3ClientPool(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3ClientConnectionResponseReleasePreservesDataAndWakesTheNextBudgetGeneration) {
    TestIdentityFiles identity;
    local_quic_response_peer firstPeer(identity);
    local_quic_response_peer secondPeer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseResponseReleaseAcrossGenerations(
        io, worker, attachment, firstPeer, secondPeer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3ClientConnectionParserRegistrationAllocationFailureClosesAndJoinsTheConnection) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exerciseParserRegistrationAllocationFailure(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3ClientConnectionProtocolErrorWinsOverCallbackAllocationFailureInOneFeed) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, true);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exerciseConnectionErrorPriority(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
}

RUVIA_TEST(http3_public_client_pool_preserves_owned_response_and_priority) {
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(exercisePublicHttp3ClientPool(
        io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
    peer.rethrow_if_failed();
}
