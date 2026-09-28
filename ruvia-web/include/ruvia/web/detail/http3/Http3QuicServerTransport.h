#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <openssl/types.h>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"
#include "ruvia/web/detail/http3/Http3QuicStreamSet.h"
#include "ruvia/web/detail/http3/Http3QuicTlsContext.h"

namespace ruvia::detail {

struct Http3QuicServerTransportConfig final {
    std::size_t maxActiveConnections{128};
    std::size_t maxLifetimePeerStreams{Http3QuicStreamSet::kMaxLifetimePeerStreams};
    std::chrono::milliseconds handshakeTimeout{10000};
    // nullopt maps to QUIC's explicit zero (no idle timeout).
    std::optional<std::chrono::milliseconds> idleTimeout{std::chrono::seconds(75)};
};

// Owns the OpenSSL QUIC domain/listener on the UDP owner thread. Both the TLS
// identity owner (certificate callback userdata) and the bridge must outlive
// this transport; the listener owns the bridge's SSL-side BIO. The network
// owner must finish/cancel an asynchronous UDP send borrowing the bridge's
// outbound bytes before advancing OpenSSL or retiring SSL state, including
// handleEvents(), acceptConnections() rollback, and handshake expiry.
class Http3QuicServerTransport final {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    enum class EventResult : unsigned char { kHandled,
        kNonFatal,
        kFatal };

    using ConnectionId = std::uint64_t;
    using Error = Http3QuicStreamSet::Error;
    enum class ConnectionCloseStatus : unsigned char {
        kPending,
        kCompleted,
        kFailure,
        kNoConnection,
        kConflict,
    };
    struct ConnectionCloseResult final {
        ConnectionCloseStatus status{ConnectionCloseStatus::kFailure};
        // SSL_free() has returned for every stream SSL owned by this connection.
        bool allStreamsRetired{};
    };
    struct ConnectionInfo final {
        ConnectionId id{};
        bool handshakeComplete{};
        bool h3Negotiated{};
        // A failed handshake can remain in the accept queue or the owned map.
        // Callers must explicitly close terminated connections to return credit.
        bool terminated{};
        std::uint64_t closeErrorCode{};
        std::uint32_t closeFlags{};
        // Available after handshake completion; zero means no negotiated idle timeout.
        std::uint64_t negotiatedIdleTimeoutMilliseconds{};
        // Populated only after handshake completion. These legacy field names carry
        // the Initial datagram's observed source path confirmed by the handshake; a
        // concrete accepted-child BIO endpoint takes precedence when available. This
        // is not a current migrated path or authenticated peer identity. Borrowed
        // through retirement.
        std::string_view remoteAddress{};
        std::uint16_t remotePort{};
        std::string_view clientCertificateSubject{};
    };

    // Accepted connections and the listener's pending queue are separate
    // populations. MAX_PENDING_CONNS is configured with the queue cap below;
    // transport construction fails if OpenSSL cannot enforce this bound.
    // Neither count is a byte-level QUIC memory budget; the wire owner and
    // handshake deadline provide those independent bounds.
    static constexpr std::size_t kMaxPendingConnections = 32;
    static constexpr std::size_t kAcceptBatchLimit = 32;
    struct AcceptedBatch final {
        std::array<ConnectionId, kAcceptBatchLimit> ids{};
        std::size_t size{};
        [[nodiscard]] bool empty() const noexcept {
            return size == 0;
        }
    };
    using StreamId = Http3QuicStreamSet::StreamId;
    static constexpr std::size_t kMaxStreamsPerConnection =
        Http3QuicStreamSet::kMaxStreamsPerConnection;
    using StreamInfo = Http3QuicStreamSet::StreamInfo;
    using AcceptedStreams = Http3QuicStreamSet::AcceptedStreams;
    using OpenStream = Http3QuicStreamSet::OpenStream;
    using StreamWrite = Http3QuicStreamSet::StreamWrite;
    using StreamRead = Http3QuicStreamSet::StreamRead;
    using StreamTermination = Http3QuicStreamSet::StreamTermination;

    Http3QuicServerTransport(Http3QuicTlsContext& tls, Http3QuicDatagramBridge& bridge,
        Http3QuicServerTransportConfig config = {});
    ~Http3QuicServerTransport();

    Http3QuicServerTransport(const Http3QuicServerTransport&) = delete;
    Http3QuicServerTransport& operator=(const Http3QuicServerTransport&) = delete;
    Http3QuicServerTransport(Http3QuicServerTransport&&) = delete;
    Http3QuicServerTransport& operator=(Http3QuicServerTransport&&) = delete;

    [[nodiscard]] EventResult handleEvents();
    [[nodiscard]] std::optional<Duration> eventTimeout();
    // Owner-clock maintenance; accepted but incomplete handshakes only. The
    // pending listener queue has its own OpenSSL policy. Completed handshakes
    // and their streams are unaffected, even after this deadline.
    [[nodiscard]] std::size_t retireExpiredHandshakes(Clock::time_point now);

