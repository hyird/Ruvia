#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/Http3ServerRequest.h"
#include "ruvia/http/HttpDatagram.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpRequestTrailers.h"
#include "ruvia/http/HttpResponseStream.h"
#include "ruvia/web/detail/http3/Http3ServerBodyBudget.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/server/inbound_buffer_resource.h"

namespace ruvia {
class WorkerMemory;
}

namespace ruvia::detail {

class RouteTable;

// Per-connection limits. A caller may additionally lend one worker-wide body
// budget to all sessions owned by that worker.
struct Http3SansIoSessionLimits final {
    std::size_t maxBufferedBodyBytes{kDefaultMaxBufferedBodyBytes};
    std::optional<std::size_t> maxStreamBodyBytes{};
    std::size_t maxStreamBacklogBytes{64 * 1024};
    std::size_t maxLiveStreams{32};
    std::size_t maxBufferedBytesInFlight{64 * 1024 * 1024};
    std::size_t maxTunnelBufferedBytes{64 * 1024};
    Http3ConnectionConfig connection{.enableConnectProtocol = true};
    std::size_t maxQuicDatagramPayloadBytes{};
    std::pmr::memory_resource* inbound_buffer_pool{};
    std::size_t max_inbound_buffer_bytes{64 * 1024 * 1024};
};

// Worker-affine receive-side slice of an HTTP/3 Web session. It synchronously
// copies request heads and buffered body bytes while Http3Connection's event
// views are valid, and resolves routes at HEADERS. Completed requests remain
// owned until release(). Handler scheduling and response writing are purposely
// not part of this first slice.
class Http3SansIoSessionEngine final {
    struct Stream;

public:
    // Worker-affine move-only dispatch borrow. The session and worker must outlive it.
    // Keep it until every request-backed object (including the response) is destroyed;
    // teardown retains the request until this lease is returned. It never crosses threads.
    // Acquisition, moves and metadata access allocate nothing; access uses stable pointers.
    class RequestLease final {
    public:
        ~RequestLease();
        RequestLease(RequestLease&& other) noexcept;
        RequestLease(const RequestLease&) = delete;
        RequestLease& operator=(const RequestLease&) = delete;
        RequestLease& operator=(RequestLease&&) = delete;
        [[nodiscard]] const Http3ServerRequest& request() const& noexcept;
        const Http3ServerRequest& request() const&& = delete;
        [[nodiscard]] const RouteResolution& resolution() const& noexcept;
        const RouteResolution& resolution() const&& = delete;

    private:
        friend class Http3SansIoSessionEngine;
        RequestLease(Http3SansIoSessionEngine& owner, Stream& stream) noexcept
            : owner_(&owner),
              stream_(&stream) {}
        Http3SansIoSessionEngine* owner_{};
        Stream* stream_{};
    };

    enum class StreamState : std::uint8_t {
        kReceiving,
        kReady,
        kRejected,
    };

    struct TunnelReadResult final {
        std::size_t bytes{};
        bool ended{};
        bool reset{};
        bool overflow{};
    };
    enum class Rejection : std::uint8_t {
        kNone,
        kExpectationUnsupported,
        kConnectUnsupported,
        kStreamingUnsupported,
        kWebSocketUnsupported,
        kResponseStreamUnsupported,
        kBodyTooLarge,
        kInFlightBodyCapacity,
        kWorkerBodyBudgetExhausted,
    };

    Http3SansIoSessionEngine(const RouteTable& routes, WorkerMemory& worker,
        Http3SansIoSessionLimits limits = {});
    // The borrowed worker budget must outlive this engine and every lease it
    // issued. It accounts bodies across sessions; per-connection limits remain
    // independently enforced by Http3SansIoSessionLimits.
    Http3SansIoSessionEngine(const RouteTable& routes, WorkerMemory& worker,
        Http3ServerBodyBudget& bodyBudget, Http3SansIoSessionLimits limits = {});
    ~Http3SansIoSessionEngine();
    Http3SansIoSessionEngine(const Http3SansIoSessionEngine&) = delete;
    Http3SansIoSessionEngine& operator=(const Http3SansIoSessionEngine&) = delete;
    Http3SansIoSessionEngine(Http3SansIoSessionEngine&&) = delete;
    Http3SansIoSessionEngine& operator=(Http3SansIoSessionEngine&&) = delete;

