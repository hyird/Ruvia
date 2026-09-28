#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <thread>

#include <openssl/types.h>

#include "ruvia/web/detail/http3/Http3QuicClientTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"
#include "ruvia/web/detail/http3/Http3QuicStreamSet.h"

namespace ruvia::detail {

// Worker-thread-affine owner for one OpenSSL QUIC client connection. The TLS
// context and datagram bridge must outlive this object; destroy this object
// before either borrowed dependency.
class Http3QuicClientTransport final {
public:
    using Duration = std::chrono::steady_clock::duration;
    using Error = Http3QuicStreamSet::Error;
    using StreamId = Http3QuicStreamSet::StreamId;
    using StreamInfo = Http3QuicStreamSet::StreamInfo;
    using AcceptedStreams = Http3QuicStreamSet::AcceptedStreams;
    using OpenStream = Http3QuicStreamSet::OpenStream;
    using StreamWrite = Http3QuicStreamSet::StreamWrite;
    using StreamRead = Http3QuicStreamSet::StreamRead;
    using StreamTermination = Http3QuicStreamSet::StreamTermination;
    static constexpr std::size_t kMaxStreamsPerConnection = 32;
    // Leave capacity for the peer control and two QPACK streams even if
    // applications open requests before receiving the peer stream prefixes.
    static constexpr std::size_t kPeerCriticalStreamReserve = 3;
    // OpenSSL 3.6 can retain underlying QUIC stream state beyond SSL_free;
    // do not reuse one connection for an unbounded lifetime of requests.
    // The owner must drain/retire it when this limit is reached.
    static constexpr std::size_t kMaxLifetimeRequests = 64;
    [[nodiscard]] static constexpr bool canOpenRequestStream(
        std::size_t openStreams, std::size_t lifetimeRequests = 0) noexcept {
        return lifetimeRequests < kMaxLifetimeRequests &&
               openStreams < kMaxStreamsPerConnection - kPeerCriticalStreamReserve;
    }
    enum class State : unsigned char {
        kNotStarted,
        kConnecting,
        kH3Ready,
        kTlsFailure,
        kAlpnMismatch,
        kTransportClosed,
    };

    // peer is the resolved UDP destination; host remains the TLS certificate
    // identity and must never be replaced by the resolved address.
    Http3QuicClientTransport(Http3QuicClientTlsContext& tls,
        Http3QuicDatagramBridge& bridge, const Http3QuicDatagramAddress& peer,
        std::string_view host);
    ~Http3QuicClientTransport();

    Http3QuicClientTransport(const Http3QuicClientTransport&) = delete;
    Http3QuicClientTransport& operator=(const Http3QuicClientTransport&) = delete;
    Http3QuicClientTransport(Http3QuicClientTransport&&) = delete;
    Http3QuicClientTransport& operator=(Http3QuicClientTransport&&) = delete;

    [[nodiscard]] State startConnect();
    // Drives handshake, retransmission, ACK and idle timers even after h3 is ready.
    // Performs at most one nonblocking OpenSSL event tick and one handshake step.
    [[nodiscard]] State handleEvents();
    [[nodiscard]] std::optional<Duration> eventTimeout() const;
    [[nodiscard]] State connectionInfo();
    [[nodiscard]] bool requestBudgetExhausted() const noexcept {
        return openedRequestsEver_ >= kMaxLifetimeRequests;
    }

    // Stream operations are available only after h3 ALPN is negotiated. A
    // pending SSL_write retry borrows the exact input range until it succeeds.
    [[nodiscard]] OpenStream openLocalBidirectionalStream();
    [[nodiscard]] OpenStream openLocalUnidirectionalStream();
    [[nodiscard]] AcceptedStreams acceptPeerStreams();
    [[nodiscard]] StreamRead readStream(StreamId id, std::span<char> output);
    [[nodiscard]] StreamWrite writeStream(StreamId id, std::span<const char> input);
    [[nodiscard]] Error finishStream(StreamId id);
    [[nodiscard]] Error streamWriteHealth(StreamId id);
    [[nodiscard]] Error resetStream(StreamId id, std::uint64_t errorCode);
    // Abort the request send direction with H3_REQUEST_CANCELLED if open;
    // release the SSL stream either way. OpenSSL requests receive cancellation
    // with its fixed STOP_SENDING(0), without confirming delivery or peer ACK.
    // Retire HTTP parser state only after close == kNone and no future reads.
    [[nodiscard]] StreamTermination terminateRequestStream(StreamId id);
    [[nodiscard]] Error closeStream(StreamId id);
    // Owner-thread only; closes streams before the parent QUIC connection.
    void close();

private:
    void requireOwnerThread() const;
    [[nodiscard]] State advanceHandshake();
    [[nodiscard]] State inspectConnection();
    [[nodiscard]] Error streamOperationError();
    [[nodiscard]] StreamWrite streamWriteError();
    [[nodiscard]] StreamRead streamReadError();

    std::thread::id ownerThread_;
    std::pmr::unsynchronized_pool_resource streamResource_;
    std::optional<Http3QuicStreamSet> streams_;
    SSL* connection_{};
    std::size_t openedRequestsEver_{};
    State state_{State::kNotStarted};
};

}  // namespace ruvia::detail