    // Owner-thread only. The caller supplies per-call pre-authorized connection credits;
    // no SSL* escapes. Each connection remains owned until retireConnectionLocally() or
    // transport destruction. Zero credits leave the OpenSSL accept queue untouched.
    [[nodiscard]] AcceptedBatch acceptConnections(std::size_t availableConnectionCredits);
    [[nodiscard]] std::optional<ConnectionInfo> connectionInfo(ConnectionId id) const;
    [[nodiscard]] AcceptedStreams acceptStreams(ConnectionId id);
    // A pending SSL_write retry borrows the exact input range: retain its address,
    // contents and length unchanged until a later call accepts it or the stream closes.
    [[nodiscard]] OpenStream openLocalUnidirectionalStream(ConnectionId id);
    [[nodiscard]] StreamWrite writeStream(ConnectionId id, StreamId streamId,
        std::span<const char> input);
    // Call only after all application bytes were accepted; no pending write may remain.
    [[nodiscard]] Error finishStream(ConnectionId id, StreamId streamId);
    [[nodiscard]] Error resetStream(ConnectionId id, StreamId streamId, std::uint64_t errorCode);
    // Resets the local sending direction, then releases the SSL wrapper even if reset fails.
    [[nodiscard]] StreamTermination terminateBidirectionalStream(ConnectionId id,
        StreamId streamId, std::uint64_t sendErrorCode);
    [[nodiscard]] StreamRead readStream(ConnectionId id, StreamId streamId, std::span<char> output);
    // Normal server retirement only; returns kWouldBlock until send FIN was
    // accepted and readStream observed peer EOF/RESET after all buffered bytes.
    // Unlike closeStream(), this never stops the peer's sending direction.
    [[nodiscard]] Error retireCompletedBidirectionalStream(ConnectionId id, StreamId streamId);
    // Force-closes either direction and may schedule STOP_SENDING for an unread peer stream.
    [[nodiscard]] Error closeStream(ConnectionId id, StreamId streamId);

    // Owner-thread only. Latches the first application error code, freezes stream
    // operations, frees all stream SSL wrappers (so pending SSL_write input is no
    // longer borrowed), then initiates or advances a rapid QUIC CONNECTION_CLOSE.
    // Repeating the same request drives a pending close; a different request returns
    // kConflict.
    // kCompleted means OpenSSL's local shutdown state is complete, not that the
    // peer received or acknowledged the close. handleEvents() advances pending
    // closes; the caller must continue its bounded UDP send/receive drive to give
    // the close packet a best-effort chance to reach the peer.
    [[nodiscard]] ConnectionCloseResult requestConnectionClose(
        ConnectionId id, Http3ConnectionErrorCode errorCode);
    // Starts a non-rapid H3_NO_ERROR shutdown. Stream wrappers remain alive
    // while OpenSSL reports the shutdown pending; the owner drives it until
    // completion or its own deadline. allStreamsRetired becomes true only after
    // OpenSSL reports completion and those wrappers have been freed.
    [[nodiscard]] ConnectionCloseResult requestGracefulConnectionClose(ConnectionId id);
    // Forced local SSL_free/erase only: emits no CONNECTION_CLOSE and waits for no
    // network event. Any asynchronous send borrowing a bridge outbound span must
    // complete or be cancelled and joined, then returned via completeOutbound(),
    // before retiring here, destroying this transport, or destroying the bridge.
    [[nodiscard]] Error retireConnectionLocally(ConnectionId id);

private:
    struct Connection;
    void requireOwnerThread() const;
    [[nodiscard]] ConnectionCloseResult requestConnectionClose(ConnectionId id,
        Http3ConnectionErrorCode errorCode, bool graceful);
    [[nodiscard]] ConnectionCloseResult driveConnectionClose(Connection& connection);

    std::thread::id ownerThread_;
    Http3QuicDatagramBridge& bridge_;
    SSL* domain_{};
    SSL* listener_{};
    std::size_t maxActive_{};
    std::size_t maxLifetimePeerStreams_{};
    Duration handshakeTimeout_{};
    std::uint64_t idleTimeoutMilliseconds_{};
    ConnectionId nextConnectionId_{1};
    struct Connection final {
        Connection(SSL* value, std::pmr::memory_resource* resource,
            Clock::time_point deadline, std::string_view initialPeerAddress,
            std::uint16_t initialPeerPort, std::string_view acceptedChildPeerAddress,
            std::uint16_t acceptedChildPeerPort, std::size_t maxLifetimePeerStreams)
            : ssl(value),
              streams(value, resource, kMaxStreamsPerConnection, maxLifetimePeerStreams),
              handshakeDeadline(deadline),
              initialObservedPeerAddress(initialPeerAddress, resource),
              acceptedChildPeerAddress(acceptedChildPeerAddress, resource),
              clientCertificateSubject(resource),
              initialObservedPeerPort(initialPeerPort),
              acceptedChildPeerPort(acceptedChildPeerPort) {}
        SSL* ssl{};
        Http3QuicStreamSet streams;
        Clock::time_point handshakeDeadline;
        // Source address observed on the Initial datagram, retained privately until
        // handshake completion confirms the path. The accepted-child BIO can provide
        // a concrete endpoint which takes precedence when available.
        std::pmr::string initialObservedPeerAddress;
        mutable std::pmr::string acceptedChildPeerAddress;
        mutable std::pmr::string clientCertificateSubject;
        std::uint16_t initialObservedPeerPort{};
        mutable std::uint16_t acceptedChildPeerPort{};
        mutable bool certificateSubjectLoaded{};
        std::optional<Http3ConnectionErrorCode> closeErrorCode;
        bool gracefulClose{};
        ConnectionCloseStatus closeStatus{ConnectionCloseStatus::kPending};
    };
    std::pmr::unsynchronized_pool_resource connectionResource_;
    std::pmr::deque<Http3QuicDatagramAddress> pendingInitialPeerPaths_;
    std::pmr::unordered_map<ConnectionId, Connection> connections_;
    std::uint64_t lastMappedInjectionGeneration_{};
};

}  // namespace ruvia::detail
