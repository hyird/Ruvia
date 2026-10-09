#include "http3/http3_quic_client_socket_session.h"

#include <algorithm>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <variant>

#include <asio/error.hpp>

#include "ruvia/core/async.h"

#include "http3/http3_quic_packet_io.h"
#include "http3/http3_quic_socket_address.h"

namespace ruvia::detail {
namespace {

http3_quic_datagram_address datagram_address(const asio::ip::udp::endpoint& endpoint) {
    auto result_value = to_http3_quic_datagram_address(endpoint);
    if ((result_value.index() != 0)) {
        throw std::invalid_argument("QUIC UDP endpoint is not a supported concrete address");
    }
    return std::get<0>(result_value);
}

ruvia::quic_connection_config client_connection_config(
    const asio::ip::udp::endpoint& local, const asio::ip::udp::endpoint& peer,
    ruvia::quic_version initial_version) {
    ruvia::quic_connection_config config{};
    config.role_ = ruvia::quic_role::client;
    config.version_ = initial_version;
    config.preferred_version_ = initial_version;
    config.local_address_ = to_quic_address(datagram_address(local));
    config.peer_address_ = to_quic_address(datagram_address(peer));
    config.local_transport_parameters_.max_datagram_frame_size_ =
        config.limits_.max_datagram_size_;
    return config;
}

// This awaiter lives inside the single driver's coroutine frame. Socket and
// timer callbacks borrow it only until *every* armed operation has completed;
// the first wake cancels losers but never resumes the owner early. No sibling
// task is created, and an initiation failure drains already-armed operations.
class http3_client_activity_wait final {
public:
    using session = http3_quic_client_socket_session;
    using clock_type = std::chrono::steady_clock;
    using time_point_type = clock_type::time_point;
    using reason_type = session::wake_reason_type;

    http3_client_activity_wait(asio::ip::udp::socket& socket,
        asio::ip::udp::socket* candidate_socket, bool pending_candidate,
        asio::steady_timer& timer, asio::steady_timer& work_timer, bool& stopping,
        bool& active, bool& work_pending, session::pump_result_type pump,
        std::optional<time_point_type> deadline_value) noexcept
        : socket_(socket),
          candidate_socket_(candidate_socket),
          pending_candidate_(pending_candidate),
          timer_(timer),
          work_timer_(work_timer),
          stopping_(stopping),
          active_(active),
          work_pending_(work_pending),
          pump_(pump),
          deadline_(deadline_value) {}

