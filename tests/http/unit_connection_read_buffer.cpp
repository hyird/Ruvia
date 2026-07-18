#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "ruvia/web/detail/server/HttpConnectionState.h"
#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/HttpServerConnectionGuards.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/HttpLimits.h"

namespace {

using ruvia::detail::applyReusableHttp1RequestBufferCompletion;
using ruvia::detail::compactConnectionReadBuffer;
using ruvia::detail::AcceptedConnectionLease;
using ruvia::detail::growReadBuffer;
using ruvia::detail::kInitialReadBufferBytes;
using ruvia::detail::Http1BufferedResponseReady;
using ruvia::detail::Http1CommittedStreamResponse;
using ruvia::detail::Http1ConnectionDisposition;
using ruvia::detail::Http1RequestBufferCompaction;
using ruvia::detail::Http1RequestBufferCompletion;
using ruvia::detail::Http1RequestBufferDiscarded;
using ruvia::detail::Http1RequestBufferPipelineRestore;
using ruvia::detail::Http1SessionRequestCompletion;
using ruvia::detail::Http1ServerRequestParseState;
using ruvia::detail::Http1ServerRequestParser;
using ruvia::detail::trimReadBufferStorage;
using ruvia::kMaxHttpHeaderBytes;

template <typename Alternative>
concept HasCompletionStatus = requires(const Alternative& value) {
    { value.status() } -> std::same_as<std::uint16_t>;
};

template <typename Alternative>
concept HasConsumedBytes = requires(const Alternative& value) {
    { value.consumedBytes() } -> std::same_as<std::size_t>;
};

template <typename Result>
concept HasAnyRvalueRequestCompletionBorrow =
    requires(Result&& value) { std::move(value).discarded(); } ||
    requires(Result&& value) { std::move(value).compaction(); } ||
    requires(Result&& value) { std::move(value).pipelineRestore(); } ||
    requires(Result&& value) { std::move(value).bufferedResponse(); } ||
    requires(Result&& value) { std::move(value).committedStream(); } ||
    requires(Result&& value) { std::move(value).bufferCompletion(); };

template <typename Pipeline>
concept AcceptsTemporaryPipelineRestore = requires(
    ruvia::detail::Http1ServerConnectionPlan connectionPlan,
    Pipeline&& pipeline) {
    Http1SessionRequestCompletion::makeBufferedPipelineRestore(
        connectionPlan,
        std::forward<Pipeline>(pipeline));
};

static_assert(!std::default_initializable<Http1RequestBufferCompletion>);
static_assert(!std::default_initializable<Http1SessionRequestCompletion>);
static_assert(!HasAnyRvalueRequestCompletionBorrow<
    Http1RequestBufferCompletion>);
static_assert(!HasAnyRvalueRequestCompletionBorrow<
    Http1SessionRequestCompletion>);
static_assert(!AcceptsTemporaryPipelineRestore<std::string>);
static_assert(!AcceptsTemporaryPipelineRestore<const std::string>);
static_assert(!AcceptsTemporaryPipelineRestore<std::pmr::string>);
static_assert(AcceptsTemporaryPipelineRestore<std::string&>);
static_assert(AcceptsTemporaryPipelineRestore<std::pmr::string&>);
static_assert(AcceptsTemporaryPipelineRestore<std::string_view>);
static_assert(!std::default_initializable<Http1RequestBufferDiscarded>);
static_assert(!std::constructible_from<
    Http1RequestBufferCompaction,
    std::size_t>);
static_assert(!std::default_initializable<Http1RequestBufferPipelineRestore>);
static_assert(!std::default_initializable<Http1BufferedResponseReady>);
static_assert(!std::constructible_from<
    Http1CommittedStreamResponse,
    std::uint16_t>);
static_assert(HasConsumedBytes<Http1RequestBufferCompaction>);
static_assert(!HasConsumedBytes<Http1RequestBufferDiscarded>);
static_assert(!HasConsumedBytes<Http1RequestBufferPipelineRestore>);
static_assert(HasCompletionStatus<Http1CommittedStreamResponse>);
static_assert(!HasCompletionStatus<Http1BufferedResponseReady>);
static_assert(std::same_as<
    decltype(std::declval<const Http1SessionRequestCompletion&>()
                 .bufferedResponse()),
    const Http1BufferedResponseReady*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1SessionRequestCompletion&>()
                 .committedStream()),
    const Http1CommittedStreamResponse*>);
static_assert(std::same_as<
    decltype(std::declval<const Http1RequestBufferCompletion&>()
                 .compaction()),
    const Http1RequestBufferCompaction*>);

