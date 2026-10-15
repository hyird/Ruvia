#include "http3/http3_client_session_driver.h"

#include <utility>

#include <asio/post.hpp>

#include "ruvia/core/async.h"

namespace ruvia::detail {

http3_client_session_driver::attempt_type::attempt_type(std::pmr::memory_resource* resource,
    http3_body_budget* body_budget, const http3_client_sans_io_response_limits& limits)
    : engine_(resource, body_budget, limits),
      receiver_(engine_),
      peer_streams_(resource),
      session_(nullptr, pmr_object_deleter<http3_quic_client_socket_session>{resource}) {
    peer_streams_.reserve(ruvia::quic_limits{}.max_streams_);
}

http3_client_session_driver::http3_client_session_driver(asio::io_context& io, http3_quic_client_tls_context& tls,
    const config_type& config, std::pmr::memory_resource* resource, http3_body_budget* body_budget)
    : io_(io),
      tls_(tls),
      config_(config),
      resource_(resource),
      body_budget_(body_budget),
      resolver_(io, resource),
      attempt_(nullptr, pmr_object_deleter<attempt_type>{resource}) {
    if (config_.host_.empty() || config_.tls_host_.empty() || config_.port_ == 0 ||
        config_.connect_timeout_ <= clock_type::duration::zero()) {
        throw std::invalid_argument("HTTP/3 client session requires a host, TLS host, port and connect timeout");
    }
}

http3_client_session_driver::~http3_client_session_driver() = default;

http3_client_sans_io_session_engine& http3_client_session_driver::engine() {
    return const_cast<http3_client_sans_io_session_engine&>(std::as_const(*this).engine());
}

const http3_client_sans_io_session_engine& http3_client_session_driver::engine() const {
    if (!attempt_) {
        throw std::logic_error("HTTP/3 protocol state requires an active connection attempt");
    }
    return attempt_->engine_;
}

http3_client_receive_driver& http3_client_session_driver::receiver() {
    if (!attempt_) {
        throw std::logic_error("HTTP/3 protocol state requires an active connection attempt");
    }
    return attempt_->receiver_;
}

http3_client_session_driver::time_point_type http3_client_session_driver::deadline_after(
    time_point_type now, clock_type::duration timeout) noexcept {
    const auto latest_start = time_point_type::max() - timeout;
    return now > latest_start ? time_point_type::max() : now + timeout;
}

// Each remaining address receives an equal share of the one connect deadline,
// but never less than 100 ms unless the deadline itself is nearer.
http3_client_session_driver::clock_type::duration http3_client_session_driver::endpoint_budget(
    time_point_type now, time_point_type connect_deadline, std::size_t remaining_endpoints) noexcept {
    const auto remaining = connect_deadline - now;
    const auto share = clock_type::duration(remaining.count() / static_cast<clock_type::duration::rep>(remaining_endpoints));
    return std::min(remaining,
        std::max(share, std::chrono::duration_cast<clock_type::duration>(std::chrono::milliseconds(100))));
}

void http3_client_session_driver::begin_attempt() {
    attempt_ = make_pmr_object<attempt_type>(resource_, resource_, body_budget_, config_.limits_);
    critical_deadline_.reset();
    if (++generation_ == 0) {
        ++generation_;
    }
}

void http3_client_session_driver::open_session(const asio::ip::udp::endpoint& peer) {
    attempt_->session_ = make_pmr_object<http3_quic_client_socket_session>(resource_, io_, peer, config_.tls_host_,
        tls_, attempt_->engine_, config_.initial_version_, config_.enable_early_data_, resource_);
}

task<void> http3_client_session_driver::close_attempt(time_point_type latest) {
    auto& attempt = *attempt_;
    if (!attempt.session_) {
        co_return;
    }
    static constexpr std::string_view reason = "HTTP/3 connection closed";
    co_await attempt.session_->shutdown({.kind_ = ruvia::quic_close_kind::application,
                                            .code_ = close_error_code_.value_or(static_cast<std::uint64_t>(http3_connection_error_code::no_error)),
                                            .reason_ = {reason.data(), reason.size()}},
        latest);
    attempt.session_.reset();
}

task<void> http3_client_session_driver::close() {
    if (!attempt_) {
        co_return;
    }
    try {
        co_await close_attempt(deadline_after(clock_type::now(), max_closing_period));
    } catch (...) {
        // The session destructor still releases transport and sockets.
        attempt_->session_.reset();
    }
    // All QUIC delivery is closed before parser storage retires or any owner
    // publishes a terminal result.
    (void)attempt_->engine_.stop();
}

void http3_client_session_driver::reset_attempt() noexcept {
    attempt_.reset();
}

void http3_client_session_driver::abandon_run(bool stopping) noexcept {
    if (!close_error_code_ && !stopping && terminal_failure_ != outcome_type::deadline) {
        close_error_code_ = static_cast<std::uint64_t>(http3_connection_error_code::internal_error);
    }
}

void http3_client_session_driver::request_stop() noexcept {
    stopping_ = true;
    resolver_.request_stop();
    if (auto* live = session()) {
        live->request_stop();
    }
}

void http3_client_session_driver::wake() noexcept {
    if (auto* live = session()) {
        live->notify_work();
    } else {
        resolver_.interrupt();
    }
}

void http3_client_session_driver::fail(outcome_type outcome, std::optional<std::uint64_t> close_code, const char* message) {
    terminal_failure_ = outcome;
    if (close_code) {
        close_error_code_ = close_code;
    }
    throw std::runtime_error(message);
}

void http3_client_session_driver::track_critical_output(time_point_type now, bool progress) {
    auto& engine = attempt_->engine_;
    bool pending = false;
    for (const auto kind : {http3_critical_stream_output::stream_kind::control,
             http3_critical_stream_output::stream_kind::qpack_encoder,
             http3_critical_stream_output::stream_kind::qpack_decoder}) {
        pending |= !engine.pending_critical_output(kind).empty();
    }
    if (!pending || !config_.write_timeout_) {
        critical_deadline_.reset();
        return;
    }
    if (progress || !critical_deadline_) {
        critical_deadline_ = deadline_after(now, *config_.write_timeout_);
    } else if (now >= *critical_deadline_) {
        // A local inactivity bound, not a peer error to report.
        fail(outcome_type::deadline, static_cast<std::uint64_t>(http3_connection_error_code::no_error),
            "HTTP/3 critical stream write timeout");
    }
}

void http3_client_session_driver::accept_peer_streams() {
    auto& attempt = *attempt_;
    const auto accepted = attempt.session_->transport().accept_streams();
    if (accepted.status_ != ruvia::quic_operation_status::accepted &&
        accepted.status_ != ruvia::quic_operation_status::need_input &&
        accepted.status_ != ruvia::quic_operation_status::would_block) {
        throw std::runtime_error("QUIC client cannot accept peer critical streams");
    }
    for (std::size_t i = 0; i < accepted.size_; ++i) {
        const auto& stream = accepted.streams_[i];
        if (!stream.readable_ || stream.writable_) {
            // RFC 9114 section 6.1: a server-initiated bidirectional stream.
            fail(outcome_type::protocol_error,
                static_cast<std::uint64_t>(http3_connection_error_code::stream_creation_error),
                "invalid peer HTTP/3 stream");
        }
        if (attempt.peer_streams_.size() == ruvia::quic_limits{}.max_streams_) {
            fail(outcome_type::protocol_error,
                static_cast<std::uint64_t>(http3_connection_error_code::excessive_load),
                "excessive peer HTTP/3 streams");
        }
        attempt.peer_streams_.push_back(stream.stream_id_);
    }
}

void http3_client_session_driver::close_peer_stream(std::size_t index) {
    auto& attempt = *attempt_;
    const auto id = attempt.peer_streams_[index];
    const auto closed = attempt.session_->transport().close_stream(id);
    if (closed != ruvia::quic_operation_status::accepted &&
        closed != ruvia::quic_operation_status::completed &&
        closed != ruvia::quic_operation_status::retired) {
        throw std::runtime_error("HTTP/3 peer stream could not close");
    }
    attempt.receiver_.retire(id);
    attempt.peer_streams_.erase(attempt.peer_streams_.begin() + static_cast<std::ptrdiff_t>(index));
}

void http3_client_session_driver::retire_peer_stream(std::uint64_t stream_id) noexcept {
    if (!attempt_) {
        return;
    }
    attempt_->receiver_.retire(stream_id);
    std::erase(attempt_->peer_streams_, stream_id);
}

task<bool> http3_client_session_driver::yield() {
    auto yielded = co_await ruvia::async_asio([this](auto completion) {
        asio::post(io_, [completion = std::move(completion)]() mutable {
            completion(asio::error_code{});
        });
    });
    co_return !yielded.error_code();
}

}  // namespace ruvia::detail
