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
    const asio::ip::udp::endpoint& local, const asio::ip::udp::endpoint& peer,
    ruvia::quic_version initial_version) {
    ruvia::quic_connection_config config{};
    config.role = ruvia::quic_role::client;
    config.version = initial_version;
    config.preferred_version = initial_version;
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

    Http3ClientActivityWait(asio::ip::udp::socket& socket,
        asio::ip::udp::socket* candidate_socket, bool pending_candidate,
        asio::steady_timer& timer, asio::steady_timer& workTimer, bool& stopping,
        bool& active, bool& workPending, Session::PumpResult pump,
        std::optional<TimePoint> deadline) noexcept
        : socket_(socket),
          candidate_socket_(candidate_socket),
          pending_candidate_(pending_candidate),
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
                armRead(socket_, false);
                if (candidate_socket_ != nullptr) {
                    armRead(*candidate_socket_, true);
                }
            }
            if (!winnerChosen_ && pump_.outputBackpressured) {
                armWrite(pending_candidate_ && candidate_socket_ != nullptr
                             ? *candidate_socket_
                             : socket_,
                    pending_candidate_ && candidate_socket_ != nullptr);
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
    void armRead(asio::ip::udp::socket& socket, bool candidate) {
        ++pending_;
        try {
            socket.async_wait(asio::ip::udp::socket::wait_read,
                [this, candidate](const asio::error_code& error) {
                    complete(Reason::kReadable, error, candidate);
                });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void armWrite(asio::ip::udp::socket& socket, bool candidate) {
        ++pending_;
        try {
            socket.async_wait(asio::ip::udp::socket::wait_write,
                [this, candidate](const asio::error_code& error) {
                    complete(Reason::kWritable, error, candidate);
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
        if (candidate_socket_ != nullptr) {
            (void)candidate_socket_->cancel(ignored);
        }
    }
    void complete(Reason event, const asio::error_code& error, bool candidate = false) {
        if (pending_ == 0) {
            std::terminate();
        }
        --pending_;
        if (!winnerChosen_) {
            winnerChosen_ = true;
            reason_ = stopping_ ? Reason::kStopped
                      : event == Reason::kApplication
                          ? (workPending_ ? Reason::kApplication : Reason::kFatal)
                      : error && candidate ? Reason::kCandidateFailure
                      : error              ? Reason::kFatal
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
    asio::ip::udp::socket* candidate_socket_{};
    bool pending_candidate_{};
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

asio::ip::udp::socket Http3QuicClientSocketSession::make_candidate_socket(
    asio::io_context& io, const asio::ip::udp::endpoint& local,
    const asio::ip::udp::endpoint& peer) {
    if (local.protocol() != peer.protocol() || local.port() == 0 ||
        local.address().is_unspecified()) {
        throw std::invalid_argument("QUIC migration requires a concrete same-family local endpoint");
    }
    asio::ip::udp::socket socket(io);
    asio::error_code error;
    socket.open(local.protocol(), error);
    if (error) {
        throw std::system_error(error, "open QUIC migration UDP socket");
    }
    socket.bind(local, error);
    if (error) {
        throw std::system_error(error, "bind QUIC migration UDP socket");
    }
    socket.connect(peer, error);
    if (error) {
        throw std::system_error(error, "connect QUIC migration UDP socket");
    }
    socket.non_blocking(true, error);
    if (error) {
        throw std::system_error(error, "set QUIC migration UDP socket nonblocking");
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
    http3_quic_client_tls_context& tls, Http3Settings settings,
    ruvia::quic_version initial_version, bool enable_early_data,
    std::pmr::memory_resource* resource)
    : ownerThread_(std::this_thread::get_id()),
      peer_(peer),
      io_(io),
      socket_(makeSocket(io_, peer_)),
      eventTimer_(io),
      workTimer_(io),
      localEndpoint_(concreteLocalEndpoint(socket_)),
      transport_(tls, client_connection_config(localEndpoint_, peer_, initial_version), tls_hostname,
          std::chrono::steady_clock::now(), resource, enable_early_data),
      settings_(settings),
      rejected_early_streams_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      early_data_enabled_(transport_.early_data_enabled()) {
    const auto prefixes = Http3LocalCriticalStreams::create(settings_);
    if (!prefixes) {
        throw std::logic_error("failed to prepare local HTTP/3 critical streams");
    }
    criticalStreams_.emplace(*prefixes);
    if (early_data_enabled_) {
        rejected_early_streams_.reserve(ruvia::quic_limits{}.max_streams);
    }
}

std::size_t Http3QuicClientSocketSession::take_rejected_early_streams(
    std::span<std::uint64_t> output) noexcept {
    requireOwnerThread();
    const auto count = std::min(output.size(), rejected_early_streams_.size());
    std::copy_n(rejected_early_streams_.begin(), count, output.begin());
    rejected_early_streams_.erase(
        rejected_early_streams_.begin(),
        rejected_early_streams_.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

void Http3QuicClientSocketSession::reset_rejected_early_streams() {
    std::array<std::uint64_t, 32> rejected{};
    bool critical_stream_rejected = false;
    while (true) {
        const auto count = transport_.connection().take_rejected_early_streams(rejected);
        for (std::size_t index = 0; index < count; ++index) {
            const auto stream_id = rejected[index];
            const bool critical = std::ranges::any_of(
                std::array{Http3CriticalStreamDriver::Kind::control,
                    Http3CriticalStreamDriver::Kind::qpack_encoder,
                    Http3CriticalStreamDriver::Kind::qpack_decoder},
                [this, stream_id](const auto kind) {
                    return criticalStreams_->streamId(kind) == stream_id;
                });
            if (critical) {
                critical_stream_rejected = true;
            } else {
                rejected_early_streams_.push_back(stream_id);
            }
        }
        if (count < rejected.size()) {
            break;
        }
    }
    if (critical_stream_rejected) {
        const auto prefixes = Http3LocalCriticalStreams::create(settings_);
        if (!prefixes) {
            throw std::logic_error("failed to rebuild HTTP/3 critical stream prefixes after 0-RTT rejection");
        }
        criticalStreams_->restart(*prefixes);
    }
}

ruvia::quic_path_migration Http3QuicClientSocketSession::start_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) {
    requireOwnerThread();
    if (closed_ || stopping_ || candidate_socket_) {
        return {.status = ruvia::quic_migration_status::rejected};
    }
    const auto info = transport_.connection().info();
    if (info.state != ruvia::quic_connection_state::ready || !info.confirmed) {
        return {.status = ruvia::quic_migration_status::rejected};
    }
    auto candidate = make_candidate_socket(io_, local_endpoint, peer_);
    const auto bound_endpoint = concreteLocalEndpoint(candidate);
    const auto address = to_quic_address(datagram_address(bound_endpoint));
    const auto migration = transport_.connection().start_path_migration(address);
    if (migration.status != ruvia::quic_migration_status::started &&
        migration.status != ruvia::quic_migration_status::validated) {
        return migration;
    }
    candidate_socket_.emplace(std::move(candidate));
    candidate_local_endpoint_ = bound_endpoint;
    failed_migration_local_endpoint_.reset();
    migration_id_ = migration.id;
    last_migration_ = migration;
    notifyWork();
    return migration;
}

std::optional<ruvia::quic_path_migration> Http3QuicClientSocketSession::path_migration(
    std::uint64_t id) const noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        return std::nullopt;
    }
    if (closed_) {
        return last_migration_ && last_migration_->id == id ? last_migration_ : std::nullopt;
    }
    return transport_.connection().path_migration(id);
}

std::optional<ruvia::quic_path_migration> Http3QuicClientSocketSession::active_path_migration() const noexcept {
    if (!migration_id_) {
        return std::nullopt;
    }
    return path_migration(*migration_id_);
}

ruvia::quic_operation_status Http3QuicClientSocketSession::cancel_path_migration(
    std::uint64_t id) {
    requireOwnerThread();
    if (closed_) {
        return ruvia::quic_operation_status::retired;
    }
    const auto status = transport_.connection().cancel_path_migration(id);
    if (status == ruvia::quic_operation_status::accepted) {
        notifyWork();
    }
    return status;
}

void Http3QuicClientSocketSession::settle_migration() noexcept {
    if (!migration_id_ || !candidate_socket_) {
        return;
    }
    const auto migration = transport_.connection().path_migration(*migration_id_);
    if (!migration || migration->status == ruvia::quic_migration_status::started) {
        return;
    }
    last_migration_ = *migration;
    asio::error_code ignored;
    if (migration->status == ruvia::quic_migration_status::validated) {
        std::swap(socket_, *candidate_socket_);
        localEndpoint_ = *candidate_local_endpoint_;
    } else {
        if (migration->status == ruvia::quic_migration_status::failed) {
            failed_migration_local_endpoint_ = candidate_local_endpoint_;
        }
        if (pending_candidate_) {
            pending_packet_size_ = 0;
            pending_candidate_ = false;
        }
    }
    (void)candidate_socket_->cancel(ignored);
    candidate_socket_->close(ignored);
    candidate_socket_.reset();
    candidate_local_endpoint_.reset();
    migration_id_.reset();
}

void Http3QuicClientSocketSession::fail_candidate_migration() noexcept {
    if (migration_id_) {
        (void)transport_.connection().fail_path_migration(*migration_id_);
    }
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
    if (std::this_thread::get_id() != ownerThread_ || activeWait_) {
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
    SocketSender sender;
    return pump_with_send(sender);
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
    const auto reason = co_await Http3ClientActivityWait(socket_,
        candidate_socket_ ? &*candidate_socket_ : nullptr, pending_candidate_, eventTimer_,
        workTimer_, stopping_, activeWait_, workPending_, pump, absoluteDeadline);
    if (reason == WakeReason::kCandidateFailure) {
        fail_candidate_migration();
    }
    if (closed_ && candidate_socket_) {
        candidate_socket_.reset();
        candidate_local_endpoint_.reset();
    }
    co_return reason;
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
    if (candidate_socket_) {
        (void)candidate_socket_->cancel(ignored);
    }
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

void Http3QuicClientSocketSession::remember_resumption_ticket(
    std::optional<ruvia::Http3Settings> settings) {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    transport_.remember_resumption_ticket(std::move(settings));
}

void Http3QuicClientSocketSession::close() noexcept {
    if (closed_) {
        return;
    }
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    requestStop();
    if (migration_id_) {
        if (const auto migration = transport_.connection().path_migration(*migration_id_)) {
            last_migration_ = *migration;
            if (last_migration_->status == ruvia::quic_migration_status::started) {
                last_migration_->status = ruvia::quic_migration_status::aborted;
            }
        }
    }
    transport_.close();
    asio::error_code ignored;
    socket_.close(ignored);
    if (candidate_socket_) {
        candidate_socket_->close(ignored);
        if (!activeWait_) {
            candidate_socket_.reset();
            candidate_local_endpoint_.reset();
        }
    }
    closed_ = true;
    pending_packet_size_ = 0;
}

}  // namespace ruvia::detail