std::pmr::string sizedBuffer(std::size_t size) {
    std::pmr::string out(std::pmr::new_delete_resource());
    out.resize(size);
    return out;
}

std::pmr::string buffer(std::string_view contents) {
    std::pmr::string out(std::pmr::new_delete_resource());
    out.assign(contents.data(), contents.size());
    return out;
}

ruvia::detail::Http1ServerConnectionPlan reusableHttp11Plan() {
    Http1ServerRequestParser parser;
    return parser.parseMessage(
        "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n")
        .connectionPlan;
}

// The live region is the first `usedBytes` of the buffer's storage.
std::string_view live(const std::pmr::string& buffer, std::size_t usedBytes) {
    return std::string_view(buffer.data(), usedBytes);
}

}  // namespace

RUVIA_TEST(http1_session_request_completion_owns_wire_and_buffer_outcome) {
    const auto reusablePlan = reusableHttp11Plan();
    const auto buffered =
        Http1SessionRequestCompletion::makeBufferedUnrestored(
            reusablePlan,
            12);
    RUVIA_CHECK(buffered.bufferedResponse() != nullptr);
    RUVIA_CHECK(buffered.committedStream() == nullptr);
    RUVIA_CHECK(buffered.connectionPlan().disposition() ==
                Http1ConnectionDisposition::kReuse);
    RUVIA_CHECK(buffered.bufferCompletion().compaction() != nullptr);
    RUVIA_CHECK_EQ(
        buffered.bufferCompletion().compaction()->consumedBytes(),
        std::size_t{12});
    RUVIA_CHECK(buffered.bufferCompletion().discarded() == nullptr);
    RUVIA_CHECK(buffered.bufferCompletion().pipelineRestore() == nullptr);

    const auto committed =
        Http1SessionRequestCompletion::makeCommittedStream(
            reusablePlan,
            207,
            19);
    RUVIA_CHECK(committed.bufferedResponse() == nullptr);
    RUVIA_CHECK(committed.committedStream() != nullptr);
    RUVIA_CHECK_EQ(
        committed.committedStream()->status(),
        std::uint16_t{207});
    RUVIA_CHECK_EQ(
        committed.bufferCompletion().compaction()->consumedBytes(),
        std::size_t{19});
}

RUVIA_TEST(http1_session_request_completion_discriminates_close_and_restore) {
    const auto closePlan =
        ruvia::detail::Http1ServerConnectionPlan::http11Close();
    const auto closing =
        Http1SessionRequestCompletion::makeBufferedClosing(
            closePlan);
    RUVIA_CHECK(closing.bufferedResponse() != nullptr);
    RUVIA_CHECK(closing.connectionPlan().disposition() ==
                Http1ConnectionDisposition::kClose);
    RUVIA_CHECK(closing.bufferCompletion().discarded() != nullptr);
    RUVIA_CHECK(closing.bufferCompletion().compaction() == nullptr);
    RUVIA_CHECK(closing.bufferCompletion().pipelineRestore() == nullptr);

    const auto unshiftedClosing =
        Http1SessionRequestCompletion::makeBufferedUnrestored(
            closePlan,
            999);
    RUVIA_CHECK(unshiftedClosing.bufferCompletion().discarded() != nullptr);
    RUVIA_CHECK(unshiftedClosing.bufferCompletion().compaction() == nullptr);

    const auto committedClosing =
        Http1SessionRequestCompletion::makeCommittedStream(
            closePlan,
            503,
            999);
    RUVIA_CHECK(committedClosing.committedStream() != nullptr);
    RUVIA_CHECK(committedClosing.bufferCompletion().discarded() != nullptr);
    RUVIA_CHECK(committedClosing.bufferCompletion().compaction() == nullptr);

    const auto restore =
        Http1SessionRequestCompletion::makeBufferedPipelineRestore(
            reusableHttp11Plan(),
            "GET /next HTTP/1.1\r\n\r\n");
    RUVIA_CHECK(restore.bufferCompletion().discarded() == nullptr);
    RUVIA_CHECK(restore.bufferCompletion().compaction() == nullptr);
    RUVIA_CHECK(restore.bufferCompletion().pipelineRestore() != nullptr);
    RUVIA_CHECK_EQ(
        restore.bufferCompletion().pipelineRestore()->pipeline(),
        std::string_view("GET /next HTTP/1.1\r\n\r\n"));
}

