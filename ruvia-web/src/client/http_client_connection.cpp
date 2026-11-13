#include "client/http_client_connection.h"

#include <algorithm>
#include <exception>
#include <system_error>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_ascii.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "client/http_client_transport_error.h"
#include "server/peer_address.h"

namespace ruvia::detail {
namespace {

// One transport read: what an idle keep-alive connection keeps for the next
// response. Larger growth (long heads, a busy tunnel) goes back to the worker.
constexpr std::size_t connection_read_buffer_retained_bytes = std::size_t{16} * 1024;

[[nodiscard]] bool alpn_allows_http1(client_alpn alpn) noexcept {
    return alpn == client_alpn::http11 || alpn == client_alpn::negotiate;
}

[[nodiscard]] bool alpn_allows_http2(client_alpn alpn) noexcept {
    return alpn == client_alpn::http2 || alpn == client_alpn::negotiate;
}

[[nodiscard]] client_tcp_endpoint http_client_endpoint(http_client_pool& pool, std::string_view dial_host,
    std::uint16_t dial_port, std::string_view server_name, client_alpn alpn) noexcept {
    const auto& config = pool.config();
    const bool https = config.scheme_ == http_scheme::https;
    return {.host_ = dial_host,
        .port_ = dial_port,
        .tls_ = https ? &config.tls_ : nullptr,
        .tcp_ = config.tcp_,
        .alpn_ = https ? client_alpn_bytes(alpn) : std::span<const unsigned char>{},
        .identity_host_ = server_name,
        .resolve_context_ = &pool,
        .resolve_ = [](void* context, client_host_resolver& resolver, const operation_deadline& timeout, std::string_view host, std::uint16_t port) -> task<client_resolve_result> {
            return static_cast<http_client_pool*>(context)->resolve_addresses(host, port, resolver, timeout);
        }};
}

}  // namespace

http_client_connection::http_client_connection(http_client_pool& pool, std::size_t index, std::uint64_t serial,
    std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, bool alias_route,
    client_alpn alpn)
    : dial_host_(dial_host, process_resource()),
      server_name_(server_name, process_resource()),
      transport_(pool.io_context(), pool.tls_context(), pool.worker(),
          http_client_endpoint(pool, dial_host_, dial_port, server_name_, alpn), pool.wire_counters()),
      read_buffer_(process_resource()),
      write_buffer_(process_resource()),
#ifdef RUVIA_ENABLE_HTTP2
      http2_(*this, transport_, pool.tasks(), pool.worker(), pool.config().http2_ping_interval_),
#endif
      connect_scheduler_(1, pool.worker(), process_resource()),
      http1_scheduler_(1, pool.worker(), process_resource()),
      alpn_(alpn),
      pool_(pool),
      index_(index),
      serial_(serial),
      alias_route_(alias_route),
#ifdef RUVIA_ENABLE_HTTP2
      http2_streams_(process_resource()),
#endif
      idle_watch_signal_(pool.worker()) {
}

http_client_connection::~http_client_connection() = default;

bool http_client_connection::serves(std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name,
    client_alpn alpn) const noexcept {
    if (fresh_ || retired_ || address_retired_ || transport_.endpoint().port_ != dial_port ||
        !http_ascii_equals_ignore_case(dial_host_, dial_host) || !http_ascii_equals_ignore_case(server_name_, server_name)) {
        return false;
    }
    // Another offer also reuses a connection whose negotiated protocol it
    // permits; an unsettled one only serves its own offer.
    return alpn_ == alpn || (protocol_ == protocol_type::http1 && alpn_allows_http1(alpn)) ||
           (protocol_ == protocol_type::http2 && alpn_allows_http2(alpn));
}

bool http_client_connection::idle() const noexcept {
    if (!connected_ || retired_) {
        return false;
    }
#ifdef RUVIA_ENABLE_HTTP2
    if (protocol_ == protocol_type::http2) {
        return http2_.usable() && http2_streams_.empty();
    }
#endif
    return protocol_ == protocol_type::http1 && http1_operations_ == 0 && idle_watch_wanted_;
}

bool http_client_connection::reclaimable() const noexcept {
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running() || !http2_streams_.empty() || state_waiters_ != nullptr) {
        return false;
    }
#endif
    return users_ == 0 && !connected_ && http1_operations_ == 0 && !idle_watcher_started_;
}

#ifdef RUVIA_ENABLE_HTTP2
void http_client_connection::on_http2_capacity() noexcept {
    pool_.admission_changed();
}

void http_client_connection::on_http2_stopped() noexcept {
    pool_.connection_retired();
}
#endif

