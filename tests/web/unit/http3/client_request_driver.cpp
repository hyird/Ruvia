#include <array>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"

#include "client/HttpClientUploadState.h"
#include "http3/Http3ClientRequestDriver.h"
#include "http3/Http3ClientSansIoSessionEngine.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using Driver = ruvia::detail::Http3ClientRequestDriver;

std::optional<ruvia::detail::Http3ClientRequestWrite> makeRequest(
    std::pmr::memory_resource* pool, std::string_view body = "payload") {
    ruvia::detail::HttpClientRequestStorage storage("POST", "/upload", pool);
    storage.setBody(body);
    auto created = ruvia::detail::Http3ClientRequestWrite::create(
        std::move(storage), "https", "example.com", pool);
    if (!created) {
        return std::nullopt;
    }
    return std::move(*created);
}

struct Received final {
    std::string method;
    std::string path;
    std::string body;
    int finished{};
    std::string trailer;
};

void receive(void* context, const ruvia::Http3ConnectionEvent& event) {
    auto& received = *static_cast<Received*>(context);
    switch (event.kind) {
        case ruvia::Http3ConnectionEventKind::kRequestHead:
            received.method = event.head->method;
            received.path = event.head->path;
            break;
        case ruvia::Http3ConnectionEventKind::kBody:
            received.body.append(event.body.data(), event.body.size());
            break;
        case ruvia::Http3ConnectionEventKind::kTrailerField:
            received.trailer = std::string(event.trailer.name) + ":" + std::string(event.trailer.value);
            break;
        case ruvia::Http3ConnectionEventKind::kMessageEnd:
            ++received.finished;
            break;
        default:
            break;
    }
}
}  // namespace

