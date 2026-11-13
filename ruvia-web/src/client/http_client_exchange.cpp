#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_lifecycle.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_pool.h"
#include "client/http_client_registry.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "http/inline_http_header_views.h"
#include "integration/operation_lane_lease.h"
#include "tls/tls_protocol_policy.h"
#include "websocket/http_websocket_connection.h"

namespace ruvia::detail {

struct http_client_output_write_input final {
    http_client_response pin_;
    std::string bytes_;
    operation_options operation_;
};

struct in_flight_output_guard final {
    http_client_response_state& state_;
    http_client_output_queue& output_;
    bool active_{true};
    ~in_flight_output_guard() noexcept {
        if (!active_) {
            return;
        }
        output_.stop();
        if (state_.pool_ != nullptr && !state_.complete_ && !state_.abandoned_) {
            state_.pool_->abandon_response(state_);
        }
    }
};

// The input pin and owned payload live in this single cold frame. Policies
// handle upload validation/trailers and tunnel retirement without another task.
// The operation's own interruption aborts the exchange or tunnel and reports
// its timeout or cancellation, even if the driver had already taken the chunk.
template <typename policy>
[[nodiscard]] task<void> write_client_output(http_client_response_state* state_value, http_client_output_write_input input) {
    auto& output = policy::output(*state_value);
    auto activity = claim_operation_lane(output.operation_active_, "HTTP client output operation is already active");
    const http_client_operation_interruption interruption(*state_value, http_client_operation_lane::output, input.operation_);
    policy::require_write(*state_value);
    if (output.chunk_ready_) {
        throw std::logic_error("HTTP client output queue still owns a chunk");
    }
    if (input.bytes_.empty()) {
        co_return;
    }
    output.chunk_ = std::move(input.bytes_);
    output.chunk_ready_ = true;
    in_flight_output_guard in_flight{*state_value, output};
    output.notify_data();
    while (output.chunk_ready_ && !output.stopped_) {
        co_await output.space_.wait();
    }
    in_flight.active_ = false;
    state_value->throw_if_output_interrupted();
    // Retirement may stop the queue after its driver acknowledged this chunk
    // but before this waiter resumes. It cannot revoke the completed write.
    if (output.chunk_ready_) {
        policy::throw_stopped(*state_value);
    }
}

template <typename policy, typename input_type>
[[nodiscard]] task<void> finish_client_output(http_client_response_state* state_value, input_type input, operation_options operation) {
    auto& output = policy::output(*state_value);
    auto activity = claim_operation_lane(output.operation_active_, "HTTP client output operation is already active");
    const http_client_operation_interruption interruption(*state_value, http_client_operation_lane::output, operation);
    if (!policy::begin_finish(*state_value, input)) {
        co_return;
    }
    if (output.chunk_ready_) {
        throw std::logic_error("HTTP client output queue still owns a chunk");
    }
    output.end_requested_ = true;
    in_flight_output_guard in_flight{*state_value, output};
    output.notify_data();
    while (!output.ended_ && (!output.stopped_ || output.completion_pending_)) {
        co_await output.space_.wait();
    }
    in_flight.active_ = false;
    state_value->throw_if_output_interrupted();
    if (!output.ended_) {
        policy::throw_stopped(*state_value);
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
struct http_client_upload_end_input final {
    http_client_response pin_;
    std::vector<http_header> fields_;
};

task<http_client_response> http_client_response_handoff::receive(
    http_client_response pin, std::string_view head_message, operation_options operation) {
    return ::ruvia::make_scoped_operation(scope_, receive_owned(std::move(pin), head_message, std::move(operation)));
}

task<http_client_response> http_client_response_handoff::receive_owned(
    http_client_response pin, std::string_view head_message, operation_options operation) {
    // `pin` keeps the response state, and with it this handoff, alive.
    auto activity = claim_operation_lane(active_, "HTTP response receive is already active");
    if (taken_) {
        throw std::logic_error("HTTP response has already been transferred");
    }
    const operation_deadline redirect_deadline(
        operation.timeout_.has_value()
            ? operation.timeout_
            : (pin.state_->upload_ && pin.state_->upload_->redirect_request_ &&
                          pin.state_->pool_ != nullptr
                      ? std::optional<operation_timeout>(
                            pin.state_->pool_->config().request_timeout_)
                      : std::nullopt));
    {
        const http_client_operation_interruption interruption(
            *pin.state_, http_client_operation_lane::read, operation);
        co_await pin.state_->await_head(head_message);
    }
    taken_ = true;
    pin.consumer_ = true;
    if (pin.state_->upload_ && pin.state_->upload_->redirect_request_) {
        auto redirect_request = std::move(*pin.state_->upload_->redirect_request_);
        pin.state_->upload_->redirect_request_.reset();
        co_return co_await http_client_request::follow_redirects(
            std::move(redirect_request), std::move(pin), redirect_deadline, true);
    }
    co_return std::move(pin);
}

void http_client_response_handoff::release(http_client_response& owner) noexcept {
    scope_.close();
    if (taken_) {
        owner.consumer_ = false;
    }
}
}  // namespace ruvia::detail
namespace ruvia {
namespace {
void require_writable(detail::http_client_response_state& state_value) {
    state_value.require_owner_worker("HTTP exchange must run on its owner worker");
    if (!state_value.upload_ || state_value.upload_->output_.stopped_ || state_value.upload_->output_.ended_ || state_value.upload_->output_.end_requested_) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload is no longer writable");
    }
}
struct upload_output_policy final {
    static detail::http_client_output_queue& output(detail::http_client_response_state& state_value) noexcept {
        return state_value.upload_->output_;
    }
    static void require_write(detail::http_client_response_state& state_value) {
        auto& queue = output(state_value);
        if (queue.stopped_ || queue.end_requested_) {
            throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload stopped");
        }
    }
    static bool begin_finish(detail::http_client_response_state& state_value, detail::http_client_upload_end_input& input) {
        require_write(state_value);
        state_value.upload_->trailers_ = std::move(input.fields_);
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::http_client_response_state& state_value) {
        if (state_value.upload_->failure_) {
            std::rethrow_exception(state_value.upload_->failure_);
        }
        if (state_value.failure_) {
            std::rethrow_exception(state_value.failure_);
        }
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP request upload stopped");
    }
};
}  // namespace

http_client_exchange::http_client_exchange(http_client_response response) noexcept
    : response_(std::move(response)),
      body_(response_.state_) {}
http_client_exchange::http_client_exchange(http_client_exchange&& other) noexcept
    : response_(std::move(other.response_)),
      body_(response_.state_) {
    other.body_.state_ = nullptr;
}
http_client_exchange& http_client_exchange::operator=(http_client_exchange&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
        body_.state_ = response_.state_;
        other.body_.state_ = nullptr;
    }
    return *this;
}
http_client_exchange::~http_client_exchange() {
    release();
}
void http_client_exchange::release() noexcept {
    auto* state_value = response_.state_;
    if (state_value == nullptr) {
        return;
    }
    auto& upload = *state_value->upload_;
    const bool abandon_upload = !upload.output_.ended_ && !upload.output_.stopped_;
    upload.output_.output_scope_.close();
    upload.response_.release(response_);
    if (abandon_upload && upload.response_.taken() &&
        !state_value->complete_ && state_value->pool_ != nullptr) {
        // Keep the retained consumer's read scope open to observe cancellation.
        state_value->pool_->abandon_response(*state_value);
    } else if (!upload.output_.ended_) {
        upload.output_.stop();
    }
    response_.release();
    body_.state_ = nullptr;
}

task<void> http_client_request_body_writer::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()), std::move(operation));
}
task<void> http_client_request_body_writer::write(std::string_view bytes_value, operation_options operation) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::string(bytes_value), std::move(operation)}));
}
task<void> http_client_request_body_writer::write_owned(std::string&& bytes_value, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::move(bytes_value), std::move(operation)}));
}
task<void> http_client_request_body_writer::end(std::span<const http_header_view> fields_value, operation_options operation) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    http_request_trailers validation;
    if (validation.append(fields_value).has_value()) {
        throw std::invalid_argument("invalid request trailer section");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_,
        detail::finish_client_output<upload_output_policy>(state_, detail::http_client_upload_end_input{http_client_response(state_), std::move(validation).take_fields()}, std::move(operation)));
}
bool http_client_request_body_writer::complete() const noexcept {
    return state_ == nullptr || state_->upload_->output_.ended_;
}
bool http_client_request_body_writer::writable() const noexcept {
    if (state_ == nullptr || !state_->upload_) {
        return false;
    }
    const auto& output = state_->upload_->output_;
    return !output.stopped_ && !output.ended_ && !output.end_requested_;
}
void http_client_request_body_writer::abandon() & noexcept {
    if (state_ == nullptr || !state_->upload_ || state_->upload_->output_.ended_ || state_->upload_->output_.stopped_) {
        return;
    }
    state_->upload_->abandoned_ = true;
    state_->upload_->output_.stop();
}

