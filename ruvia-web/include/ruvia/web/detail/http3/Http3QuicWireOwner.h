#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/web/detail/http3/Http3DatagramEndpoint.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3QuicTlsContext.h"

namespace ruvia::detail {

// Owner-thread-only driver for long-lived HTTP/3 QUIC wire I/O on the server
// server network thread. It does not admit connections or connect the listener to
// App/workers. The io_context and borrowed TLS context must outlive this owner.
class Http3QuicWireOwner final {
public:
    using Clock = std::chrono::steady_clock;

    enum class ProtocolPumpResult : std::uint8_t { kIdle,
        kProgress,
        kFatal };

    struct ProtocolPump final {
        void* context{};
        ProtocolPumpResult (*drive)(void*, http3_quic_server_transport&,
            Http3DatagramEndpoint&) noexcept {};
    };

    struct StopStatus final {
        bool stopping{};
        bool socketDone{};
        bool timerHandlersRetired{};
        bool transportDestroyed{};
        bool sendInFlight{};
        bool failed{};

        [[nodiscard]] bool complete() const noexcept {
            return stopping && socketDone && timerHandlersRetired && transportDestroyed &&
                   !sendInFlight;
        }
    };

    // timerHandlerResource is borrowed through owner destruction and supplies
    // the associated allocator for timer waits and their retirement posts.
    Http3QuicWireOwner(asio::io_context& networkIo,
        Http3DatagramEndpoint::udp::endpoint bindEndpoint, http3_quic_tls_context& tls,
        ruvia::quic_server_config transportConfig = {},
        std::pmr::memory_resource* timerHandlerResource = nullptr);
    Http3QuicWireOwner(asio::io_context& networkIo,
        Http3DatagramEndpoint::udp::endpoint bindEndpoint, http3_quic_tls_context& tls,
        ruvia::quic_server_config transportConfig,
        std::pmr::memory_resource* timerHandlerResource, ProtocolPump protocolPump);
    ~Http3QuicWireOwner();

    Http3QuicWireOwner(const Http3QuicWireOwner&) = delete;
    Http3QuicWireOwner& operator=(const Http3QuicWireOwner&) = delete;
    Http3QuicWireOwner(Http3QuicWireOwner&&) = delete;
    Http3QuicWireOwner& operator=(Http3QuicWireOwner&&) = delete;

    // Bind UDP and construct the HTTP QUIC server without receiving packets or
    // scheduling timers. Called once on the owner thread before the application's
    // serving barrier is released.
    // Failure starts shutdown and is rethrown; pollStop() must complete before
    // destruction, just as after a failed start().
    void prepare();
    // Start receiving after prepare(). Synchronous failures are recorded and
    // rethrown after initiating shutdown; the owner must remain alive until
    // pollStop() reports complete.
    void start();
    void requestStop() noexcept;
    // NetworkRuntime uses this gate when it must retire connections and channels
    // after a fatal socket stop but before destroying the QUIC transport.
    void deferTransportRetirement() noexcept;
    void releaseTransportRetirement() noexcept;
    // Requests another bounded owner-thread turn after a worker/channel wake.
    void requestDrive() noexcept;

    // Call on the owner thread outside I/O callbacks to perform the final ordered
    // destruction once socket and timer completion handlers have really retired.
    void pollStop() noexcept;
    [[nodiscard]] StopStatus stopStatus() const noexcept;
    [[nodiscard]] std::uint16_t boundPort() const noexcept;
    [[nodiscard]] http3_quic_server_transport* transport() noexcept {
        return transport_ ? &*transport_ : nullptr;
    }
    [[nodiscard]] bool outboundQuiescent() const noexcept {
        return endpoint_.outbound_quiescent();
    }
    [[nodiscard]] bool socketIoQuiescent() const noexcept {
        requireOwnerThread();
        return endpoint_.socket_done() && !endpoint_.send_in_flight();
    }
    [[nodiscard]] bool consumeTransportActivity() noexcept {
        requireOwnerThread();
        return std::exchange(transportActivity_, false);
    }
    [[nodiscard]] std::size_t timerExpirations() const noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    void rethrowFailure() const;

private:
    class TimerWaitHandler final {
    public:
        using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