RUVIA_TEST(http3ClientRequestDriverRebuildsRejectedEarlyRequestOnFreshStream) {
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::HttpClientRequestStorage storage("GET", "/safe", &pool);
    auto created = ruvia::detail::Http3ClientRequestWrite::create(
        std::move(storage), "https", "example.com", &pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    std::array<std::string, 2> wires;
    std::size_t attempt{};
    std::size_t registrations{};
    auto open = [&] {
        return ruvia::quic_stream_open_result{
            .status = ruvia::quic_operation_status::accepted,
            .stream_id = attempt == 0 ? 0U : 4U};
    };
    auto register_response = [&](std::uint64_t id, ruvia::HttpKnownMethod method) {
        RUVIA_CHECK_EQ(id, attempt == 0 ? 0U : 4U);
        RUVIA_CHECK(method == ruvia::HttpKnownMethod::kGet);
        ++registrations;
        return true;
    };
    auto write = [&](std::uint64_t, std::span<const char> bytes) {
        wires[attempt].append(bytes.data(), bytes.size());
        return ruvia::quic_stream_write_result{
            .status = ruvia::quic_operation_status::accepted, .accepted = bytes.size()};
    };
    auto finish = [](std::uint64_t) { return ruvia::quic_operation_status::accepted; };
    for (int i = 0; i < 8 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive(open, register_response, write, finish) != Driver::Result::kFatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK_EQ(registrations, 1U);
    RUVIA_CHECK(driver.replay_after_rejected_early_stream("https", "example.com", &pool));
    attempt = 1;
    for (int i = 0; i < 8 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive(open, register_response, write, finish) != Driver::Result::kFatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK_EQ(driver.streamId(), std::optional<Driver::StreamId>{4});
    RUVIA_CHECK_EQ(registrations, 2U);
    RUVIA_CHECK(!wires[0].empty() && wires[0] == wires[1]);
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kServer, &pool);
    Received received;
    RUVIA_CHECK(peer.feed(4, wires[1], true, false, receive, &received).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(received.method == "GET" && received.path == "/safe");
    RUVIA_CHECK_EQ(received.finished, 1);
}

RUVIA_TEST(http3ClientRequestDriverRetriesExactWantBytesAndFinishesAfterPayload) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = makeRequest(&pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    std::string wire;
    std::string pendingBytes;
    const char* pendingAddress = nullptr;
    std::size_t pendingSize{};
    int openAttempts{};
    int registrations{};
    int finishAttempts{};
    bool acceptedInitialPart{};
    bool wantIssued{};

    auto open = [&]() -> ruvia::quic_stream_open_result {
        ++openAttempts;
        if (openAttempts == 1) {
            return {.status = ruvia::quic_operation_status::would_block};
        }
        return {.status = ruvia::quic_operation_status::accepted, .stream_id = 0};
    };
    auto registerResponse = [&](std::uint64_t id, ruvia::HttpKnownMethod method) {
        RUVIA_CHECK_EQ(id, 0U);
        RUVIA_CHECK(method == ruvia::HttpKnownMethod::kPost);
        ++registrations;
        RUVIA_CHECK(wire.empty());
        return true;
    };
    auto write = [&](std::uint64_t id, std::span<const char> offered) -> ruvia::quic_stream_write_result {
        RUVIA_CHECK_EQ(id, 0U);
        RUVIA_CHECK_EQ(registrations, 1);
        if (!acceptedInitialPart) {
            wire.push_back(offered.front());
            acceptedInitialPart = true;
            return {.status = ruvia::quic_operation_status::accepted, .accepted = 1};
        }
        if (!wantIssued) {
            pendingAddress = offered.data();
            pendingSize = offered.size();
            pendingBytes.assign(offered.data(), offered.size());
            wantIssued = true;
            return {.status = ruvia::quic_operation_status::would_block};
        }
        if (pendingAddress) {
            RUVIA_CHECK(offered.data() == pendingAddress);
            RUVIA_CHECK_EQ(offered.size(), pendingSize);
            RUVIA_CHECK(std::string_view(offered.data(), offered.size()) == pendingBytes);
            pendingAddress = nullptr;
        }
        wire.append(offered.data(), offered.size());
        return {.status = ruvia::quic_operation_status::accepted, .accepted = offered.size()};
    };
    auto finish = [&](std::uint64_t id) {
        RUVIA_CHECK_EQ(id, 0U);
        ++finishAttempts;
        RUVIA_CHECK(!wire.empty());
        return finishAttempts == 1 ? ruvia::quic_operation_status::would_block : ruvia::quic_operation_status::accepted;
    };
    RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) == Driver::Result::kBlocked);
    RUVIA_CHECK(!driver.streamId());
    for (int i = 0; i < 128 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) != Driver::Result::kFatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK_EQ(openAttempts, 2);
    RUVIA_CHECK_EQ(registrations, 1);
    RUVIA_CHECK_EQ(finishAttempts, 2);
    RUVIA_CHECK(wantIssued && pendingAddress == nullptr);

    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kServer, &pool);
    Received received;
    const auto result = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(result.status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(received.method == "POST" && received.path == "/upload");
    RUVIA_CHECK(received.body == "payload" && received.finished == 1);
}

RUVIA_TEST(http3ClientRequestDriverConsumesEveryFrameAcrossSingleByteAcknowledgements) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = makeRequest(&pool, "repeated");
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    std::string wire;
    std::string pending;
    const char* pendingAddress = nullptr;
    int registrations{};
    int wants{};
    int accepted{};
    auto open = [] { return ruvia::quic_stream_open_result{.status = ruvia::quic_operation_status::accepted, .stream_id = 0}; };
    auto registerResponse = [&](std::uint64_t, ruvia::HttpKnownMethod method) {
        RUVIA_CHECK(method == ruvia::HttpKnownMethod::kPost);
        ++registrations;
        return true;
    };
    auto write = [&](std::uint64_t, std::span<const char> offered) -> ruvia::quic_stream_write_result {
        if (pendingAddress == nullptr) {
            pendingAddress = offered.data();
            pending.assign(offered.data(), offered.size());
            ++wants;
            return {.status = ruvia::quic_operation_status::would_block};
        }
        RUVIA_CHECK(offered.data() == pendingAddress);
        RUVIA_CHECK(std::string_view(offered.data(), offered.size()) == pending);
        pendingAddress = nullptr;
        wire.push_back(offered.front());
        ++accepted;
        return {.status = ruvia::quic_operation_status::accepted, .accepted = 1};
    };
    auto finish = [](std::uint64_t) { return ruvia::quic_operation_status::accepted; };
    for (int i = 0; i < 512 && !driver.finished() && !driver.failed(); ++i) {
        (void)driver.drive(open, registerResponse, write, finish);
    }
    RUVIA_CHECK(driver.finished() && !driver.failed());
    RUVIA_CHECK_EQ(registrations, 1);
    RUVIA_CHECK_EQ(wants, accepted);
    RUVIA_CHECK(pendingAddress == nullptr);
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kServer, &pool);
    Received received;
    const auto result = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(result.status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(received.body == "repeated" && received.finished == 1);
}