void http_client_connection::capture_connection_info(http_client_response_state& response, bool reused) const {
    if (response.connection_info_.id_ != connection_id_) {
        response.connection_info_.peer_ip_ = peer_ip_;
        response.connection_info_.peer_port_ = transport_.peer_endpoint().port();
        response.connection_info_.id_ = connection_id_;
    }
    response.connection_info_.reused_ = reused;
    response.connection_info_.connect_duration_ = reused ? std::chrono::steady_clock::duration{} : transport_.connect_duration();
    response.connection_info_.tls_duration_ = reused ? std::chrono::steady_clock::duration{} : transport_.tls_duration();
}

void http_client_connection::retire_addresses() noexcept {
    address_retired_ = true;
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running()) {
        http2_.mark_draining();
        for (auto* stream : http2_streams_) {
            if (stream->state_->tunnel_) {
                cancel_http2_stream(stream->request_id_, client_abort_reason::closing);
            }
        }
        if (http2_streams_.empty()) {
            close_idle();
        }
        return;
    }
#endif
    if (http1_response_ == nullptr || http1_response_->tunnel_) {
        close();
    }
}

#ifdef RUVIA_ENABLE_HTTP2
std::size_t http_client_connection::available_http2_request_streams() const noexcept {
    return protocol_ == protocol_type::http2
               ? http2_.available_request_streams(
                     pool_.config().max_concurrent_http2_streams_per_connection_)
               : 0;
}
#endif

void http_client_connection::throw_if_aborted() const {
    if (const auto reason = transport_.abort_reason(); reason != client_abort_reason::none) {
        throw_http_client_abort(reason);
    }
}

void http_client_connection::abort_http1_response() noexcept {
    if (http1_response_ == nullptr) {
        return;
    }
    http1_response_->request_informational_stop();
    if (auto* output = http1_response_->output()) {
        output->stop();
    }
    http1_response_->space_signal_.notify();
}

void http_client_connection::close() noexcept {
    end_idle();
    abort_http1_response();
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running()) {
        http2_.fail({.status_ = client_transport_status::io_failed,
            .error_ = std::make_error_code(std::errc::operation_canceled)});
        return;
    }
#endif
    transport_.close();
    connected_ = false;
    protocol_ = protocol_type::unknown;
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_streams_.empty()) {
        // Registered streams may still borrow the retired connection object.
        http2_.reset();
    }
#endif
    read_buffer_.clear();
    write_buffer_.clear();
    // A parked idle watcher ends with the connection.
    idle_watch_signal_.notify();
    pool_.connection_retired();
}

void http_client_connection::begin_idle() noexcept {
    if (!connected_) {
        return;
    }
    if (address_retired_ || fresh_) {
        close_idle();
        return;
    }
    if (protocol_ == protocol_type::http1 && !idle_watch_wanted_) {
        idle_watch_wanted_ = true;
        if (idle_watcher_started_) {
            idle_watch_signal_.notify();
        } else {
            try {
                pool_.tasks().spawn(watch_idle_http1());
                idle_watcher_started_ = true;
            } catch (...) {
                // An unwatched idle connection is not kept.
                close();
                return;
            }
        }
    }
    const auto& idle_timeout = pool_.config().idle_timeout_;
    if (!idle_timeout || idle_timer_.registered()) {
        return;
    }
    try {
        (void)pool_.worker().schedule_timer(idle_timer_, deadline_after(idle_timeout->value()),
            [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    close_idle();
                }
            });
    } catch (...) {
        // An idle connection without its bound is not kept.
        close();
    }
}

void http_client_connection::end_idle() noexcept {
    idle_order_ = 0;
    idle_timer_.cancel();
    if (std::exchange(idle_watch_wanted_, false)) {
        ++idle_period_;
        // An idle HTTP/1 connection has no other pending operation, and the
        // next one starts only after this returns.
        if (std::exchange(idle_wait_pending_, false)) {
            transport_.cancel_wait();
        }
    }
}

void http_client_connection::close_idle() noexcept {
    // Every use of the connection ends the idle bound first; these hold only
    // for a connection nothing reused.
    if (!connected_ || http1_response_ != nullptr) {
        return;
    }
#ifdef RUVIA_ENABLE_HTTP2
    if (!http2_streams_.empty()) {
        return;
    }
    if (protocol_ == protocol_type::http2 && http2_.running()) {
        end_idle();
        close_http2_session();
        return;
    }
#endif
    close();
}

#ifdef RUVIA_ENABLE_HTTP2
void http_client_connection::close_http2_session() noexcept {
    const client_transport_result closed{
        .status_ = client_transport_status::io_failed, .error_ = std::make_error_code(std::errc::operation_canceled)};
    // Without a running generation that can send GOAWAY it fails at once.
    (void)http2_.begin_close(closed);
}
#endif