task<http_client_response> http_client_exchange::response(operation_options operation) & {
    auto* state_value = response_.state_;
    if (state_value == nullptr || state_value->upload_->response_.taken()) {
        throw std::logic_error("HTTP exchange response has already been transferred");
    }
    state_value->require_owner_worker("HTTP exchange must run on its owner worker");
    return state_value->upload_->response_.receive(
        http_client_response(state_value), "HTTP request failed before response head", std::move(operation));
}
}  // namespace ruvia

namespace ruvia {
namespace {
void require_output(detail::http_client_response_state* state_value, bool finishing) {
    if (state_value == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    state_value->require_owner_worker("HTTP client tunnel requires its owner worker");
    // Normal retirement stops the queue after both directions complete, but
    // cannot invalidate a finish that has already succeeded. Writes and failed
    // or abandoned tunnels still reject terminal output operations.
    const bool completed_finish = finishing && state_value->tunnel_ && state_value->tunnel_->output_.ended_ && state_value->complete_;
    if (!state_value->tunnel_ || (state_value->pool_ == nullptr && !completed_finish) || (state_value->tunnel_->output_.stopped_ && !completed_finish) || !state_value->tunnel_->accepted_ ||
        state_value->abandoned_ || state_value->failure_ || state_value->pool_closed_) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client tunnel is closed");
    }
    auto& output = state_value->tunnel_->output_;
    if (!finishing && (output.ended_ || output.end_requested_)) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client tunnel sending direction is closed");
    }
}
struct tunnel_output_policy final {
    static detail::http_client_output_queue& output(detail::http_client_response_state& state_value) noexcept {
        return state_value.tunnel_->output_;
    }
    static void require_write(detail::http_client_response_state& state_value) {
        auto& queue = output(state_value);
        if (queue.stopped_ || queue.end_requested_ || state_value.abandoned_) {
            throw_stopped(state_value);
        }
    }
    static bool begin_finish(detail::http_client_response_state& state_value, http_client_response&) {
        auto& queue = output(state_value);
        if (queue.ended_) {
            return false;
        }
        if (queue.stopped_ || state_value.abandoned_) {
            throw_stopped(state_value);
        }
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::http_client_response_state& state_value) {
        if (state_value.failure_) {
            std::rethrow_exception(state_value.failure_);
        }
        if (state_value.pool_closed_) {
            throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client tunnel failed");
        }
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client tunnel output stopped");
    }
};
void abort_tunnel(detail::http_client_response_state* state, bool malformed) noexcept {
    if (state == nullptr || !state->tunnel_ || state->pool_ == nullptr) {
        return;
    }
    if (state->tunnel_->output_.stopped_ && !malformed) {
        return;
    }
    if (!state->memory_domain().worker().is_current()) {
        std::terminate();
    }
    state->tunnel_->output_.stop();
    if ((!state->complete_ || malformed) && !state->abandoned_) {
        state->pool_->abandon_response(*state, malformed);
    }
}
}  // namespace
http_client_tunnel::http_client_tunnel(http_client_response response) noexcept
    : response_(std::move(response)) {}
