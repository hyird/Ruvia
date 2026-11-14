#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ssl/error.hpp>

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "context.h"
#include "context_access.h"
#include "crypto.h"
#include "event_loop.h"
#include "http1_client_request.h"
#include "http1_client_response.h"
#include "http1_server_response.h"
#include "http_client.h"
#include "http_client_pool.h"
#include "http_client_redirect.h"
#include "http_client_types.h"
#include "http_connect_udp.h"
#include "http_header.h"
#include "http_request.h"
#include "http_server.h"
#include "inline_storage.h"
#include "stop_token.h"
#include "streaming.h"
#include "streaming_session.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"

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

[[nodiscard]] client_tcp_endpoint http_client_endpoint(http_client_pool& pool, std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, client_alpn alpn) noexcept {
    const auto& config = pool.config();
    const bool https = config.scheme_ == http_scheme::https;
    return {.host_ = dial_host, .port_ = dial_port, .tls_ = https ? &config.tls_ : nullptr, .tcp_ = config.tcp_, .alpn_ = https ? client_alpn_bytes(alpn) : std::span<const unsigned char>{}, .identity_host_ = server_name, .resolve_context_ = &pool, .resolve_ = [](void* context, client_host_resolver& resolver, const operation_deadline& timeout, std::string_view host, std::uint16_t port) -> task<client_resolve_result> {
        return static_cast<http_client_pool*>(context)->resolve_addresses(host, port, resolver, timeout);
    }};
}

}  // namespace

http_client_connection::http_client_connection(http_client_pool& pool, std::size_t index, std::uint64_t serial, std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, bool alias_route, client_alpn alpn) :
    dial_host_(dial_host),
    server_name_(server_name),
    transport_(pool.io_context(), pool.tls_context(), pool.worker(), http_client_endpoint(pool, dial_host_, dial_port, server_name_, alpn), pool.wire_counters()),
    read_buffer_(),
    write_buffer_(),
#ifdef RUVIA_ENABLE_HTTP2
    http2_(*this, transport_, pool.tasks(), pool.worker(), pool.config().http2_ping_interval_),
#endif
    connect_scheduler_(1, pool.worker()),
    http1_scheduler_(1, pool.worker()),
    alpn_(alpn),
    pool_(pool),
    index_(index),
    serial_(serial),
    alias_route_(alias_route),
#ifdef RUVIA_ENABLE_HTTP2
    http2_streams_(),
#endif
    idle_watch_signal_(pool.worker()) {
}

http_client_connection::~http_client_connection() = default;

bool http_client_connection::serves(std::string_view dial_host, std::uint16_t dial_port, std::string_view server_name, client_alpn alpn) const noexcept {
    if (fresh_ || retired_ || address_retired_ || transport_.endpoint().port_ != dial_port || !http_ascii_equals_ignore_case(dial_host_, dial_host) || !http_ascii_equals_ignore_case(server_name_, server_name)) {
        return false;
    }
    // Another offer also reuses a connection whose negotiated protocol it
    // permits; an unsettled one only serves its own offer.
    return alpn_ == alpn || (protocol_ == protocol_type::http1 && alpn_allows_http1(alpn)) || (protocol_ == protocol_type::http2 && alpn_allows_http2(alpn));
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
    if ((users_ == 0 && http1_operations_ == 0 && http1_response_ == nullptr) || (http1_response_ != nullptr && http1_response_->tunnel_)) {
        close();
    }
}

#ifdef RUVIA_ENABLE_HTTP2
std::size_t http_client_connection::available_http2_request_streams() const noexcept {
    return protocol_ == protocol_type::http2 ? http2_.available_request_streams(pool_.config().max_concurrent_http2_streams_per_connection_) : 0;
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
        http2_.fail({.status_ = client_transport_status::io_failed, .error_ = std::make_error_code(std::errc::operation_canceled)});
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
        (void)pool_.worker().schedule_timer(idle_timer_, deadline_after(idle_timeout->value()), [this](worker_timer_outcome outcome) noexcept {
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
    const client_transport_result closed{.status_ = client_transport_status::io_failed, .error_ = std::make_error_code(std::errc::operation_canceled)};
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
    const auto stream = std::ranges::find_if(http2_streams_, [cancellation_id](const http2_stream* registered) {
        return registered->cancellation_id_ == cancellation_id;
    });
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

task<bool> http_client_connection::ensure_connected(const operation_deadline& operation_timeout_value, const operation_deadline& acquire_timeout, stop_token stop_token_value, http_client_response_state& response) {
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
            throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "upstream did not negotiate HTTP/2");
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
    const auto started = co_await http2_.start({.enable_push_ = config.push_.enabled_, .receive_origin_advertisements_ = config.advertisements_.receive_origins_}, timeout);
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

namespace ruvia {
namespace detail {

http_client_websocket_handshake::http_client_websocket_handshake(websocket_client_negotiation_config_view config) :
    negotiation_(config) {
    std::array<std::uint8_t, 16> nonce;
    if (!fill_random_bytes(nonce)) {
        throw std::runtime_error("failed to generate WebSocket handshake key");
    }
    http_base64_encode(key_.data(), nonce);
}

http_client_output_queue* http_client_request_storage::output() const noexcept {
    return tunnel_ != nullptr ? &tunnel_->output_ : (upload_ != nullptr ? &upload_->output_ : nullptr);
}

http_client_request_storage::http_client_request_storage(std::string_view method, std::string_view target, std::span<const http_header_view> fields) :
    http_client_request_storage(method, target) {
    if (fields.empty()) {
        return;
    }
    headers_.reserve(fields.size());
    for (const auto& field : fields) {
        headers_.emplace_back(field.name(), field.value());
    }
}

http_client_request_storage::http_client_request_storage(std::string_view method, std::string_view target) :
    method_(method),
    target_(target),
    route_authority_(method_) {}

http_client_request_storage::http_client_request_storage(http_client_request_storage&& other) noexcept :
    options_(std::move(other.options_)),
    informational_(std::move(other.informational_)),
    pool_owner_(std::move(other.pool_owner_)),
    method_(std::move(other.method_)),
    target_(std::move(other.target_)),
    headers_(std::move(other.headers_)),
    body_(std::move(other.body_)),
    tunnel_authority_(std::move(other.tunnel_authority_)),
    tunnel_protocol_(std::move(other.tunnel_protocol_)),
    websocket_(std::move(other.websocket_)),
    host_stop_(std::move(other.host_stop_)),
    is_tunnel_(other.is_tunnel_),
    is_upgrade_(other.is_upgrade_),
    tunnel_(std::exchange(other.tunnel_, nullptr)),
    has_body_(std::exchange(other.has_body_, false)),
    upload_(std::exchange(other.upload_, nullptr)),
    route_authority_(other.route_authority_) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(method_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(target_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(headers_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(body_)>);
}

void http_client_request_storage::set_tunnel(std::string_view authority, std::string_view protocol) {
    tunnel_authority_.assign(authority);
    tunnel_protocol_.assign(protocol);
    is_tunnel_ = true;
}

http_client_request_storage& http_client_request_storage::set_body(std::string_view body) {
    body_.assign(body);
    has_body_ = true;
    return *this;
}

namespace {
// host[:port] without userinfo, like the authority of base_url_.
[[nodiscard]] http_authority_view parse_route_authority(std::string_view authority) {
    const auto parsed = parse_http_authority(borrowed_text(authority));
    if (!parsed || authority.find_first_of("@/?#") != std::string_view::npos || parsed->host().empty() || parsed->port_kind() == http_authority_port_kind::empty || parsed->port() == 0) {
        throw std::invalid_argument("HTTP request authority must be host[:port]");
    }
    auto host = parsed->host();
    if (host.starts_with('[')) {
        host = host.substr(1, host.size() - 2);
    }
    validate_client_origin_host(host, "HTTP request authority host must not be empty", "HTTP request authority host is invalid");
    return *parsed;
}
}  // namespace

void validate_http_client_request_options(const http_client_request_options& options) {
    if (options.field_source_ != http_request_field_source::generated && options.field_source_ != http_request_field_source::forwarded) {
        throw std::invalid_argument("invalid HTTP request field source");
    }
    validate_http_client_route_options(options);
}

std::shared_ptr<http_client_informational_callback> share_http_client_informational(http_client_informational_callback&& callback) {
    if (!callback) {
        return nullptr;
    }
    return std::make_shared<http_client_informational_callback>(std::move(callback));
}

void validate_http_client_route_options(const http_client_request_options& options) {
    if (options.authority_) {
        (void)parse_route_authority(*options.authority_);
    }
    if (options.dial_host_) {
        validate_client_origin_host(*options.dial_host_, "HTTP request dial host must not be empty", "HTTP request dial host is invalid");
    }
    if (options.dial_port_ && options.dial_port_->value() == 0) {
        throw std::invalid_argument("HTTP request dial port must be positive");
    }
    if (options.server_name_) {
        validate_client_origin_host(*options.server_name_, "HTTP request TLS server name must not be empty", "HTTP request TLS server name is invalid");
    }
    if (options.versions_) {
        // The alias scheme is checked with the route.
        validate_http_client_versions(*options.versions_, http_scheme::https);
    }
}

http_client_request_options copy_http_client_request_settings(const http_client_request_options& options) {
    // Binding every member keeps this copy exhaustive: a new option fails to compile here.
    const auto& [decode_content, field_source, fresh_connection, timeouts, informational, authority, dial_host, dial_port, server_name, versions] = options;
    (void)informational;
    return {.decode_content_ = decode_content, .field_source_ = field_source, .fresh_connection_ = fresh_connection, .timeouts_ = timeouts, .informational_ = {}, .authority_ = authority, .dial_host_ = dial_host, .dial_port_ = dial_port, .server_name_ = server_name, .versions_ = versions};
}

void http_client_request_storage::resolve_route(const http_client_config_storage& config) {
    validate_http_client_route_options(options_);
    const bool https = config.scheme_ == http_scheme::https;
    if (options_.server_name_ && !https) {
        throw std::invalid_argument("HTTP request TLS server name requires an https origin");
    }
    if (!config.fixed_origin_ && !options_.authority_) {
        throw std::invalid_argument("HTTP client alias without a fixed origin requires authority_ on each request");
    }
    const port_number default_port = https ? 443 : 80;
    std::optional<http_origin_view> authority_origin;
    std::string_view authority_host;
    if (options_.authority_) {
        const auto parsed = parse_route_authority(*options_.authority_);
        const http_origin_options origin{.host_ = parsed.host(), .port_ = parsed.effective_port(default_port)};
        authority_origin = https ? http_origin_view::https(origin) : http_origin_view::http(origin);
        route_authority_ = make_http_origin_authority(*authority_origin);
        authority_host = parsed.host();
        if (authority_host.starts_with('[')) {
            authority_host = authority_host.substr(1, authority_host.size() - 2);
        }
    }
    const auto origin_value = authority_origin ? *authority_origin : config.origin();
    http_client_route route{
        .origin_ = origin_value,
        .authority_ = authority_origin ? std::string_view(route_authority_) : std::string_view(config.authority_),
        // An alias without a fixed origin (or dial target) dials the authority.
        .dial_host_ = config.dial_host_.empty() ? authority_host : std::string_view(config.dial_host_),
        .dial_port_ = config.dial_port_ != 0 ? config.dial_port_ : origin_value.port(),
        .server_name_ = {},
        .alias_origin_ = true,
        .alias_transport_ = true,
        .versions_ = options_.versions_.value_or(config.versions_)
    };
    if (options_.versions_) {
        validate_http_client_versions(*options_.versions_, config.scheme_);
    }
    if (authority_origin) {
        route.alias_origin_ = config.fixed_origin_ && http_authorities_equal(borrowed_text(route.authority_), borrowed_text(std::string_view(config.authority_)), default_port);
    }
    if (is_tunnel_ && !is_upgrade_ && options_.authority_) {
        if (websocket_ != nullptr) {
            // The alias authority named by the WebSocket handshake follows the override.
            tunnel_authority_.assign(route.authority_);
        } else if (tunnel_protocol_.empty() || !http_authorities_equal(borrowed_text(std::string_view(tunnel_authority_)), borrowed_text(route.authority_), default_port)) {
            throw std::invalid_argument("a CONNECT request names its own authority");
        }
    }
    if (options_.dial_host_) {
        route.dial_host_ = *options_.dial_host_;
    }
    if (options_.dial_port_) {
        route.dial_port_ = options_.dial_port_->value();
    }
    const auto alias_server_name = https ? config.tls_.tls_host(config.host_) : std::string_view{};
    route.server_name_ = options_.server_name_ ? std::string_view(*options_.server_name_) : (!route.alias_origin_ && https ? authority_host : alias_server_name);
    route.alias_transport_ = config.fixed_origin_ && http_ascii_equals_ignore_case(route.dial_host_, config.dial_host_) && route.dial_port_ == config.dial_port_ && http_ascii_equals_ignore_case(route.server_name_, alias_server_name);
    route_.emplace(route);
}

}  // namespace detail

namespace {
void append_query_component(std::string& output, std::string_view value) {
    for (const unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            output.push_back(static_cast<char>(byte));
        } else {
            http_append_percent_byte(output, byte);
        }
    }
}

void replace_field(std::vector<http_header>& fields, std::string_view name, std::string_view value) {
    http_header replacement(name, value);
    const auto found = std::ranges::find_if(fields, [name](const http_header& field) {
        return http_ascii_equals_ignore_case(field.name(), name);
    });
    if (found == fields.end()) {
        fields.push_back(std::move(replacement));
    } else {
        *found = std::move(replacement);
    }
}

bool hop_field(std::string_view name) noexcept {
    return http_ascii_equals_ignore_case(name, "connection") || http_ascii_equals_ignore_case(name, "keep-alive") || http_ascii_equals_ignore_case(name, "proxy-connection") || http_ascii_equals_ignore_case(name, "transfer-encoding") || http_ascii_equals_ignore_case(name, "proxy-authenticate") || http_ascii_equals_ignore_case(name, "proxy-authorization") || http_ascii_equals_ignore_case(name, "te") || http_ascii_equals_ignore_case(name, "trailer") || http_ascii_equals_ignore_case(name, "upgrade");
}

bool connection_named_field(std::span<const http_header> fields, std::string_view name) {
    for (const auto& field : fields) {
        if (!http_ascii_equals_ignore_case(field.name(), "connection")) {
            continue;
        }
        auto tokens = field.value();
        while (!tokens.empty()) {
            const auto comma = tokens.find(',');
            auto token = tokens.substr(0, comma);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
                token.remove_prefix(1);
            }
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
                token.remove_suffix(1);
            }
            if (http_ascii_equals_ignore_case(token, name)) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            tokens.remove_prefix(comma + 1);
        }
    }
    return false;
}

// One bound for the whole forward: commit, body and end share the deadline
// started here and the caller's stop token.
task<void> pipe_response(http_client_response response, detail::http_client_response_state& state, response_stream_writer& output, operation_options operation) {
    if (operation.stop_token_.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client operation cancelled", std::nullopt, &state.connection_info_, state.request_written_);
    }
    const detail::operation_steps steps(std::move(operation));
    stream_commit_result committed{};
    try {
        committed = co_await output.commit(state.downstream_step(steps));
    } catch (...) {
        state.rethrow_downstream_failure(steps);
    }
    if (committed == stream_commit_result::body) {
        co_await state.consume_body<void>(state.downstream_step(steps), &output);
        std::vector<http_header_view> trailers;
        trailers.reserve(response.trailers().size());
        for (const auto& field : response.trailers()) {
            if (!hop_field(field.name()) && !connection_named_field(response.headers(), field.name())) {
                trailers.emplace_back(field.name(), field.value());
            }
        }
        try {
            co_await output.end(trailers, state.downstream_step(steps));
        } catch (...) {
            state.rethrow_downstream_failure(steps);
        }
    }
}

// The origin a request addresses: its authority override, else the alias.
// Redirect targets resolve against it.
[[nodiscard]] http_origin_view request_origin(const http_client_ref& client, const http_client_request_options& options) {
    if (!options.authority_) {
        return client.origin();
    }
    const auto parsed = detail::parse_route_authority(*options.authority_);
    const bool https = client.scheme() == http_scheme::https;
    const http_origin_options origin{.host_ = parsed.host(), .port_ = parsed.effective_port(https ? 443 : 80)};
    return https ? http_origin_view::https(origin) : http_origin_view::http(origin);
}

void clear_route_overrides(http_client_request_options& options) noexcept {
    options.authority_.reset();
    options.dial_host_.reset();
    options.dial_port_.reset();
    options.server_name_.reset();
}

// A task frame may bind exactly one scope. This outer frame binds the
// destination while its child remains bound to the source response.
task<void> pipe_response_in_destination(task<void> source) {
    co_await std::move(source);
}
}  // namespace

http_client_request::http_client_request(const http_client_ref& client, std::string_view method, std::string_view path) :
    client_(client),
    method_(method),
    target_(path),
    headers_(),
    body_() {
    client_.registration_.require_active();
    if (!is_valid_http_method_token(method) || (!is_valid_http_origin_form_target(path) && !(method == "OPTIONS" && path == "*"))) {
        throw std::invalid_argument("HTTP request requires a valid method and relative origin-form target");
    }
}

http_client_request http_client_ref::request(std::string_view method, std::string_view path) const {
    registration_.require_active();
    return http_client_request(*this, method, path);
}
http_client_request http_client_ref::get(std::string_view path) const {
    return request("GET", path);
}
http_client_request http_client_ref::post(std::string_view path) const {
    return request("POST", path);
}
http_client_request http_client_ref::put(std::string_view path) const {
    return request("PUT", path);
}
http_client_request http_client_ref::patch(std::string_view path) const {
    return request("PATCH", path);
}
http_client_request http_client_ref::del(std::string_view path) const {
    return request("DELETE", path);
}
http_client_request http_client_ref::head(std::string_view path) const {
    return request("HEAD", path);
}
http_client_request http_client_ref::options(std::string_view path) const {
    return request("OPTIONS", path);
}

http_client_request& http_client_request::header(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    replace_field(headers_, name, value);
    return *this;
}
http_client_request& http_client_request::headers(std::span<const http_header_view> fields) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value());
        }
    } catch (...) {
        while (headers_.size() > old_size) {
            headers_.pop_back();
        }
        throw;
    }
    return *this;
}
http_client_request& http_client_request::query(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    if (target_ == "*") {
        throw std::invalid_argument("asterisk target cannot contain query parameters");
    }
    const auto old_size = target_.size();
    const char separator = target_.find('?') == std::string_view::npos ? '?' : '&';
    try {
        target_.push_back(separator);
        append_query_component(target_, name);
        target_.push_back('=');
        append_query_component(target_, value);
    } catch (...) {
        target_.resize(old_size);
        throw;
    }
    return *this;
}
http_client_request& http_client_request::body(std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    std::string owned(value);
    body_.swap(owned);
    has_body_ = true;
    return *this;
}
http_client_request& http_client_request::body(std::span<const std::byte> value) {
    return body(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
}
http_client_request& http_client_request::configure(http_client_request_options options) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    detail::validate_http_client_request_options(options);
    informational_callback_ = detail::share_http_client_informational(std::move(options.informational_));
    request_options_ = std::move(options);
    return *this;
}
http_client_request& http_client_request::on_redirect(http_client_redirect_observer observer) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    redirect_observer_ = std::move(observer);
    return *this;
}
http_client_request& http_client_request::on_redirect(http_client_async_redirect_observer observer) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    async_redirect_observer_ = std::move(observer);
    return *this;
}
http_client_request& http_client_request::redirects(std::size_t maximum) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    max_redirects_ = maximum;
    return *this;
}