task<void> http_client_connection::watch_idle_http1() {
    // Pingora's idle_poll: an HTTP/1 peer sends nothing between exchanges, so
    // readiness of an idle connection is its EOF, a reset or unsolicited bytes,
    // and the connection closes at once instead of waiting for reuse or
    // idle_timeout_. Readiness is awaited on the raw socket without running
    // the TLS engine; idle_status() then classifies it.
    // Parked between idle periods while connected; a closed connection's
    // watcher ends so the pool can reclaim the owner.
    while (!retired_ && connected_) {
        if (!idle_watch_wanted_) {
            co_await idle_watch_signal_.wait();
            continue;
        }
        const auto period = idle_period_;
        idle_wait_pending_ = true;
        co_await transport_.wait_readable();
        if (period != idle_period_ || retired_) {
            // end_idle() cancelled the wait: the connection is in use or closed.
            continue;
        }
        idle_wait_pending_ = false;
        const auto status = transport_.idle_status();
        if (status == client_idle_transport_status::reusable) {
            continue;
        }
        if (status == client_idle_transport_status::pending_data) {
            pool_.record_idle_connection_with_pending_data();
        }
        close();
    }
    idle_watcher_started_ = false;
    pool_.connection_retired();
}

void http_client_connection::retain_idle_buffers() noexcept {
    // Unread bytes belong to the transport; only an empty buffer is trimmed.
    if (read_buffer_.empty()) {
        read_buffer_.clear_retaining_small(connection_read_buffer_retained_bytes);
    }
    write_buffer_.clear_retaining_small();
}

void http_client_connection::shutdown() noexcept {
    retired_ = true;
    (void)connect_scheduler_.close();
    (void)http1_scheduler_.close();
    idle_watch_signal_.notify();
#ifdef RUVIA_ENABLE_HTTP2
    if (http2_.running() && !http2_.failed() && http2_streams_.empty()) {
        // No connect or HTTP/1 exchange runs beside a live session.
        end_idle();
        close_http2_session();
        return;
    }
#endif
    transport_.abort(client_abort_reason::closing);
    abort_http1_response();
    close();
}

void http_client_connection::cancel(std::uint64_t generation, client_abort_reason reason) noexcept {
    if (generation_ != generation) {
        return;
    }
    transport_.abort(reason);
    abort_http1_response();
    transport_.close();
    connected_ = false;
    protocol_ = protocol_type::unknown;
#ifdef RUVIA_ENABLE_HTTP2
    http2_.wake_writer();
    http2_.state_signal().notify();
#endif
}

bool http_client_connection::cancel_by_id(std::uint64_t cancellation_id) noexcept {
    if (cancellation_id_ == cancellation_id) {
        cancellation_id_ = 0;
        cancel(generation_, client_abort_reason::cancelled);
        return true;
    }
#ifdef RUVIA_ENABLE_HTTP2
    for (auto* waiter = state_waiters_; waiter; waiter = waiter->next_) {
        if (waiter->cancellation_id_ == cancellation_id) {
            http2_.state_signal().notify();
            return true;
        }
    }
    const auto stream = std::ranges::find_if(http2_streams_,
        [cancellation_id](const http2_stream* registered) { return registered->cancellation_id_ == cancellation_id; });
    if (stream == http2_streams_.end()) {
        return false;
    }
    (*stream)->cancellation_id_ = 0;
    cancel_http2_stream((*stream)->request_id_, client_abort_reason::cancelled);
    return true;
#else
    return false;
#endif
}

task<bool> http_client_connection::ensure_connected(const operation_deadline& operation_timeout_value,
    const operation_deadline& acquire_timeout, stop_token stop_token_value,
    http_client_response_state& response) {
    const auto acquired = co_await connect_scheduler_.acquire({.timeout_ = acquire_timeout.timeout(), .stop_token_ = stop_token_value});
    if (!acquired.has_value()) {
        throw_http_client_wait_failure(acquired.status(), "HTTP client connect wait");
    }
    // However the connect ends, requests waiting for this route's protocol re-check.
    struct admission_publication final {
        http_client_pool& pool_;
        ~admission_publication() {
            pool_.admission_changed();
        }
    } publication{pool_};
    const auto connect_slot = acquired.value();
    struct connect_lease final {
        pool_lease_scheduler& scheduler_;
        std::size_t slot_;
        ~connect_lease() {
            scheduler_.release(slot_);
        }
    } connect_lease_value{connect_scheduler_, connect_slot};
    if (response.abandoned_) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP response was abandoned");
    }
    end_idle();
    if (address_retired_ && protocol_ != protocol_type::http2) {
        close();
    }
    if (retired_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client connection is retired");
    }
    if (connected_) {
#ifdef RUVIA_ENABLE_HTTP2
        if (protocol_ == protocol_type::http2) {
            if (http2_.usable()) {
                co_return true;
            }
            // A draining session retires once its last stream left.
        } else
#endif
        {
            const auto idle_status = transport_.idle_status();
            if (idle_status == client_idle_transport_status::reusable) {
                co_return true;
            }
            if (idle_status == client_idle_transport_status::pending_data) {
                pool_.record_idle_connection_with_pending_data();
            }
            close();
        }
    }
    if (stop_token_value.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client connect cancelled");
    }
