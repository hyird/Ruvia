#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/http/Http3Settings.h"
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
        kCandidateFailure,
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
        Http3Settings settings = {.qpackMaxTableCapacity = 4096, .qpackBlockedStreams = 16},
        ruvia::quic_version initial_version = ruvia::quic_version::v1,
        bool enable_early_data = false,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
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
    void remember_resumption_ticket(std::optional<ruvia::Http3Settings> settings);
    [[nodiscard]] std::size_t take_rejected_early_streams(
        std::span<std::uint64_t> output) noexcept;
    [[nodiscard]] bool early_data_enabled() const noexcept {
        return early_data_enabled_;
    }
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
    [[nodiscard]] ruvia::quic_path_migration start_path_migration(
        const asio::ip::udp::endpoint& local_endpoint);
    [[nodiscard]] std::optional<ruvia::quic_path_migration> path_migration(
        std::uint64_t id) const noexcept;
    [[nodiscard]] std::optional<ruvia::quic_path_migration> active_path_migration() const noexcept;
    [[nodiscard]] ruvia::quic_operation_status cancel_path_migration(std::uint64_t id);
    [[nodiscard]] ruvia::quic_stream_write_result writeCriticalStream(
        ruvia::http3_critical_stream_output::stream_kind kind, std::span<const char> bytes);
    void close() noexcept;

private:
    friend struct Http3QuicClientSocketSessionTestAccess;

    [[nodiscard]] static asio::ip::udp::socket makeSocket(asio::io_context& io,
        const asio::ip::udp::endpoint& peer);
    [[nodiscard]] static asio::ip::udp::endpoint concreteLocalEndpoint(
        const asio::ip::udp::socket& socket);
    [[nodiscard]] static asio::ip::udp::socket make_candidate_socket(asio::io_context& io,
        const asio::ip::udp::endpoint& local, const asio::ip::udp::endpoint& peer);
    void settle_migration() noexcept;
    void fail_candidate_migration() noexcept;
    void reset_rejected_early_streams();
    void requireOwnerThread() const;
    struct SocketSender final {
        [[nodiscard]] std::size_t operator()(asio::ip::udp::socket& socket,
            asio::const_buffer packet, asio::error_code& error) const {
            return socket.send(packet, 0, error);
        }
    };
    template <typename Send>
    [[nodiscard]] bool send_pending(PumpResult& result, Send& send);
    template <typename Send>
    [[nodiscard]] PumpResult pump_with_send(Send& send);

    std::thread::id ownerThread_;
    asio::ip::udp::endpoint peer_;
    asio::io_context& io_;
    asio::ip::udp::socket socket_;
    std::optional<asio::ip::udp::socket> candidate_socket_{};
    std::optional<asio::ip::udp::endpoint> candidate_local_endpoint_{};
    std::optional<asio::ip::udp::endpoint> failed_migration_local_endpoint_{};
    std::optional<std::uint64_t> migration_id_{};
    std::optional<ruvia::quic_path_migration> last_migration_{};
    bool pending_candidate_{};
    asio::steady_timer eventTimer_;
    asio::steady_timer workTimer_;
    asio::ip::udp::endpoint localEndpoint_;
    http3_quic_client_transport transport_;
    Http3Settings settings_;
    std::pmr::vector<std::uint64_t> rejected_early_streams_;
    bool early_data_enabled_{};
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

#include "ruvia/web/detail/http3/Http3QuicClientSocketSession.inl"