task<http_client_tunnel_result> http_client_request::upgrade(http_client_tunnel_config config) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    client_.registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes) {
        throw std::invalid_argument("invalid HTTP Upgrade tunnel policy");
    }
    const detail::inline_http_header_views<16> fields(headers_);
    detail::http_client_request_storage request(method_, target_, fields.view());
    request.options_ = detail::copy_http_client_request_settings(request_options_);
    request.informational_ = informational_callback_;
    request.pool_owner_ = client_.pool_owner_;
    request.set_host_stop(client_.host_stop_token_);
    request.set_upgrade();
    if (has_body_) {
        request.set_body(body_);
    }
    auto operation = make_scoped_operation(client_.registration_.scope(), client_.pool_->open_tunnel(std::move(request), config, client_.options_));
    consumed_ = true;
    std::string(method_.get_allocator()).swap(method_);
    std::string(target_.get_allocator()).swap(target_);
    std::vector<http_header>(headers_.get_allocator()).swap(headers_);
    std::string(body_.get_allocator()).swap(body_);
    return operation;
}

std::vector<http_header_view> http_client_request::field_views() const {
    std::vector<http_header_view> fields;
    fields.reserve(headers_.size());
    for (const auto& field : headers_) {
        fields.emplace_back(field.name(), field.value());
    }
    return fields;
}

task<http_client_response> http_client_request::send() {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    client_.registration_.require_active();
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), send_owned(std::move(*this)));
}

namespace {

[[nodiscard]] std::string format_origin_target_url(const http_origin_view& origin, std::string_view target) {
    const auto authority = make_http_origin_authority(origin);
    const std::string_view scheme_prefix = origin.scheme() == http_scheme::https ? "https://" : "http://";
    std::string url;
    url.reserve(scheme_prefix.size() + authority.size() + target.size());
    url.append(scheme_prefix);
    url.append(authority);
    if (!target.empty() && target.front() != '/' && target != "*") {
        url.push_back('/');
    }
    url.append(target);
    return url;
}

}  // namespace

task<http_client_response> http_client_request::send_owned(http_client_request request) {
    // One deadline spans every redirect hop; each hop receives it as an absolute bound.
    const operation_deadline total(request.client_.options_.timeout_.has_value() ? request.client_.options_.timeout_ : std::optional<operation_timeout>(request.client_.pool_->config().request_timeout_));
    co_return co_await follow_redirects(std::move(request), std::nullopt, total, false);
}

