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
#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"

namespace ruvia::detail {

// Worker-affine UDP socket and QUIC client adapter; the caller owns all waits.
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
        bool inputBackpressured{};
        bool outputBackpressured{};
        bool criticalStreamsReady{};
        bool criticalOutputProgress{};
        std::optional<std::chrono::steady_clock::duration> eventTimeout;
    };

    Http3QuicClientSocketSession(asio::io_context& io,
        const asio::ip::udp::endpoint& peer, std::string_view tls_hostname,
        http3_quic_client_tls_context& tls,
        Http3Settings settings = {.qpackMaxTableCapacity = 4096, .qpackBlockedStreams = 16});
    ~Http3QuicClientSocketSession();

    Http3QuicClientSocketSession(const Http3QuicClientSocketSession&) = delete;
    Http3QuicClientSocketSession& operator=(const Http3QuicClientSocketSession&) = delete;
    Http3QuicClientSocketSession(Http3QuicClientSocketSession&&) = delete;
    Http3QuicClientSocketSession& operator=(Http3QuicClientSocketSession&&) = delete;

    [[nodiscard]] PumpResult pump();
    [[nodiscard]] Task<void> waitReadable();
    [[nodiscard]] Task<void> waitWritable();
    [[nodiscard]] Task<WakeReason> waitForActivity(PumpResult pump,
        std::optional<std::chrono::steady_clock::time_point> absoluteDeadline = {});
    void requestStop() noexcept;
    void notifyWork() noexcept;
    [[nodiscard]] bool consumeWorkNotification() noexcept;
    [[nodiscard]] ruvia::quic_connection& transport() noexcept {
        return transport_.connection();
    }
    [[nodiscard]] const ruvia::quic_connection& transport() const noexcept {
        return transport_.connection();
    }
    [[nodiscard]] const asio::ip::udp::endpoint& localEndpoint() const noexcept {
        return localEndpoint_;
    }
    [[nodiscard]] const asio::ip::udp::endpoint& peerEndpoint() const noexcept {
        return peer_;
    }
    [[nodiscard]] ruvia::quic_stream_write_result writeCriticalStream(
        ruvia::http3_critical_stream_output::stream_kind kind, std::span<const char> bytes);
    void close() noexcept;

private:
    [[nodiscard]] static asio::ip::udp::socket makeSocket(asio::io_context& io,
        const asio::ip::udp::endpoint& peer);
    [[nodiscard]] static asio::ip::udp::endpoint concreteLocalEndpoint(
        const asio::ip::udp::socket& socket);
    void requireOwnerThread() const;
    [[nodiscard]] bool sendPending(PumpResult& result);

    std::thread::id ownerThread_;
    asio::ip::udp::endpoint peer_;
    asio::ip::udp::socket socket_;
    asio::steady_timer eventTimer_;
    asio::steady_timer workTimer_;
    asio::ip::udp::endpoint localEndpoint_;
    http3_quic_client_transport transport_;
    std::optional<Http3CriticalStreamDriver> criticalStreams_;
    std::array<std::byte, kDatagramBufferSize> receiveBuffer_{};
    std::array<std::byte, kDatagramBufferSize> packetBuffer_{};
    std::size_t pending_packet_size_{};
    bool stopping_{};
    bool activeWait_{};
    bool workPending_{};
    bool closed_{};
};

}  // namespace ruvia::detail