    [[nodiscard]] Http3ConnectionResult feed(std::uint64_t streamId,
        std::string_view bytes, bool fin = false, bool reset = false) noexcept;
    // Only a ready, never-dispatched request may be acquired. Read-only raw queries
    // below are for synchronous inspection; asynchronous handlers hold a lease.
    [[nodiscard]] std::optional<RequestLease> acquireRequest(std::uint64_t streamId) & noexcept;
    std::optional<RequestLease> acquireRequest(std::uint64_t streamId) && = delete;
    [[nodiscard]] std::optional<std::uint64_t> peerMaxPushId() const noexcept {
        return connection_.peerMaxPushId();
    }
    [[nodiscard]] std::size_t maxRememberedPushes() const noexcept {
        return limits_.connection.maxRememberedPushes;
    }
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> preparePushPromise(
        std::uint64_t parentStreamId, std::uint64_t pushId, HttpPushRequestView request) {
        return connection_.preparePushPromise(parentStreamId, pushId, request);
    }
    // Copies validated promise metadata into an ordinary request lease on the
    // actual server UNI stream. Prefix publication remains the driver's job.
    [[nodiscard]] std::expected<std::pmr::vector<char>, Http3ConnectionErrorCode> admitPushStream(
        std::uint64_t streamId, std::uint64_t pushId);
    void observePushCancellation(void* context, void (*cancel)(void*, std::uint64_t) noexcept);
    [[nodiscard]] const Http3ServerRequest* request(std::uint64_t streamId) const noexcept;
    [[nodiscard]] const RouteResolution* resolution(std::uint64_t streamId) const noexcept;
    [[nodiscard]] StreamState streamState(std::uint64_t streamId) const noexcept;
    [[nodiscard]] Rejection rejection(std::uint64_t streamId) const noexcept;
    [[nodiscard]] bool streamingRequest(std::uint64_t streamId) const noexcept;
    [[nodiscard]] bool canAcceptInput(std::uint64_t streamId, std::size_t wireBytes) const noexcept;
    [[nodiscard]] Rejection streamingBodyFailure(std::uint64_t streamId) const noexcept;
    [[nodiscard]] const HttpRequestTrailers* requestTrailers(std::uint64_t streamId) const noexcept;
    [[nodiscard]] const std::optional<HttpPriority>* requestPriorityUpdate(std::uint64_t streamId) const noexcept;
    void bindControlOutputWake(void* context, void (*wake)(void*) noexcept);
    [[nodiscard]] bool queueOriginAdvertisement(std::span<const std::string_view> origins);
    [[nodiscard]] std::span<const char> pendingControlOutput() const noexcept {
        return controlOutput_;
    }
    [[nodiscard]] bool consumeControlOutput(std::size_t bytes) noexcept;
    // Worker-PMR bounded tunnel queue. Output is copied synchronously; no
    // borrowed buffer or parser view escapes the feed callback.
    [[nodiscard]] TunnelReadResult readTunnelData(
        std::uint64_t streamId, std::span<char> output) noexcept;
    [[nodiscard]] Http3DatagramReceiveStatus receiveDatagram(Http3DatagramView datagram);
    [[nodiscard]] std::optional<std::pmr::string> takeDatagram(std::uint64_t streamId);
    [[nodiscard]] HttpDatagramSessionConfig datagramConfig(std::uint64_t streamId) const;
    [[nodiscard]] bool tunnelInputOverflowed(std::uint64_t streamId) const noexcept;
    [[nodiscard]] bool tunnelReceiveEnded(std::uint64_t streamId) const noexcept;
    // Effective peer response field-section limit. nullopt means the peer has
    // not sent the setting or omitted it (RFC 9114 default: unlimited).
    [[nodiscard]] std::optional<std::uint64_t> peerMaxFieldSectionSize() const noexcept;
    [[nodiscard]] std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeConnectResponseHead(std::uint64_t streamId, const HttpResponse& response) {
        return connection_.encodeConnectResponseHead(streamId, response);
    }
    [[nodiscard]] std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeResponseHead(std::uint64_t streamId, const HttpResponse& response, HttpBufferedResponseWritePlan plan) {
        return connection_.encodeResponseHead(streamId, response, plan);
    }
    [[nodiscard]] std::expected<Http3StreamingResponseHead, Http3ResponseHeadFailure> encodeStreamingResponseHead(std::uint64_t streamId, HttpResponse response, HttpKnownMethod method, http_response_stream_kind kind, http_response_trailer_intent trailers) {
        return connection_.encodeStreamingResponseHead(streamId, std::move(response), method, kind, trailers);
    }
    [[nodiscard]] std::expected<Http3ResponseHead, Http3ResponseHeadFailure> encodeInterimResponseHead(std::uint64_t streamId, const HttpInterimResponseHead& response) {
        return connection_.encodeInterimResponseHead(streamId, response);
    }
    [[nodiscard]] std::expected<Http3ResponseFieldSection, Http3ResponseHeadFailure> encodeResponseTrailers(std::uint64_t streamId, std::span<const Http3FieldSectionFieldView> fields) {
        return connection_.encodeResponseTrailers(streamId, fields);
    }
    [[nodiscard]] std::span<const char> pendingQpackEncoderOutput() const noexcept {
        return connection_.pendingQpackEncoderOutput();
    }
    [[nodiscard]] std::span<const char> pendingQpackDecoderOutput() const noexcept {
        return connection_.pendingQpackDecoderOutput();
    }
    [[nodiscard]] bool consumeQpackEncoderOutput(std::size_t size) noexcept {
        return connection_.consumeQpackEncoderOutput(size);
    }
    [[nodiscard]] bool consumeQpackDecoderOutput(std::size_t size) noexcept {
        return connection_.consumeQpackDecoderOutput(size);
    }
    [[nodiscard]] std::size_t activeStreamCount() const noexcept;
    [[nodiscard]] bool terminated() const noexcept;
    [[nodiscard]] std::pmr::memory_resource* inbound_buffer_pool() noexcept {
        return &inbound_buffers_;
    }
    // Release only after validated FIN (including a rejected request), or
    // report transport RESET via feed() to retire an incomplete receive side.
    // Never silently drop the body budget for a still-delivering or leased
    // stream. stop/reset retain leased storage until its handler releases it.
    [[nodiscard]] bool release(std::uint64_t streamId) noexcept;
    // Caller must first stop QUIC delivery for this stream. Retires local
    // receive state without fabricating a peer RESET, even before HEADERS finish.
    // A dispatch lease continues to pin its storage and body budget.
    [[nodiscard]] bool cancelRequest(std::uint64_t streamId) noexcept;
    void stop() noexcept;

private:
    struct StreamDeleter final {
        std::pmr::memory_resource* resource{};
        void operator()(Stream* stream) const noexcept;
    };
    using StreamPtr = std::unique_ptr<Stream, StreamDeleter>;
    static void onConnectionEvent(void* context, const Http3ConnectionEvent& event);
    void handleEvent(const Http3ConnectionEvent& event);
    void releaseStream(std::uint64_t streamId) noexcept;
    void releaseLease(Stream& stream) noexcept;
    void rejectBody(Stream& stream, Rejection reason) noexcept;
    void terminate(Http3ConnectionResult failure) noexcept;

    const RouteTable& routes_;
    WorkerMemory& worker_;
    const Http3SansIoSessionLimits limits_;
    inbound_buffer_resource inbound_buffers_;
    Http3ServerBodyBudget* bodyBudget_{nullptr};
    std::size_t bufferedBytesInFlight_{0};
    std::size_t tunnelBytesInFlight_{0};
    std::size_t activeLeases_{0};
    Http3Connection connection_;
    std::pmr::string controlOutput_;
    void* controlWakeContext_{};
    void (*controlWake_)(void*) noexcept {};
    void* pushCancellationContext_{};
    void (*pushCancellation_)(void*, std::uint64_t) noexcept {};
    std::pmr::unordered_map<std::uint64_t, StreamPtr> streams_;
    Http3ConnectionResult failure_{Http3ConnectionStatus::kConnectionError,
        Http3ConnectionErrorScope::kConnection, Http3ConnectionErrorCode::kInternalError};
    bool terminated_{false};
};

}  // namespace ruvia::detail