task<http_client_response> http_client_request::follow_redirects(http_client_request request, std::optional<http_client_response> initial_response, const operation_deadline& total, bool upload_initial_hop) {
    for (std::size_t redirects = 0;; ++redirects) {
        std::optional<http_client_response> hop_response;
        if (redirects == 0 && initial_response.has_value()) {
            hop_response.emplace(std::move(*initial_response));
            initial_response.reset();
        } else {
            request.client_.registration_.require_active();
            if (total.expired()) {
                throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP redirect deadline expired", std::nullopt, nullptr, redirects != 0, redirects);
            }
            request.client_.options_.timeout_ = total.timeout();
            std::optional<task<http_client_response>> dispatched;
            {
                const detail::inline_http_header_views<16> fields(request.headers_);
                const http_client_request_view view{.method_ = request.method_, .target_ = request.target_, .headers_ = fields.view(), .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_) : http_client_request_content_view::none()};
                dispatched.emplace(request.client_.dispatch(view, request.request_options_, request.informational_callback_));
            }
            if (redirects == request.max_redirects_) {
                std::string(request.method_.get_allocator()).swap(request.method_);
                std::string(request.target_.get_allocator()).swap(request.target_);
                std::vector<http_header>(request.headers_.get_allocator()).swap(request.headers_);
                std::string(request.body_.get_allocator()).swap(request.body_);
                try {
                    co_return co_await std::move(*dispatched);
                } catch (const http_client_error& error) {
                    if (redirects != 0 && error.redirect_hops() == 0) {
                        throw http_client_error(error.code(), error.what(), error.status(), error.connection_info(), error.request_written(), redirects);
                    }
                    throw;
                }
            }
            try {
                hop_response.emplace(co_await std::move(*dispatched));
            } catch (const http_client_error& error) {
                if (redirects != 0 && error.redirect_hops() == 0) {
                    throw http_client_error(error.code(), error.what(), error.status(), error.connection_info(), error.request_written(), redirects);
                }
                throw;
            }
        }

        auto response = std::move(*hop_response);
        if (redirects == request.max_redirects_) {
            co_return response;
        }
        const auto status = response.status().value();
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            co_return response;
        }
        const http_client_request_view current_view{.method_ = request.method_, .target_ = request.target_, .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_) : http_client_request_content_view::none()};
        const auto plan = plan_http_client_redirect_request(current_view, {.status_ = response.status()});
        if (upload_initial_hop && redirects == 0 && plan.content_disposition() != http_client_redirect_content_disposition::drop) {
            co_return response;
        }

        std::optional<std::string_view> location;
        for (const auto& field : response.headers()) {
            if (http_ascii_equals_ignore_case(field.name(), "location")) {
                if (location) {
                    throw http_client_error(make_error_code(http_client_errc::protocol_error), "repeated redirect Location", std::nullopt, &response.connection_info(), true, redirects);
                }
                location = field.value();
            }
        }
        if (!location) {
            co_return response;
        }
        const auto current_origin = request_origin(request.client_, request.request_options_);
        auto resolved = resolve_http_client_redirect_target(current_origin, {.current_target_ = request.target_, .location_ = *location});
        auto* next = std::get_if<http_client_resolved_redirect>(&resolved);
        if (next == nullptr) {
            co_return response;
        }

        if (request.redirect_observer_ || request.async_redirect_observer_) {
            const auto source_url = format_origin_target_url(current_origin, request.target_);
            const auto target_url = format_origin_target_url(next->origin(), next->target());
            const http_client_redirect_hop hop_event(redirects, current_origin, request.target_, source_url, response.connection_info(), response.status(), *location, next->origin(), next->target(), target_url, next->cross_origin());
            if (request.redirect_observer_) {
                if (!request.redirect_observer_(hop_event)) {
                    co_return response;
                }
                request.client_.registration_.require_active();
            }
            if (request.async_redirect_observer_) {
                stop_source deadline_stop;
                stop_group cancellation{request.client_.options_.stop_token_, request.client_.host_stop_token_, deadline_stop.token()};
                worker_timer_registration timer;
                if (const auto deadline_value = total.deadline()) {
                    (void)request.client_.pool_->worker().schedule_timer(timer, *deadline_value, [&deadline_stop](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            deadline_stop.request_stop();
                        }
                    });
                }
                const auto require_live_observer = [&]() {
                    if (total.expired()) {
                        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP redirect policy deadline expired", std::nullopt, &response.connection_info(), true, redirects);
                    }
                    if (cancellation.token().stop_requested()) {
                        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP redirect policy cancelled", std::nullopt, &response.connection_info(), true, redirects);
                    }
                };
                require_live_observer();
                const auto accepted = co_await request.async_redirect_observer_(hop_event, cancellation.token());
                require_live_observer();
                if (!accepted) {
                    co_return response;
                }
                request.client_.registration_.require_active();
            }
        }

        const bool has_custom_policy = static_cast<bool>(request.redirect_observer_) || static_cast<bool>(request.async_redirect_observer_);
        auto next_pool = request.client_.pool_owner_;
        if (next->cross_origin()) {
            if (!has_custom_policy && request.client_.scheme() == http_scheme::https && next->scheme() != http_scheme::https) {
                co_return response;
            }
            auto authority = make_http_origin_authority(next->origin());
            // Route overrides belong to the origin that set them.
            clear_route_overrides(request.request_options_);
            if (!request.client_.pool_->config().fixed_origin_ && has_custom_policy && next->scheme() == request.client_.scheme()) {
                // An alias without a fixed origin reaches the accepted origin itself.
                request.request_options_.authority_.emplace(std::string_view(authority));
            } else {
                if (request.client_.registry_ == nullptr) {
                    co_return response;
                }
                next_pool = request.client_.registry_->find_origin(next->scheme(), authority);
                if (next_pool == nullptr) {
                    co_return response;
                }
            }
        }

        if (upload_initial_hop && redirects == 0 && response.state_ != nullptr && response.state_->upload_) {
            response.state_->upload_->abandoned_ = true;
            response.state_->upload_->output_.stop();
        }

        request.method_.assign(plan.method());
        request.target_.assign(next->target());
        const bool drop = plan.content_disposition() == http_client_redirect_content_disposition::drop;
        std::erase_if(request.headers_, [&](const http_header& field) {
            const auto name = field.name();
            return http_ascii_equals_ignore_case(name, "host") || (next->cross_origin() && (http_ascii_equals_ignore_case(name, "authorization") || http_ascii_equals_ignore_case(name, "proxy-authorization") || http_ascii_equals_ignore_case(name, "cookie"))) || (drop && (http_ascii_equals_ignore_case(name.substr(0, 8), "content-") || http_ascii_equals_ignore_case(name, "digest") || http_ascii_equals_ignore_case(name, "repr-digest") || http_ascii_equals_ignore_case(name, "transfer-encoding") || http_ascii_equals_ignore_case(name, "trailer") || http_ascii_equals_ignore_case(name, "expect")));
        });
        if (drop) {
            request.has_body_ = false;
            std::string(request.body_.get_allocator()).swap(request.body_);
        }
        request.client_.pool_owner_ = std::move(next_pool);
        request.client_.pool_ = request.client_.pool_owner_.get();
    }
}

task<http_client_exchange> http_client_request::upload(http_client_upload_config config) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    if (has_body_) {
        throw std::invalid_argument("streamed upload cannot also have a buffered body");
    }
    client_.registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.continue_timeout_.value() <= std::chrono::milliseconds::zero() || (config.expectation_ != http_client_request_expectation::none && config.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), upload_owned(std::move(*this), config));
}

task<http_client_exchange> http_client_request::upload_owned(http_client_request request, http_client_upload_config config) {
    std::optional<task<http_client_exchange>> dispatched;
    {
        const detail::inline_http_header_views<16> fields(request.headers_);
        detail::http_client_request_storage storage(request.method_, request.target_, fields.view());
        storage.options_ = detail::copy_http_client_request_settings(request.request_options_);
        storage.informational_ = request.informational_callback_;
        storage.pool_owner_ = request.client_.pool_owner_;
        storage.set_host_stop(request.client_.host_stop_token_);
        dispatched.emplace(request.client_.pool_->open_request(std::move(storage), config, request.client_.options_));
    }
    auto exchange = co_await std::move(*dispatched);
    if (request.max_redirects_ != 0 && exchange.body().state_ != nullptr && exchange.body().state_->upload_) {
        exchange.body().state_->upload_->redirect_request_ = std::make_unique<http_client_request>(std::move(request));
    }
    co_return exchange;
}

http_client_connect_request::http_client_connect_request(const http_client_ref& client, std::string_view authority, std::string_view target, bool udp) :
    client_(client),
    authority_(authority),
    protocol_(),
    target_(target),
    headers_(),
    udp_(udp) {}
