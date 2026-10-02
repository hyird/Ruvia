#include "ruvia/web/detail/http3/Http3QuicClientSocketSession.h"

#include <algorithm>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/web/detail/http3/Http3QuicPacketIo.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

namespace ruvia::detail {
namespace {

http3_quic_datagram_address datagram_address(const asio::ip::udp::endpoint& endpoint) {
    auto result = to_http3_quic_datagram_address(endpoint);
    if (!result) {
        throw std::invalid_argument("QUIC UDP endpoint is not a supported concrete address");
    }
    return *result;
}

ruvia::quic_connection_config client_connection_config(
    const asio::ip::udp::endpoint& local, const asio::ip::udp::endpoint& peer) {
    ruvia::quic_connection_config config{};
    config.role = ruvia::quic_role::client;
    config.local_address = to_quic_address(datagram_address(local));
    config.peer_address = to_quic_address(datagram_address(peer));
    config.local_transport_parameters.max_datagram_frame_size =
        config.limits.max_datagram_size;
    return config;
}

// This awaiter lives inside the single driver's coroutine frame. Socket and
// timer callbacks borrow it only until *every* armed operation has completed;
// the first wake cancels losers but never resumes the owner early. No sibling
// Task is created, and an initiation failure drains already-armed operations.
class Http3ClientActivityWait final {
public:
    using Session = Http3QuicClientSocketSession;
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Reason = Session::WakeReason;

    Http3ClientActivityWait(asio::ip::udp::socket& socket, asio::steady_timer& timer,
        asio::steady_timer& workTimer, bool& stopping, bool& active,
        bool& workPending, Session::PumpResult pump,
        std::optional<TimePoint> deadline) noexcept
        : socket_(socket),
          timer_(timer),
          workTimer_(workTimer),
          stopping_(stopping),
          active_(active),
          workPending_(workPending),
          pump_(pump),
          deadline_(deadline) {}

    Http3ClientActivityWait(const Http3ClientActivityWait&) = delete;
    Http3ClientActivityWait& operator=(const Http3ClientActivityWait&) = delete;
    ~Http3ClientActivityWait() {
        if (pending_ != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] bool await_ready() noexcept {
        if (stopping_ || pump_.status == Session::PumpStatus::kClosed) {
            reason_ = Reason::kStopped;
            return true;
        }
        if (pump_.status == Session::PumpStatus::kFatal) {
            reason_ = Reason::kFatal;
            return true;
        }
        if (active_) {
            reason_ = Reason::kFatal;
            return true;
        }
        if (deadline_ && Clock::now() >= *deadline_) {
            reason_ = Reason::kDeadline;
            return true;
        }
        if (workPending_) {
            reason_ = Reason::kApplication;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (stopping_) {
            reason_ = Reason::kStopped;
            return false;
        }
        if (active_) {
            reason_ = Reason::kFatal;
            return false;
        }
        if (workPending_) {
            reason_ = Reason::kApplication;
            return false;
        }
        active_ = true;
        ownsActive_ = true;
        continuation_ = continuation;
        arming_ = true;
        try {
            const auto now = Clock::now();
            std::optional<TimePoint> wakeAt = deadline_;
            auto timeout = pump_.eventTimeout;
            if (pump_.inputBackpressured) {
                const auto retry = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::milliseconds(1));
                timeout = timeout ? std::min(*timeout, retry) : retry;
            }
            if (timeout) {
                auto duration = std::max(*timeout,
                    std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(1)));
                duration = std::min(duration, TimePoint::max() - now);
                const auto quicWake = now + duration;
                if (!wakeAt || quicWake < *wakeAt) {
                    wakeAt = quicWake;
                }
            }
            if (!pump_.inputBackpressured) {
                armRead();
            }
            if (!winnerChosen_ && pump_.outputBackpressured) {
                armWrite();
            }
            if (!winnerChosen_ && wakeAt) {
                armTimer(*wakeAt);
            }
            // A full input BIO must never depend solely on another socket read.
            if (!winnerChosen_ && pending_ == 0) {
                armTimer(now + std::chrono::milliseconds(1));
            }
            if (!winnerChosen_) {
                armWork();
            }
        } catch (...) {
            failure_ = std::current_exception();
            reason_ = Reason::kFatal;
            winnerChosen_ = true;
            cancelOutstanding();
            if (pending_ == 0) {
                woke_ = true;
            }
        }
        arming_ = false;
        return !woke_;
    }