RUVIA_TEST(http3ClientRequestDriverCanDeferUnopenedRequestToFreshConnection) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = makeRequest(&pool, "fresh");
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    int registrations{};
    std::string wire;
    auto registerResponse = [&](std::uint64_t, ruvia::HttpKnownMethod) {
        ++registrations;
        return true;
    };
    auto write = [&](std::uint64_t id, std::span<const char> bytes) {
        RUVIA_CHECK_EQ(id, 8U);
        wire.append(bytes.data(), bytes.size());
        return ruvia::quic_stream_write_result{.status = ruvia::quic_operation_status::accepted,
            .accepted = bytes.size()};
    };
    auto finish = [](std::uint64_t) { return ruvia::quic_operation_status::accepted; };
    RUVIA_CHECK(driver.drive([] { return ruvia::quic_stream_open_result{
                                      .status = ruvia::quic_operation_status::draining}; },
                    registerResponse, write, finish) == Driver::Result::kConnectionDraining);
    RUVIA_CHECK(!driver.streamId() && !driver.failed());
    RUVIA_CHECK_EQ(registrations, 0);
    RUVIA_CHECK(wire.empty());
    auto request = driver.takeRequestAfterRetirement();
    RUVIA_CHECK(request && request->body() == "fresh");
    RUVIA_CHECK(driver.failed() && !driver.takeRequestAfterRetirement());
    auto rebuilt = ruvia::detail::Http3ClientRequestWrite::create(
        std::move(*request), "https", "example.com", &pool);
    RUVIA_CHECK(rebuilt.has_value());
    Driver replacement(std::move(*rebuilt));
    for (int i = 0; i < 32 && !replacement.finished(); ++i) {
        RUVIA_CHECK(replacement.drive([] { return ruvia::quic_stream_open_result{.status = ruvia::quic_operation_status::accepted, .stream_id = 8}; },
                        registerResponse, write, finish) != Driver::Result::kFatal);
    }
    RUVIA_CHECK(replacement.finished());
    RUVIA_CHECK_EQ(registrations, 1);
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kServer, &pool);
    Received received;
    RUVIA_CHECK(peer.feed(8, wire, true, false, receive, &received).status ==
                ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(received.body == "fresh" && received.finished == 1);
}

RUVIA_TEST(http3ClientRequestDriverNeverWritesBeforeResponseRegistration) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = makeRequest(&pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    int writes{};
    auto result = driver.drive([] { return ruvia::quic_stream_open_result{.status = ruvia::quic_operation_status::accepted, .stream_id = 4}; },
        [](std::uint64_t, ruvia::HttpKnownMethod) { return false; },
        [&](std::uint64_t, std::span<const char>) -> ruvia::quic_stream_write_result {
            ++writes;
            return {.status = ruvia::quic_operation_status::accepted, .accepted = 1};
        },
        [](std::uint64_t) { return ruvia::quic_operation_status::accepted; });
    RUVIA_CHECK(result == Driver::Result::kFatal);
    RUVIA_CHECK(driver.failed());
    RUVIA_CHECK(driver.streamId() == 4);
    RUVIA_CHECK_EQ(writes, 0);
}

