#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/detail/http3/Http3ClientReceiveDriver.h"

#include "test_harness.h"

namespace {
using Driver = ruvia::detail::Http3ClientReceiveDriver;
using Engine = ruvia::detail::Http3ClientSansIoSessionEngine;
using Read = ruvia::detail::Http3QuicStreamSet::StreamRead;

std::vector<char> frame(std::uint64_t type, std::span<const char> payload) {
    std::vector<char> output(16);
    const auto header = ruvia::encodeHttp3VarInt(output, type);
    const auto length = ruvia::encodeHttp3VarInt(
        std::span<char>(output).subspan(*header), payload.size());
    output.resize(*header + *length);
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}
std::vector<char> responseHead() {
    std::pmr::monotonic_buffer_resource temp;
    constexpr std::array fields{ruvia::Http3FieldSectionFieldView{":status", "200"},
        ruvia::Http3FieldSectionFieldView{"x-received", "owned"}};
    const auto encoded = ruvia::encodeHttp3FieldSection(fields, &temp);
    return frame(1, *encoded);
}
struct FakeRead final {
    std::vector<char> wire;
    std::size_t position{};
    std::size_t maxChunk{3};
    bool reset{};

    Read operator()(std::uint64_t, std::span<char> destination) {
        if (reset) {
            return {.status = Read::Status::kReset};
        }
        if (position == wire.size()) {
            return {.status = Read::Status::kFin};
        }
        const auto size = std::min({destination.size(), wire.size() - position, maxChunk});
        std::copy_n(wire.data() + position, size, destination.data());
        position += size;
        return {.status = Read::Status::kData, .size = size};
    }
};

struct NestedReceive final {
    Driver* driver{};
    std::string ownedBody;
    bool attempted{};
    bool rejected{};
    bool nestedRead{};
};
void tryRecursiveRead(void* raw, const ruvia::Http3ConnectionEvent& event) {
    if (event.kind != ruvia::Http3ConnectionEventKind::kBody) {
        return;
    }
    auto& owner = *static_cast<NestedReceive*>(raw);
    owner.attempted = true;
    try {
        (void)owner.driver->drive(event.streamId,
            [&owner](std::uint64_t, std::span<char> output) -> Read {
                owner.nestedRead = true;
                output[0] = '?';
                return {.status = Read::Status::kData, .size = 1};
            });
    } catch (const std::logic_error&) {
        owner.rejected = true;
    }
    owner.ownedBody.append(event.body.data(), event.body.size());
}
}  // namespace

RUVIA_TEST(http3ClientReceiveDriverPreservesOnlyPeerResetCodesWithoutNarrowing) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    const std::array<std::optional<std::uint64_t>, 4> codes{
        std::nullopt, 0, static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestRejected),
        (std::uint64_t{1} << 62) - 1};
    for (std::size_t index = 0; index < codes.size(); ++index) {
        const auto id = static_cast<std::uint64_t>(index * 4);
        RUVIA_CHECK(engine.registerRequest(id, ruvia::HttpKnownMethod::kPost).scope ==
                    ruvia::Http3ConnectionErrorScope::kNone);
        const auto reset = driver.drive(id, [&](std::uint64_t, std::span<char>) -> Read {
            return {.status = Read::Status::kReset, .peerResetErrorCode = codes[index]};
        });
        RUVIA_CHECK(reset.status == Driver::Status::kStreamReset);
        RUVIA_CHECK(reset.peerResetErrorCode == codes[index]);
        RUVIA_CHECK(reset.peerReportsUnprocessed == (index == 2));
        RUVIA_CHECK(engine.response(id)->reset && !engine.response(id)->complete);
        RUVIA_CHECK(engine.release(id));
    }
    RUVIA_CHECK(engine.registerRequest(16, ruvia::HttpKnownMethod::kPost).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    FakeRead responded{.wire = responseHead(), .maxChunk = Driver::kReadBlockBytes};
    RUVIA_CHECK(driver.drive(16, responded).status == Driver::Status::kProgress);
    const auto contradicted = driver.drive(16, [](std::uint64_t, std::span<char>) -> Read {
        return {.status = Read::Status::kReset,
            .peerResetErrorCode = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestRejected)};
    });
    RUVIA_CHECK(contradicted.status == Driver::Status::kStreamReset);
    RUVIA_CHECK(!contradicted.peerReportsUnprocessed);
    RUVIA_CHECK(engine.release(16));
    // A critical-stream reset is a connection error, never a retryable request rejection.
    FakeRead control{.wire = {0}, .maxChunk = 8};
    const auto settings = frame(4, {});
    control.wire.insert(control.wire.end(), settings.begin(), settings.end());
    RUVIA_CHECK(driver.drive(3, control).status == Driver::Status::kProgress);
    const auto failed = driver.drive(3, [](std::uint64_t, std::span<char>) -> Read {
        return {.status = Read::Status::kReset,
            .peerResetErrorCode = static_cast<std::uint64_t>(ruvia::Http3ConnectionErrorCode::kRequestRejected)};
    });
    RUVIA_CHECK(failed.status == Driver::Status::kConnectionError);
    RUVIA_CHECK(!failed.peerResetErrorCode && !failed.peerReportsUnprocessed);
}

