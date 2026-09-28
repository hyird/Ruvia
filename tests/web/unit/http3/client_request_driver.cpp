#include <array>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/web/detail/http3/Http3ClientRequestDriver.h"

#include "test_harness.h"

namespace {
using Driver = ruvia::detail::Http3ClientRequestDriver;
using StreamSet = ruvia::detail::Http3QuicStreamSet;

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
        case ruvia::Http3ConnectionEventKind::kMessageEnd:
            ++received.finished;
            break;
        default:
            break;
    }
}
}  // namespace

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

    auto open = [&]() -> StreamSet::OpenStream {
        ++openAttempts;
        if (openAttempts == 1) {
            return {.error = StreamSet::Error::kStreamLimitRetry};
        }
        return {.id = 0};
    };
    auto registerResponse = [&](StreamSet::StreamId id, ruvia::HttpKnownMethod method) {
        RUVIA_CHECK_EQ(id, 0U);
        RUVIA_CHECK(method == ruvia::HttpKnownMethod::kPost);
        ++registrations;
        RUVIA_CHECK(wire.empty());
        return true;
    };
    auto write = [&](StreamSet::StreamId id, std::span<const char> offered) -> StreamSet::StreamWrite {
        RUVIA_CHECK_EQ(id, 0U);
        RUVIA_CHECK_EQ(registrations, 1);
        if (!acceptedInitialPart) {
            wire.push_back(offered.front());
            acceptedInitialPart = true;
            return {.status = StreamSet::StreamWrite::Status::kAccepted, .bytes = 1};
        }
        if (!wantIssued) {
            pendingAddress = offered.data();
            pendingSize = offered.size();
            pendingBytes.assign(offered.data(), offered.size());
            wantIssued = true;
            return {.status = StreamSet::StreamWrite::Status::kWouldBlock};
        }
        if (pendingAddress) {
            RUVIA_CHECK(offered.data() == pendingAddress);
            RUVIA_CHECK_EQ(offered.size(), pendingSize);
            RUVIA_CHECK(std::string_view(offered.data(), offered.size()) == pendingBytes);
            pendingAddress = nullptr;
        }
        wire.append(offered.data(), offered.size());
        return {.status = StreamSet::StreamWrite::Status::kAccepted, .bytes = offered.size()};
    };
    auto finish = [&](StreamSet::StreamId id) {
        RUVIA_CHECK_EQ(id, 0U);
        ++finishAttempts;
        RUVIA_CHECK(!wire.empty());
        return finishAttempts == 1 ? StreamSet::Error::kWouldBlock : StreamSet::Error::kNone;
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
    auto open = [] { return StreamSet::OpenStream{.id = 0}; };
    auto registerResponse = [&](StreamSet::StreamId, ruvia::HttpKnownMethod method) {
        RUVIA_CHECK(method == ruvia::HttpKnownMethod::kPost);
        ++registrations;
        return true;
    };
    auto write = [&](StreamSet::StreamId, std::span<const char> offered) -> StreamSet::StreamWrite {
        if (pendingAddress == nullptr) {
            pendingAddress = offered.data();
            pending.assign(offered.data(), offered.size());
            ++wants;
            return {.status = StreamSet::StreamWrite::Status::kWouldBlock};
        }
        RUVIA_CHECK(offered.data() == pendingAddress);
        RUVIA_CHECK(std::string_view(offered.data(), offered.size()) == pending);
        pendingAddress = nullptr;
        wire.push_back(offered.front());
        ++accepted;
        return {.status = StreamSet::StreamWrite::Status::kAccepted, .bytes = 1};
    };
    auto finish = [](StreamSet::StreamId) { return StreamSet::Error::kNone; };
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
    auto registerResponse = [&](StreamSet::StreamId, ruvia::HttpKnownMethod) {
        ++registrations;
        return true;
    };
    auto write = [&](StreamSet::StreamId id, std::span<const char> bytes) {
        RUVIA_CHECK_EQ(id, 8U);
        wire.append(bytes.data(), bytes.size());
        return StreamSet::StreamWrite{.status = StreamSet::StreamWrite::Status::kAccepted,
            .bytes = bytes.size()};
    };
    auto finish = [](StreamSet::StreamId) { return StreamSet::Error::kNone; };
    RUVIA_CHECK(driver.drive([] { return StreamSet::OpenStream{
                                      .error = StreamSet::Error::kConnectionRequestLimit}; },
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
        RUVIA_CHECK(replacement.drive([] { return StreamSet::OpenStream{.id = 8}; },
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
    auto result = driver.drive([] { return StreamSet::OpenStream{.id = 4}; },
        [](StreamSet::StreamId, ruvia::HttpKnownMethod) { return false; },
        [&](StreamSet::StreamId, std::span<const char>) -> StreamSet::StreamWrite {
            ++writes;
            return {.status = StreamSet::StreamWrite::Status::kAccepted, .bytes = 1};
        },
        [](StreamSet::StreamId) { return StreamSet::Error::kNone; });
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
    auto registerResponse = [&](StreamSet::StreamId id, ruvia::HttpKnownMethod method) {
        RUVIA_CHECK(method == ruvia::HttpKnownMethod::kHead);
        return responses.registerClientRequest(id, method).scope == ruvia::Http3ConnectionErrorScope::kNone;
    };
    auto write = [&](StreamSet::StreamId, std::span<const char> offered) {
        wire.append(offered.data(), offered.size());
        return StreamSet::StreamWrite{.status = StreamSet::StreamWrite::Status::kAccepted,
            .bytes = offered.size()};
    };
    for (int i = 0; i < 16 && !driver.finished(); ++i) {
        RUVIA_CHECK(driver.drive([] { return StreamSet::OpenStream{.id = 0}; },
                        registerResponse, write,
                        [](StreamSet::StreamId) { return StreamSet::Error::kNone; }) !=
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
    auto open = [] { return StreamSet::OpenStream{.id = 0}; };
    auto registerResponse = [](StreamSet::StreamId, ruvia::HttpKnownMethod) { return true; };
    auto write = [](StreamSet::StreamId, std::span<const char> offered) {
        return StreamSet::StreamWrite{.status = StreamSet::StreamWrite::Status::kAccepted,
            .bytes = offered.size()};
    };
    auto finish = [](StreamSet::StreamId) { return StreamSet::Error::kFatal; };
    for (int i = 0; i < 16 && !driver.failed(); ++i) {
        (void)driver.drive(open, registerResponse, write, finish);
    }
    RUVIA_CHECK(driver.failed() && !driver.finished());
}