http_client_connect_request http_client_ref::connect(std::string_view authority) const {
    registration_.require_active();
    return http_client_connect_request(*this, authority, {}, false);
}
http_client_connect_request http_client_ref::udp(std::string_view path) const {
    registration_.require_active();
    return http_client_connect_request(*this, pool_->config().authority_, path, true);
}
http_client_connect_request& http_client_connect_request::header(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    replace_field(headers_, name, value);
    return *this;
}
http_client_connect_request& http_client_connect_request::headers(std::span<const http_header_view> fields) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value());
        }
    } catch (...) {
        while (headers_.size() > old_size) {
            headers_.pop_back();
        }
        throw;
    }
    return *this;
}
http_client_connect_request& http_client_connect_request::configure(http_client_request_options options) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    detail::validate_http_client_request_options(options);
    informational_callback_ = detail::share_http_client_informational(std::move(options.informational_));
    request_options_ = std::move(options);
    return *this;
}
http_client_connect_request& http_client_connect_request::protocol(std::string_view value) {
    if (consumed_ || udp_) {
        throw std::logic_error("CONNECT protocol cannot be changed");
    }
    protocol_.assign(value);
    return *this;
}
http_client_connect_request& http_client_connect_request::target(std::string_view value) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    target_.assign(value);
    return *this;
}
task<http_client_tunnel_result> http_client_connect_request::open(http_client_tunnel_config config) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    const detail::inline_http_header_views<16> fields(headers_);
    auto dispatched = udp_ ? client_.dispatch_udp({.target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_) : client_.dispatch_connect({.authority_ = authority_, .protocol_ = protocol_, .target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_);
    consumed_ = true;
    std::string(authority_.get_allocator()).swap(authority_);
    std::string(protocol_.get_allocator()).swap(protocol_);
    std::string(target_.get_allocator()).swap(target_);
    std::vector<http_header>(headers_.get_allocator()).swap(headers_);
    return dispatched;
}

task<void> http_client_response::pipe_to(context& output, operation_options operation) & {
    return pipe_to_destination(output.draft(), output.stream(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to(request_stream& output, operation_options operation) & {
    return pipe_to_destination(output.draft(), output.response(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to_destination(context_response_editor draft, response_stream_writer& writer, ::ruvia::operation_scope& destination, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    state_->require_owner_worker("HTTP response requires its owner worker");
    if (!state_->body_operation_scope_.active() || !destination.active()) {
        throw std::logic_error("HTTP response pipe_to scope is closed");
    }
    draft.status(status());
    const auto fields = headers();
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        if (!hop_field(field->name()) && !connection_named_field(fields, field->name()) && !http_ascii_equals_ignore_case(field->name(), "content-length")) {
            const bool repeated = std::any_of(fields.begin(), field, [&](const http_header& previous) {
                return http_ascii_equals_ignore_case(previous.name(), field->name());
            });
            draft.header(field->name(), field->value(), {.mode_ = repeated ? http_response_header_mode::append : http_response_header_mode::replace});
        }
    }
    auto source = make_scoped_operation(state_->body_operation_scope_, pipe_response(http_client_response(state_), *state_, writer, std::move(operation)));
    return make_scoped_operation(destination, pipe_response_in_destination(std::move(source)));
}

#ifdef RUVIA_ENABLE_HTTP3
namespace {
task<void> migrate_client_pool(std::shared_ptr<detail::http_client_pool> pool, std::string address, std::uint16_t port, operation_options options, stop_token host_stop) {
    co_await pool->migrate_http3(std::move(address), port, std::move(options), std::move(host_stop));
}
}  // namespace
#endif

task<void> http_client_ref::migrate(std::string_view local_address, std::uint16_t port) const {
    registration_.require_active();
#ifdef RUVIA_ENABLE_HTTP3
    return make_scoped_operation(registration_.scope(), migrate_client_pool(pool_owner_, std::string(local_address), port, options_, host_stop_token_));
#else
    (void)local_address;
    (void)port;
    throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 support is disabled");
#endif
}

const std::error_category& http_client_category() noexcept {
    class category final : public std::error_category {
    public:
        const char* name() const noexcept override {
            return "ruvia.http_client";
        }
        std::string message(int value) const override {
            switch (static_cast<http_client_errc>(value)) {
                case http_client_errc::not_configured:
                    return "HTTP client is not configured";
                case http_client_errc::invalid_request:
                    return "invalid HTTP request";
                case http_client_errc::resolve_failed:
                    return "HTTP name resolution failed";
                case http_client_errc::connect_failed:
                    return "HTTP connection failed";
                case http_client_errc::tls_failed:
                    return "HTTP TLS handshake failed";
                case http_client_errc::protocol_unavailable:
                    return "HTTP protocol unavailable";
                case http_client_errc::io_error:
                    return "HTTP I/O failed";
                case http_client_errc::protocol_error:
                    return "HTTP protocol error";
                case http_client_errc::response_too_large:
                    return "HTTP response too large";
                case http_client_errc::queue_full:
                    return "HTTP request queue full";
                case http_client_errc::closing:
                    return "HTTP client closing";
                case http_client_errc::result_budget_exceeded:
                    return "HTTP result budget exceeded";
                case http_client_errc::handshake_rejected:
                    return "HTTP handshake rejected";
                case http_client_errc::message_too_large:
                    return "WebSocket message too large";
            }
            return "unknown HTTP client error";
        }
    };
    static const category instance;
    return instance;
}

namespace {

constexpr std::size_t response_body_read_chunk_bytes = std::size_t{16} * 1024;

void check_response_operation_affinity(void* target) noexcept {
    const auto& state_value = *static_cast<const detail::http_client_response_state*>(target);
    if (!state_value.memory_domain().worker().is_current()) {
        std::terminate();
    }
}

detail::http_client_response_state& require_response_state(detail::http_client_response_state* state_value) {
    if (state_value == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    return *state_value;
}

detail::http_client_response_state& require_response_body_state(detail::http_client_response_state* state_value) {
    auto& state = require_response_state(state_value);
    check_response_operation_affinity(&state);
    return state;
}

task<void> refresh_client_dns(std::shared_ptr<detail::http_client_pool> pool, operation_options options, stop_token host_stop) {
    stop_group cancellation(host_stop, options.stop_token_);
    options.stop_token_ = cancellation.token();
    co_await pool->refresh_dns(std::move(options));
}

[[nodiscard]] ip_address to_ruvia_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id())).normalized_ipv4();
}

[[nodiscard]] task<std::vector<ip_address>> resolve_dns_owned(worker_ref worker, std::string host, operation_options options, std::shared_ptr<const http_client_address_validator> validator = nullptr, std::uint16_t validator_port = 80) {
    const operation_deadline timeout(options.timeout_);
    if (options.stop_token_.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP DNS resolution cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired()) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP DNS resolution timed out", std::nullopt, nullptr, false);
    }
    detail::client_host_resolver resolver(worker);
    auto pending = resolver.resolve(host, validator_port, timeout, options.stop_token_);
    detail::client_resolve_result result;
    if (auto* immediate = std::get_if<detail::client_resolve_result>(&pending)) {
        result = std::move(*immediate);
    } else {
        result = co_await std::move(std::get<task<detail::client_resolve_result>>(pending));
    }
    if (options.stop_token_.stop_requested() || result.error_ == asio::error::operation_aborted) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP DNS resolution cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired() || result.error_ == asio::error::timed_out) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP DNS resolution timed out", std::nullopt, nullptr, false);
    }
    if (result.error_ == asio::error::no_buffer_space) {
        throw http_client_error(make_error_code(http_client_errc::queue_full), "HTTP DNS resolution blocking pool queue full", std::nullopt, nullptr, false);
    }
    if (result.error_ || (!result.literal_ && result.addresses_.empty())) {
        throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP DNS resolution failed", std::nullopt, nullptr, false);
    }
    std::vector<ip_address> addresses;
    const auto append_endpoint = [&](const asio::ip::tcp::endpoint& endpoint) {
        const auto resolved_ip = to_ruvia_ip_address(endpoint.address());
        if (validator && !(*validator)(resolved_ip, endpoint.port())) {
            throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP dial address rejected", std::nullopt, nullptr, false);
        }
        addresses.push_back(resolved_ip);
    };
    if (result.literal_) {
        append_endpoint(*result.literal_);
    } else {
        addresses.reserve(result.addresses_.size());
        for (const auto& entry : result.addresses_) {
            append_endpoint(entry.endpoint());
        }
    }
    std::ranges::sort(addresses);
    const auto unique_range = std::ranges::unique(addresses);
    addresses.erase(unique_range.begin(), unique_range.end());
    co_return addresses;
}

[[nodiscard]] task<std::vector<ip_address>> resolve_client_handle_dns(std::shared_ptr<detail::http_client_pool> pool, std::string host, operation_options handle_options, operation_options call_options, stop_token host_stop) {
    stop_group cancellation{host_stop, handle_options.stop_token_, call_options.stop_token_};
    std::optional<operation_timeout> effective_timeout = handle_options.timeout_;
    if (call_options.timeout_) {
        if (!effective_timeout || call_options.timeout_->duration() < effective_timeout->duration()) {
            effective_timeout = call_options.timeout_;
        }
    }
    if (!effective_timeout) {
        effective_timeout = operation_timeout(pool->config().connect_timeout_);
    }
    co_return co_await resolve_dns_owned(pool->worker(), std::move(host), {.timeout_ = effective_timeout, .stop_token_ = cancellation.token()}, pool->config().address_validator_, pool->config().dial_port_);
}
}  // namespace

http_client_response::http_client_response(detail::http_client_pool& pool) :
    state_(pool.response_memory().create_state(pool)),
    body_(state_) {
    const auto& config = pool.config();
    state_->effective_connect_timeout_ = config.connect_timeout_;
    state_->effective_read_timeout_ = config.read_timeout_;
    state_->effective_write_timeout_ = config.write_timeout_;
    state_->effective_acquire_timeout_ = config.acquire_timeout_;
    state_->decode_content_ = config.decode_content_;
}

http_client_response::http_client_response(detail::http_client_response_state* state_value) noexcept :
    state_(state_value),
    body_(state_value),
    consumer_(false) {
    if (state_ != nullptr) {
        state_->retain_reference();
    }
}

http_client_response::http_client_response(http_client_response&& other) noexcept :
    state_(std::exchange(other.state_, nullptr)),
    body_(state_),
    consumer_(other.consumer_) {
    other.body_.state_ = nullptr;
}

http_client_response& http_client_response::operator=(http_client_response&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    state_ = std::exchange(other.state_, nullptr);
    consumer_ = other.consumer_;
    body_.state_ = state_;
    other.body_.state_ = nullptr;
    other.consumer_ = false;
    return *this;
}

http_client_response::~http_client_response() {
    release();
}

void http_client_response::release() noexcept {
    if (state_ == nullptr) {
        return;
    }
    check_response_operation_affinity(state_);
    auto* state_value = std::exchange(state_, nullptr);
    body_.state_ = nullptr;
    if (consumer_) {
        state_value->body_operation_scope_.close();
    }
    if (consumer_ && !state_value->complete_ && !state_value->abandoned_ && state_value->pool_ != nullptr) {
        state_value->pool_->abandon_response(*state_value);
    }
    state_value->release_reference();
}

std::span<const http_client_informational_response> http_client_response::informational_responses() const& {
    return require_response_state(state_).informational_;
}

http_status_code http_client_response::status() const {
    return require_response_state(state_).status_.status();
}
http_received_status http_client_response::received_status() const {
    return require_response_state(state_).status_;
}
http_protocol_version http_client_response::protocol_version() const {
    return require_response_state(state_).protocol_version_;
}
http_client_response_framing http_client_response::framing() const {
    return require_response_state(state_).framing_;
}
std::optional<std::uint64_t> http_client_response::content_length() const {
    return require_response_state(state_).content_length_;
}
const http_client_connection_info& http_client_response::connection_info() const& {
    return require_response_state(state_).connection_info_;
}
void http_client_response::reprioritize(http_priority priority) & {
    auto& state_value = require_response_state(state_);
    check_response_operation_affinity(&state_value);
    if (state_value.complete_) {
        // The peer already finished the stream; there is nothing left to schedule.
        return;
    }
    if (state_value.abandoned_ || state_value.pool_ == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::closing), "response transport is retired", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    state_value.pool_->reprioritize(state_value, priority);
}
std::span<const http_header> http_client_response::headers() const& {
    return require_response_state(state_).headers_;
}
std::span<const http_header> http_client_response::trailers() const& {
    return require_response_state(state_).trailers_;
}

void detail::http_client_response_state::retain_reference() noexcept {
    check_response_operation_affinity(this);
    if (references_ == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references_;
}

void detail::http_client_response_state::release_reference() noexcept {
    check_response_operation_affinity(this);
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        memory_domain_.destroy_state(this);
    }
}

void detail::http_client_response_state::notify_producer_space() noexcept {
    space_signal_.notify();
}

void detail::http_client_response_state::rethrow_response_failure() const {
    if (failure_) {
        try {
            std::rethrow_exception(failure_);
        } catch (const http_client_error& error) {
            if (error.connection_info() == nullptr && connection_info_.id_ != 0) {
                throw http_client_error(error.code(), error.what(), error.status(), &connection_info_, request_written_);
            }
            throw;
        }
    }
}

task<void> detail::http_client_response_state::dispatch_informational(const operation_deadline& timeout) {
    while (informational_delivered_ < informational_.size()) {
        const auto index = informational_delivered_++;
        auto callback = informational_callback_;
        if (!callback || !*callback) {
            continue;
        }
        if (!informational_abort_) {
            informational_abort_.emplace();
        }
        if (informational_stop_requested_) {
            informational_abort_->request_stop();
        }
        stop_source deadline_stop;
        stop_group cancellation{informational_stop_, informational_abort_->token(), deadline_stop.token()};
        worker_timer_registration timer;
        if (const auto deadline = timeout.deadline()) {
            (void)memory_domain_.worker().schedule_timer(timer, *deadline, [&deadline_stop](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    deadline_stop.request_stop();
                }
            });
        }
        if (timeout.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "informational callback deadline expired");
        }
        if (cancellation.token().stop_requested()) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "informational callback cancelled");
        }
        // 每头字段独立拥有；外层 vector 的移动不改变字段 span 的地址。
        co_await (*callback)(informational_[index].status(), informational_[index].headers(), cancellation.token());
        if (timeout.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "informational callback deadline expired");
        }
        if (cancellation.token().stop_requested()) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "informational callback cancelled");
        }
    }
}

task<void> detail::http_client_response_state::await_head(std::string_view head_message) {
    while ((!head_ready_ || informational_active_) && !failure_ && !pool_closed_) {
        throw_if_read_interrupted();
        co_await head_signal_.wait();
    }
    rethrow_response_failure();
    if (pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), head_message, std::nullopt, connection_info_.id_ != 0 ? &connection_info_ : nullptr);
    }
    if ((tunnel_ && tunnel_->accepted_) || !body_decode_required_) {
        co_return;
    }
    if (pool_ != nullptr) {
        pool_->release_response_data(*this);
    }
    notify_producer_space();
    while (!complete_) {
        throw_if_read_interrupted();
        co_await data_signal_.wait();
    }
    rethrow_response_failure();
    if (pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client encoded response failed", std::nullopt, connection_info_.id_ != 0 ? &connection_info_ : nullptr);
    }
}