RUVIA_TEST(http3ClientRequestDriverWritesExplicitHeadContentButForbidsResponsePayload) {
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::HttpClientRequestStorage storage("HEAD", "/resource", &pool);
    storage.setBody("explicit-content");
    auto created = ruvia::detail::Http3ClientRequestWrite::create(
        std::move(storage), "https", "example.com", &pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    std::string wire;
    ruvia::Http3Connection responses(ruvia::Http3PeerRole::kClient, &pool);
    auto registerResponse = [&](std::uint64_t id, ruvia::HttpKnownMethod method) {
        RUVIA_CHECK(method == ruvia::HttpKnownMethod::kHead);
        return responses.registerClientRequest(id, method).scope == ruvia::Http3ConnectionErrorScope::kNone;
    };
    auto write = [&](std::uint64_t, std::span<const char> offered) {
        wire.append(offered.data(), offered.size());
        return ruvia::quic_stream_write_result{.status = ruvia::quic_operation_status::accepted,
            .accepted = offered.size()};
    };
    for (int i = 0; i < 16 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive([] { return ruvia::quic_stream_open_result{.status = ruvia::quic_operation_status::accepted, .stream_id = 0}; },
                        registerResponse, write,
                        [](std::uint64_t) { return ruvia::quic_operation_status::accepted; }) !=
                    Driver::Result::kFatal);
    }
    RUVIA_CHECK(driver.finished());
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kServer, &pool);
    Received received;
    const auto result = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(result.status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK(received.method == "HEAD" && received.body == "explicit-content");
    // A HEAD request body must not weaken the independently registered
    // response rules: HEADERS(:status=200) followed by DATA("x") is invalid.
    constexpr std::array<char, 8> invalidResponse{1, 3, 0, 0, static_cast<char>(0xd9), 0, 1, 'x'};
    const auto response = responses.feed(0, invalidResponse, true, false, [](void*, const ruvia::Http3ConnectionEvent&) {}, nullptr);
    RUVIA_CHECK(response.scope == ruvia::Http3ConnectionErrorScope::kStream);
    RUVIA_CHECK(response.code == ruvia::Http3ConnectionErrorCode::kMessageError);
}

RUVIA_TEST(http3ClientRequestDriverFinFailureCannotCommitFinish) {
    std::pmr::unsynchronized_pool_resource pool;
    auto created = makeRequest(&pool, "");
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    auto open = [] { return ruvia::quic_stream_open_result{.status = ruvia::quic_operation_status::accepted, .stream_id = 0}; };
    auto registerResponse = [](std::uint64_t, ruvia::HttpKnownMethod) { return true; };
    auto write = [](std::uint64_t, std::span<const char> offered) {
        return ruvia::quic_stream_write_result{.status = ruvia::quic_operation_status::accepted,
            .accepted = offered.size()};
    };
    auto finish = [](std::uint64_t) { return ruvia::quic_operation_status::closing; };
    for (int i = 0; i < 16 && !driver.failed(); ++i) {
        (void)driver.drive(open, registerResponse, write, finish);
    }
    RUVIA_CHECK(driver.failed() && !driver.finished());
}