RUVIA_TEST(http1_request_buffer_completion_applies_exactly_one_cleanup) {
    auto readBuffer = buffer("REQPIPE");
    std::size_t usedBytes = 7;
    const auto compacted =
        Http1SessionRequestCompletion::makeBufferedUnrestored(
            reusableHttp11Plan(),
            3);
    applyReusableHttp1RequestBufferCompletion(
        compacted.bufferCompletion(),
        readBuffer,
        usedBytes);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{4});
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), std::string_view("PIPE"));

    // A body runtime that kept the pipelined bytes out of the read buffer -- so
    // that the completed request's borrowed views survived until the response was
    // written -- hands them over here. Installing them replaces the live bytes and
    // grows the buffer when the handover does not fit.
    constexpr std::string_view handedOver = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto restore =
        Http1SessionRequestCompletion::makeBufferedPipelineRestore(
            reusableHttp11Plan(),
            handedOver);
    applyReusableHttp1RequestBufferCompletion(
        restore.bufferCompletion(),
        readBuffer,
        usedBytes);
    RUVIA_CHECK_EQ(usedBytes, handedOver.size());
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), handedOver);
    RUVIA_CHECK(readBuffer.size() >= handedOver.size());
}

RUVIA_TEST(connection_read_buffer_partial_consume_moves_remainder) {
    // Two pipelined requests; consuming the first slides the second to the front.
    auto readBuffer = buffer("REQ1REQ2");
    std::size_t usedBytes = 8;
    compactConnectionReadBuffer(readBuffer, usedBytes, 4);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{4});
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), std::string_view("REQ2"));
}

RUVIA_TEST(connection_read_buffer_consume_nothing_is_unchanged) {
    auto readBuffer = buffer("REQ1REQ2");
    std::size_t usedBytes = 8;
    compactConnectionReadBuffer(readBuffer, usedBytes, 0);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{8});
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), std::string_view("REQ1REQ2"));
}

RUVIA_TEST(connection_read_buffer_consume_all_empties_region) {
    auto readBuffer = buffer("REQ1REQ2");
    std::size_t usedBytes = 8;
    compactConnectionReadBuffer(readBuffer, usedBytes, 8);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{0});
}

RUVIA_TEST(connection_read_buffer_overlapping_move_is_correct) {
    // The remaining region (4 bytes) overlaps its destination; the move must
    // still reproduce it exactly.
    auto readBuffer = buffer("ABCDEF");
    std::size_t usedBytes = 6;
    compactConnectionReadBuffer(readBuffer, usedBytes, 2);
    RUVIA_CHECK_EQ(usedBytes, std::size_t{4});
    RUVIA_CHECK_EQ(live(readBuffer, usedBytes), std::string_view("CDEF"));
}

RUVIA_TEST(grow_read_buffer_doubles_when_full) {
    auto readBuffer = sizedBuffer(8 * 1024);
    growReadBuffer(readBuffer, /*usedBytes=*/8 * 1024);  // full -> grow
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{16 * 1024});
}

RUVIA_TEST(grow_read_buffer_caps_at_header_limit) {
    // Doubling would overshoot the header limit; growth clamps to it so an
    // attacker cannot drive unbounded buffer growth with header bursts.
    auto readBuffer = sizedBuffer(40 * 1024);
    growReadBuffer(readBuffer, /*usedBytes=*/40 * 1024);
    RUVIA_CHECK_EQ(readBuffer.size(), kMaxHttpHeaderBytes);  // min(80K, 64K)
}

RUVIA_TEST(grow_read_buffer_at_limit_does_not_grow) {
    auto readBuffer = sizedBuffer(kMaxHttpHeaderBytes);
    growReadBuffer(readBuffer, /*usedBytes=*/kMaxHttpHeaderBytes);
    RUVIA_CHECK_EQ(readBuffer.size(), kMaxHttpHeaderBytes);  // hard ceiling holds
}

RUVIA_TEST(grow_read_buffer_no_growth_when_not_full) {
    auto readBuffer = sizedBuffer(8 * 1024);
    growReadBuffer(readBuffer, /*usedBytes=*/100);  // room remains
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{8 * 1024});
}

RUVIA_TEST(trim_read_buffer_reclaims_overgrown_capacity) {
    // A buffer that spilled past the header limit is reclaimed to the initial
    // size once mostly drained, and the still-live prefix is preserved.
    auto readBuffer = sizedBuffer(70 * 1024);  // capacity > 64K shrink threshold
    readBuffer[0] = 'A';
    readBuffer[1] = 'B';
    readBuffer[2] = 'C';
    trimReadBufferStorage(readBuffer, /*usedBytes=*/3);
    RUVIA_CHECK_EQ(readBuffer.size(), kInitialReadBufferBytes);    // back to initial
    RUVIA_CHECK(readBuffer.capacity() < kMaxHttpHeaderBytes);      // capacity reclaimed
    RUVIA_CHECK(readBuffer[0] == 'A' && readBuffer[1] == 'B' && readBuffer[2] == 'C');
}

