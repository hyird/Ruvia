#include "ruvia/web/websocket_client.h"

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <openssl/rand.h>

#include "ruvia/core/bytes.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/worker_handle.h"

#include "client/websocket_client_state.h"

namespace ruvia::detail {

websocket_client_state::websocket_client_state(event_loop loop, const websocket_client_config& config)
    : loop_(client_lifecycle<websocket_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      config_(config, memory_.resource()),
      tls_context_([&] {
          asio::ssl::context context(asio::ssl::context::tls_client);
          if (config_.scheme_ == websocket_scheme::wss) {
              configure_client_tls_context(*context.native_handle(), config_.tls_);
          }
          return context;
      }()),
      transport_(loop_.io_context(), tls_context_, worker_,
          client_tcp_endpoint{.host_ = config_.host_,
              .port_ = config_.port_,
              .tls_ = config_.scheme_ == websocket_scheme::wss ? &config_.tls_ : nullptr,
              .tcp_ = config_.tcp_,
              .alpn_ = config.protocol_ == websocket_client_protocol::http2 ? client_alpn::http2 : client_alpn::http11},
          wire_counters_),
      write_signal_(worker_),
      input_(memory_.allocator<char>()),
      selected_subprotocol_(memory_.allocator<char>()),
      heartbeat_tasks_(worker_, {.resource_ = memory_.resource()}),
      lifecycle_(*this, loop_, worker_) {}

void websocket_client_state::throw_not_ready() {
    throw websocket_client_error(
        websocket_client_error::code_type::invalid_state, "WebSocket client is not connected");
}

void websocket_client_state::require_current() const {
    if (!worker_.is_current()) {
        throw std::logic_error("WebSocket client must be used on its bound event loop");
    }
}

ruvia::websocket_connection& websocket_client_state::require_protocol() noexcept {
    if (!protocol_.has_value()) {
        std::terminate();
    }
    return protocol_.value();
}

bool websocket_client_state::protocol_open() const noexcept {
    // close_now() aborts the protocol, so only a live connection stays open.
    return protocol_.has_value() && protocol_->liveness_mode() == websocket_liveness_mode::open;
}

bool websocket_client_state::generate_mask(void*, websocket_mask_key_type& key) noexcept {
    return RAND_bytes_ex(nullptr, reinterpret_cast<unsigned char*>(key.data()), key.size(), 0) ==
           1;
}

void websocket_client_state::check_operation_affinity(void* target) noexcept {
    const auto& worker_value = *static_cast<const worker_handle*>(target);
    if (!worker_value.is_current()) {
        std::terminate();
    }
}

void websocket_client_state::arm(worker_timer_registration& timer,
    std::optional<std::chrono::milliseconds> timeout, client_abort_reason reason) {
    timer.cancel();
    if (!timeout.has_value()) {
        return;
    }
    // close_now() cancels every state-owned timer and operation timers die
    // with their frames, all before the lifecycle can retire `this`.
    (void)worker_.schedule_timer(timer, worker_timer_deadline_after(*timeout), [this, reason](worker_timer_outcome outcome) noexcept {
        if (outcome == worker_timer_outcome::expired) {
            retire(reason);
        }
    });
}

void websocket_client_state::disarm(worker_timer_registration& timer) noexcept {
    timer.cancel();
}

websocket_client_error websocket_client_state::abort_error(client_abort_reason reason) {
    switch (reason) {
        case client_abort_reason::timeout:
            return websocket_client_error(
                websocket_client_error::code_type::timeout, "WebSocket client operation timed out");
        case client_abort_reason::cancelled:
            return websocket_client_error(
                websocket_client_error::code_type::cancelled, "WebSocket client operation was cancelled");
        case client_abort_reason::none:
        case client_abort_reason::closing:
            break;
    }
    return websocket_client_error(websocket_client_error::code_type::closing, "WebSocket client is closing");
}

void websocket_client_state::throw_if_aborted() const {
    if (abort_reason_ != client_abort_reason::none) {
        throw abort_error(abort_reason_);
    }
}

websocket_client_error websocket_client_state::transport_error(const client_transport_result& result) const {
    const auto code = [&] {
        switch (result.status_) {
            case client_transport_status::resolve_failed:
                return websocket_client_error::code_type::resolve_failed;
            case client_transport_status::connect_failed:
                return websocket_client_error::code_type::connect_failed;
            case client_transport_status::tls_failed:
                return websocket_client_error::code_type::tls_failed;
            case client_transport_status::ok:
            case client_transport_status::eof:
            case client_transport_status::aborted:
            case client_transport_status::io_failed:
                break;
        }
        return websocket_client_error::code_type::io_error;
    }();
    if (result.status_ == client_transport_status::aborted) {
        return abort_error(abort_reason_);
    }
    if (result.status_ == client_transport_status::eof) {
        return websocket_client_error(code, "WebSocket transport ended");
    }
    return websocket_client_error(code, error_code_text(result.error_));
}

void websocket_client_state::throw_transport_failure(const client_transport_result& result) {
    retire(result.status_ == client_transport_status::aborted ? result.abort_ : client_abort_reason::closing);
    throw transport_error(result);
}

websocket_client_state::operation_guard_type::operation_guard_type(
    websocket_client_state& state_value, const operation_options& options)
    : state_(state_value) {
    state_.arm(timer_, options.timeout_, client_abort_reason::timeout);
    if (options.stop_token_.stop_requested()) {
        state_.retire(client_abort_reason::cancelled);
    } else if (options.stop_token_.stoppable()) {
        // The registration is reset before this guard's operation completes,
        // and the lifecycle joins operations before retiring the state.
        options.stop_token_.register_callback(cancellation_, [state = &state_]() noexcept {
            state->request_retire(client_abort_reason::cancelled);
        });
    }
    state_.throw_if_aborted();
}

websocket_client_state::operation_guard_type::~operation_guard_type() {
    cancellation_.reset();
    timer_.cancel();
}

websocket_client_handle websocket_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    validate_operation_options(options);
    return websocket_client_handle(*this, lifecycle_.operation_scope(), lifecycle_.options(std::move(options)));
}

bool websocket_client_state::connected() {
    require_current();
    return protocol_open();
}

std::string_view websocket_client_state::subprotocol() {
    require_current();
    return selected_subprotocol_;
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_client_handle::websocket_client_handle(detail::websocket_client_state& state_value,
    ::ruvia::operation_scope& scope, operation_options options) noexcept
    : state_(&state_value),
      options_(std::move(options)),
      registration_(scope, this, &websocket_client_handle::expire_capability) {}

websocket_client_handle::websocket_client_handle(const websocket_client_handle& other) noexcept
    : state_(other.state_),
      options_(other.options_),
      registration_(other.registration_, this) {}

void websocket_client_handle::expire_capability(void* target) noexcept {
    static_cast<websocket_client_handle*>(target)->state_ = nullptr;
}

websocket_client_handle websocket_client_handle::with_options(operation_options options) const {
    validate_operation_options(options);
    registration_.require_active();
    return websocket_client_handle(
        *state_, registration_.scope(), merge_operation_options(options_, std::move(options)));
}

task<std::optional<websocket_message>> websocket_client_handle::read() const {
    registration_.require_active();
    return state_->read(options_);
}

task<void> websocket_client_handle::text(std::string_view payload_value, websocket_send_options options) const {
    registration_.require_active();
    return state_->write(websocket_opcode::text, payload_value, options_, options);
}

task<void> websocket_client_handle::text_owned(std::pmr::string&& payload_value, websocket_send_options options) const {
    registration_.require_active();
    return state_->write(websocket_opcode::text, std::move(payload_value), options_, options);
}

task<void> websocket_client_handle::binary(std::string_view payload_value, websocket_send_options options) const {
    registration_.require_active();
    return state_->write(websocket_opcode::binary, payload_value, options_, options);
}

task<void> websocket_client_handle::binary(std::span<const std::byte> payload_value, websocket_send_options options) const {
    return binary(as_chars(payload_value), options);
}

task<void> websocket_client_handle::binary_owned(std::pmr::string&& payload_value, websocket_send_options options) const {
    registration_.require_active();
    return state_->write(websocket_opcode::binary, std::move(payload_value), options_, options);
}

task<void> websocket_client_handle::ping(std::string_view payload_value) const {
    registration_.require_active();
    return state_->write(websocket_opcode::ping, payload_value, options_);
}

task<void> websocket_client_handle::ping_owned(std::pmr::string&& payload_value) const {
    registration_.require_active();
    return state_->write(websocket_opcode::ping, std::move(payload_value), options_);
}

task<void> websocket_client_handle::pong(std::string_view payload_value) const {
    registration_.require_active();
    return state_->write(websocket_opcode::pong, payload_value, options_);
}

task<void> websocket_client_handle::pong_owned(std::pmr::string&& payload_value) const {
    registration_.require_active();
    return state_->write(websocket_opcode::pong, std::move(payload_value), options_);
}

task<void> websocket_client_handle::close(websocket_close_options options) const {
    registration_.require_active();
    return state_->close(options, options_);
}

void websocket_client_handle::abort() noexcept {
    if (state_ != nullptr) {
        state_->abort();
    }
}

websocket_client::websocket_client(event_loop loop, const websocket_client_config& config)
    : state_(std::make_shared<detail::websocket_client_state>(std::move(loop), config)) {
    state_->lifecycle().bind_stop();
}

websocket_client::~websocket_client() {
    state_->abort();
}

task<void> websocket_client::connect() & {
    return state_->lifecycle().connect();
}

websocket_client_handle websocket_client::with_options(operation_options options) const& {
    return state_->handle(std::move(options));
}

websocket_client_handle websocket_client::handle() const& {
    return state_->handle({});
}

void websocket_client::close() noexcept {
    state_->abort();
}

task<void> websocket_client::shutdown() & {
    return state_->lifecycle().shutdown();
}

bool websocket_client::connected() const {
    return state_->connected();
}

std::string_view websocket_client::subprotocol() const& {
    return state_->subprotocol();
}

const worker_handle& websocket_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
