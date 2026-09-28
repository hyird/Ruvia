#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <thread>

#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamDriver.h"
#include "ruvia/web/detail/http3/Http3QuicClientTransport.h"

namespace ruvia::detail {

// Worker-thread-affine UDP socket + QUIC bridge + transport owner. This is a
// sans-runtime pump, not a production client driver: the caller owns all waits.
class Http3QuicClientSocketSession final {
public:
    static constexpr std::size_t kDatagramBufferSize = 65536;
    static constexpr std::size_t kBatchSize = 32;

    enum class PumpStatus : unsigned char { kActive,
        kWouldBlock,
        kClosed,
        kFatal };
    enum class WakeReason : unsigned char {
        kReadable,
        kWritable,
        kQuicEvent,
        kApplication,
        kDeadline,
        kStopped,
        kFatal,
    };
    struct PumpResult final {
        PumpStatus status{PumpStatus::kActive};
        std::size_t received{};
        std::size_t sent{};
        // Input BIO full: retry the one owned datagram after a bounded QUIC
        // event tick; waiting only for socket readability would deadlock.
        bool inputBackpressured{};
        // A borrowed outbound datagram remains stable until the socket is writable.
        bool outputBackpressured{};
        // Requests must wait for local SETTINGS and QPACK stream prefixes to
        // be accepted by QUIC. Progress calls for another bounded pump tick.
        bool criticalStreamsReady{};
        bool criticalOutputProgress{};
        std::optional<Http3QuicClientTransport::Duration> eventTimeout;
    };

    Http3QuicClientSocketSession(asio::io_context& io,
        const asio::ip::udp::endpoint& peer, std::string_view tlsHostname,
        Http3QuicClientTlsContext& tls);
    ~Http3QuicClientSocketSession();

    Http3QuicClientSocketSession(const Http3QuicClientSocketSession&) = delete;
    Http3QuicClientSocketSession& operator=(const Http3QuicClientSocketSession&) = delete;
    Http3QuicClientSocketSession(Http3QuicClientSocketSession&&) = delete;
    Http3QuicClientSocketSession& operator=(Http3QuicClientSocketSession&&) = delete;

    // The caller owns the single pump driver and all of its started Tasks.
    // A fatal result requires close(); closing the socket wakes suspended waits,
    // which must be joined before destroying this session.
    [[nodiscard]] PumpResult pump();
    [[nodiscard]] Task<void> waitReadable();
    [[nodiscard]] Task<void> waitWritable();
    // One worker-local wait cycle. Waits for readable UDP, conditional writable
    // UDP, application work and the earlier QUIC event/absolute deadline;
    // cancellation drains
    // every armed handler before the Task resumes. Only one cycle may run at a
    // time, and it must not overlap the separate legacy waitReadable/Writable
    // calls. The caller owns the started Task until it completes.
    [[nodiscard]] Task<WakeReason> waitForActivity(PumpResult pump,
        std::optional<std::chrono::steady_clock::time_point> absoluteDeadline = {});
    // Worker-owner only. Wakes every active wait without destroying transport;
    // await/join the driver before close() or destroying this session.
    void requestStop() noexcept;
    // A latched, owner-worker notification: enqueue/cancel work, then notify.
    // The sole driver consumes the latch before processing that work. Off-worker
    // callers must first post to its stable worker endpoint.
    void notifyWork() noexcept;
    [[nodiscard]] bool consumeWorkNotification() noexcept;
    [[nodiscard]] Http3QuicClientTransport& transport() noexcept {
        return transport_;
    }
    [[nodiscard]] const Http3QuicClientTransport& transport() const noexcept {
        return transport_;
    }
    [[nodiscard]] const asio::ip::udp::endpoint& localEndpoint() const noexcept {
        return localEndpoint_;
    }
    [[nodiscard]] const asio::ip::udp::endpoint& peerEndpoint() const noexcept {
        return peer_;
    }
    void close() noexcept;

private:
    [[nodiscard]] static asio::ip::udp::socket makeSocket(asio::io_context& io,
        const asio::ip::udp::endpoint& peer);
    [[nodiscard]] static asio::ip::udp::endpoint concreteLocalEndpoint(
        const asio::ip::udp::socket& socket);
    void requireOwnerThread() const;

    std::thread::id ownerThread_;
    asio::ip::udp::endpoint peer_;
    Http3QuicDatagramAddress peerAddress_;
    asio::ip::udp::socket socket_;
    asio::steady_timer eventTimer_;
    asio::steady_timer workTimer_;
    asio::ip::udp::endpoint localEndpoint_;
    Http3QuicDatagramBridge bridge_;
    Http3QuicClientTransport transport_;
    std::optional<Http3CriticalStreamDriver> criticalStreams_;
    Http3QuicOutboundDatagram outbound_{};
    std::array<std::byte, kDatagramBufferSize> receiveBuffer_{};
    std::array<std::byte, kDatagramBufferSize> pendingDatagram_{};
    std::size_t pendingSize_{};
    bool outboundOutstanding_{};
    bool started_{};
    bool stopping_{};
    bool activeWait_{};
    bool workPending_{};
    bool closed_{};
};

}  // namespace ruvia::detail