#ifdef RUVIA_ENABLE_HTTP2
    // A previous HTTP/2 generation may still be draining or unwinding its
    // drivers and streams; they borrow the transport that connect() replaces.
    // Waiting for them is part of acquiring this slot, and must not abort
    // their transport, so it precedes this connect's cancellation binding.
    if (http2_.running() || !http2_streams_.empty()) {
        if (http2_streams_.empty()) {
            // A session that admits no request and has no stream left.
            close_http2_session();
        }
        co_await wait_for_http2_session_stop(acquire_timeout, stop_token_value, response);
        if (retired_) {
            throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client connection is retired");
        }
    }
#endif
    transport_.reset_abort();
    next_nonzero_id(generation_);
    struct connect_generation final {
        http_client_connection& connection_;
        ~connect_generation() {
            next_nonzero_id(connection_.generation_);
        }
    } connect_generation_value{*this};
    http_client_response_cancellation cancellation(pool_.cancellation_target(), cancellation_id_, response);
    cancellation.arm(stop_token_value);
    throw_if_aborted();
#ifdef RUVIA_ENABLE_HTTP2
    http2_.reset();
#endif
    if (pool_.prefers_http11() && alpn_ == client_alpn::negotiate) {
        // The origin required HTTP/1.1 (RFC 9113 section 7): later connects
        // offer only http/1.1, like Pingora's prefer_h1.
        transport_.offer_alpn(client_alpn::http11);
    }
    const auto timeout = operation_timeout_value.constrained_by(response.effective_connect_timeout_);
    struct connect_attempt final {
        http_client_connection& connection_;
        bool committed_{false};
        ~connect_attempt() {
            if (!committed_) {
                connection_.close();
            }
        }
    } attempt{*this};
    const auto connected = co_await transport_.connect(timeout);
    if (transport_.peer_endpoint().port() != 0) {
        connection_id_ = pool_.next_connection_id();
        peer_ip_ = to_ip_address(transport_.peer_endpoint().address()).normalized_ipv4();
        capture_connection_info(response, false);
    }
    if (!connected.ok()) {
        throw_http_client_transport_error(connected, "upstream closed while connecting");
    }
#ifdef RUVIA_ENABLE_HTTP2
    const auto& config = pool_.config();
    if (transport_.secure()) {
        const auto alpn = transport_.alpn();
        if (alpn_ == client_alpn::http2 && alpn != "h2") {
            throw http_client_error(
                make_error_code(http_client_errc::protocol_unavailable), "upstream did not negotiate HTTP/2");
        }
        protocol_ = alpn == "h2" ? protocol_type::http2 : protocol_type::http1;
    } else {
        // Plaintext HTTP/2 is prior knowledge (RFC 9113 section 3.3).
        protocol_ = alpn_ == client_alpn::http2 ? protocol_type::http2 : protocol_type::http1;
    }
#else
    protocol_ = protocol_type::http1;
#endif
    connected_ = true;
    address_retired_ = false;
#ifdef RUVIA_ENABLE_HTTP2
    if (protocol_ != protocol_type::http2) {
        attempt.committed_ = true;
        co_return false;
    }
    const auto started = co_await http2_.start({.enable_push_ = config.push_.enabled_,
                                                   .receive_origin_advertisements_ = config.advertisements_.receive_origins_},
        timeout);
    switch (started.status_) {
        case http2_client_session_start_status::started:
            http2_session_requests_ = 0;
            attempt.committed_ = true;
            co_return false;
        case http2_client_session_start_status::transport_failed:
            throw_http_client_transport_error(started.transport_, "upstream closed during HTTP/2 preface");
        case http2_client_session_start_status::protocol_failed:
            throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid HTTP/2 connection preface");
    }
    std::terminate();
#else
    attempt.committed_ = true;
    co_return false;
#endif
}

}  // namespace ruvia::detail