namespace {
[[noreturn]] void throw_interrupted(const detail::http_client_response_state& state_value, detail::operation_interrupt_cause cause, std::string_view timed_out, std::string_view cancelled) {
    const bool timeout = cause == detail::operation_interrupt_cause::timed_out;
    throw http_client_error(timeout ? std::make_error_code(std::errc::timed_out) : std::make_error_code(std::errc::operation_canceled), timeout ? timed_out : cancelled, std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
}
}  // namespace

void detail::http_client_response_state::throw_if_read_interrupted() const {
    if (read_interrupted()) {
        throw_interrupted(*this, *read_operation_->cause(), "HTTP client read timed out", "HTTP client read cancelled");
    }
}

operation_options detail::http_client_response_state::downstream_step(const operation_steps& steps) const {
    auto step = steps.try_next();
    if (!step.has_value()) {
        throw_interrupted(*this, operation_interrupt_cause::timed_out, "HTTP client read timed out", "HTTP client read cancelled");
    }
    return std::move(*step);
}

void detail::http_client_response_state::rethrow_downstream_failure(const operation_steps& steps) const {
    try {
        throw;
    } catch (const std::system_error& error) {
        if (steps.interrupted(error)) {
            throw_interrupted(*this, error.code() == std::errc::timed_out ? operation_interrupt_cause::timed_out : operation_interrupt_cause::cancelled, "HTTP client read timed out", "HTTP client read cancelled");
        }
        throw;
    }
}

void detail::http_client_response_state::throw_if_output_interrupted() const {
    if (output_operation_ != nullptr && output_operation_->interrupted()) {
        throw_interrupted(*this, *output_operation_->cause(), "HTTP client write timed out", "HTTP client write cancelled");
    }
}

detail::http_client_operation_interruption::http_client_operation_interruption(http_client_response_state& state, http_client_operation_lane lane, const operation_options& operation) try :
    state_(state),
    lane_(lane),
    interrupt_(state.interrupt_source_, state.memory_domain().worker(), operation, &state, lane == http_client_operation_lane::read ? &wake_reader : &abort_output, lane == http_client_operation_lane::read ? "HTTP client read" : "HTTP client write") {
    (lane == http_client_operation_lane::read ? state.read_operation_ : state.output_operation_) = &interrupt_;
} catch (const std::system_error&) {
    // The only system_error at construction is an already-stopped token.
    throw_interrupted(state, operation_interrupt_cause::cancelled, {}, "HTTP client operation cancelled");
}

detail::http_client_operation_interruption::~http_client_operation_interruption() {
    (lane_ == http_client_operation_lane::read ? state_.read_operation_ : state_.output_operation_) = nullptr;
}

void detail::http_client_operation_interruption::wake_reader(void* state) noexcept {
    auto& state_value = *static_cast<http_client_response_state*>(state);
    state_value.head_signal_.notify();
    state_value.data_signal_.notify();
}

void detail::http_client_operation_interruption::abort_output(void* state) noexcept {
    auto& state_value = *static_cast<http_client_response_state*>(state);
    if (auto* queue = state_value.output()) {
        queue->stop();
    }
    if (state_value.pool_ != nullptr && !state_value.complete_ && !state_value.abandoned_) {
        state_value.pool_->abandon_response(state_value);
    }
}

void detail::http_client_response_state::discard_response_body() noexcept {
    {
        std::string empty_buffered;
        std::string empty_pending;
        buffered_.swap(empty_buffered);
        pending_.swap(empty_pending);
    }
    offset_ = 0;
    notify_producer_space();
}

void detail::http_client_response_state::discard_failed_encoded_response() noexcept {
    if (!complete_ && body_decode_required_) {
        // Encoded bytes are withheld until the complete representation decodes.
        content_coding_.reset();
        discard_response_body();
    }
}

void detail::http_client_response_state::release_consumed_body_prefix() {
    if (offset_ > buffered_.size()) {
        std::terminate();
    }
    // A partly consumed buffer keeps its prefix behind the cursor: compacting
    // it on every chunk would move the unread tail once per read. Producers
    // only append to pending_, so the retained prefix never limits them.
    if (offset_ == 0 || offset_ != buffered_.size()) {
        return;
    }
    http_clear_string_retaining_small(buffered_, response_body_read_chunk_bytes);
    offset_ = 0;
    if (pending_.empty() && buffered_.capacity() > pending_.capacity()) {
        pending_.swap(buffered_);
    }
    notify_producer_space();
}

void detail::http_client_response_state::promote_pending_data() {
    auto& state_value = *this;
    if (state_value.offset_ != state_value.buffered_.size() || state_value.pending_.empty()) {
        return;
    }
    if (state_value.pool_ != nullptr) {
        state_value.pool_->release_response_data(state_value);
    }
    http_clear_string_retaining_small(state_value.buffered_, response_body_read_chunk_bytes);
    state_value.offset_ = 0;
    state_value.buffered_.swap(state_value.pending_);
    state_value.notify_producer_space();
}

bool http_client_response_body::complete() const {
    if (state_ == nullptr) {
        return true;
    }
    if (!state_->receive_complete() || state_->offset_ != state_->buffered_.size() || !state_->pending_.empty()) {
        return false;
    }
    state_->rethrow_response_failure();
    if (state_->pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP response body read failed", std::nullopt, state_->connection_info_.id_ != 0 ? &state_->connection_info_ : nullptr);
    }
    return true;
}

task<std::optional<std::span<const std::byte>>> http_client_response_body::read(operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(state_value.body_operation_scope_, state_value.consume_body<std::span<const std::byte>>(std::move(operation)));
}

task<std::optional<std::string_view>> http_client_response_body::text(operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(state_value.body_operation_scope_, state_value.consume_body<std::string_view>(std::move(operation)));
}

template <typename view>
task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> detail::http_client_response_state::consume_body(operation_options operation, response_stream_writer* output) {
    auto& state_value = *this;
    auto activity = claim_operation_lane(state_value.body_operation_active_, "HTTP client response body operation is already active");
    [[maybe_unused]] const operation_steps steps(operation);
    const http_client_operation_interruption interruption(state_value, http_client_operation_lane::read, operation);
    for (;;) {
        // A forward is bounded as a whole: its bound also ends it between
        // chunks that are already buffered.
        if constexpr (std::is_void_v<view>) {
            state_value.throw_if_read_interrupted();
        }
        // Reclaim a returned borrow only at the next consumption step. A pipe
        // commits its cursor only after the downstream write succeeds.
        state_value.release_consumed_body_prefix();
        while ((state_value.body_decode_required_ && !state_value.receive_complete()) || (state_value.buffered_.empty() && state_value.pending_.empty() && !state_value.receive_complete())) {
            state_value.throw_if_read_interrupted();
            co_await state_value.data_signal_.wait();
        }
        promote_pending_data();
        if (state_value.offset_ == state_value.buffered_.size()) {
            state_value.rethrow_response_failure();
            if (state_value.pool_closed_) {
                constexpr auto message = std::is_void_v<view> ? "HTTP response body forwarding failed" : "HTTP response body read failed";
                throw http_client_error(make_error_code(http_client_errc::closing), message, std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
            }
            state_value.discard_response_body();
            if constexpr (std::is_void_v<view>) {
                co_return;
            } else {
                co_return std::nullopt;
            }
        }
        const auto count = std::min(response_body_read_chunk_bytes, state_value.buffered_.size() - state_value.offset_);
        const auto chunk = std::string_view(state_value.buffered_).substr(state_value.offset_, count);
        if constexpr (std::is_void_v<view>) {
            try {
                co_await output->write(std::as_bytes(std::span(chunk.data(), chunk.size())), state_value.downstream_step(steps));
            } catch (...) {
                state_value.rethrow_downstream_failure(steps);
            }
            state_value.offset_ += count;
        } else {
            state_value.offset_ += count;
            if constexpr (std::same_as<view, std::string_view>) {
                co_return chunk;
            } else {
                co_return std::as_bytes(std::span(chunk.data(), chunk.size()));
            }
        }
    }
}

template task<std::optional<std::span<const std::byte>>> detail::http_client_response_state::consume_body<std::span<const std::byte>>(operation_options, response_stream_writer*);
template task<std::optional<std::string_view>> detail::http_client_response_state::consume_body<std::string_view>(operation_options, response_stream_writer*);
template task<void> detail::http_client_response_state::consume_body<void>(operation_options, response_stream_writer*);

task<http_client_response_bytes> http_client_response_body::read_all(std::size_t max_bytes, operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(state_value.body_operation_scope_, state_value.read_all(max_bytes, std::move(operation)));
}

task<http_client_response_bytes> detail::http_client_response_state::read_all(std::size_t max_bytes, operation_options operation) {
    auto& state_value = *this;
    auto activity = claim_operation_lane(state_value.body_operation_active_, "HTTP client response body operation is already active");
    const http_client_operation_interruption interruption(state_value, http_client_operation_lane::read, operation);
    state_value.release_consumed_body_prefix();
    state_value.collect_all_ = true;
    if (state_value.pool_ != nullptr) {
        state_value.pool_->release_response_data(state_value);
    }
    state_value.notify_producer_space();
    while (!state_value.receive_complete()) {
        if (state_value.read_interrupted()) {
            // The collected prefix stays buffered for the next body operation,
            // which restores ordinary backpressure.
            state_value.collect_all_ = false;
            state_value.throw_if_read_interrupted();
        }
        co_await state_value.data_signal_.wait();
    }
    state_value.rethrow_response_failure();
    if (state_value.pool_closed_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP response body read failed", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    const auto remaining = state_value.buffered_.size() - state_value.offset_;
    const auto effective_limit = std::min(max_bytes, state_value.buffered_limit_);
    if (state_value.pending_.size() > effective_limit || remaining > effective_limit - state_value.pending_.size()) {
        throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP response body exceeds read_all byte limit", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    const auto total_remaining = remaining + state_value.pending_.size();
    if (total_remaining == 0) {
        state_value.discard_response_body();
        co_return http_client_response_bytes{};
    }
    auto reservation = detail::http_client_result_budget_lease::try_acquire(state_value.memory_domain_.result_budget_domain_, total_remaining);
    if (!reservation) {
        throw http_client_error(make_error_code(http_client_errc::result_budget_exceeded), "HTTP client retained result byte budget is exhausted", std::nullopt, state_value.connection_info_.id_ != 0 ? &state_value.connection_info_ : nullptr, state_value.request_written_);
    }
    http_client_response_bytes result_value(total_remaining, std::move(*reservation));
    if (remaining != 0) {
        result_value.append(std::as_bytes(std::span(state_value.buffered_).subspan(state_value.offset_)));
    }
    if (!state_value.pending_.empty()) {
        result_value.append(std::as_bytes(std::span(state_value.pending_)));
    }
    // The result uses independent thread-safe storage. Only release the
    // worker-owned response buffers after the complete copy succeeds.
    state_value.discard_response_body();
    co_return std::move(result_value);
}

task<void> http_client_response_body::pipe_to(response_stream_writer& output, operation_options operation) & {
    auto& state_value = require_response_body_state(state_);
    return ::ruvia::make_scoped_operation(state_value.body_operation_scope_, state_value.consume_body<void>(std::move(operation), &output));
}

std::optional<std::string_view> http_client_response::header(std::string_view name) const& {
    const auto& headers_value = require_response_state(state_).headers_;
    const auto match = std::ranges::find_if(headers_value, [name](const auto& header_value) {
        return http_ascii_equals_ignore_case(header_value.name(), name);
    });
    return match == headers_value.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

std::optional<std::string_view> http_client_response::trailer(std::string_view name) const& {
    const auto& trailers_value = require_response_state(state_).trailers_;
    const auto match = std::ranges::find_if(trailers_value, [name](const auto& header_value) {
        return http_ascii_equals_ignore_case(header_value.name(), name);
    });
    return match == trailers_value.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

http_client_ref::http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope) noexcept :
    pool_(&pool),
    registration_(scope, this, &http_client_ref::expire_capability) {}

http_client_ref::http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope, operation_options options) noexcept :
    pool_(&pool),
    options_(std::move(options)),
    registration_(scope, this, &http_client_ref::expire_capability) {}

http_client_ref::http_client_ref(const http_client_ref& other) :
    pool_(other.pool_),
    pool_owner_(other.pool_owner_),
    options_(other.options_),
    host_stop_token_(other.host_stop_token_),
    registration_(other.registration_, this),
    registry_(other.registry_) {}

void http_client_ref::expire_capability(void* target) noexcept {
    auto& self = *static_cast<http_client_ref*>(target);
    self.pool_ = nullptr;
    self.pool_owner_.reset();
}

http_client_ref http_client_ref::with_options(operation_options options) const {
    registration_.require_active();
    http_client_ref copy(*this);
    copy.options_ = override_operation_options(options_, std::move(options));
    return copy;
}

http_client_ref http_client_ref::bind_caller_scope(::ruvia::operation_scope& scope, const stop_token& host_stop) && noexcept {
    http_client_ref bound(*pool_, scope, std::move(options_));
    bound.pool_owner_ = std::move(pool_owner_);
    bound.registry_ = registry_;
    bound.host_stop_token_ = host_stop;
    return bound;
}

task<http_client_response> http_client_ref::dispatch(const http_client_request_view& view, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    detail::http_client_request_storage request(view.method_.view(), view.target_.view(), view.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_host_stop(host_stop_token_);
    if (const auto* bytes = view.content_.borrowed_bytes()) {
        request.set_body(bytes->value());
    }
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->execute(std::move(request), options_));
}

task<http_client_exchange> http_client_ref::dispatch_upload(const http_client_request_view& head, http_client_upload_config upload, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (head.content_.borrowed_bytes() != nullptr || upload.max_chunk_bytes_ == 0 || upload.continue_timeout_.value() <= std::chrono::milliseconds::zero() || (upload.expectation_ != http_client_request_expectation::none && upload.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    detail::http_client_request_storage request(head.method_.view(), head.target_.view(), head.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_request(std::move(request), upload, options_));
}

task<http_client_tunnel_result> http_client_ref::dispatch_udp(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (!is_valid_http_origin_form_target(head.target_) || config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes) {
        throw std::invalid_argument("invalid CONNECT-UDP request or tunnel policy");
    }
    for (const auto& field : head.headers_) {
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            throw std::invalid_argument("CONNECT-UDP Capsule-Protocol is driver-owned");
        }
    }
    detail::inline_http_header_views<16> fields(head.headers_.size() + 1U);
    for (const auto& field : head.headers_) {
        fields.push_back(field);
    }
    fields.push_back(http_header_view{"Capsule-Protocol", "?1"});
    // The dispatch owns fields before temporary preparation storage dies.
    const std::string_view authority = request_options.authority_ ? std::string_view(*request_options.authority_) : std::string_view(pool_->config().authority_);
    return dispatch_connect({.authority_ = authority, .protocol_ = "connect-udp", .target_ = head.target_, .headers_ = fields.view()}, config, request_options, informational);
}

task<http_client_tunnel_result> http_client_ref::dispatch_connect(const http_client_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes || (head.protocol_.empty() ? (!is_valid_http_connect_authority(head.authority_) || !head.target_.empty()) : (!is_valid_http_method_token(head.protocol_) || !is_valid_http_origin_form_target(head.target_) || !parse_http_authority(head.authority_)))) {
        throw std::invalid_argument("invalid CONNECT request or tunnel policy");
    }
    if (head.protocol_.view() == "connect-udp" && validate_http_connect_udp_request_to_send({.version_ = http_protocol_version::http2, .method_ = "CONNECT", .scheme_ = scheme() == http_scheme::https ? "https" : "http", .authority_ = head.authority_, .path_ = head.target_, .headers_ = head.headers_}).has_value()) {
        throw std::invalid_argument("invalid CONNECT-UDP request head");
    }
    detail::http_client_request_storage request("CONNECT", head.target_.view(), head.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_tunnel(head.authority_.view(), head.protocol_.view());
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_tunnel(std::move(request), config, options_));
}

http_client_stats http_client_ref::stats() const {
    registration_.require_active();
    return pool_->stats();
}

std::vector<http_client_endpoint_address> http_client_ref::dns_snapshot() const {
    registration_.require_active();
    return pool_->dns_snapshot();
}
void http_client_ref::update_dns(std::span<const http_client_endpoint_address> addresses) const {
    registration_.require_active();
    pool_->update_dns(addresses);
}
task<void> http_client_ref::refresh_dns() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), refresh_client_dns(pool_owner_, options_, host_stop_token_));
}
task<std::vector<ip_address>> http_client_ref::resolve_dns(operation_options options) const {
    registration_.require_active();
    if (pool_->config().dial_host_.empty()) {
        throw std::logic_error("HTTP client alias without a fixed origin or dial host has no default DNS target");
    }
    return resolve_dns(pool_->config().dial_host_, std::move(options));
}

task<std::vector<ip_address>> http_client_ref::resolve_dns(std::string_view host, operation_options options) const {
    registration_.require_active();
    if (host.empty() || host.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("DNS host must not be empty or contain NUL");
    }
    detail::require_client_host_lookup(host, pool_->worker());
    return make_scoped_operation(registration_.scope(), resolve_client_handle_dns(pool_owner_, std::string(host), options_, std::move(options), host_stop_token_));
}
bool http_client_ref::close_connection(std::uint64_t id) const {
    registration_.require_active();
    return pool_->close_connection(id);
}
std::optional<http_client_push> http_client_ref::next_push() const {
    registration_.require_active();
    return pool_->next_push();
}

std::optional<http_client_advertisement> http_client_ref::next_advertisement() const {
    registration_.require_active();
    return pool_->next_advertisement();
}

std::string_view http_client_ref::host() const& {
    registration_.require_active();
    return pool_->host();
}

std::uint16_t http_client_ref::port() const {
    registration_.require_active();
    return pool_->port();
}

http_scheme http_client_ref::scheme() const {
    registration_.require_active();
    return pool_->scheme();
}

http_origin_view http_client_ref::origin() const& {
    registration_.require_active();
    return pool_->origin();
}

http_client_ref context::client() const {
    return services().client_registries().client(operation_scope_, capabilities_.stop_token());
}

http_client_ref context::client(std::string_view alias) const {
    return services().client_registries().client(alias, operation_scope_, capabilities_.stop_token());
}

task<std::vector<ip_address>> resolve_dns(const worker_ref& worker, std::string_view host, operation_options options) {
    if (!worker.valid()) {
        throw std::invalid_argument("DNS resolution requires a valid worker handle");
    }
    if (host.empty() || host.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("DNS host must not be empty or contain NUL");
    }
    detail::require_client_host_lookup(host, worker);
    return resolve_dns_owned(worker, std::string(host), std::move(options));
}

task<std::vector<ip_address>> resolve_dns(event_loop loop, std::string_view host, operation_options options) {
    return resolve_dns(loop.worker(), host, std::move(options));
}
}  // namespace ruvia