RUVIA_TEST(http3ClientReceiveDriverFeedsInterleavedFramesAndPublishesOnlyValidFin) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kHead).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    FakeRead get{.wire = responseHead(), .maxChunk = 2};
    const auto payload = frame(0, std::span<const char>("ok", 2));
    get.wire.insert(get.wire.end(), payload.begin(), payload.end());
    FakeRead head{.wire = responseHead(), .maxChunk = 1};
    bool getComplete{};
    bool headComplete{};
    for (int i = 0; i < 80 && (!getComplete || !headComplete); ++i) {
        if (!getComplete) {
            const auto result = driver.drive(0, get);
            RUVIA_CHECK(result.status == Driver::Status::kProgress ||
                        result.status == Driver::Status::kResponseComplete);
            getComplete = result.status == Driver::Status::kResponseComplete;
        }
        if (!headComplete) {
            const auto result = driver.drive(4, head);
            RUVIA_CHECK(result.status == Driver::Status::kProgress ||
                        result.status == Driver::Status::kResponseComplete);
            headComplete = result.status == Driver::Status::kResponseComplete;
        }
    }
    RUVIA_CHECK(getComplete && headComplete);
    const auto response = engine.response(0);
    const auto headResponse = engine.response(4);
    RUVIA_CHECK(response && response->status == 200 && response->complete);
    RUVIA_CHECK(response && std::string_view(response->body.data(), response->body.size()) == "ok");
    RUVIA_CHECK(response && response->headers.size() == 1 &&
                response->headers.front().value == "owned");
    RUVIA_CHECK(headResponse && headResponse->complete && headResponse->body.empty());
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK(engine.release(4));
}

RUVIA_TEST(http3ClientReceiveDriverRejectsReentryBeforeTouchingBorrowedScratch) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    NestedReceive observed{.driver = &driver};
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet,
                          {.callback = tryRecursiveRead, .context = &observed})
                    .scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(engine.feed(0, responseHead()).scope == ruvia::Http3ConnectionErrorScope::kNone);
    FakeRead input{.wire = frame(0, std::span<const char>("ok", 2)),
        .maxChunk = Driver::kReadBlockBytes};
    const auto data = driver.drive(0, input);
    RUVIA_CHECK(data.status == Driver::Status::kProgress);
    RUVIA_CHECK(observed.attempted && observed.rejected && !observed.nestedRead);
    RUVIA_CHECK(observed.ownedBody == "ok");
    RUVIA_CHECK(driver.drive(0, input).status == Driver::Status::kResponseComplete);
    RUVIA_CHECK(engine.response(0)->complete && engine.release(0));
}