RUVIA_TEST(http3ClientRequestDriverStreamsBoundedChunksAndTrailingHeadersAfterContinueGate) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    auto run = [&]() -> ruvia::Task<void> {
        std::pmr::unsynchronized_pool_resource pool;
        const auto worker = attachment.loop().handle();
        ruvia::detail::HttpClientUploadState upload(worker, &pool,
            {.contentLength = 6, .expectation = ruvia::HttpClientRequestExpectation::kContinue});
        ruvia::detail::HttpClientRequestStorage storage("POST", "/upload", &pool);
        storage.bindUpload(upload);
        auto created = ruvia::detail::Http3ClientRequestWrite::create(std::move(storage), "https", "example.test", &pool);
        RUVIA_CHECK(created.has_value());
        if (!created) {
            attachment.stop();
            co_return;
        }
        Driver driver(std::move(*created));
        std::string wire;
        auto open = [] { return ruvia::quic_stream_open_result{.status = ruvia::quic_operation_status::accepted, .stream_id = 0}; };
        auto registerResponse = [](auto, auto) { return true; };
        auto write = [&](auto, std::span<const char> bytes) {
            wire.append(bytes.data(), bytes.size());
            return ruvia::quic_stream_write_result{.status = ruvia::quic_operation_status::accepted, .accepted = bytes.size()};
        };
        int fins{};
        auto finish = [&](auto) { ++fins; return ruvia::quic_operation_status::accepted; };
        RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) == Driver::Result::kProgress);
        const auto headSize = wire.size();
        upload.output.chunk.assign("abc");
        upload.output.chunkReady = true;
        RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) == Driver::Result::kBlocked);
        RUVIA_CHECK(driver.waitingForContent());
        RUVIA_CHECK_EQ(wire.size(), headSize);
        upload.contentReleased = true;
        for (int tick = 0; tick < 4 && upload.output.chunkReady; ++tick) {
            RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) != Driver::Result::kFatal);
        }
        RUVIA_CHECK(!upload.output.chunkReady && upload.output.chunk.empty());
        RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) == Driver::Result::kBlocked);
        upload.output.chunk.assign("def");
        upload.output.chunkReady = true;
        for (int tick = 0; tick < 4 && upload.output.chunkReady; ++tick) {
            RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) != Driver::Result::kFatal);
        }
        upload.trailers.push_back(ruvia::HttpHeader::copyOf("x-end", "retained", &pool));
        upload.output.endRequested = true;
        for (int tick = 0; tick < 4 && !driver.finished(); ++tick) {
            RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) != Driver::Result::kFatal);
        }
        RUVIA_CHECK(driver.finished() && upload.output.ended);
        RUVIA_CHECK_EQ(fins, 1);
        ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, &pool);
        Received received;
        const auto result = server.feed(0, wire, true, false, receive, &received);
        RUVIA_CHECK(result.status == ruvia::Http3ConnectionStatus::kMessageEnd);
        RUVIA_CHECK_EQ(received.body, "abcdef");
        RUVIA_CHECK_EQ(received.trailer, "x-end:retained");
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http3ClientRequestTrailersHonorPeerFieldLimitAfterCursorMove) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    auto run = [&]() -> ruvia::Task<void> {
        std::pmr::unsynchronized_pool_resource pool;
        ruvia::detail::Http3ClientSansIoSessionEngine engine(&pool);
        const auto prefixes = ruvia::Http3LocalCriticalStreams::create({.maxFieldSectionSize = 512});
        RUVIA_CHECK(prefixes.has_value());
        RUVIA_CHECK(engine.feed(3, prefixes->controlPrefix()).scope == ruvia::Http3ConnectionErrorScope::kNone);
        const auto worker = attachment.loop().handle();
        ruvia::detail::HttpClientUploadState upload(worker, &pool, {});
        upload.contentReleased = true;
        ruvia::detail::HttpClientRequestStorage storage("POST", "/upload", &pool);
        storage.bindUpload(upload);
        auto cursor = ruvia::detail::Http3ClientRequestWrite::create(std::move(storage), "https", "example.com", &pool);
        RUVIA_CHECK(cursor.has_value());
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kPost).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(cursor->prepareConnectionHead(0, engine));
        ruvia::detail::Http3ClientRequestWrite moved(std::move(*cursor));
        const auto head = moved.next();
        RUVIA_CHECK(head.has_value());
        RUVIA_CHECK(moved.acknowledge(head->size()).has_value());
        upload.trailers.push_back(ruvia::HttpHeader::copyOf("x-end", std::string(513, 't'), &pool));
        upload.output.endRequested = true;
        const auto rejected = moved.next();
        RUVIA_CHECK(!rejected && moved.failed());
        RUVIA_CHECK(!upload.output.ended);
        attachment.stop();
        co_return;
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
}