    [[nodiscard]] Reason await_resume() {
        if (ownsActive_) {
            active_ = false;
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (stopping_) {
            return Reason::kStopped;
        }
        if (deadline_ && Clock::now() >= *deadline_) {
            return Reason::kDeadline;
        }
        return workPending_ ? Reason::kApplication : reason_;
    }

private:
    void armRead() {
        ++pending_;
        try {
            socket_.async_wait(asio::ip::udp::socket::wait_read,
                [this](const asio::error_code& error) {
                    complete(Reason::kReadable, error);
                });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void armWrite() {
        ++pending_;
        try {
            socket_.async_wait(asio::ip::udp::socket::wait_write,
                [this](const asio::error_code& error) {
                    complete(Reason::kWritable, error);
                });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void armTimer(TimePoint at) {
        timer_.expires_at(at);
        ++pending_;
        try {
            timer_.async_wait([this](const asio::error_code& error) {
                complete(Reason::kQuicEvent, error);
            });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void armWork() {
        workTimer_.expires_at(TimePoint::max());
        ++pending_;
        try {
            workTimer_.async_wait([this](const asio::error_code& error) {
                complete(Reason::kApplication, error);
            });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void cancelOutstanding() noexcept {
        asio::error_code ignored;
        (void)timer_.cancel(ignored);
        (void)workTimer_.cancel(ignored);
        (void)socket_.cancel(ignored);
    }
    void complete(Reason event, const asio::error_code& error) {
        if (pending_ == 0) {
            std::terminate();
        }
        --pending_;
        if (!winnerChosen_) {
            winnerChosen_ = true;
            reason_ = stopping_ ? Reason::kStopped
                      : event == Reason::kApplication
                          ? (workPending_ ? Reason::kApplication : Reason::kFatal)
                      : error ? Reason::kFatal
                              : event;
            cancelOutstanding();
        }
        if (pending_ != 0 || woke_) {
            return;
        }
        woke_ = true;
        if (!arming_) {
            continuation_.resume();
        }
    }

    asio::ip::udp::socket& socket_;
    asio::steady_timer& timer_;
    asio::steady_timer& workTimer_;
    bool& stopping_;
    bool& active_;
    bool& workPending_;
    Session::PumpResult pump_;
    std::optional<TimePoint> deadline_;
    std::coroutine_handle<> continuation_{};
    std::exception_ptr failure_;
    Reason reason_{Reason::kFatal};
    std::size_t pending_{};
    bool ownsActive_{};
    bool arming_{};
    bool winnerChosen_{};
    bool woke_{};
};

}  // namespace

asio::ip::udp::socket Http3QuicClientSocketSession::makeSocket(asio::io_context& io,
    const asio::ip::udp::endpoint& peer) {
    asio::ip::udp::socket socket(io);
    asio::error_code error;
    socket.open(peer.protocol(), error);
    if (error) {
        throw std::system_error(error, "open QUIC UDP socket");
    }
    socket.connect(peer, error);
    if (error) {
        if (error == asio::error::in_progress || error == asio::error::would_block ||
            error == asio::error::try_again) {
            throw std::system_error(error, "QUIC UDP connect is still in progress");
        }
        throw std::system_error(error, "connect QUIC UDP socket");
    }
    socket.non_blocking(true, error);
    if (error) {
        throw std::system_error(error, "set QUIC UDP socket nonblocking");
    }
    return socket;
}

asio::ip::udp::endpoint Http3QuicClientSocketSession::concreteLocalEndpoint(
    const asio::ip::udp::socket& socket) {
    asio::error_code error;
    auto endpoint = socket.local_endpoint(error);
    if (error) {
        throw std::system_error(error, "read QUIC UDP local endpoint");
    }
    (void)datagram_address(endpoint);
    return endpoint;
}

Http3QuicClientSocketSession::Http3QuicClientSocketSession(asio::io_context& io,
    const asio::ip::udp::endpoint& peer, std::string_view tls_hostname,
    http3_quic_client_tls_context& tls, Http3Settings settings)
    : ownerThread_(std::this_thread::get_id()),
      peer_(peer),
      socket_(makeSocket(io, peer_)),
      eventTimer_(io),
      workTimer_(io),
      localEndpoint_(concreteLocalEndpoint(socket_)),
      transport_(tls, client_connection_config(localEndpoint_, peer_), tls_hostname,
          std::chrono::steady_clock::now()) {
    const auto prefixes = Http3LocalCriticalStreams::create(settings);
    if (!prefixes) {
        throw std::logic_error("failed to prepare local HTTP/3 critical streams");
    }
    criticalStreams_.emplace(*prefixes);
}

ruvia::quic_stream_write_result Http3QuicClientSocketSession::writeCriticalStream(
    ruvia::http3_critical_stream_output::stream_kind kind, std::span<const char> bytes) {
    requireOwnerThread();
    if (!criticalStreams_ || !criticalStreams_->complete()) {
        return {.status = ruvia::quic_operation_status::would_block};
    }
    const auto id = criticalStreams_->streamId(kind);
    if (!id) {
        return {.status = ruvia::quic_operation_status::closing};
    }
    const auto input = std::as_bytes(bytes);
    return transport_.connection().write_stream(*id, input);
}

Http3QuicClientSocketSession::~Http3QuicClientSocketSession() {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    close();
}

void Http3QuicClientSocketSession::requireOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("QUIC UDP session used outside its owner thread");
    }
}

bool Http3QuicClientSocketSession::sendPending(PumpResult& result) {
    if (pending_packet_size_ == 0) {
        return true;
    }
    asio::error_code error;
    const auto size = socket_.send(asio::buffer(packetBuffer_.data(), pending_packet_size_), 0, error);
    if (error == asio::error::would_block || error == asio::error::try_again) {
        result.outputBackpressured = true;
        return true;
    }
    if (error || size != pending_packet_size_) {
        result.status = PumpStatus::kFatal;
        return false;
    }
    pending_packet_size_ = 0;
    ++result.sent;
    return true;
}

Http3QuicClientSocketSession::PumpResult Http3QuicClientSocketSession::pump() {
    requireOwnerThread();
    PumpResult result;
    if (closed_ || stopping_) {
        result.status = PumpStatus::kClosed;
        return result;
    }

    if (!sendPending(result) || result.outputBackpressured) {
        return result;
    }

    auto& connection = transport_.connection();
    for (std::size_t packet = 0; packet < kBatchSize; ++packet) {
        asio::ip::udp::endpoint sender;
        asio::error_code error;
        const auto size = socket_.receive_from(asio::buffer(receiveBuffer_), sender, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            break;
        }
        if (error) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        if (sender != peer_) {
            continue;
        }
        ++result.received;
        if (size == 0) {
            continue;
        }
        const auto datagram = ruvia::quic_datagram_view{
            std::span<const std::byte>(receiveBuffer_.data(), size),
            to_quic_address(datagram_address(localEndpoint_)),
            to_quic_address(datagram_address(sender))};
        (void)connection.receive(datagram, std::chrono::steady_clock::now());
    }

    const auto now = std::chrono::steady_clock::now();
    (void)transport_.handle_expiry(now);

    const auto connection_info = connection.info();
    if (connection_info.state == ruvia::quic_connection_state::failed ||
        connection_info.state == ruvia::quic_connection_state::retired) {
        result.status = PumpStatus::kFatal;
        return result;
    }
    if (connection_info.state == ruvia::quic_connection_state::ready) {
        const auto critical = criticalStreams_->drive(
            [&connection](Http3CriticalStreamDriver::Kind) {
                return connection.open_stream(true);
            },
            [&connection](std::uint64_t id, std::span<const char> bytes) {
                return connection.write_stream(id, std::as_bytes(bytes));
            });
        if (critical == Http3CriticalStreamDriver::Result::kFatal) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        result.criticalStreamsReady = critical == Http3CriticalStreamDriver::Result::kReady;
        result.criticalOutputProgress = critical == Http3CriticalStreamDriver::Result::kProgress;
        for (std::size_t index = 0; index < 3; ++index) {
            const auto id = criticalStreams_->streamId(
                static_cast<Http3CriticalStreamDriver::Kind>(index));
            if (!id) {
                continue;
            }
            const auto health = connection.write_health(*id);
            if (health != ruvia::quic_operation_status::accepted &&
                health != ruvia::quic_operation_status::would_block &&
                health != ruvia::quic_operation_status::need_input) {
                result.status = PumpStatus::kFatal;
                return result;
            }
        }
    }

    for (std::size_t packet = 0; packet < kBatchSize; ++packet) {
        const auto output = transport_.write_packet(packetBuffer_, now);
        if (output.size == 0) {
            break;
        }
        if (output.size > packetBuffer_.size()) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        const auto target = to_udp_endpoint(from_quic_address(output.peer));
        if (!target || *target != peer_) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        pending_packet_size_ = output.size;
        if (!sendPending(result) || result.outputBackpressured) {
            break;
        }
    }

    if (connection.info().state == ruvia::quic_connection_state::ready) {
        result.criticalStreamsReady = criticalStreams_->complete();
    }
    if (const auto expiry = transport_.next_expiry()) {
        result.eventTimeout = *expiry > now ? *expiry - now
                                            : std::chrono::steady_clock::duration::zero();
    }
    if (result.received == 0 && result.sent == 0 && !result.criticalOutputProgress &&
        result.status == PumpStatus::kActive) {
        result.status = PumpStatus::kWouldBlock;
    }
    return result;
}

Task<void> Http3QuicClientSocketSession::waitReadable() {
    requireOwnerThread();
    auto completion = co_await ruvia::asyncAsio([this](auto handler) {
        socket_.async_wait(asio::ip::udp::socket::wait_read, std::move(handler));
    });
    if (completion.errorCode()) {
        throw std::system_error(completion.errorCode(), "wait for QUIC UDP input");
    }
}

Task<void> Http3QuicClientSocketSession::waitWritable() {
    requireOwnerThread();
    auto completion = co_await ruvia::asyncAsio([this](auto handler) {
        socket_.async_wait(asio::ip::udp::socket::wait_write, std::move(handler));
    });
    if (completion.errorCode()) {
        throw std::system_error(completion.errorCode(), "wait for QUIC UDP output");
    }
}

Task<Http3QuicClientSocketSession::WakeReason> Http3QuicClientSocketSession::waitForActivity(
    PumpResult pump, std::optional<std::chrono::steady_clock::time_point> absoluteDeadline) {
    requireOwnerThread();
    co_return co_await Http3ClientActivityWait(socket_, eventTimer_, workTimer_,
        stopping_, activeWait_, workPending_, pump, absoluteDeadline);
}

void Http3QuicClientSocketSession::requestStop() noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    stopping_ = true;
    asio::error_code ignored;
    (void)eventTimer_.cancel(ignored);
    (void)workTimer_.cancel(ignored);
    (void)socket_.cancel(ignored);
}

void Http3QuicClientSocketSession::notifyWork() noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    workPending_ = true;
    asio::error_code ignored;
    (void)workTimer_.cancel(ignored);
}

bool Http3QuicClientSocketSession::consumeWorkNotification() noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    return std::exchange(workPending_, false);
}

void Http3QuicClientSocketSession::close() noexcept {
    if (closed_) {
        return;
    }
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    requestStop();
    transport_.close();
    asio::error_code ignored;
    socket_.close(ignored);
    closed_ = true;
    pending_packet_size_ = 0;
}

}  // namespace ruvia::detail