http_client_tunnel::http_client_tunnel(http_client_tunnel&& other) noexcept
    : response_(std::move(other.response_)) {}
http_client_tunnel& http_client_tunnel::operator=(http_client_tunnel&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
    }
    return *this;
}
http_client_tunnel::~http_client_tunnel() {
    release();
}
void http_client_tunnel::release() noexcept {
    if (auto* state_value = response_.state_; state_value != nullptr) {
        state_value->tunnel_->output_.output_scope_.close();
        state_value->tunnel_->output_.stop();
    }
    response_.release();
}
void http_client_tunnel::abort() & noexcept {
    abort_tunnel(response_.state_, false);
}
void http_client_tunnel::abort_malformed() & noexcept {
    abort_tunnel(response_.state_, true);
}
http_udp_tunnel http_client_tunnel::udp(http_capsule_config config) && {
    return http_udp_tunnel(std::move(*this).datagrams(config));
}
task<std::optional<std::span<const std::byte>>> http_client_tunnel::read(operation_options operation) & {
    if (response_.state_ == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    return response_.stream().read(std::move(operation));
}
task<std::optional<std::string_view>> http_client_tunnel::text(operation_options operation) & {
    if (response_.state_ == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    return response_.stream().text(std::move(operation));
}
task<void> http_client_tunnel::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()), std::move(operation));
}
task<void> http_client_tunnel::write(std::string_view bytes_value, operation_options operation) & {
    require_output(response_.state_, false);
    auto& state_value = *response_.state_;
    if (bytes_value.size() > state_value.tunnel_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP client tunnel chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::write_client_output<tunnel_output_policy>(&state_value, detail::http_client_output_write_input{http_client_response(&state_value), std::string(bytes_value), std::move(operation)}));
}
task<void> http_client_tunnel::write_owned(std::string&& bytes_value, operation_options operation) {
    require_output(response_.state_, false);
    auto& state_value = *response_.state_;
    if (bytes_value.size() > state_value.tunnel_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP client tunnel chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::write_client_output<tunnel_output_policy>(&state_value, detail::http_client_output_write_input{http_client_response(&state_value), std::move(bytes_value), std::move(operation)}));
}
task<void> http_client_tunnel::finish(operation_options operation) & {
    require_output(response_.state_, true);
    auto& state_value = *response_.state_;
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::finish_client_output<tunnel_output_policy>(&state_value, http_client_response(&state_value), std::move(operation)));
}
}  // namespace ruvia