RUVIA_TEST(http3ClientPriorityUpdatesUseControlFramesAndBoundPendingOutput) {
    ruvia::test::CountingMemoryResource pool;
    ruvia::detail::Http3ClientSansIoSessionEngine engine(&pool);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(!engine.queuePriorityUpdate(4, {.urgency = 1}));
    RUVIA_CHECK(!engine.queuePriorityUpdate(0, {.urgency = 8}));
    RUVIA_CHECK(engine.pendingControlOutput().empty());
    RUVIA_CHECK(engine.queuePriorityUpdate(0, {.urgency = 1, .incremental = true}));
    RUVIA_CHECK(engine.queuePriorityUpdate(0, {.urgency = 6}));
    auto prefixes = ruvia::Http3LocalCriticalStreams::create({});
    std::string wire(prefixes->controlPrefix().data(), prefixes->controlPrefix().size());
    const auto pending = engine.pendingControlOutput();
    wire.append(pending.data(), pending.size());
    ruvia::Http3Connection server(ruvia::Http3PeerRole::kServer, &pool);
    std::vector<ruvia::HttpPriority> priorities;
    const auto onEvent = [](void* raw, const ruvia::Http3ConnectionEvent& event) {
        if (event.priorityUpdate) {
            static_cast<std::vector<ruvia::HttpPriority>*>(raw)->push_back(event.priorityUpdate->fields.requestPriority());
        }
    };
    RUVIA_CHECK(server.feed(2, wire, false, false, onEvent, &priorities).scope == ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(priorities.size() == 2);
    RUVIA_CHECK(priorities[0].urgency == 1 && priorities[0].incremental);
    RUVIA_CHECK(priorities[1].urgency == 6 && !priorities[1].incremental);
    const auto bytes = pending.size();
    RUVIA_CHECK(!engine.consumeControlOutput(bytes + 1));
    RUVIA_CHECK(engine.consumeControlOutput(1));
    RUVIA_CHECK(engine.consumeControlOutput(bytes - 1));
    RUVIA_CHECK(engine.pendingControlOutput().empty());
    const auto baseline = pool.liveAllocations();
    for (unsigned operation = 0; operation != 128; ++operation) {
        RUVIA_CHECK(engine.queuePriorityUpdate(0, {.urgency = 1}));
        RUVIA_CHECK(engine.consumeControlOutput(engine.pendingControlOutput().size()));
        RUVIA_CHECK_EQ(pool.liveAllocations(), baseline);
    }
    std::size_t queued{};
    while (engine.queuePriorityUpdate(0, {.urgency = 2})) {
        ++queued;
        if (queued > 65536) {
            RUVIA_CHECK(false);
            break;
        }
    }
    RUVIA_CHECK(queued > 1 && engine.pendingControlOutput().size() <= ruvia::kMaxHttpHeaderBytes);
    RUVIA_CHECK(engine.stop().scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(engine.pendingControlOutput().empty());
    RUVIA_CHECK(!engine.queuePriorityUpdate(0, {}));
}

RUVIA_TEST(http3ClientRequestHeadHonorsPeerFieldLimitWithStaticAndDynamicQpack) {
    for (const std::uint64_t capacity : {0U, 256U}) {
        std::pmr::unsynchronized_pool_resource pool;
        ruvia::detail::Http3ClientSansIoSessionEngine engine(&pool);
        const auto prefixes = ruvia::Http3LocalCriticalStreams::create({.qpackMaxTableCapacity = capacity,
            .maxFieldSectionSize = 0,
            .qpackBlockedStreams = 2});
        RUVIA_CHECK(prefixes.has_value());
        if (!prefixes) {
            continue;
        }
        RUVIA_CHECK(engine.feed(3, prefixes->controlPrefix(), false, false).scope == ruvia::Http3ConnectionErrorScope::kNone);
        RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kPost).scope == ruvia::Http3ConnectionErrorScope::kNone);
        const auto pending = engine.pendingEncoderOutput();
        const std::string encoderBefore(pending.data(), pending.size());
        auto request = makeRequest(&pool);
        RUVIA_CHECK(request.has_value());
        if (request) {
            RUVIA_CHECK(!request->prepareConnectionHead(0, engine));
            const auto encoderAfter = engine.pendingEncoderOutput();
            RUVIA_CHECK_EQ(std::string_view(encoderAfter.data(), encoderAfter.size()), std::string_view(encoderBefore));
        }
    }
}