namespace ruvia::detail {
namespace {

// The largest request head the client sends: the header limit plus the
// request line and generated fields.
constexpr std::size_t max_client_request_head_bytes = max_http_header_bytes + 1024;
constexpr std::size_t response_read_chunk_bytes = 16384;

// Prepares one HTTP/1 request head (`prepare(std::span<char>)`) into the
// uninitialized storage of `buffer`, first at its retained capacity. A larger
// head asks for exactly its required size once, up to `max_head_bytes`; the
// caller reports the remaining too-small result.
template <typename prepare_type>
[[nodiscard]] std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> prepare_http1_request_head(http_io_buffer& buffer, std::size_t max_head_bytes, const prepare_type& prepare) {
    auto first = prepare(buffer.prepare((std::max)(buffer.capacity(), http_retained_string_bytes)));
    const auto* too_small = std::get_if<http1_client_request_buffer_too_small>(&first);
    if (too_small == nullptr || too_small->required_head_bytes() > max_head_bytes) {
        return first;
    }
    return prepare(buffer.prepare(too_small->required_head_bytes()));
}

// The returned plan borrows only the request body and the output buffer, not
// the temporary header descriptors or the assembled Cookie field.
[[nodiscard]] auto prepare_client_head(http_client_pool& pool, const http_client_request_storage& request, const http_client_upload_state* upload, http_io_buffer& write_buffer) {
    client_prepared_request_scratch scratch;
    auto source = pool.policy().prepare_request(request, scratch);
    const auto headers = static_cast<std::span<const http_header_view>>(source.headers_);
    // Host comes from the request route (its authority override, else the alias).
    auto origin = request.route().origin_;
    const bool connect_udp = request.udp_tunnel();
    if (connect_udp) {
        const auto authority = parse_http_authority(borrowed_text(request.tunnel_authority()));
        if (!authority) {
            throw http_client_error(make_error_code(http_client_errc::invalid_request), "invalid CONNECT-UDP authority");
        }
        const http_origin_options options{.host_ = authority->host(), .port_ = authority->port()};
        origin = pool.config().scheme_ == http_scheme::https ? http_origin_view::https(options) : http_origin_view::http(options);
    }
    http1_client_request_writer writer;
    const auto prepare = [&](std::span<char> buffer) {
        if (const auto* websocket = request.websocket()) {
            return prepare_http1_websocket_client_request(origin, source.target_.view(), websocket->key_, websocket->negotiation_, headers, buffer, request.options_.field_source_);
        }
        if (connect_udp) {
            return writer.prepare_connect_udp(origin, source.target_, headers, buffer, {.field_source_ = request.options_.field_source_});
        }
        if (request.is_tunnel() && !request.is_upgrade()) {
            return writer.prepare_connect(borrowed_text(request.tunnel_authority()), headers, buffer, {.field_source_ = request.options_.field_source_});
        }
        if (upload != nullptr) {
            return writer.prepare_streaming(origin, {.method_ = source.method_, .target_ = source.target_, .headers_ = headers, .content_length_ = upload->config_.content_length_}, buffer, {.expectation_ = upload->config_.expectation_, .field_source_ = request.options_.field_source_});
        }
        return writer.prepare(origin, source, buffer, {.field_source_ = request.options_.field_source_});
    };
    return prepare_http1_request_head(write_buffer, max_client_request_head_bytes, prepare);
}

// One contiguous write() part. A temporary argument outlives the write: it is
// destroyed only at the end of the full-expression that awaits it.
[[nodiscard]] std::array<asio::const_buffer, 1> single_part(std::string_view bytes) noexcept {
    return {asio::buffer(bytes)};
}

[[nodiscard]] std::span<const asio::const_buffer> coalesce_tls_write_parts(bool secure, http_io_buffer& write_buffer, std::span<asio::const_buffer> parts) {
    std::size_t non_empty = 0;
    std::size_t total_bytes = 0;
    for (const auto& part : parts) {
        if (part.size() == 0) {
            continue;
        }
        ++non_empty;
        if (part.size() > http_retained_string_bytes - total_bytes) {
            return parts;
        }
        total_bytes += part.size();
    }
    if (!secure || non_empty <= 1) {
        return parts;
    }
    write_buffer.clear();
    const auto first = parts.front();
    const auto prepared = write_buffer.prepare(total_bytes);
    if (static_cast<const char*>(first.data()) == prepared.data()) {
        write_buffer.commit(first.size());
    } else if (first.size() != 0) {
        std::memcpy(prepared.data(), first.data(), first.size());
        write_buffer.commit(first.size());
    }
    for (const auto& part : parts.subspan(1)) {
        if (part.size() != 0) {
            write_buffer.append(std::string_view(static_cast<const char*>(part.data()), part.size()));
        }
    }
    parts[0] = asio::buffer(write_buffer.view());
    return parts.first(1);
}

[[noreturn]] void throw_body_timeout() {
    throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP/1 response body decoding timed out");
}

// The parser validates the exact offered protocols and Connection options;
// every accepted Upgrade uses the same transparent byte tunnel as CONNECT.
[[nodiscard]] bool opens_tunnel(const http_client_request_storage& request, const http1_parsed_client_response_head& head) noexcept {
    return request.is_tunnel() && (head.plan().connect_tunnel() != nullptr || ((request.is_upgrade() || request.udp_tunnel() || request.websocket() != nullptr) && head.plan().protocol_upgrade() != nullptr));
}

// Publishes one decoder step's body bytes or trailers to the consumer, or
// throws its failure.
void publish_body_progress(http_client_response_state& state_value, const http1_client_response_body_decoder::result_type& decoded, std::size_t max_response_bytes) {
    if (const auto* body = decoded.output()) {
        if (!state_value.append_received_body(body->bytes(), max_response_bytes)) {
            throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP response exceeds configured byte limit");
        }
    } else if (const auto* trailers = decoded.trailers()) {
        const auto fields = trailers->fields();
        state_value.trailers_.reserve(state_value.trailers_.size() + fields.size());
        for (const auto& field : fields) {
            state_value.trailers_.emplace_back(field.name(), field.value());
        }
    } else if (const auto* failure = decoded.protocol_failure()) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), http1_client_response_body_error_message(failure->error()));
    } else if (decoded.decoder_failure() != nullptr) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP/1 response body decoder failed");
    }
}

}  // namespace

task<void> http1_client_exchange::write(std::span<const asio::const_buffer> parts, const operation_deadline& timeout) {
    response_->request_written_ = true;
    const auto written = co_await connection_.transport_.write(parts, timeout.constrained_by(response_->effective_write_timeout_));
    if (!written.ok()) {
        throw_http_client_transport_error(written, "upstream closed while receiving the HTTP request");
    }
}

void http1_client_exchange::restart_inactivity() noexcept {
    inactivity_timer_.cancel();
    if (!inactivity_bound_) {
        return;
    }
    try {
        (void)pool_.worker().schedule_timer(inactivity_timer_, deadline_after(inactivity_bound_->value()), [this, generation = connection_.generation_](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                connection_.cancel(generation, client_abort_reason::timeout);
            }
        });
    } catch (...) {
        // An exchange without its inactivity bound is not continued.
        connection_.cancel(connection_.generation_, client_abort_reason::timeout);
    }
}

void http1_client_exchange::stop_inactivity() noexcept {
    inactivity_bound_.reset();
    inactivity_timer_.cancel();
}

task<client_transport_result> http1_client_exchange::read_append(http_io_buffer& target, std::size_t max_bytes, const operation_deadline& timeout, bool read_inactivity_bound) {
    auto read_space = target.prepare_spare();
    if (read_space.empty()) {
        read_space = target.prepare(max_bytes);
    } else {
        read_space = read_space.first(std::min(max_bytes, read_space.size()));
    }
    // Each read restarts the inactivity bound, inside the exchange deadline.
    auto read = co_await connection_.transport_.read_some(read_space, read_inactivity_bound ? timeout.constrained_by(response_->effective_read_timeout_) : timeout);
    // close() may clear the target while the read is pending; never publish
    // bytes of a closed connection into it.
    if (!connection_.connected_) {
        if (read.ok() || read.status_ == client_transport_status::eof) {
            read.status_ = client_transport_status::io_failed;
        }
        throw_http_client_transport_error(read, "upstream closed the HTTP connection");
    }
    target.commit(read.bytes_);
    if (read.ok() || read.status_ == client_transport_status::eof) {
        co_return read;
    }
    throw_http_client_transport_error(read, "upstream closed the HTTP connection");
}