namespace ruvia::detail {

// Only adapts tunnel I/O. All WebSocket framing, lanes, control traffic,
// interruption and close policy live in websocket_session_connection.
class http_client_websocket_transport final {
public:
    http_client_websocket_transport(http_client_tunnel tunnel, connection_scanner::entry& activity)
        : tunnel_(std::move(tunnel)),
          response_(*tunnel_.response_.state_),
          activity_(activity) {}

    template <typename check_type>
    task<bool> read_more(http_io_buffer& bytes, check_type check, asio::cancellation_slot slot) {
        struct cancellation_reset final {
            asio::cancellation_slot slot_;
            ~cancellation_reset() {
                if (slot_.is_connected()) {
                    slot_.clear();
                }
            }
        } reset{slot};
        if (slot.is_connected()) {
            slot.assign([this](asio::cancellation_type) { response_.data_signal_.notify(); });
        }
        while (response_.offset_ == response_.buffered_.size() && response_.pending_.empty() &&
               !response_.receive_complete() && !response_.failure_ && !response_.pool_closed_ && !response_.abandoned_) {
            check();
            co_await response_.data_signal_.wait();
        }
        check();
        try {
            const auto chunk = co_await tunnel_.read();
            if (!chunk) {
                co_return false;
            }
            bytes.append(as_chars(*chunk));
            co_return true;
        } catch (const http_client_error& error) {
            throw inbound_transport_error(transport_error(error), "WebSocket tunnel read");
        }
    }
    task<std::error_code> write_bytes(std::string_view bytes, http_stream_end end) {
        try {
            while (!bytes.empty()) {
                const auto chunk = bytes.substr(0, 16384);
                co_await tunnel_.write(chunk);
                activity_.note_write_progress();
                bytes.remove_prefix(chunk.size());
            }
            if (end == http_stream_end::end) {
                co_await tunnel_.finish();
            }
            co_return std::error_code{};
        } catch (const http_client_error& error) {
            co_return transport_error(error);
        }
    }
    void abort() noexcept {
        tunnel_.abort();
        response_.data_signal_.notify();
    }
    http_client_response_state& response() noexcept {
        return response_;
    }

private:
    static std::error_code transport_error(const http_client_error& error) noexcept {
        if (error.code() == std::errc::timed_out) {
            return std::make_error_code(std::errc::timed_out);
        }
        if (error.code() == std::errc::operation_canceled) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        return std::make_error_code(std::errc::io_error);
    }
    http_client_tunnel tunnel_;
    http_client_response_state& response_;
    connection_scanner::entry& activity_;
};

class client_websocket_session final : public std::enable_shared_from_this<client_websocket_session> {
public:
    client_websocket_session(http_client_pool& pool, http_client_tunnel tunnel,
        const http_client_websocket_handshake& handshake, websocket_client_deflate_offer deflate,
        websocket_lifecycle_options lifecycle, std::size_t message_limit)
        : worker_(pool.worker()),
          retire_signal_(worker_),
          scanner_(worker_, scanner_options(lifecycle)),
          connection_(http_client_websocket_transport(std::move(tunnel), entry_), worker_, entry_, normalize_websocket_lifecycle_options(lifecycle),
              protocol_byte_limit::limited(message_limit), {},
              {.compression_ = handshake.compression_, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_key, .deflater_ = deflate.deflater_},
              std::string(handshake.selected_subprotocol_)),
          scanner_guard_(scanner_, entry_, {&connection_, &abort_transport, &abort_transport}) {
        entry_.enter_long_lived();
        scanner_.start();
    }
    static void retire(void* target) noexcept {
        auto& self = *static_cast<client_websocket_session*>(target);
        self.retiring_ = true;
        self.retire_signal_.notify();
    }
    static task<void> run(std::shared_ptr<client_websocket_session> self, stop_token pool_stop) {
        auto stop_registration = pool_stop.register_callback([self = self.get()] {
            self->retire_signal_.notify();
        });
        auto& response = self->connection_.transport().response();
        while (!self->retiring_ && !response.pool_closed_ && !pool_stop.stop_requested()) {
            co_await self->retire_signal_.wait();
        }
        stop_registration.reset();
        self->scanner_.stop();
        co_await self->connection_.detach_and_drain_writes();
        if (const auto failure = self->connection_.take_background_failure()) {
            report_unhandled_failure("WebSocket client session", failure);
        }
    }

private:
    static connection_scanner_options scanner_options(const websocket_lifecycle_options& lifecycle) {
        auto interval = std::chrono::milliseconds(1000);
        const auto tighten = [&](const std::optional<timeout_duration>& timeout) {
            if (timeout) {
                interval = std::min(interval, std::max(std::chrono::milliseconds(1), timeout->value() / 2));
            }
        };
        tighten(lifecycle.heartbeat_.ping_interval_);
        tighten(lifecycle.heartbeat_.pong_timeout_);
        tighten(lifecycle.close_handshake_timeout_);
        return {.scan_interval_ = interval};
    }
    static bool mask_key(void*, websocket_mask_key_type& key) noexcept {
        return fill_random_bytes(std::span<unsigned char>(reinterpret_cast<unsigned char*>(key.data()), key.size()));
    }
    static void abort_transport(void* target) noexcept {
        static_cast<websocket_session_connection<http_client_websocket_transport>*>(target)->abort();
    }
    worker_ref worker_;
    worker_signal retire_signal_;
    connection_scanner scanner_;
    connection_scanner::entry entry_;

public:
    websocket_session_connection<http_client_websocket_transport> connection_;

private:
    connection_scanner::guard scanner_guard_;
    bool retiring_{false};
};

}  // namespace ruvia::detail