RUVIA_TEST(http3ClientRequestDriverUsesPeerQpackSettingsAndEmitsDynamicEncoderInstructions) {
    std::pmr::unsynchronized_pool_resource pool;
    ruvia::detail::Http3ClientSansIoSessionEngine engine(&pool);
    auto prefixes = ruvia::Http3LocalCriticalStreams::create({.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2});
    RUVIA_CHECK(prefixes.has_value());
    if (!prefixes) {
        return;
    }
    RUVIA_CHECK(engine.feed(3, prefixes->controlPrefix()).scope == ruvia::Http3ConnectionErrorScope::kNone);
    auto created = makeRequest(&pool);
    RUVIA_CHECK(created.has_value());
    if (!created) {
        return;
    }
    Driver driver(std::move(*created));
    std::string wire;
    auto open = [] { return ruvia::quic_stream_open_result{.status = ruvia::quic_operation_status::accepted, .stream_id = 0}; };
    auto registerResponse = [&](auto id, auto method) {
        const auto registered = engine.registerRequest(id, method);
        return registered.scope == ruvia::Http3ConnectionErrorScope::kNone && driver.prepareConnectionHead(id, engine);
    };
    auto write = [&](auto, std::span<const char> bytes) {
        wire.append(bytes.data(), bytes.size());
        return ruvia::quic_stream_write_result{.status = ruvia::quic_operation_status::accepted, .accepted = bytes.size()};
    };
    auto finish = [](auto) { return ruvia::quic_operation_status::accepted; };
    for (int tick = 0; tick < 8 && !driver.finished(); ++tick) {
        RUVIA_CHECK(driver.drive(open, registerResponse, write, finish) != Driver::Result::kFatal);
    }
    RUVIA_CHECK(driver.finished());
    RUVIA_CHECK(!engine.pendingEncoderOutput().empty());
    ruvia::Http3Connection peer(ruvia::Http3PeerRole::kServer, &pool,
        {.qpackMaxTableCapacity = 256, .qpackBlockedStreams = 2});
    Received received;
    const auto blocked = peer.feed(0, wire, true, false, receive, &received);
    RUVIA_CHECK(blocked.status == ruvia::Http3ConnectionStatus::kQpackBlocked);
    std::string instructions(1, char{2});
    const auto pending = engine.pendingEncoderOutput();
    instructions.append(pending.data(), pending.size());
    RUVIA_CHECK(peer.feed(6, std::span<const char>(instructions.data(), instructions.size()), false, false, receive, &received).scope == ruvia::Http3ConnectionErrorScope::kNone);
    const auto resumed = peer.feed(0, std::span<const char>(wire.data(), wire.size()).subspan(blocked.consumedBytes), true, false, receive, &received);
    RUVIA_CHECK(resumed.status == ruvia::Http3ConnectionStatus::kMessageEnd);
    RUVIA_CHECK_EQ(received.body, "payload");
    RUVIA_CHECK(engine.consumeEncoderOutput(pending.size()));
}
