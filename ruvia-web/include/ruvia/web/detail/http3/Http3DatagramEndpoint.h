#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <thread>

#include <asio/ip/udp.hpp>

#include "ruvia/web/detail/http3/Http3QuicDatagramBridge.h"
#include "ruvia/web/detail/http3/Http3UdpSocket.h"

namespace ruvia::detail {

// Owner-thread adapter for a UDP socket owned by the server network thread and
// its QUIC datagram BIO bridge. Wildcard binds retain the packet's concrete local destination through
// pktinfo so OpenSSL can emit replies from the address the peer contacted. It
// owns no QUIC/TLS object or timer and is not App wiring.
class Http3DatagramEndpoint final {
public:
    using Udp = asio::ip::udp;

    enum class NotificationKind : std::uint8_t {
        kInputAvailable,
        kOutputDrained,
        kStopping,
    };

    struct Notification final {
        // The caller owns context and keeps it alive through endpoint drain.
        void* context{};
        void (*notify)(void*, NotificationKind) noexcept {};
    };

    enum class PumpResult : std::uint8_t {
        kIdle,
        kPending,
        kBackpressured,
        kStopped,
        kError,
    };

    enum class StopStatus : std::uint8_t {
        kPending,
        kDone,
        kError,
    };

    // A lease keeps the endpoint's bridge alive while its SSL-side BIO is owned
    // elsewhere. Destroy that BIO owner before releasing the lease.
    class BridgeLease final {
    public:
        BridgeLease(BridgeLease&& other) noexcept;
        BridgeLease& operator=(BridgeLease&& other) noexcept;
        ~BridgeLease();

        BridgeLease(const BridgeLease&) = delete;
        BridgeLease& operator=(const BridgeLease&) = delete;

        [[nodiscard]] Http3QuicDatagramBridge& bridge() const noexcept;

    private:
        friend class Http3DatagramEndpoint;
        explicit BridgeLease(Http3DatagramEndpoint& owner) noexcept;
        void reset() noexcept;

        Http3DatagramEndpoint* owner_{};
    };

    Http3DatagramEndpoint(asio::io_context& networkIo, Udp::endpoint bindEndpoint,
        Notification notification);
    ~Http3DatagramEndpoint();

    Http3DatagramEndpoint(const Http3DatagramEndpoint&) = delete;
    Http3DatagramEndpoint& operator=(const Http3DatagramEndpoint&) = delete;
    Http3DatagramEndpoint(Http3DatagramEndpoint&&) = delete;
    Http3DatagramEndpoint& operator=(Http3DatagramEndpoint&&) = delete;

    // prepare() opens/binds UDP first, then constructs the bridge using the actual
    // bound port. A requested port of zero selects an ephemeral port.
    void prepare();
    [[nodiscard]] std::uint16_t boundPort() const noexcept;
    [[nodiscard]] BridgeLease acquireBridge();

    // start() arms the single receive slot. Notifications run synchronously on the
    // owner thread under a callback-depth lifetime guard. A callback may re-enter
    // requestStop(); kStopping is emitted once and stopping state is committed first.
    // Callbacks must not destroy this endpoint. kInputAvailable signals successful injection or first BIO backpressure;
    // kOutputDrained is emitted only when a send completion drains the queue. Calling
    // sendPending() after external QUIC progress does not notify when already idle.
    // kBackpressured retains the completed receive view without rearming; retry it
    // after protocol progress. Sends are serialized and advanced by completion.
    [[nodiscard]] PumpResult start() noexcept;
    [[nodiscard]] PumpResult retryHeldReceive() noexcept;
    [[nodiscard]] PumpResult sendPending() noexcept;
    // Owner-thread-only observation of the serialized UDP send slot.
    [[nodiscard]] bool sendInFlight() const noexcept;
    [[nodiscard]] bool outboundQuiescent() const noexcept;

    // Nonblocking owner-thread shutdown. socketDone() includes all socket callback
    // storage retirement; stopStatus() also waits for every BridgeLease to be released.
    void requestStop() noexcept;
    [[nodiscard]] bool socketDone() const noexcept;
    [[nodiscard]] StopStatus stopStatus() const noexcept;
    [[nodiscard]] std::error_code error() const noexcept;

private:
    static Udp::endpoint checkedBindEndpoint(Udp::endpoint endpoint);
    static Notification checkedNotification(Notification notification);
    static void receiveCompletion(void* context, std::error_code error,
        Http3UdpSocket::ReceiveView view) noexcept;
    static void sendCompletion(void* context, std::error_code error,
        std::size_t size) noexcept;

    void requireOwnerThread() const noexcept;
    void releaseBridgeLease() noexcept;
    [[nodiscard]] bool armReceive() noexcept;
    [[nodiscard]] PumpResult injectHeldReceive() noexcept;
    [[nodiscard]] PumpResult sendPendingImpl(bool notifyWhenDrained) noexcept;
    void handleReceive(std::error_code error, Http3UdpSocket::ReceiveView view) noexcept;
    void handleSend(std::error_code error, std::size_t size) noexcept;
    void notify(NotificationKind kind) noexcept;
    void fail(std::error_code error) noexcept;

    Udp::endpoint bindEndpoint_;
    Udp::endpoint boundEndpoint_;
    std::thread::id ownerThread_;
    Http3UdpSocket socket_;
    Notification notification_;
    std::optional<Http3QuicDatagramBridge> bridge_;
    Http3UdpSocket::ReceiveView heldReceive_;
    std::error_code error_;
    // Synchronous callback lifetime guard; stopStatus stays pending while nonzero.
    std::size_t callbackDepth_{};
    std::size_t sendSize_{};
    bool prepared_{};
    bool started_{};
    bool receiveArmed_{};
    bool hasHeldReceive_{};
    // A retained packet gets one wakeup until it is accepted or discarded.
    bool heldReceiveBackpressureNotified_{};
    bool sendInFlight_{};
    bool stopping_{};
    bool stoppingNotified_{};
    bool bridgeLeaseActive_{};
};

}  // namespace ruvia::detail