namespace ruvia {

http_client_websocket_request::http_client_websocket_request(const http_client_ref& client, std::string_view path)
    : client_(client),
      path_(path),
      headers_(),
      subprotocols_() {
    require_open();
    if (!is_valid_http_origin_form_target(path_)) {
        throw std::invalid_argument("WebSocket path must use origin-form");
    }
}
void http_client_websocket_request::require_open() const {
    client_.registration_.require_active();
    if (!client_.pool_->worker().is_current() || consumed_) {
        throw std::logic_error("WebSocket builder is consumed or used outside its worker");
    }
}
http_client_websocket_request& http_client_websocket_request::header(std::string_view name, std::string_view value) {
    require_open();
    headers_.emplace_back(name, value);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::headers(std::span<const http_header_view> fields) {
    require_open();
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
http_client_websocket_request& http_client_websocket_request::subprotocol(std::string_view value) {
    require_open();
    subprotocols_.emplace_back(value);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::deflate(websocket_client_deflate_offer offer) {
    require_open();
    deflate_ = offer;
    return *this;
}
http_client_websocket_request& http_client_websocket_request::lifecycle(websocket_lifecycle_options options) {
    require_open();
    lifecycle_ = detail::normalize_websocket_lifecycle_options(options);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::max_message_bytes(std::size_t limit) {
    require_open();
    if (limit == 0) {
        throw std::invalid_argument("WebSocket message limit must be greater than zero");
    }
    max_message_bytes_ = limit;
    return *this;
}
http_client_websocket_request& http_client_websocket_request::configure(http_client_request_options options) {
    require_open();
    detail::validate_http_client_request_options(options);
    request_options_ = std::move(options);
    return *this;
}
task<websocket> http_client_websocket_request::connect() {
    require_open();
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), connect_owned(std::move(*this)));
}
task<websocket> http_client_websocket_request::connect_owned(http_client_websocket_request request) {
    request.client_.registration_.require_active();
    auto& pool = *request.client_.pool_;
    std::shared_ptr<detail::http_client_websocket_handshake> handshake;
    std::optional<detail::http_client_request_storage> stored;
    {
        const detail::inline_http_header_views headers(request.headers_);
        validate_http1_websocket_client_handshake_configuration(headers.view());
        std::vector<std::string_view> protocols;
        protocols.reserve(request.subprotocols_.size());
        for (const auto& protocol : request.subprotocols_) {
            protocols.emplace_back(protocol);
        }
        handshake = std::make_shared<detail::http_client_websocket_handshake>(websocket_client_negotiation_config_view{.subprotocols_ = std::span<const std::string_view>(protocols), .deflate_ = request.deflate_});
        stored.emplace("GET", request.path_, headers.view());
        stored->options_ = detail::copy_http_client_request_settings(request.request_options_);
        stored->informational_ = detail::share_http_client_informational(std::move(request.request_options_.informational_));
        stored->pool_owner_ = request.client_.pool_owner_;
        stored->set_host_stop(request.client_.host_stop_token_);
        stored->set_tunnel(pool.config().authority_, "websocket");
        stored->set_websocket(handshake);
    }
    std::string(request.path_.get_allocator()).swap(request.path_);
    std::vector<http_header>(request.headers_.get_allocator()).swap(request.headers_);
    std::vector<std::string>(request.subprotocols_.get_allocator()).swap(request.subprotocols_);
    // The opening deadline covers admission, transport establishment and every
    // handshake byte, but must not become the established tunnel's lifetime.
    // After opening, user cancellation belongs to each operation's snapshot.
    stop_source opening_stop;
    auto user_stop = request.client_.options_.stop_token_.register_callback(
        [&opening_stop] { opening_stop.request_stop(); });
    bool opening_expired = false;
    worker_timer_registration opening_timer;
    const auto connect_timeout = request.request_options_.timeouts_.connect_.resolve(pool.config().connect_timeout_);
    if (connect_timeout) {
        (void)pool.worker().schedule_timer(opening_timer, deadline_after(connect_timeout->value()),
            [&opening_stop, &opening_expired](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    opening_expired = true;
                    opening_stop.request_stop();
                }
            });
    }
    auto opening_options = request.client_.options_;
    opening_options.stop_token_ = opening_stop.token();
    std::optional<http_client_tunnel_result> opened;
    try {
        opened.emplace(co_await pool.open_tunnel(std::move(*stored), {}, std::move(opening_options)));
    } catch (...) {
        opening_timer.cancel();
        if (opening_expired) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "WebSocket opening handshake timed out");
        }
        throw;
    }
    opening_timer.cancel();
    user_stop.reset();
    if (opening_expired) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "WebSocket opening handshake timed out");
    }
    auto* tunnel = opened->tunnel();
    if (!tunnel) {
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected),
            "WebSocket handshake rejected", opened->response()->status());
    }
    auto owner = std::make_shared<detail::client_websocket_session>(pool, std::move(*tunnel), *handshake, request.deflate_, request.lifecycle_, request.max_message_bytes_);
    auto result = detail::make_websocket_facade(owner->connection_);
    pool.tasks().spawn(detail::client_websocket_session::run(owner, pool.pool_stop_token()));
    result.state_->owner_ = owner;
    result.state_->retire_ = &detail::client_websocket_session::retire;
    result.state_->defaults_ = request.client_.options_;
    result.state_->read_timeout_ = request.request_options_.timeouts_.read_.resolve(pool.config().read_timeout_);
    co_return result;
}
http_client_websocket_request http_client_ref::websocket(std::string_view path) const {
    registration_.require_active();
    return http_client_websocket_request(*this, path);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// Reject invalid aliases before any pool or alias-index storage is allocated.
template <typename config_type>
void validate_http_client_configs(std::span<const config_type> definitions) {
    validate_capability_aliases(definitions, "HTTP client alias must not be empty", "duplicate HTTP client alias");
}

}  // namespace