    http3_client_activity_wait(const http3_client_activity_wait&) = delete;
    http3_client_activity_wait& operator=(const http3_client_activity_wait&) = delete;
    ~http3_client_activity_wait() {
        if (pending_ != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] bool await_ready() noexcept {
        if (stopping_ || pump_.status_ == session::pump_status_type::closed) {
            reason_ = reason_type::stopped;
            return true;
        }
        if (pump_.status_ == session::pump_status_type::fatal) {
            reason_ = reason_type::fatal;
            return true;
        }
        if (active_) {
            reason_ = reason_type::fatal;
            return true;
        }
        if (deadline_ && clock_type::now() >= *deadline_) {
            reason_ = reason_type::deadline;
            return true;
        }
        if (work_pending_) {
            reason_ = reason_type::application;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (stopping_) {
            reason_ = reason_type::stopped;
            return false;
        }
        if (active_) {
            reason_ = reason_type::fatal;
            return false;
        }
        if (work_pending_) {
            reason_ = reason_type::application;
            return false;
        }
        active_ = true;
        owns_active_ = true;
        continuation_ = continuation;
        arming_ = true;
        try {
            const auto now = clock_type::now();
            std::optional<time_point_type> wake_at = deadline_;
            auto timeout = pump_.event_timeout_;
            if (pump_.input_backpressured_) {
                const auto retry = std::chrono::duration_cast<clock_type::duration>(
                    std::chrono::milliseconds(1));
                timeout = timeout ? std::min(*timeout, retry) : retry;
            }
            if (timeout) {
                auto duration = std::max(*timeout,
                    std::chrono::duration_cast<clock_type::duration>(std::chrono::milliseconds(1)));
                duration = std::min(duration, time_point_type::max() - now);
                const auto quic_wake = now + duration;
                if (!wake_at || quic_wake < *wake_at) {
                    wake_at = quic_wake;
                }
            }
            if (!pump_.input_backpressured_) {
                arm_read(socket_, false);
                if (candidate_socket_ != nullptr) {
                    arm_read(*candidate_socket_, true);
                }
            }
            if (!winner_chosen_ && pump_.output_backpressured_) {
                arm_write(pending_candidate_ && candidate_socket_ != nullptr
                              ? *candidate_socket_
                              : socket_,
                    pending_candidate_ && candidate_socket_ != nullptr);
            }
            if (!winner_chosen_ && wake_at) {
                arm_timer(*wake_at);
            }
            // A full input BIO must never depend solely on another socket read.
            if (!winner_chosen_ && pending_ == 0) {
                arm_timer(now + std::chrono::milliseconds(1));
            }
            if (!winner_chosen_) {
                arm_work();
            }
        } catch (...) {
            failure_ = std::current_exception();
            reason_ = reason_type::fatal;
            winner_chosen_ = true;
            cancel_outstanding();
            if (pending_ == 0) {
                woke_ = true;
            }
        }
        arming_ = false;
        return !woke_;
    }

    [[nodiscard]] reason_type await_resume() {
        if (owns_active_) {
            active_ = false;
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (stopping_) {
            return reason_type::stopped;
        }
        if (deadline_ && clock_type::now() >= *deadline_) {
            return reason_type::deadline;
        }
        return work_pending_ ? reason_type::application : reason_;
    }

private:
    void arm_read(asio::ip::udp::socket& socket, bool candidate_value) {
        ++pending_;
        try {
            socket.async_wait(asio::ip::udp::socket::wait_read,
                [this, candidate_value](const asio::error_code& error) {
                    complete(reason_type::readable, error, candidate_value);
                });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void arm_write(asio::ip::udp::socket& socket, bool candidate_value) {
        ++pending_;
        try {
            socket.async_wait(asio::ip::udp::socket::wait_write,
                [this, candidate_value](const asio::error_code& error) {
                    complete(reason_type::writable, error, candidate_value);
                });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void arm_timer(time_point_type at) {
        timer_.expires_at(at);
        ++pending_;
        try {
            timer_.async_wait([this](const asio::error_code& error) {
                complete(reason_type::quic_event, error);
            });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void arm_work() {
        work_timer_.expires_at(time_point_type::max());
        ++pending_;
        try {
            work_timer_.async_wait([this](const asio::error_code& error) {
                complete(reason_type::application, error);
            });
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void cancel_outstanding() noexcept {
        asio::error_code ignored;
        (void)timer_.cancel();
        (void)work_timer_.cancel();
        (void)socket_.cancel(ignored);
        if (candidate_socket_ != nullptr) {
            (void)candidate_socket_->cancel(ignored);
        }
    }
    void complete(reason_type event, const asio::error_code& error, bool candidate = false) {
        if (pending_ == 0) {
            std::terminate();
        }
        --pending_;
        if (!winner_chosen_) {
            winner_chosen_ = true;
            reason_ = stopping_ ? reason_type::stopped
                      : event == reason_type::application
                          ? (work_pending_ ? reason_type::application : reason_type::fatal)
                      : error && candidate ? reason_type::candidate_failure
                      : error              ? reason_type::fatal
                                           : event;
            cancel_outstanding();
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
    asio::steady_timer& work_timer_;
    bool& stopping_;
    bool& active_;
    bool& work_pending_;
    session::pump_result_type pump_;
    std::optional<time_point_type> deadline_;
    std::coroutine_handle<> continuation_{};
    std::exception_ptr failure_;
    reason_type reason_{reason_type::fatal};
    std::size_t pending_{};
    bool owns_active_{};
    bool arming_{};
    bool winner_chosen_{};
    bool woke_{};
};

}  // namespace

asio::ip::udp::socket http3_quic_client_socket_session::make_socket(asio::io_context& io,
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

asio::ip::udp::socket http3_quic_client_socket_session::make_candidate_socket(
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

asio::ip::udp::endpoint http3_quic_client_socket_session::concrete_local_endpoint(
    const asio::ip::udp::socket& socket) {
    asio::error_code error;
    auto endpoint = socket.local_endpoint(error);
    if (error) {
        throw std::system_error(error, "read QUIC UDP local endpoint");
    }
    (void)datagram_address(endpoint);
    return endpoint;
}

http3_quic_client_socket_session::http3_quic_client_socket_session(asio::io_context& io,
    const asio::ip::udp::endpoint& peer, std::string_view tls_hostname,
    http3_quic_client_tls_context& tls, http3_settings settings,
    ruvia::quic_version initial_version, bool enable_early_data,
    std::pmr::memory_resource* resource)
    : owner_thread_(std::this_thread::get_id()),
      peer_(peer),
      io_(io),
      socket_(make_socket(io_, peer_)),
      event_timer_(io),
      work_timer_(io),
      local_endpoint_(concrete_local_endpoint(socket_)),
      transport_(tls, client_connection_config(local_endpoint_, peer_, initial_version), tls_hostname,
          std::chrono::steady_clock::now(), resource, enable_early_data),
      settings_(settings),
      rejected_early_streams_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      early_data_enabled_(transport_.early_data_enabled()) {
    const auto prefixes = http3_local_critical_streams::create(settings_);
    if ((prefixes.index() != 0)) {
        throw std::logic_error("failed to prepare local HTTP/3 critical streams");
    }
    critical_streams_.emplace(std::get<0>(prefixes));
    if (early_data_enabled_) {
        rejected_early_streams_.reserve(ruvia::quic_limits{}.max_streams_);
    }
}

std::size_t http3_quic_client_socket_session::take_rejected_early_streams(
    std::span<std::uint64_t> output) noexcept {
    require_owner_thread();
    const auto count = std::min(output.size(), rejected_early_streams_.size());
    std::copy_n(rejected_early_streams_.begin(), count, output.begin());
    rejected_early_streams_.erase(
        rejected_early_streams_.begin(),
        rejected_early_streams_.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

void http3_quic_client_socket_session::reset_rejected_early_streams() {
    std::array<std::uint64_t, 32> rejected{};
    bool critical_stream_rejected = false;
    while (true) {
        const auto count = transport_.connection().take_rejected_early_streams(rejected);
        for (std::size_t index = 0; index < count; ++index) {
            const auto stream_id = rejected[index];
            const bool critical = std::ranges::any_of(
                std::array{http3_critical_stream_driver::kind_type::control,
                    http3_critical_stream_driver::kind_type::qpack_encoder,
                    http3_critical_stream_driver::kind_type::qpack_decoder},
                [this, stream_id](const auto kind) {
                    return critical_streams_->stream_id(kind) == stream_id;
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
        const auto prefixes = http3_local_critical_streams::create(settings_);
        if ((prefixes.index() != 0)) {
            throw std::logic_error("failed to rebuild HTTP/3 critical stream prefixes after 0-RTT rejection");
        }
        critical_streams_->restart(std::get<0>(prefixes));
    }
}

ruvia::quic_path_migration http3_quic_client_socket_session::start_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) {
    require_owner_thread();
    if (closed_ || stopping_ || candidate_socket_) {
        return {.status_ = ruvia::quic_migration_status::rejected};
    }
    const auto info = transport_.connection().info();
    if (info.state_ != ruvia::quic_connection_state::ready || !info.confirmed_) {
        return {.status_ = ruvia::quic_migration_status::rejected};
    }
    auto candidate_value = make_candidate_socket(io_, local_endpoint, peer_);
    const auto bound_endpoint = concrete_local_endpoint(candidate_value);
    const auto address = to_quic_address(datagram_address(bound_endpoint));
    const auto migration = transport_.connection().start_path_migration(address);
    if (migration.status_ != ruvia::quic_migration_status::started &&
        migration.status_ != ruvia::quic_migration_status::validated) {
        return migration;
    }
    candidate_socket_.emplace(std::move(candidate_value));
    candidate_local_endpoint_ = bound_endpoint;
    failed_migration_local_endpoint_.reset();
    migration_id_ = migration.id_;
    last_migration_ = migration;
    notify_work();
    return migration;
}

std::optional<ruvia::quic_path_migration> http3_quic_client_socket_session::path_migration(
    std::uint64_t id) const noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        return std::nullopt;
    }
    if (closed_) {
        return last_migration_ && last_migration_->id_ == id ? last_migration_ : std::nullopt;
    }
    return transport_.connection().path_migration(id);
}

std::optional<ruvia::quic_path_migration> http3_quic_client_socket_session::active_path_migration() const noexcept {
    if (!migration_id_) {
        return std::nullopt;
    }
    return path_migration(*migration_id_);
}

ruvia::quic_operation_status http3_quic_client_socket_session::cancel_path_migration(
    std::uint64_t id) {
    require_owner_thread();
    if (closed_) {
        return ruvia::quic_operation_status::retired;
    }
    const auto status = transport_.connection().cancel_path_migration(id);
    if (status == ruvia::quic_operation_status::accepted) {
        notify_work();
    }
    return status;
}

void http3_quic_client_socket_session::settle_migration() noexcept {
    if (!migration_id_ || !candidate_socket_) {
        return;
    }
    const auto migration = transport_.connection().path_migration(*migration_id_);
    if (!migration || migration->status_ == ruvia::quic_migration_status::started) {
        return;
    }
    last_migration_ = *migration;
    asio::error_code ignored;
    if (migration->status_ == ruvia::quic_migration_status::validated) {
        std::swap(socket_, *candidate_socket_);
        local_endpoint_ = *candidate_local_endpoint_;
    } else {
        if (migration->status_ == ruvia::quic_migration_status::failed) {
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

void http3_quic_client_socket_session::fail_candidate_migration() noexcept {
    if (migration_id_) {
        (void)transport_.connection().fail_path_migration(*migration_id_);
    }
}

ruvia::quic_stream_write_result http3_quic_client_socket_session::write_critical_stream(
    ruvia::http3_critical_stream_output::stream_kind kind, std::span<const char> bytes_value) {
    require_owner_thread();
    if (!critical_streams_ || !critical_streams_->complete()) {
        return {.status_ = ruvia::quic_operation_status::would_block};
    }
    const auto id = critical_streams_->stream_id(kind);
    if (!id) {
        return {.status_ = ruvia::quic_operation_status::closing};
    }
    const auto input = std::as_bytes(bytes_value);
    return transport_.connection().write_stream(*id, input);
}

http3_quic_client_socket_session::~http3_quic_client_socket_session() {
    if (std::this_thread::get_id() != owner_thread_ || active_wait_) {
        std::terminate();
    }
    close();
}

void http3_quic_client_socket_session::require_owner_thread() const {
    if (std::this_thread::get_id() != owner_thread_) {
        throw std::logic_error("QUIC UDP session used outside its owner thread");
    }
}

http3_quic_client_socket_session::pump_result_type http3_quic_client_socket_session::pump() {
    socket_sender_type sender;
    return pump_with_send(sender);
}

task<void> http3_quic_client_socket_session::wait_readable() {
    require_owner_thread();
    auto completion = co_await ruvia::async_asio([this](auto handler) {
        socket_.async_wait(asio::ip::udp::socket::wait_read, std::move(handler));
    });
    if (completion.error_code()) {
        throw std::system_error(completion.error_code(), "wait for QUIC UDP input");
    }
}

task<void> http3_quic_client_socket_session::wait_writable() {
    require_owner_thread();
    auto completion = co_await ruvia::async_asio([this](auto handler) {
        socket_.async_wait(asio::ip::udp::socket::wait_write, std::move(handler));
    });
    if (completion.error_code()) {
        throw std::system_error(completion.error_code(), "wait for QUIC UDP output");
    }
}

task<http3_quic_client_socket_session::wake_reason_type> http3_quic_client_socket_session::wait_for_activity(
    pump_result_type pump, std::optional<std::chrono::steady_clock::time_point> absolute_deadline) {
    require_owner_thread();
    const auto reason = co_await http3_client_activity_wait(socket_,
        candidate_socket_ ? &*candidate_socket_ : nullptr, pending_candidate_, event_timer_,
        work_timer_, stopping_, active_wait_, work_pending_, pump, absolute_deadline);
    if (reason == wake_reason_type::candidate_failure) {
        fail_candidate_migration();
    }
    if (closed_ && candidate_socket_) {
        candidate_socket_.reset();
        candidate_local_endpoint_.reset();
    }
    co_return reason;
}

void http3_quic_client_socket_session::request_stop() noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    stopping_ = true;
    asio::error_code ignored;
    (void)event_timer_.cancel();
    (void)work_timer_.cancel();
    (void)socket_.cancel(ignored);
    if (candidate_socket_) {
        (void)candidate_socket_->cancel(ignored);
    }
}

void http3_quic_client_socket_session::notify_work() noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    work_pending_ = true;
    asio::error_code ignored;
    (void)work_timer_.cancel();
}

bool http3_quic_client_socket_session::consume_work_notification() noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    return std::exchange(work_pending_, false);
}

void http3_quic_client_socket_session::remember_resumption_ticket(
    std::optional<ruvia::http3_settings> settings) {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    transport_.remember_resumption_ticket(std::move(settings));
}

void http3_quic_client_socket_session::close() noexcept {
    if (closed_) {
        return;
    }
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    request_stop();
    if (migration_id_) {
        if (const auto migration = transport_.connection().path_migration(*migration_id_)) {
            last_migration_ = *migration;
            if (last_migration_->status_ == ruvia::quic_migration_status::started) {
                last_migration_->status_ = ruvia::quic_migration_status::aborted;
            }
        }
    }
    transport_.close();
    asio::error_code ignored;
    socket_.close(ignored);
    if (candidate_socket_) {
        candidate_socket_->close(ignored);
        if (!active_wait_) {
            candidate_socket_.reset();
            candidate_local_endpoint_.reset();
        }
    }
    closed_ = true;
    pending_packet_size_ = 0;
}

}  // namespace ruvia::detail