task<void> http1_client_exchange::run(const http_client_request_storage& request, const operation_deadline& timeout, stop_token stop_token_value, http_client_response_state& state_value) {
    if (!request.is_upgrade() && !request.tunnel_protocol().empty() && !request.udp_tunnel() && request.websocket() == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "Extended CONNECT requires HTTP/2");
    }
    response_ = &state_value;
    auto& connection = connection_;
    connection.end_idle();
    connection.transport_.reset_abort();
    next_nonzero_id(connection.generation_);
    struct exchange_generation final {
        http_client_connection& connection_;
        ~exchange_generation() {
            next_nonzero_id(connection_.generation_);
            connection_.http1_response_ = nullptr;
        }
    } exchange_generation_value{connection};
    http_client_response_cancellation cancellation(pool_.cancellation_target(), connection.cancellation_id_, state_value);
    // Bound for the exchange so a cancellation or shutdown stops the
    // response's output and wakes its producer waits.
    connection.http1_response_ = &state_value;
    state_value.transport_ = http_client_response_transport::http1;
    state_value.request_method_ = classify_http_method(request.method());
    cancellation.arm(stop_token_value);

    auto* const upload = state_value.upload_ ? &*state_value.upload_ : nullptr;
    // The head is written into the connection's retained, uninitialized buffer.
    auto& write_buffer = connection_.write_buffer_;
    write_buffer.clear();
    auto prepared_result = prepare_client_head(pool_, request, upload, write_buffer);
    auto* prepared = std::get_if<prepared_http1_client_request>(&prepared_result);
    if (!prepared) {
        const auto* failure = std::get_if<http1_client_request_prepare_error>(&prepared_result);
        throw http_client_error(make_error_code(http_client_errc::invalid_request), failure != nullptr ? http1_client_request_prepare_error_message(*failure) : std::string_view("HTTP request head is too large"));
    }
    http1_client_response_parser parser(std::move(*prepared).take_exchange_state());
    const auto* immediate = prepared->content_plan().immediate();
    const bool immediate_content = immediate != nullptr && !immediate->bytes().empty();
    if (!immediate_content) {
        // Streaming/continue-gated content sends its head before the upload.
        co_await write(single_part(prepared->head()), timeout);
        write_buffer.clear_retaining_small();
    }
    if (upload == nullptr && !immediate_content) {
        co_await receive(request, timeout, state_value, parser);
        connection_.retain_idle_buffers();
        co_return;
    }
    task_scope writers(pool_.worker());
    std::exception_ptr writer_failure;
    bool immediate_completed = false;
    // An origin may read the whole request before it answers: until the final
    // head, sending progress also restarts the read inactivity bound.
    inactivity_bound_ = state_value.effective_read_timeout_;
    restart_inactivity();
    std::array<asio::const_buffer, 2> parts;
    if (immediate_content) {
        parts = {asio::buffer(prepared->head()), asio::buffer(immediate->bytes())};
        // Preserve one scatter/gather write (and small TLS coalescing) while
        // receiving 1xx concurrently, without copying the owned request body.
        const auto write_parts = coalesce_tls_write_parts(connection_.transport_.secure(), write_buffer, parts);
        writers.spawn(write_immediate(write_parts, timeout, parser, immediate_completed, writer_failure));
    } else {
        writers.spawn(write_upload(state_value, timeout, http1_request_content_writer(*prepared->content_plan().streaming()), parser, writer_failure));
    }
    std::exception_ptr receive_failure;
    try {
        co_await receive(request, timeout, state_value, parser);
    } catch (...) {
        receive_failure = std::current_exception();
    }
    if (receive_failure) {
        if (upload != nullptr) {
            upload->output_.stop();
        }
        connection_.close();
    } else {
        // Response framing can finish before the request content. Publish that
        // boundary independently, but keep the connection leased until the
        // writer finishes the request or is explicitly stopped.
        state_value.complete_ = true;
        state_value.data_signal_.notify();
        if (immediate_content && !immediate_completed) {
            // A complete early response ends the buffered exchange. Never wait
            // for a peer that stopped consuming content, or pool its remainder.
            connection_.close();
        }
    }
    worker_timer_registration upload_completion_timer;
    if (upload != nullptr && !upload->output_.ended_ && !upload->output_.stopped_) {
        if (timeout.expired()) {
            connection_.cancel(connection_.generation_, client_abort_reason::timeout);
        } else if (const auto deadline = timeout.deadline()) {
            (void)pool_.worker().schedule_timer(upload_completion_timer, *deadline, [this, generation = connection_.generation_](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    connection_.cancel(generation, client_abort_reason::timeout);
                }
            });
        }
    }
    co_await writers.join();
    upload_completion_timer.cancel();
    if ((upload != nullptr && !upload->output_.ended_) || (immediate_content && !immediate_completed)) {
        connection_.close();
    }
    connection_.retain_idle_buffers();
    // The receive side decides an ordinary rejected write, including our own
    // early-response close. Preserve its callback/protocol failure as well.
    // Timeout, cancellation, TLS and local write failures still fail both lanes.
    const bool receive_decides_write = immediate_content && writer_failure && http_client_failure_code(writer_failure) == make_error_code(http_client_errc::io_error);
    if (writer_failure && !receive_decides_write) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
    connection_.throw_if_aborted();
}

task<void> http1_client_exchange::receive(const http_client_request_storage& request, const operation_deadline& timeout, http_client_response_state& state_value, http1_client_response_parser& parser) {
    auto& read_buffer = connection_.read_buffer_;
    // The parsed head is move-constructible only, so it is emplaced once.
    std::optional<http1_parsed_client_response_head> head;
    while (!head) {
        connection_.throw_if_aborted();
        if (timeout.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP response head timed out");
        }
        auto parse_result = parser.parse(read_buffer.view());
        if (parse_result.failure()) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error), http1_client_response_parse_error_message(*parse_result.failure()));
        }
        if (parse_result.need_more()) {
            if (read_buffer.size() >= max_http_header_bytes) {
                throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP response head is too large");
            }
            if (!(co_await read_append(read_buffer, response_read_chunk_bytes, timeout, !inactivity_bound_)).ok()) {
                throw http_client_error(make_error_code(http_client_errc::io_error), "upstream closed before the HTTP response head");
            }
            restart_inactivity();
            continue;
        }
        auto* parsed = parse_result.parsed();
        if (parsed == nullptr) {
            std::terminate();
        }
        if (parsed->plan().informational() == nullptr) {
            head.emplace(std::move(*parsed));
            break;
        }
        auto informational = std::move(*parsed).take_head();
        state_value.retain_informational(informational.status(), std::move(informational).take_headers(), parsed->plan().request_content_signal());
        read_buffer.consume(parsed->consumed_bytes());
        // No further transport input or parsing while the consumer is suspended.
        if (state_value.informational_callback_) {
            co_await state_value.dispatch_informational(timeout);
        }
    }
    if (auto* websocket = request.websocket()) {
        const auto negotiated = validate_http1_websocket_client_response(websocket->key_, websocket->negotiation_, *head);
        const auto* selected = std::get_if<websocket_client_negotiation_result_view>(&negotiated);
        if (selected == nullptr) {
            throw http_client_error(make_error_code(http_client_errc::handshake_rejected), "invalid HTTP/1 WebSocket handshake response", head->head().status());
        }
        websocket->selected_subprotocol_ = selected->selected_subprotocol_;
        websocket->compression_ = selected->compression_;
    }
    const auto& plan = head->plan();
    // The parser's message-length precedence is authoritative, including HEAD,
    // 205 framing, ignored HTTP/1.0 Transfer-Encoding and accepted tunnels.
    state_value.content_length_ = head->head().content_length();
    if (plan.known_length() != nullptr) {
        state_value.framing_ = http_client_response_framing::fixed_length;
    } else if (plan.chunked() != nullptr) {
        state_value.framing_ = http_client_response_framing::chunked;
    } else if (plan.close_delimited() != nullptr) {
        state_value.framing_ = http_client_response_framing::close_delimited;
    } else if (plan.connect_tunnel() != nullptr) {
        state_value.framing_ = http_client_response_framing::tunnel;
    } else if (plan.protocol_upgrade() != nullptr) {
        state_value.framing_ = http_client_response_framing::protocol_upgrade;
    } else {
        state_value.framing_ = http_client_response_framing::none;
    }
    stop_inactivity();
    const bool tunnel_opened = opens_tunnel(request, *head);
    if (!tunnel_opened && (plan.connect_tunnel() != nullptr || plan.protocol_upgrade() != nullptr)) {
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "HTTP tunnel and protocol upgrade responses require a dedicated API");
    }
    const http_client_final_head final_head{.status_ = head->head().received_status(), .protocol_version_ = head->head().protocol_version(), .request_content_signal_ = plan.request_content_signal(), .origin_target_ = pool_.policy().addresses_origin(request) ? std::optional(request.target()) : std::nullopt, .opens_tunnel_ = tunnel_opened};
    // Parsed fields already use the response resource: transfer, not copy.
    state_value.headers_ = std::move(*head).take_head().take_headers();
    read_buffer.consume(head->consumed_bytes());
    publish_http_client_final_head(state_value, pool_.policy(), final_head);
    if (tunnel_opened) {
        // The HTTP request deadline ends at a WebSocket's accepted Upgrade.
        // Its subsequent operations carry their own deadlines.
        const auto tunnel_timeout = request.websocket() ? operation_deadline() : timeout;
        co_await run_tunnel(state_value, tunnel_timeout, request.websocket() != nullptr);
        co_return;
    }
    http1_client_response_body_decoder body_decoder(plan);
    const bool content_semantics_present = plan.without_content() == nullptr;

    // The body loop stays in this frame: one exchange costs one receive frame.
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    // Empty framing still runs the decoder and boundary checks, but its
    // mandatory nonempty scratch can stay inline. Other responses borrow
    // uninitialized worker storage without inflating this coroutine frame.
    const auto* known_length = plan.known_length();
    const bool empty_body = !content_semantics_present || (known_length != nullptr && !known_length->requires_body_consumption());
    const bool needs_transfer_scratch = !empty_body && !plan.discards_content() && ((plan.chunked() != nullptr && !plan.chunked()->transfer_codings().empty()) || (plan.close_delimited() != nullptr && !plan.close_delimited()->transfer_codings().empty()));
    std::array<char, 1> empty_output{};
    http_io_buffer output_storage;
    const auto transfer_output = needs_transfer_scratch ? output_storage.prepare(http_body_buffer_bytes) : std::span<char>{};
    bool eof = false;
    http1_close_policy persistence = http1_close_policy::close_after_response;
    for (;;) {
        // Buffered protocol input may still complete the response without I/O.
        // Drain it within the queue limit before waiting for the consumer, so
        // a complete retained response does not depend on a later body read.
        if (!state_value.collect_all_ && state_value.pending_.size() >= max_response_bytes) {
            co_await wait_for_body_space(state_value, timeout);
        }
        connection_.throw_if_aborted();
        const auto output = needs_transfer_scratch ? transfer_output : (!empty_body && !read_buffer.empty() ? std::span<char>(read_buffer.data(), std::min(read_buffer.size(), http_body_buffer_bytes)) : std::span<char>(empty_output));
        const auto capacity = state_value.collect_all_ ? output.size() : std::min(output.size(), max_response_bytes - state_value.pending_.size());
        const auto scratch = output.first(capacity);
        const auto decoded = eof ? body_decoder.finish_input(read_buffer.view(), scratch) : body_decoder.decode(read_buffer.view(), scratch);
        const auto consumed = decoded.consumed_bytes();
        publish_body_progress(state_value, decoded, max_response_bytes);
        // Output/trailers have been owned before their source is compacted.
        read_buffer.consume(consumed);
        if (const auto* complete = decoded.complete()) {
            persistence = complete->persistence();
            break;
        }
        if (decoded.need_input() == nullptr || (consumed != 0 && !read_buffer.empty())) {
            continue;
        }
        if (eof) {
            throw std::logic_error("HTTP body decoder requested input after EOF");
        }
        // Only a new network read spends inactivity budget. Outstanding delivery
        // (including the last borrowed chunk) postpones that read, not decoder
        // completion on bytes already received.
        if (!state_value.collect_all_ && (!state_value.pending_.empty() || !state_value.buffered_.empty())) {
            co_await wait_for_body_space(state_value, timeout);
        }
        const auto read = co_await read_append(read_buffer, response_read_chunk_bytes, timeout);
        // A TLS peer that drops TCP without close_notify leaves the message end
        // unauthenticated: an attacker could cut a close-delimited body short
        // (RFC 9112 §9.8). Every other framing still needed bytes here too.
        if (read.error_ == asio::ssl::error::stream_truncated) {
            throw http_client_error(make_error_code(http_client_errc::io_error), "upstream closed the TLS connection without close_notify");
        }
        eof = !read.ok();
    }
    // Bytes beyond this response's framing cannot belong to a response the
    // client requested. Preserve the complete framed response, but retire the
    // transport so those bytes are discarded rather than parsed as a future
    // response. A rejected CONNECT likewise leaves subsequent ownership
    // unknown (RFC 9112 §9.6).
    const bool reusable = persistence == http1_close_policy::allow_reuse && (!request.is_tunnel() || request.is_upgrade()) && read_buffer.empty();
    finish_http_client_response_body(state_value, timeout, content_semantics_present, max_response_bytes);
    if (!reusable) {
        connection_.close();
    }
}