struct http_client_registry::origin_preparation final {
    origin_preparation(const http_client_config& source, const worker_ref& worker,
        stop_token owner_stop, stop_token user_stop, const std::optional<tls_trust_store>& system_trust)
        : alias_(source.alias_.view()),
          config_(source, client_tls_files::keep_paths),
          cancellation_(owner_stop, user_stop) {
        if (config_.scheme_ == http_scheme::https) {
            if (!config_.tls_.ca_file_.has_value()) {
                config_.tls_.trust_store_ = system_trust;
            }
            const auto password = source.tls_.identity_
                                      ? std::string_view(source.tls_.identity_->private_key_password_)
                                      : std::string_view{};
            tls_.emplace(prepare_client_tls_config(worker, std::move(config_.tls_), password, cancellation_.token()));
        }
    }

    std::string alias_;
    http_client_config_storage config_;
    stop_group cancellation_;
    std::optional<task<client_tls_config_storage>> tls_;
};

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_ref& worker_value,
    std::span<const http_client_definition_type> definitions,
    const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      services_(services),
      retirements_(worker_) {
    validate_http_client_configs(definitions);
    initialize(definitions);
    preparations_.bind_worker(worker_);
}

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_ref& worker_value,
    std::span<const http_client_config> configs,
    const http_client_worker_services& services)
    : io_context_(io_context),
      worker_(worker_value),
      services_(services),
      retirements_(worker_) {
    validate_http_client_configs(configs);
    initialize(configs);
    preparations_.bind_worker(worker_);
}

template <typename config_type>
void http_client_registry::initialize(std::span<const config_type> configs) {
    aliases_.reserve(configs.size());
    for (const auto& config : configs) {
        auto pool = std::make_shared<http_client_pool>(io_context_, worker_, startup_config(config), services_);
        const std::string_view alias_view(config.alias_);
        const auto pos = std::ranges::lower_bound(aliases_, alias_view, std::less<>{}, &alias_binding::alias_);
        aliases_.insert(pos, alias_binding{std::string(alias_view), std::move(pool), next_origin_order_++});
    }
}

http_client_registry::~http_client_registry() = default;

http_client_config_storage http_client_registry::startup_config(const http_client_definition_type& definition) {
    // Application definitions were parsed once for every worker.
    return http_client_config_storage(definition.config_);
}