RUVIA_TEST(http3ClientReceiveDriverHonorsApplicationReadCapacityWithoutLosingFrames) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    FakeRead input{.wire = responseHead(), .maxChunk = Driver::kReadBlockBytes};
    const auto data = frame(0, std::span<const char>("hello", 5));
    input.wire.insert(input.wire.end(), data.begin(), data.end());
    bool touched{};
    const auto blocked = driver.drive(0, [&touched](std::uint64_t, std::span<char>) -> Read {
            touched = true;
            return {.status = Read::Status::kFatal}; }, 0);
    RUVIA_CHECK(blocked.status == Driver::Status::kBlocked && !touched);
    bool complete{};
    for (int i = 0; i < 40 && !complete; ++i) {
        const auto result = driver.drive(0, [&input, &ruvia_ctx](std::uint64_t id, std::span<char> output) -> Read {
                RUVIA_CHECK(output.size() <= 4);
                return input(id, output); }, 4);
        RUVIA_CHECK(result.bytes <= 4);
        RUVIA_CHECK(result.status == Driver::Status::kProgress ||
                    result.status == Driver::Status::kResponseComplete);
        complete = result.status == Driver::Status::kResponseComplete;
    }
    RUVIA_CHECK(complete);
    const auto response = engine.response(0);
    RUVIA_CHECK(response && response->complete &&
                std::string_view(response->body.data(), response->body.size()) == "hello");
    RUVIA_CHECK(engine.release(0));
}

RUVIA_TEST(http3ClientReceiveDriverPeerCriticalFinFailsWholeConnection) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    FakeRead control{.wire = {0}, .maxChunk = 8};
    const auto settings = frame(4, {});
    control.wire.insert(control.wire.end(), settings.begin(), settings.end());
    while (control.position != control.wire.size()) {
        RUVIA_CHECK(driver.drive(3, control).status == Driver::Status::kProgress);
    }
    const auto closed = driver.drive(3, control);
    RUVIA_CHECK(closed.status == Driver::Status::kConnectionError);
    RUVIA_CHECK(closed.protocol.code == ruvia::Http3ConnectionErrorCode::kClosedCriticalStream);
    RUVIA_CHECK(engine.response(0)->result.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(engine.release(0));
}

RUVIA_TEST(http3ClientReceiveDriverTransportFailureWakesEveryIncompleteResponse) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    RUVIA_CHECK(engine.registerRequest(4, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const auto failed = driver.drive(0, [](std::uint64_t, std::span<char>) -> Read {
        return {.status = Read::Status::kFatal};
    });
    RUVIA_CHECK(failed.status == Driver::Status::kTransportError);
    RUVIA_CHECK(failed.protocol.status == ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
    RUVIA_CHECK(failed.protocol.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    RUVIA_CHECK(engine.response(0)->result.status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
    RUVIA_CHECK(engine.response(4)->result.status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
    RUVIA_CHECK(engine.release(0));
    RUVIA_CHECK(engine.release(4));
    RUVIA_CHECK(engine.registerRequest(8, ruvia::HttpKnownMethod::kGet).status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
}

RUVIA_TEST(http3ClientReceiveDriverDoesNotMaskEarlierConnectionProtocolFailure) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const auto unexpected = frame(0, std::span<const char>("bad", 3));
    const auto protocol = engine.feed(0, unexpected);
    RUVIA_CHECK(protocol.scope == ruvia::Http3ConnectionErrorScope::kConnection);
    const auto result = driver.drive(0, [](std::uint64_t, std::span<char>) -> Read {
        return {.status = Read::Status::kFatal};
    });
    RUVIA_CHECK(result.status == Driver::Status::kConnectionError);
    RUVIA_CHECK(result.protocol.code == protocol.code);
    RUVIA_CHECK(engine.release(0));
}

RUVIA_TEST(http3ClientReceiveDriverRejectsInvalidTransportByteCountBeforeFeeding) {
    std::pmr::unsynchronized_pool_resource pool;
    Engine engine(&pool);
    Driver driver(engine);
    RUVIA_CHECK(engine.registerRequest(0, ruvia::HttpKnownMethod::kGet).scope ==
                ruvia::Http3ConnectionErrorScope::kNone);
    const auto wrong = driver.drive(0, [](std::uint64_t, std::span<char>) -> Read { return {.status = Read::Status::kData, .size = 5}; }, 4);
    RUVIA_CHECK(wrong.status == Driver::Status::kTransportError);
    RUVIA_CHECK(engine.response(0)->result.status ==
                ruvia::detail::Http3ClientSansIoSessionStatus::kTransportError);
    RUVIA_CHECK(!engine.response(0)->complete);
    RUVIA_CHECK(engine.release(0));
}