task<void> http1_client_exchange::wait_for_body_space(http_client_response_state& state_value, const operation_deadline& timeout) {
    while (!state_value.collect_all_ && (!state_value.pending_.empty() || !state_value.buffered_.empty())) {
        connection_.throw_if_aborted();
        const auto deadline = timeout.deadline();
        if (deadline && *deadline <= std::chrono::steady_clock::now()) {
            throw_body_timeout();
        }
        bool expired = false;
        worker_timer_registration timer;
        if (deadline) {
            (void)pool_.worker().schedule_timer(timer, *deadline, [&expired, &state_value](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    expired = true;
                    state_value.space_signal_.notify();
                }
            });
        }
        co_await state_value.space_signal_.wait();
        timer.cancel();
        connection_.throw_if_aborted();
        if (expired || timeout.expired()) {
            throw_body_timeout();
        }
        if (state_value.abandoned_) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP response body was abandoned");
        }
    }
}

void http1_client_exchange::expire_tunnel() noexcept {
    connection_.transport_.abort(client_abort_reason::timeout);
    connection_.abort_http1_response();
    connection_.close();
}

task<void> http1_client_exchange::write_tunnel_output(http_client_output_queue& output, const operation_deadline& timeout, tls_tunnel_output* tls, std::exception_ptr& failure) {
    const bool secure = connection_.transport_.secure();
    auto& stream = connection_.transport_.stream();
    try {
        std::optional<http_socket_tunnel_transport<asio::ssl::stream<client_tls_socket>>> tls_transport;
        if (tls != nullptr) {
            tls_transport.emplace(stream, tls, nullptr);
        }
        http_socket_tunnel_transport plain_transport(stream.next_layer().socket(), nullptr, nullptr);
        while (!output.ended_ && !output.stopped_) {
            if (!output.chunk_ready_ && !output.end_requested_) {
                co_await output.data_.wait();
                continue;
            }
            const auto write_timeout = timeout.constrained_by(response_->effective_write_timeout_);
            if (write_timeout.expired()) {
                expire_tunnel();
                connection_.throw_if_aborted();
            }
            worker_timer_registration write_timer;
            if (const auto deadline = write_timeout.deadline()) {
                (void)pool_.worker().schedule_timer(write_timer, *deadline, [this](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        expire_tunnel();
                    }
                });
            }
            const auto ending = output.end_requested_ && !output.chunk_ready_ ? http_stream_end::end : http_stream_end::keep_open;
            const auto bytes_value = output.chunk_ready_ ? std::string_view(output.chunk_) : std::string_view{};
            std::size_t completed_bytes = 0;
            const auto error = tls_transport.has_value() ? co_await tls_transport->write_bytes(bytes_value, ending, &completed_bytes) : co_await plain_transport.write_bytes(bytes_value, ending, &completed_bytes);
            pool_.wire_counters().sent_ += completed_bytes;
            write_timer.cancel();
            restart_inactivity();
            connection_.throw_if_aborted();
            if (error) {
                throw http_client_error(secure && is_tls_error(error) ? make_error_code(http_client_errc::tls_failed) : make_error_code(http_client_errc::io_error), error_code_text(error));
            }
            if (ending == http_stream_end::end) {
                output.finish();
            } else {
                output.acknowledge_chunk();
            }
        }
    } catch (...) {
        failure = std::current_exception();
        connection_.close();
        output.stop();
    }
}

task<void> http1_client_exchange::run_tunnel(http_client_response_state& state_value, const operation_deadline& timeout, bool websocket) {
    auto& connection = connection_;
    auto& output = state_value.tunnel_->output_;
    const auto max_response_bytes = pool_.config().max_response_bytes_;
    const bool secure = connection.transport_.secure();
    auto& stream = connection.transport_.stream();
    // Everything that can throw after the TLS writer starts runs inside the
    // receive try below, so the started writer is always joined before `tls`
    // is destroyed; the scope and timer slots are allocated first.
    task_scope writers(pool_.worker());
    worker_timer_registration lifetime_timer;
    std::optional<tls_tunnel_output> tls;
    if (secure) {
        tls.emplace(*stream.native_handle(), stream.next_layer().socket(), pool_.worker());
        tls->start();
    }
    std::exception_ptr writer_failure;
    std::exception_ptr receive_failure;
    try {
        if (timeout.expired()) {
            expire_tunnel();
            connection.throw_if_aborted();
        }
        if (const auto deadline = timeout.deadline()) {
            (void)pool_.worker().schedule_timer(lifetime_timer, *deadline, [this](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    expire_tunnel();
                }
            });
        }
        inactivity_bound_ = websocket ? std::nullopt : state_value.tunnel_->config_.idle_timeout_;
        restart_inactivity();
        writers.spawn(write_tunnel_output(output, timeout, tls ? &*tls : nullptr, writer_failure));
        auto& read_buffer = connection.read_buffer_;
        constexpr std::size_t read_chunk_bytes = 4096;
        for (;;) {
            while (state_value.pending_.size() >= max_response_bytes && !state_value.abandoned_ && connection.connected_ && !writer_failure) {
                co_await state_value.space_signal_.wait();
                connection.throw_if_aborted();
            }
            if (state_value.abandoned_) {
                throw http_client_error(std::make_error_code(std::errc::operation_canceled), "CONNECT tunnel abandoned");
            }
            if (!connection.connected_ || writer_failure) {
                break;
            }
            const auto room = max_response_bytes - state_value.pending_.size();
            if (!read_buffer.empty()) {
                const auto count = std::min(read_buffer.size(), room);
                state_value.pending_.append(read_buffer.data(), count);
                read_buffer.consume(count);
                state_value.data_signal_.notify();
                continue;
            }
            // The consumer may swap pending_ away while a read is in flight, so
            // reads land in the connection buffer and move on the next pass.
            // A WebSocket arms read inactivity only while its application is
            // receiving; the background tunnel still observes the total deadline.
            // read_timeout_ no longer applies; the tunnel's idle timeout counts
            // both directions. A WebSocket arms read inactivity only while its
            // application is receiving.
            const auto read = co_await read_append(read_buffer, std::min(read_chunk_bytes, room), timeout, false);
            restart_inactivity();
            if (!read.ok()) {
                // Same rule as a message body: a TLS peer that drops TCP
                // without close_notify leaves the tunnel end unauthenticated
                // (RFC 8446 section 6.1), so it is a failure, not an orderly end.
                if (read.error_ == asio::ssl::error::stream_truncated) {
                    throw http_client_error(make_error_code(http_client_errc::io_error), "upstream closed the TLS connection without close_notify");
                }
                state_value.tunnel_->receive_ended_ = true;
                state_value.data_signal_.notify();
                break;
            }
        }
    } catch (...) {
        receive_failure = std::current_exception();
        connection.close();
        output.stop();
    }
    co_await writers.join();
    if (tls) {
        co_await tls->join();
    }
    stop_inactivity();
    lifetime_timer.cancel();
    connection.close();
    connection.retain_idle_buffers();
    if (writer_failure) {
        std::rethrow_exception(writer_failure);
    }
    if (receive_failure) {
        std::rethrow_exception(receive_failure);
    }
}

task<void> http1_client_exchange::write_immediate(std::span<const asio::const_buffer> parts, const operation_deadline& timeout, http1_client_response_parser& parser, bool& completed, std::exception_ptr& failure) {
    try {
        co_await write(parts, timeout);
        restart_inactivity();
        completed = true;
        const auto status = parser.complete_request_content();
        if (status == http1_client_request_content_completion_status::already_complete) {
            std::terminate();
        }
        connection_.write_buffer_.clear_retaining_small();
    } catch (...) {
        failure = std::current_exception();
        // A plain write failure can race a valid final response in the other
        // TCP direction; let its framing decide. Other failures stop the reader.
        if (http_client_failure_code(failure) != make_error_code(http_client_errc::io_error)) {
            connection_.close();
        }
    }
}

task<void> http1_client_exchange::write_upload(http_client_response_state& state_value, const operation_deadline& timeout, http1_request_content_writer writer, http1_client_response_parser& parser, std::exception_ptr& failure) {
    auto& upload = *state_value.upload_;
    worker_timer_registration continue_timer;
    if (!upload.content_released_) {
        (void)pool_.worker().schedule_timer(continue_timer, deadline_after(upload.config_.continue_timeout_.value()), [&upload, &parser](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired && !upload.output_.stopped_) {
                parser.release_request_content();
                upload.content_released_ = true;
                upload.output_.notify_data();
            }
        });
    }
    try {
        for (;;) {
            if (upload.output_.stopped_) {
                writer.abort();
                co_return;
            }
            connection_.throw_if_aborted();
            if (timeout.expired()) {
                throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP upload timed out");
            }
            if (!upload.content_released_ || (!upload.output_.chunk_ready_ && !upload.output_.end_requested_)) {
                co_await upload.output_.data_.wait();
                continue;
            }
            writer.release_content();
            continue_timer.cancel();
            if (upload.output_.chunk_ready_) {
                const auto chunk = writer.plan_chunk(std::span<const char>(upload.output_.chunk_.data(), upload.output_.chunk_.size()));
                if (chunk.index() != 0) {
                    throw http_client_error(make_error_code(http_client_errc::invalid_request), "HTTP upload content length mismatch");
                }
                const auto& planned = std::get<0>(chunk);
                // The write lane has its own deadline, so the concurrent
                // response reader keeps its read deadline.
                std::array<asio::const_buffer, 3> parts{asio::buffer(planned.prefix()), asio::buffer(planned.payload()), asio::buffer(planned.suffix())};
                const auto write_parts = coalesce_tls_write_parts(connection_.transport_.secure(), connection_.write_buffer_, parts);
                co_await write(write_parts, timeout);
                restart_inactivity();
                if (write_parts.size() == 1) {
                    connection_.write_buffer_.clear_retaining_small();
                }
                writer.commit_chunk();
                upload.output_.acknowledge_chunk();
                continue;
            }
            auto section_budget = http_trailer_section_budget();
            (void)section_budget.add(http1_last_chunk_prefix.size());
            for (const auto& field : upload.trailers_) {
                (void)section_budget.add_field(field.name(), field.value());
            }
            const inline_http_header_views trailers(upload.trailers_);
            // Keep the last-chunk line and small trailer sections in this
            // operation; larger trailer sections allocate only their exact wire size.
            constexpr std::size_t inline_ending_bytes = 256;
            std::array<char, inline_ending_bytes> inline_ending;
            http_io_buffer scratch;
            const auto required_bytes = section_budget.bytes();
            const auto finish_buffer = required_bytes <= inline_ending.size() ? std::span<char>(inline_ending.data(), required_bytes) : scratch.prepare(required_bytes);
            const auto ending = writer.plan_finish(finish_buffer, trailers.view());
            if (ending.index() != 0) {
                throw http_client_error(make_error_code(http_client_errc::invalid_request), "HTTP upload trailer or final length rejected");
            }
            struct completion_guard final {
                http_client_upload_state& upload_;
                explicit completion_guard(http_client_upload_state& value) :
                    upload_(value) {
                    upload_.output_.completion_pending_ = true;
                }
                ~completion_guard() {
                    upload_.output_.completion_pending_ = false;
                    upload_.output_.space_.notify();
                }
            } completion(upload);
            if (const auto bytes = std::get<0>(ending); !bytes.empty()) {
                co_await write(single_part(bytes), timeout);
            }
            writer.commit_finish();
            const auto completed = parser.complete_request_content();
            if (completed == http1_client_request_content_completion_status::exchange_terminal && !state_value.head_ready_) {
                // The receiver already failed this exchange after the parser
                // left await_response; it owns and reports that failure.
                co_return;
            }
            upload.output_.finish();
            co_return;
        }
    } catch (...) {
        const auto upload_failure = std::current_exception();
        // TCP directions are independent: a peer can reject more request
        // bytes while a complete final response is already on its way. Keep
        // reading after a plain transport write failure; framing decides
        // whether that response is valid. Timeout, cancellation, local
        // request errors and explicit abandonment still terminate both lanes.
        const bool preserve_response = !state_value.abandoned_ && connection_.transport_.abort_reason() == client_abort_reason::none && http_client_failure_code(upload_failure) == make_error_code(http_client_errc::io_error);
        if (preserve_response) {
            upload.failure_ = upload_failure;
            if (!state_value.complete_ && !upload.output_.stopped_) {
                failure = upload_failure;
            }
        } else if (!state_value.complete_ && !upload.output_.stopped_) {
            failure = upload_failure;
            // The caller's pending write() wakes on stop() below and reports
            // the published exchange failure instead of a bare cancellation.
            if (state_value.failure_ == nullptr) {
                state_value.failure_ = failure;
            }
        }
        upload.output_.stop();
        if (!preserve_response) {
            connection_.close();
        }
    }
}

}  // namespace ruvia::detail