http_client_config_storage http_client_registry::startup_config(const http_client_config& config) {
    http_client_config_storage storage(config, client_tls_files::keep_paths);
    if (storage.scheme_ == http_scheme::https) {
        if (!storage.tls_.ca_file_.has_value()) {
            if (!system_trust_.has_value()) {
                system_trust_ = tls_trust_store::load_default();
            }
            storage.tls_.trust_store_ = system_trust_;
        }
        storage.tls_.parse_files(config.tls_.identity_ ? std::string_view(config.tls_.identity_->private_key_password_)
                                                       : std::string_view{});
    }
    return storage;
}

void http_client_registry::require_mutable() const {
    if (!worker_.is_current()) {
        throw std::logic_error("HTTP client origins must be changed on their bound worker");
    }
    if (closing_ || !worker_.accepting()) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client registry is closing", std::nullopt, nullptr, false);
    }
}

task<void> http_client_registry::upsert(const http_client_config& config, operation_options options) {
    require_mutable();
    validate_capability_alias(config.alias_.view(), "HTTP client alias must not be empty");
    auto input = std::make_unique<origin_preparation>(config, worker_, preparation_stop_.token(), options.stop_token_, system_trust_);
    return make_scoped_operation(preparations_, prepare_origin(std::move(input), std::move(options)));
}

void http_client_registry::require_prepared(stop_token stop, const operation_deadline& timeout) const {
    require_mutable();
    if (stop.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP origin update cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired()) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP origin update deadline expired", std::nullopt, nullptr, false);
    }
}

task<void> http_client_registry::prepare_origin(preparation_owner_type input, operation_options options) {
    const operation_deadline timeout(options.timeout_);
    const auto stop = input->cancellation_.token();
    require_prepared(stop, timeout);
    if (input->tls_) {
        std::exception_ptr failure;
        try {
            input->config_.tls_ = co_await std::move(*input->tls_);
        } catch (...) {
            failure = std::current_exception();
        }
        input->tls_.reset();
        // Cancellation cannot interrupt an OpenSSL file read. Join it first,
        // then decide whether the completed candidate may still be published.
        require_prepared(stop, timeout);
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!input->config_.tls_.ca_file_.has_value() && !system_trust_.has_value()) {
            system_trust_ = input->config_.tls_.trust_store_;
        }
    }
    auto replacement = std::make_shared<http_client_pool>(io_context_, worker_, std::move(input->config_), services_);
    require_prepared(stop, timeout);
    publish(std::move(input->alias_), std::move(replacement));
}

void http_client_registry::publish(std::string alias, pool_owner_type replacement) {
    const auto position = std::ranges::lower_bound(aliases_, std::string_view(alias), std::less<>{}, &alias_binding::alias_);
    if (position != aliases_.end() && position->alias_ == alias) {
        auto joining = position->pool_->join();
        retirements_.spawn(retire_origin(position, std::move(replacement), std::move(joining)));
        return;
    }
    if (next_origin_order_ == std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("HTTP client origin registration order is exhausted");
    }
    aliases_.insert(position, alias_binding{std::move(alias), std::move(replacement), next_origin_order_});
    ++next_origin_order_;
}

bool http_client_registry::remove(std::string_view alias) {
    require_mutable();
    validate_capability_alias(alias, "HTTP client alias must not be empty");
    const auto position = std::ranges::lower_bound(aliases_, alias, std::less<>{}, &alias_binding::alias_);
    if (position == aliases_.end() || position->alias_ != alias) {
        return false;
    }
    auto joining = position->pool_->join();
    retirements_.spawn(retire_origin(position, {}, std::move(joining)));
    return true;
}

task<void> http_client_registry::retire_origin(alias_map_type::iterator position,
    pool_owner_type replacement, task<void> joining) {
    // task_scope allocates its child before starting this coroutine. Everything
    // through publication is then noexcept: a failed admission changes nothing.
    auto retired = std::exchange(position->pool_, std::move(replacement));
    if (!position->pool_) {
        aliases_.erase(position);
    }
    retired->close_now();
    co_await std::move(joining);
}

void http_client_registry::close_now() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    preparation_stop_.request_stop();
    for (auto& binding : aliases_) {
        binding.pool_->close_now();
    }
}