RUVIA_TEST(trim_read_buffer_normalizes_moderately_grown_buffer_in_place) {
    // The common case: a buffer that doubled (capacity still under the 64K
    // shrink threshold) and is now mostly drained is resized back to the initial
    // size IN PLACE -- the normalize branch, distinct from the fresh-allocation
    // reclaim path for buffers that overgrew past the header limit. The live prefix
    // (within the initial size) must survive the in-place shrink.
    const auto liveIndex = kInitialReadBufferBytes - 100;
    auto readBuffer = sizedBuffer(2 * kInitialReadBufferBytes);
    readBuffer[0] = 'X';
    readBuffer[liveIndex] = 'Y';
    trimReadBufferStorage(readBuffer, /*usedBytes=*/liveIndex + 1);
    RUVIA_CHECK_EQ(readBuffer.size(), kInitialReadBufferBytes);  // normalized to initial
    RUVIA_CHECK(readBuffer[0] == 'X');
    RUVIA_CHECK(readBuffer[liveIndex] == 'Y');                   // live bytes preserved
}

RUVIA_TEST(trim_read_buffer_keeps_buffer_when_still_heavily_used) {
    auto readBuffer = sizedBuffer(70 * 1024);
    trimReadBufferStorage(readBuffer, /*usedBytes=*/9000);  // > initial -> keep as-is
    RUVIA_CHECK_EQ(readBuffer.size(), std::size_t{70 * 1024});
}

RUVIA_TEST(accepted_connection_lease_acquires_moves_and_releases_once) {
    static_assert(!std::default_initializable<AcceptedConnectionLease>);
    static_assert(!std::copy_constructible<AcceptedConnectionLease>);
    static_assert(std::move_constructible<AcceptedConnectionLease>);
    static_assert(!std::is_move_assignable_v<AcceptedConnectionLease>);

    asio::io_context ioContext;
    asio::ip::tcp::socket socket(ioContext);
    socket.open(asio::ip::tcp::v4());
    std::size_t count = 0;
    struct ReleaseObservation final {
        asio::ip::tcp::socket* socket{nullptr};
        std::size_t notifications{0};
        bool socketClosedBeforeNotification{false};
    } observation;
    {
        AcceptedConnectionLease admitted(
            std::move(socket),
            count,
            &observation,
            [](void* target) noexcept {
                auto& observed = *static_cast<ReleaseObservation*>(target);
                ++observed.notifications;
                observed.socketClosedBeforeNotification =
                    observed.socket != nullptr &&
                    !observed.socket->is_open();
            });
        RUVIA_CHECK_EQ(count, std::size_t{1});
        RUVIA_CHECK(admitted.socket().is_open());

        AcceptedConnectionLease session(std::move(admitted));
        observation.socket = &session.socket();
        RUVIA_CHECK_EQ(count, std::size_t{1});
        RUVIA_CHECK(session.socket().is_open());
    }
    RUVIA_CHECK_EQ(count, std::size_t{0});
    RUVIA_CHECK_EQ(observation.notifications, std::size_t{1});
    RUVIA_CHECK(observation.socketClosedBeforeNotification);
}

RUVIA_TEST(accepted_connection_lease_rolls_back_throwing_initiation) {
    asio::io_context ioContext;
    asio::ip::tcp::socket socket(ioContext);
    socket.open(asio::ip::tcp::v4());
    std::size_t count = 0;
    bool released = false;

    try {
        AcceptedConnectionLease admitted(
            std::move(socket),
            count,
            &released,
            [](void* target) noexcept {
                *static_cast<bool*>(target) = true;
            });
        RUVIA_CHECK_EQ(count, std::size_t{1});

        const auto throwingInitiation = [](
            AcceptedConnectionLease) -> void {
            throw std::runtime_error("spawn initiation failed");
        };
        throwingInitiation(std::move(admitted));
        RUVIA_CHECK(false);
    } catch (const std::runtime_error& error) {
        RUVIA_CHECK(
            std::string_view(error.what()) == "spawn initiation failed");
    }

    RUVIA_CHECK_EQ(count, std::size_t{0});
    RUVIA_CHECK(released);
}
