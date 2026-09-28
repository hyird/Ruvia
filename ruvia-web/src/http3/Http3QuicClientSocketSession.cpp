#include "ruvia/web/detail/http3/Http3QuicClientSocketSession.h"

#include <algorithm>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/error.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"

namespace ruvia::detail {
namespace {

Http3QuicDatagramAddress datagramAddress(const asio::ip::udp::endpoint& endpoint) {
    // The BIO representation intentionally cannot retain IPv6 scope identifiers.
    // Scoped/link-local/mapped and wildcard endpoints are rejected; consequently
    // IPv6 scope changes and UDP peer migration are not supported by this session.
    auto result = toHttp3QuicDatagramAddress(endpoint);
    if (!result) {
        throw std::invalid_argument("QUIC UDP endpoint is not a supported concrete address");
    }
    return *result;
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
    // Connect before enabling nonblocking mode. If a platform nevertheless reports
    // EINPROGRESS, it is not success: this synchronous session cannot finish connect.
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
    (void)datagramAddress(endpoint);
    return endpoint;
}

Http3QuicClientSocketSession::Http3QuicClientSocketSession(asio::io_context& io,
    const asio::ip::udp::endpoint& peer, std::string_view tlsHostname,
    Http3QuicClientTlsContext& tls)
    : ownerThread_(std::this_thread::get_id()),
      peer_(peer),
      peerAddress_(datagramAddress(peer_)),
      socket_(makeSocket(io, peer_)),
      eventTimer_(io),
      workTimer_(io),
      localEndpoint_(concreteLocalEndpoint(socket_)),
      bridge_(datagramAddress(localEndpoint_)),
      transport_(tls, bridge_, peerAddress_, tlsHostname) {
    const auto prefixes = Http3LocalCriticalStreams::create();
    if (!prefixes) {
        throw std::logic_error("failed to prepare local HTTP/3 critical streams");
    }
    criticalStreams_.emplace(*prefixes);
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

Http3QuicClientSocketSession::PumpResult Http3QuicClientSocketSession::pump() {
    requireOwnerThread();
    PumpResult result;
    if (closed_ || stopping_) {
        result.status = PumpStatus::kClosed;
        return result;
    }

    bool sawWouldBlock = false;
    for (std::size_t packet = 0; packet != kBatchSize; ++packet) {
        if (pendingSize_ != 0) {
            const auto injected = bridge_.inject(
                std::span<const std::byte>(pendingDatagram_.data(), pendingSize_), peerAddress_);
            if (injected == Http3QuicDatagramBridge::InjectResult::kFatal) {
                result.status = PumpStatus::kFatal;
                return result;
            }
            if (injected == Http3QuicDatagramBridge::InjectResult::kFull) {
                break;
            }
            pendingSize_ = 0;
        }

        asio::ip::udp::endpoint sender;
        asio::error_code error;
        const std::size_t size = socket_.receive_from(asio::buffer(receiveBuffer_), sender, 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            sawWouldBlock = true;
            break;
        }
        if (error) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        ++result.received;
        // A connected UDP socket must never feed a datagram from a different peer
        // to the TLS connection, even if a platform unexpectedly surfaces one.
        if (sender != peer_) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        if (size == 0) {
            continue;  // An empty UDP datagram cannot contain a QUIC packet.
        }
        const auto injected = bridge_.inject(
            std::span<const std::byte>(receiveBuffer_.data(), size), peerAddress_);
        if (injected == Http3QuicDatagramBridge::InjectResult::kFatal) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        if (injected == Http3QuicDatagramBridge::InjectResult::kFull) {
            std::copy_n(receiveBuffer_.begin(), size, pendingDatagram_.begin());
            pendingSize_ = size;
            break;
        }
    }

    const auto state = started_ ? transport_.handleEvents() : transport_.startConnect();
    started_ = true;
    if (state == Http3QuicClientTransport::State::kTlsFailure ||
        state == Http3QuicClientTransport::State::kAlpnMismatch ||
        state == Http3QuicClientTransport::State::kTransportClosed) {
        result.status = state == Http3QuicClientTransport::State::kTransportClosed
                            ? PumpStatus::kClosed
                            : PumpStatus::kFatal;
        return result;
    }
    if (state == Http3QuicClientTransport::State::kH3Ready) {
        const auto critical = criticalStreams_->drive(
            [this](Http3CriticalStreamDriver::Kind) {
                return transport_.openLocalUnidirectionalStream();
            },
            [this](Http3QuicClientTransport::StreamId id, std::span<const char> bytes) {
                return transport_.writeStream(id, bytes);
            });
        if (critical == Http3CriticalStreamDriver::Result::kFatal) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        result.criticalStreamsReady = critical == Http3CriticalStreamDriver::Result::kReady;
        result.criticalOutputProgress = critical == Http3CriticalStreamDriver::Result::kProgress;
        // Once prefixes finish, the startup driver stops writing. Still check
        // every open critical stream after each QUIC event: a peer STOP_SENDING
        // or connection failure must not silently leave a broken H3 session.
        for (std::size_t index = 0; index != 3; ++index) {
            const auto id = criticalStreams_->streamId(
                static_cast<Http3CriticalStreamDriver::Kind>(index));
            if (id && transport_.streamWriteHealth(*id) != Http3QuicStreamSet::Error::kNone) {
                result.status = PumpStatus::kFatal;
                return result;
            }
        }
    }

    for (std::size_t packet = 0; packet != kBatchSize; ++packet) {
        if (!outboundOutstanding_) {
            const auto available = bridge_.takeOutbound(outbound_);
            if (available == Http3QuicDatagramBridge::OutboundResult::kEmpty) {
                break;
            }
            if (available == Http3QuicDatagramBridge::OutboundResult::kBusy) {
                break;
            }
            if (available == Http3QuicDatagramBridge::OutboundResult::kFatal) {
                result.status = PumpStatus::kFatal;
                return result;
            }
            outboundOutstanding_ = true;
        }
        const auto target = toHttp3UdpEndpoint(outbound_.destination);
        const auto source = outbound_.hasSource
                                ? toHttp3UdpEndpoint(outbound_.source)
                                : decltype(target){};
        if (!target || *target != peer_ ||
            (outbound_.hasSource && (!source || *source != localEndpoint_))) {
            // A connected UDP socket cannot implement source or peer migration.
            result.status = PumpStatus::kFatal;
            return result;
        }
        asio::error_code error;
        const std::size_t size = socket_.send(asio::buffer(outbound_.bytes.data(), outbound_.bytes.size()), 0, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            sawWouldBlock = true;
            break;  // Keep the bridge's borrowed span outstanding for the next pump.
        }
        bridge_.completeOutbound();
        outboundOutstanding_ = false;
        if (error || size != outbound_.bytes.size()) {
            result.status = PumpStatus::kFatal;
            return result;
        }
        ++result.sent;
    }

    result.inputBackpressured = pendingSize_ != 0;
    result.outputBackpressured = outboundOutstanding_;
    result.eventTimeout = transport_.eventTimeout();
    if (sawWouldBlock && result.received == 0 && result.sent == 0 &&
        !result.criticalOutputProgress) {
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
    pendingSize_ = 0;
    outboundOutstanding_ = false;
}

}  // namespace ruvia::detail