task<void> http_client_registry::join() {
    close_now();
    std::exception_ptr failure;
    co_await preparations_.close_and_join();
    // Removed origins are no longer in aliases_; their retirement children own
    // the old pools until all transport drivers finish.
    try {
        co_await retirements_.join();
    } catch (...) {
        failure = std::current_exception();
    }
    for (auto& binding : aliases_) {
        try {
            co_await binding.pool_->join();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

http_client_ref http_client_registry::get_single_or_default(::ruvia::operation_scope& scope, stop_token host_stop) const {
    return aliases_.size() == 1 ? get(aliases_.front().alias_, scope, std::move(host_stop)) : get(scope, std::move(host_stop));
}

http_client_ref http_client_registry::get(::ruvia::operation_scope& scope, stop_token host_stop) const {
    return get(default_capability_alias, scope, std::move(host_stop));
}

http_client_ref http_client_registry::get(std::string_view alias, ::ruvia::operation_scope& scope, stop_token host_stop) const {
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "http client registry is closing", std::nullopt, nullptr, false);
    }
    const auto found = std::ranges::lower_bound(aliases_, alias, std::less<>{}, &alias_binding::alias_);
    if (found == aliases_.end() || found->alias_ != alias) {
        throw http_client_error(make_error_code(http_client_errc::not_configured), "named HTTP client is not configured", std::nullopt, nullptr, false);
    }
    http_client_ref result(*found->pool_, scope);
    result.pool_owner_ = found->pool_;
    result.registry_ = this;
    result.host_stop_token_ = std::move(host_stop);
    return result;
}

std::shared_ptr<http_client_pool> http_client_registry::find_origin(http_scheme scheme, std::string_view authority) const {
    if (closing_) {
        throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client registry is closing", std::nullopt, nullptr, false);
    }
    const alias_binding* found = nullptr;
    // Aliases sharing one origin retain registration precedence, independent
    // of lexical lookup order or a replacement pool's storage position.
    for (const auto& binding : aliases_) {
        const auto& pool = binding.pool_;
        if ((found == nullptr || binding.order_ < found->order_) && pool->scheme() == scheme &&
            http_authorities_equal(pool->config().authority_, authority, scheme == http_scheme::https ? 443 : 80)) {
            found = &binding;
        }
    }
    return found == nullptr ? pool_owner_type{} : found->pool_;
}

class http_client_state final : public std::enable_shared_from_this<http_client_state> {
public:
    http_client_state(event_loop loop, std::span<const http_client_config> configs,
        const http_client_worker_config& worker_config);

    http_client_state(const http_client_state&) = delete;
    http_client_state& operator=(const http_client_state&) = delete;

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }

    [[nodiscard]] http_client_ref client();
    [[nodiscard]] http_client_ref client(std::string_view alias);
    [[nodiscard]] task<void> upsert(const http_client_config& config, operation_options options) {
        lifecycle_.require_ready();
        return registry_.upsert(config, std::move(options));
    }
    [[nodiscard]] bool remove(std::string_view alias) {
        lifecycle_.require_ready();
        return registry_.remove(alias);
    }

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<http_client_state>;
    static constexpr std::string_view client_name{"HTTP"};
    [[noreturn]] static void throw_not_ready();
    [[nodiscard]] http_client_registry& backend() noexcept {
        return registry_;
    }

    event_loop loop_;
    worker_ref worker_;
    http_client_registry registry_;
    // Retires scopes before backend storage and its allocator are destroyed.
    client_lifecycle<http_client_state> lifecycle_;
};

http_client_state::http_client_state(event_loop loop, std::span<const http_client_config> configs,
    const http_client_worker_config& worker_config)
    : loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
      worker_(loop_.worker()),
      // Responses may outlive this client: the process resource backs them.
      registry_(loop_.io_context(), worker_, configs, make_http_client_worker_services(worker_config)),
      lifecycle_(*this, loop_, worker_, client_phase::connected) {}

void http_client_state::throw_not_ready() {
    throw http_client_error(make_error_code(http_client_errc::closing), "HTTP client is closing", std::nullopt, nullptr, false);
}

http_client_ref http_client_state::client() {
    lifecycle_.require_ready();
    return registry_.get_single_or_default(lifecycle_.operation_scope(), lifecycle_.token());
}

http_client_ref http_client_state::client(std::string_view alias) {
    lifecycle_.require_ready();
    return registry_.get(alias, lifecycle_.operation_scope(), lifecycle_.token());
}
}  // namespace ruvia::detail

namespace ruvia {

http_client::http_client(event_loop loop, const http_client_config& config,
    const http_client_worker_config& worker)
    : http_client(std::move(loop), std::span<const http_client_config>(&config, 1), worker) {}

http_client::http_client(event_loop loop, std::span<const http_client_config> configs,
    const http_client_worker_config& worker)
    : state_(std::make_shared<detail::http_client_state>(std::move(loop), configs, worker)) {
    state_->bind_stop();
}

http_client::~http_client() {
    state_->request_close();
}

http_client_ref http_client::client() const& {
    return state_->client();
}

http_client_ref http_client::client(std::string_view alias) const& {
    return state_->client(alias);
}

task<void> http_client::upsert(const http_client_config& config, operation_options options) & {
    return state_->upsert(config, std::move(options));
}

bool http_client::remove(std::string_view alias) & {
    return state_->remove(alias);
}

void http_client::close() noexcept {
    state_->request_close();
}

task<void> http_client::shutdown() & {
    return state_->shutdown();
}

const worker_ref& http_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
