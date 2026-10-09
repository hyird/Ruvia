#include "ruvia/web/websocket_client.h"

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <openssl/rand.h>

#include "ruvia/core/worker_handle.h"

#include "client/websocket_client_internal.h"
#include "client/websocket_client_state.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] event_loop require_event_loop(event_loop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("WebSocket client requires a valid event loop");
    }
    return loop;
}

}  // namespace

websocket_client_state::websocket_client_state(event_loop loop, const websocket_client_config& config)
    : loop_(require_event_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      config_(config, memory_.resource()),
      tls_context_([&] {
          asio::ssl::context context(asio::ssl::context::tls_client);
          if (config_.scheme_ == websocket_scheme::wss) {
              configure_client_tls_context(*context.native_handle(), config_.transport_.view());
          }
          return context;
      }()),
      resolver_(loop_.io_context()),
      stream_(loop_.io_context(), tls_context_),
      write_signal_(worker_),
      close_state_(loop_, worker_),
      input_(memory_.allocator<char>()),
      selected_subprotocol_(memory_.allocator<char>()) {
    input_.reserve(websocket_client_transport_buffer_bytes);
}

websocket_client_state::~websocket_client_state() {
    const auto phase = phase_.load(std::memory_order_acquire);
    if ((phase != phase_type::closed && phase != phase_type::fresh) ||
        (close_state_.task_started() && !close_state_.complete())) {
        std::terminate();
    }
}

void websocket_client_state::bind_stop() {
    std::weak_ptr<websocket_client_state> weak = shared_from_this();
    stop_registration_ = loop_.on_stop([weak = std::move(weak)]() -> task<void> {
        if (const auto state = weak.lock()) {
            co_await shutdown_owned(state, client_close_state::observation_mode_type::retirement);
        }
    });
}

void websocket_client_state::require_current() const {
    if (!worker_.is_current()) {
        throw std::logic_error("WebSocket client must be used on its bound event loop");
    }
}

void websocket_client_state::require_open() const {
    require_current();
    if (phase_.load(std::memory_order_acquire) != phase_type::open) {
        throw websocket_client_error(
            websocket_client_error::code_type::invalid_state, "WebSocket client is not connected");
    }
}

ruvia::websocket_connection& websocket_client_state::require_protocol() noexcept {
    if (!protocol_.has_value()) {
        std::terminate();
    }
    return protocol_.value();
}

std::uint16_t websocket_client_state::port() const noexcept {
    return config_.port_.value_or(config_.scheme_ == websocket_scheme::wss ? 443 : 80);
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
    std::optional<std::chrono::milliseconds> timeout, abort_reason_type reason) {
    timer.cancel();
    if (!timeout.has_value()) {
        return;
    }
    std::weak_ptr<websocket_client_state> weak = shared_from_this();
    (worker_).schedule_timer(timer, worker_timer_deadline_after(*timeout), [weak = std::move(weak), reason](worker_timer_outcome outcome) noexcept {
        if (outcome != worker_timer_outcome::expired) {
            return;
        }
        if (const auto state = weak.lock()) {
            state->close_on_worker(reason);
        }
    });
}

void websocket_client_state::disarm(worker_timer_registration& timer) noexcept {
    timer.cancel();
}

void websocket_client_state::throw_abort() const {
    switch (abort_reason_) {
        case abort_reason_type::none:
            return;
        case abort_reason_type::timeout:
            throw websocket_client_error(
                websocket_client_error::code_type::timeout, "WebSocket client operation timed out");
        case abort_reason_type::cancelled:
            throw websocket_client_error(
                websocket_client_error::code_type::cancelled, "WebSocket client operation was cancelled");
        case abort_reason_type::closing:
            throw websocket_client_error(
                websocket_client_error::code_type::closing, "WebSocket client is closing");
    }
}

websocket_client_state::operation_guard_type::operation_guard_type(
    websocket_client_state& state_value, const operation_options& options)
    : state_(state_value) {
    state_.arm(timer_, options.timeout_, abort_reason_type::timeout);
    if (options.stop_token_.stoppable()) {
        options.stop_token_.register_callback(cancellation_, websocket_client_stop_abort{
                                                                 state_.weak_from_this()});
    }
    if (options.stop_token_.stop_requested()) {
        state_.close_on_worker(abort_reason_type::cancelled);
    }
    state_.throw_abort();
}

websocket_client_state::operation_guard_type::~operation_guard_type() {
    cancellation_.reset();
    timer_.cancel();
}

websocket_client_handle websocket_client_state::handle(operation_options options) {
    require_open();
    validate_operation_options(options);
    options = merge_operation_options(
        operation_options{.stop_token_ = stop_source_.token()}, std::move(options));
    return websocket_client_handle(shared_from_this(), operation_scope_, std::move(options));
}

bool websocket_client_state::connected() {
    require_current();
    return phase_.load(std::memory_order_acquire) == phase_type::open;
}

std::string_view websocket_client_state::subprotocol() {
    require_current();
    return selected_subprotocol_;
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_client_handle::websocket_client_handle(std::shared_ptr<detail::websocket_client_state> state_value,
    ::ruvia::operation_scope& scope, operation_options options) noexcept
    : state_(std::move(state_value)),
      options_(std::move(options)),
      registration_(scope, this, &websocket_client_handle::expire_capability) {}

websocket_client_handle::websocket_client_handle(const websocket_client_handle& other) noexcept
    : state_(other.state_),
      options_(other.options_),
      registration_(other.registration_, this) {}

void websocket_client_handle::expire_capability(void* target) noexcept {
    static_cast<websocket_client_handle*>(target)->state_.reset();
}

websocket_client_handle websocket_client_handle::with_options(operation_options options) const {
    detail::validate_operation_options(options);
    registration_.require_active();
    return websocket_client_handle(
        state_, registration_.scope(), detail::merge_operation_options(options_, std::move(options)));
}

scoped_operation<std::optional<websocket_message>> websocket_client_handle::read() const {
    registration_.require_active();
    return state_->read(options_);
}

scoped_operation<void> websocket_client_handle::text(std::string_view payload_value, websocket_send_options options) const {
    registration_.require_active();
    return state_->write(websocket_opcode::text, payload_value, options_, options);
}

scoped_operation<void> websocket_client_handle::binary(std::string_view payload_value, websocket_send_options options) const {
    registration_.require_active();
    return state_->write(websocket_opcode::binary, payload_value, options_, options);
}

scoped_operation<void> websocket_client_handle::ping(std::string_view payload_value) const {
    registration_.require_active();
    return state_->write(websocket_opcode::ping, payload_value, options_);
}

scoped_operation<void> websocket_client_handle::pong(std::string_view payload_value) const {
    registration_.require_active();
    return state_->write(websocket_opcode::pong, payload_value, options_);
}

scoped_operation<void> websocket_client_handle::close(websocket_close_options options) const {
    registration_.require_active();
    return state_->close(options, options_);
}

void websocket_client_handle::abort() noexcept {
    if (state_) {
        state_->abort();
    }
}

websocket_client::websocket_client(event_loop loop, const websocket_client_config& config)
    : state_(std::make_shared<detail::websocket_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

websocket_client::~websocket_client() {
    state_->abort();
}

task<void> websocket_client::connect() & {
    return state_->connect();
}

websocket_client_handle websocket_client::with_options(operation_options options) const& {
    return state_->handle(std::move(options));
}

scoped_operation<std::optional<websocket_message>> websocket_client::read() const& {
    return with_options({}).read();
}

scoped_operation<void> websocket_client::text(std::string_view payload_value, websocket_send_options options) const& {
    return with_options({}).text(payload_value, options);
}

scoped_operation<void> websocket_client::binary(std::string_view payload_value, websocket_send_options options) const& {
    return with_options({}).binary(payload_value, options);
}

scoped_operation<void> websocket_client::ping(std::string_view payload_value) const& {
    return with_options({}).ping(payload_value);
}

scoped_operation<void> websocket_client::pong(std::string_view payload_value) const& {
    return with_options({}).pong(payload_value);
}

scoped_operation<void> websocket_client::close(websocket_close_options options) const& {
    return with_options({}).close(options);
}

void websocket_client::abort() noexcept {
    state_->abort();
}

task<void> websocket_client::shutdown() & {
    return state_->shutdown();
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