        TimerWaitHandler(Http3QuicWireOwner& owner, std::uint64_t generation) noexcept;
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_;
        }
        TimerWaitHandler(TimerWaitHandler&& other) noexcept;
        ~TimerWaitHandler();

        TimerWaitHandler(const TimerWaitHandler&) = delete;
        TimerWaitHandler& operator=(const TimerWaitHandler&) = delete;
        TimerWaitHandler& operator=(TimerWaitHandler&&) = delete;

        void operator()(const asio::error_code& error) noexcept;

    private:
        Http3QuicWireOwner* owner_{};
        std::uint64_t generation_{};
        allocator_type allocator_;
    };

    class TimerRetirementHandler final {
    public:
        using allocator_type = std::pmr::polymorphic_allocator<std::byte>;

        explicit TimerRetirementHandler(Http3QuicWireOwner& owner) noexcept;
        [[nodiscard]] allocator_type get_allocator() const noexcept {
            return allocator_;
        }
        TimerRetirementHandler(TimerRetirementHandler&& other) noexcept;
        ~TimerRetirementHandler();

        TimerRetirementHandler(const TimerRetirementHandler&) = delete;
        TimerRetirementHandler& operator=(const TimerRetirementHandler&) = delete;
        TimerRetirementHandler& operator=(TimerRetirementHandler&&) = delete;

        void operator()() noexcept;

    private:
        Http3QuicWireOwner* owner_{};
        allocator_type allocator_;
    };

    static void endpointNotification(void* context,
        Http3DatagramEndpoint::notification_kind kind) noexcept;

    void requireOwnerThread() const noexcept;
    void onEndpointNotification(Http3DatagramEndpoint::notification_kind kind) noexcept;
    void drive() noexcept;
    void updateDeadline();
    void reconcileTimer() noexcept;
    void armTimer(Clock::time_point deadline) noexcept;
    void onTimerCompletion(std::uint64_t generation,
        const asio::error_code& error) noexcept;
    void onTimerHandlerRetired(std::uint64_t generation) noexcept;
    void onTimerRetirementWake() noexcept;
    void onTimerRetirementHandlerRetired() noexcept;
    void beginStop() noexcept;
    void recordFailure(std::exception_ptr failure) noexcept;
    void recordCurrentFailure() noexcept;
    void recordError(std::error_code error, const char* operation) noexcept;
    void captureEndpointError() noexcept;

    std::thread::id ownerThread_;
    asio::io_context& networkIo_;
    http3_quic_tls_context& tls_;
    ruvia::quic_server_config transportConfig_;
    Http3DatagramEndpoint endpoint_;
    asio::steady_timer timer_;
    std::pmr::polymorphic_allocator<std::byte> timerHandlerAllocator_;
    ProtocolPump protocolPump_{};
    std::optional<http3_quic_server_transport> transport_;
    std::optional<Clock::time_point> desiredDeadline_;
    std::optional<Clock::time_point> armedDeadline_;
    std::exception_ptr failure_;
    std::uint64_t timerGeneration_{};
    std::uint64_t armedGeneration_{};
    std::uint64_t failedTimerSubmissionGeneration_{};
    std::size_t timerExpirations_{};
    bool timerSubmitting_{};
    bool prepared_{};
    bool started_{};
    bool stopping_{};
    bool driving_{};
    bool driveRequested_{};
    bool timerWaitOutstanding_{};
    bool timerCancelRequested_{};
    bool timerCompletionDelivered_{};
    bool timerWakeOutstanding_{};
    bool timerHandlersRetired_{true};
    bool transportDestroyed_{true};
    bool transportRetirementReleased_{true};
    bool transportActivity_{};
};

}  // namespace ruvia::detail
