#include "http_client.h"

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

#include <asio/cancellation_signal.hpp>

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "context.h"
#include "context_access.h"
#include "event_loop.h"
#include "http1_client_request.h"
#include "http1_client_response.h"
#include "http1_server_response.h"
#include "http_client_pool.h"
#include "http_client_types.h"
#include "http_connect_udp.h"
#include "http_content_coding.h"
#include "http_cookie.h"
#include "http_header.h"
#include "http_request.h"
#include "http_server.h"
#include "stop_token.h"
#include "streaming.h"
#include "streaming_session.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"
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

struct http_client_upload_end_input final {
    http_client_response pin_;
    std::vector<http_header> fields_;
};

task<http_client_response> http_client_response_handoff::receive(http_client_response pin, std::string_view head_message, operation_options operation) {
    return ::ruvia::make_scoped_operation(scope_, receive_owned(std::move(pin), head_message, std::move(operation)));
}

task<http_client_response> http_client_response_handoff::receive_owned(http_client_response pin, std::string_view head_message, operation_options operation) {
    // `pin` keeps the response state, and with it this handoff, alive.
    auto activity = claim_operation_lane(active_, "HTTP response receive is already active");
    if (taken_) {
        throw std::logic_error("HTTP response has already been transferred");
    }
    const operation_deadline redirect_deadline(operation.timeout_.has_value() ? operation.timeout_ : (pin.state_->upload_ && pin.state_->upload_->redirect_request_ && pin.state_->pool_ != nullptr ? std::optional<operation_timeout>(pin.state_->pool_->config().request_timeout_) : std::nullopt));
    {
        const http_client_operation_interruption interruption(*pin.state_, http_client_operation_lane::read, operation);
        co_await pin.state_->await_head(head_message);
    }
    taken_ = true;
    pin.consumer_ = true;
    if (pin.state_->upload_ && pin.state_->upload_->redirect_request_) {
        auto redirect_request = std::move(*pin.state_->upload_->redirect_request_);
        pin.state_->upload_->redirect_request_.reset();
        co_return co_await http_client_request::follow_redirects(std::move(redirect_request), std::move(pin), redirect_deadline, true);
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

http_client_exchange::http_client_exchange(http_client_response response) noexcept :
    response_(std::move(response)),
    body_(response_.state_) {}
http_client_exchange::http_client_exchange(http_client_exchange&& other) noexcept :
    response_(std::move(other.response_)),
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
    if (abandon_upload && upload.response_.taken() && !state_value->complete_ && state_value->pool_ != nullptr) {
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
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_, detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::string(bytes_value), std::move(operation)}));
}
task<void> http_client_request_body_writer::write_owned(std::string&& bytes_value, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    require_writable(*state_);
    if (bytes_value.size() > state_->upload_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_, detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{http_client_response(state_), std::move(bytes_value), std::move(operation)}));
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
    return ::ruvia::make_scoped_operation(state_->upload_->output_.output_scope_, detail::finish_client_output<upload_output_policy>(state_, detail::http_client_upload_end_input{http_client_response(state_), std::move(validation).take_fields()}, std::move(operation)));
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
    return state_value->upload_->response_.receive(http_client_response(state_value), "HTTP request failed before response head", std::move(operation));
}
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
    if (!state_value->tunnel_ || (state_value->pool_ == nullptr && !completed_finish) || (state_value->tunnel_->output_.stopped_ && !completed_finish) || !state_value->tunnel_->accepted_ || state_value->abandoned_ || state_value->failure_ || state_value->pool_closed_) {
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
http_client_tunnel::http_client_tunnel(http_client_response response) noexcept :
    response_(std::move(response)) {}
http_client_tunnel::http_client_tunnel(http_client_tunnel&& other) noexcept :
    response_(std::move(other.response_)) {}
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
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_, detail::write_client_output<tunnel_output_policy>(&state_value, detail::http_client_output_write_input{http_client_response(&state_value), std::string(bytes_value), std::move(operation)}));
}
task<void> http_client_tunnel::write_owned(std::string&& bytes_value, operation_options operation) {
    require_output(response_.state_, false);
    auto& state_value = *response_.state_;
    if (bytes_value.size() > state_value.tunnel_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP client tunnel chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_, detail::write_client_output<tunnel_output_policy>(&state_value, detail::http_client_output_write_input{http_client_response(&state_value), std::move(bytes_value), std::move(operation)}));
}
task<void> http_client_tunnel::finish(operation_options operation) & {
    require_output(response_.state_, true);
    auto& state_value = *response_.state_;
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_, detail::finish_client_output<tunnel_output_policy>(&state_value, http_client_response(&state_value), std::move(operation)));
}
}  // namespace ruvia

namespace ruvia::detail {

// Only adapts tunnel I/O. All WebSocket framing, lanes, control traffic,
// interruption and close policy live in websocket_session_connection.
class http_client_websocket_transport final {
public:
    http_client_websocket_transport(http_client_tunnel tunnel, connection_scanner::entry& activity) :
        tunnel_(std::move(tunnel)),
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
            slot.assign([this](asio::cancellation_type) {
                response_.data_signal_.notify();
            });
        }
        while (response_.offset_ == response_.buffered_.size() && response_.pending_.empty() && !response_.receive_complete() && !response_.failure_ && !response_.pool_closed_ && !response_.abandoned_) {
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
    client_websocket_session(http_client_pool& pool, http_client_tunnel tunnel, const http_client_websocket_handshake& handshake, websocket_client_deflate_offer deflate, websocket_lifecycle_options lifecycle, std::size_t message_limit) :
        worker_(pool.worker()),
        retire_signal_(worker_),
        scanner_(worker_, scanner_options(lifecycle)),
        connection_(http_client_websocket_transport(std::move(tunnel), entry_), worker_, entry_, normalize_websocket_lifecycle_options(lifecycle), protocol_byte_limit::limited(message_limit), {}, {.compression_ = handshake.compression_, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_key, .deflater_ = deflate.deflater_}, std::string(handshake.selected_subprotocol_)),
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

http_client_websocket_request::http_client_websocket_request(const http_client_ref& client, std::string_view path) :
    client_(client),
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
    auto user_stop = request.client_.options_.stop_token_.register_callback([&opening_stop] {
        opening_stop.request_stop();
    });
    bool opening_expired = false;
    worker_timer_registration opening_timer;
    const auto connect_timeout = request.request_options_.timeouts_.connect_.resolve(pool.config().connect_timeout_);
    if (connect_timeout) {
        (void)pool.worker().schedule_timer(opening_timer, deadline_after(connect_timeout->value()), [&opening_stop, &opening_expired](worker_timer_outcome outcome) noexcept {
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
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected), "WebSocket handshake rejected", opened->response()->status());
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
    origin_preparation(const http_client_config& source, const worker_ref& worker, stop_token owner_stop, stop_token user_stop, const std::optional<tls_trust_store>& system_trust) :
        alias_(source.alias_.view()),
        config_(source, client_tls_files::keep_paths),
        cancellation_(owner_stop, user_stop) {
        if (config_.scheme_ == http_scheme::https) {
            if (!config_.tls_.ca_file_.has_value()) {
                config_.tls_.trust_store_ = system_trust;
            }
            const auto password = source.tls_.identity_ ? std::string_view(source.tls_.identity_->private_key_password_) : std::string_view{};
            tls_.emplace(prepare_client_tls_config(worker, std::move(config_.tls_), password, cancellation_.token()));
        }
    }

    std::string alias_;
    http_client_config_storage config_;
    stop_group cancellation_;
    std::optional<task<client_tls_config_storage>> tls_;
};

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_ref& worker_value, std::span<const http_client_definition_type> definitions, const http_client_worker_services& services) :
    io_context_(io_context),
    worker_(worker_value),
    services_(services),
    retirements_(worker_) {
    validate_http_client_configs(definitions);
    initialize(definitions);
    preparations_.bind_worker(worker_);
}

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_ref& worker_value, std::span<const http_client_config> configs, const http_client_worker_services& services) :
    io_context_(io_context),
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
        storage.tls_.parse_files(config.tls_.identity_ ? std::string_view(config.tls_.identity_->private_key_password_) : std::string_view{});
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

task<void> http_client_registry::retire_origin(alias_map_type::iterator position, pool_owner_type replacement, task<void> joining) {
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
        if ((found == nullptr || binding.order_ < found->order_) && pool->scheme() == scheme && http_authorities_equal(pool->config().authority_, authority, scheme == http_scheme::https ? 443 : 80)) {
            found = &binding;
        }
    }
    return found == nullptr ? pool_owner_type{} : found->pool_;
}

class http_client_state final : public std::enable_shared_from_this<http_client_state> {
public:
    http_client_state(event_loop loop, std::span<const http_client_config> configs, const http_client_worker_config& worker_config);

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

http_client_state::http_client_state(event_loop loop, std::span<const http_client_config> configs, const http_client_worker_config& worker_config) :
    loop_(client_lifecycle<http_client_state>::require_loop(std::move(loop))),
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

http_client::http_client(event_loop loop, const http_client_config& config, const http_client_worker_config& worker) :
    http_client(std::move(loop), std::span<const http_client_config>(&config, 1), worker) {}

http_client::http_client(event_loop loop, std::span<const http_client_config> configs, const http_client_worker_config& worker) :
    state_(std::make_shared<detail::http_client_state>(std::move(loop), configs, worker)) {
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

namespace ruvia::detail {

void http_client_response_memory_domain::charge_in_flight(std::size_t bytes_value) {
    if (bytes_value != 0 && !result_budget_domain_->reserve_in_flight(bytes_value)) {
        throw http_client_error(make_error_code(http_client_errc::result_budget_exceeded), "HTTP client in-flight response memory budget is exhausted");
    }
}

void http_client_response_memory_domain::release_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value != 0) {
        result_budget_domain_->release_in_flight(bytes_value);
    }
}

http_client_response_memory_domain::http_client_response_memory_domain(const worker_ref& worker_value, const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) :
    worker_(worker_value),
    result_budget_domain_(result_budget_domain) {
    if (!worker_.valid()) {
        throw std::invalid_argument("HTTP client response memory requires a worker");
    }
}

http_client_response_memory_domain::~http_client_response_memory_domain() {
    if (phase_ != phase_type::prepared) {
        require_current();
    }
    if (state_head_ != nullptr || references_ != 0) {
        std::terminate();
    }
}

http_client_response_memory_domain::owner http_client_response_memory_domain::create(const worker_ref& worker_value, const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    return owner(new http_client_response_memory_domain(worker_value, result_budget_domain), deleter_type{});
}

void http_client_response_memory_domain::destroy() noexcept {
    delete this;
}

void http_client_response_memory_domain::deleter_type::operator()(http_client_response_memory_domain* domain) const noexcept {
    if (domain != nullptr) {
        domain->release_root();
    }
}

void http_client_response_memory_domain::require_current() const noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
}

void http_client_response_memory_domain::retain() noexcept {
    require_current();
    if (references_ == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references_;
}

void http_client_response_memory_domain::release() noexcept {
    require_current();
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        destroy();
    }
}

void http_client_response_memory_domain::release_root() noexcept {
    // Startup rollback has never published worker-affine response storage.
    // No worker scheduling is necessary (or possible after failed preparation).
    if (phase_ == phase_type::prepared) {
        if (references_ != 1 || state_head_ != nullptr) {
            std::terminate();
        }
        references_ = 0;
        destroy();
        return;
    }
    // Active storage is retired explicitly by the pool on its owning worker;
    // do not conceal a missing join with unstructured deferred destruction.
    release();
}

http_client_response_state* http_client_response_memory_domain::create_state(http_client_pool& pool) {
    require_current();
    if (phase_ == phase_type::retired) {
        throw std::logic_error("HTTP client response transport is retired");
    }
    phase_ = phase_type::active;
    charge_in_flight(sizeof(http_client_response_state));
    http_client_response_state* state_value = nullptr;
    try {
        state_value = new http_client_response_state(*this);
    } catch (...) {
        release_in_flight(sizeof(http_client_response_state));
        throw;
    }
    state_value->pool_ = &pool;
    attach_state(*state_value);
    return state_value;
}

void http_client_response_memory_domain::attach_state(http_client_response_state& state_value) noexcept {
    require_current();
    state_value.next_memory_state_ = state_head_;
    if (state_head_ != nullptr) {
        state_head_->previous_memory_state_ = &state_value;
    }
    state_head_ = &state_value;
    retain();
}

void http_client_response_memory_domain::destroy_state(http_client_response_state* state_value) noexcept {
    require_current();
    if (state_value == nullptr || &state_value->memory_domain_ != this) {
        std::terminate();
    }
    if (state_value->pool_ != nullptr) {
        unlink_state(*state_value);
    } else if (state_value->previous_memory_state_ != nullptr || state_value->next_memory_state_ != nullptr) {
        std::terminate();
    }
    delete state_value;
    release_in_flight(sizeof(http_client_response_state));
    release();
}

void http_client_response_memory_domain::unlink_state(http_client_response_state& state_value) noexcept {
    if (state_value.previous_memory_state_ != nullptr) {
        state_value.previous_memory_state_->next_memory_state_ = state_value.next_memory_state_;
    } else if (state_head_ == &state_value) {
        state_head_ = state_value.next_memory_state_;
    } else {
        std::terminate();
    }
    if (state_value.next_memory_state_ != nullptr) {
        state_value.next_memory_state_->previous_memory_state_ = state_value.previous_memory_state_;
    }
    state_value.previous_memory_state_ = nullptr;
    state_value.next_memory_state_ = nullptr;
}

void http_client_response_memory_domain::detach_transport_bindings(http_client_pool& pool) noexcept {
    require_current();
    phase_ = phase_type::retired;
    while (state_head_ != nullptr) {
        auto* state_value = state_head_;
        if (state_value->pool_ != &pool) {
            std::terminate();
        }
        // Revoke registration before notifying consumers. A continuation may
        // release this state or a sibling; never cache a next pointer across it.
        unlink_state(*state_value);
        state_value->detach_transport_bindings();
    }
}

http_client_response_state::http_client_response_state(http_client_response_memory_domain& memory_domain) :
    head_signal_(memory_domain.worker()),
    data_signal_(memory_domain.worker()),
    space_signal_(memory_domain.worker()),
    memory_domain_(memory_domain) {
    body_operation_scope_.bind_worker(memory_domain.worker());
}

void http_client_response_state::require_owner_worker(const char* message) const {
    if (!memory_domain_.worker().is_current()) {
        throw std::logic_error(message);
    }
}

void http_client_response_state::detach_transport_bindings() noexcept {
    if (tunnel_) {
        tunnel_->output_.stop();
    }
    if (upload_) {
        upload_->output_.stop();
    }
#ifdef RUVIA_ENABLE_HTTP2
    http2_data_credit_.reset();
#endif
#ifdef RUVIA_ENABLE_HTTP3
    http3_data_credit_.reset();
#endif
    connection_index_ = 0;
    connection_serial_ = 0;
    request_id_ = 0;
    cancellation_id_ = 0;
    stream_id_ = 0;
    pool_ = nullptr;
    transport_ = http_client_response_transport::unassigned;
    if (!complete_) {
        pool_closed_ = true;
        complete_ = true;
    }
    head_signal_.notify();
    data_signal_.notify();
    space_signal_.notify();
}

http_client_result_budget_domain::http_client_result_budget_domain(http_client_result_budget_config config) :
    max_retained_bytes_(config.max_retained_bytes_),
    max_in_flight_bytes_(config.max_in_flight_bytes_) {
    if (max_retained_bytes_ == 0 || max_in_flight_bytes_ == 0) {
        throw std::invalid_argument("HTTP client result byte budget must be greater than zero");
    }
}

bool http_client_result_budget_domain::reserve_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value > max_in_flight_bytes_ - in_flight_bytes_) {
        return false;
    }
    in_flight_bytes_ += bytes_value;
    return true;
}

void http_client_result_budget_domain::release_in_flight(std::size_t bytes_value) noexcept {
    if (bytes_value > in_flight_bytes_) {
        std::terminate();
    }
    in_flight_bytes_ -= bytes_value;
}

bool http_client_result_budget_domain::try_reserve(std::size_t bytes_value) noexcept {
    auto retained = retained_bytes_.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes_value > max_retained_bytes_ - retained) {
            return false;
        }
        if (retained_bytes_.compare_exchange_weak(retained, retained + bytes_value, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void http_client_result_budget_domain::release(std::size_t bytes_value) noexcept {
    const auto previous = retained_bytes_.fetch_sub(bytes_value, std::memory_order_acq_rel);
    if (previous < bytes_value) {
        std::terminate();
    }
}

http_client_result_budget_lease::http_client_result_budget_lease(const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept :
    domain_(domain),
    bytes_(bytes_value) {}

http_client_result_budget_lease::http_client_result_budget_lease(http_client_result_budget_lease&& other) noexcept :
    domain_(std::move(other.domain_)),
    bytes_(std::exchange(other.bytes_, 0)) {}

http_client_result_budget_lease::~http_client_result_budget_lease() {
    if (domain_ != nullptr) {
        domain_->release(bytes_);
    }
}

std::optional<http_client_result_budget_lease> http_client_result_budget_lease::try_acquire(const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept {
    if (!domain->try_reserve(bytes_value)) {
        return std::nullopt;
    }
    return http_client_result_budget_lease(domain, bytes_value);
}

}  // namespace ruvia::detail

namespace ruvia {

http_client_response_bytes::http_client_response_bytes(std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value) :
    data_(reserve_bytes == 0 ? nullptr : std::make_unique_for_overwrite<std::byte[]>(reserve_bytes)) {
    auto [domain, reserved] = std::move(lease_value).release();
    budget_domain_ = std::move(domain);
    reserved_ = reserved;
}

http_client_response_bytes::http_client_response_bytes(http_client_response_bytes&& other) noexcept :
    budget_domain_(std::move(other.budget_domain_)),
    data_(std::move(other.data_)),
    size_(std::exchange(other.size_, 0)),
    reserved_(std::exchange(other.reserved_, 0)) {}

http_client_response_bytes& http_client_response_bytes::operator=(http_client_response_bytes&& other) noexcept {
    if (this != &other) {
        release();
        budget_domain_ = std::move(other.budget_domain_);
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
        reserved_ = std::exchange(other.reserved_, 0);
    }
    return *this;
}

http_client_response_bytes::~http_client_response_bytes() {
    release();
}

void http_client_response_bytes::release() noexcept {
    // Free the owned byte buffer before making its reservation available to another result.
    data_.reset();
    if (budget_domain_ != nullptr) {
        budget_domain_->release(reserved_);
        budget_domain_.reset();
    }
    size_ = 0;
    reserved_ = 0;
}

void http_client_response_bytes::append(std::span<const std::byte> bytes_value) noexcept {
    if (!bytes_value.empty()) {
        std::memcpy(data_.get() + size_, bytes_value.data(), bytes_value.size());
        size_ += bytes_value.size();
    }
}

}  // namespace ruvia

namespace ruvia::detail {

http_client_worker_limits::http_client_worker_limits(const http_client_worker_config& config) :
    max_idle_http1_(config.max_idle_http1_),
    max_idle_multiplexed_(config.max_idle_multiplexed_),
    max_dns_refreshes_(config.max_concurrent_dns_refreshes_) {
    if (max_dns_refreshes_ == 0) {
        throw std::invalid_argument("HTTP client DNS refresh concurrency must be greater than zero");
    }
}

void http_client_worker_limits::join(http_client_pool& pool) {
    if (std::ranges::find(pools_, &pool) == pools_.end()) {
        pools_.push_back(&pool);
    }
}

void http_client_worker_limits::leave(http_client_pool& pool) noexcept {
    std::erase(pools_, &pool);
}

void http_client_worker_limits::enforce_idle_limits() noexcept {
    enforce_idle_limit(false, max_idle_http1_);
    enforce_idle_limit(true, max_idle_multiplexed_);
}

void http_client_worker_limits::enforce_idle_limit(bool multiplexed, std::size_t maximum) noexcept {
    const auto total = [this, multiplexed] {
        std::size_t count = 0;
        for (const auto* pool : pools_) {
            count += pool->idle_count(multiplexed);
        }
        return count;
    };
    if (total() <= maximum) {
        return;
    }
    // Pool counters are upper bounds; recount before closing anything.
    for (auto* pool : pools_) {
        pool->recount_idle();
    }
    for (auto remaining = total(); remaining > maximum; --remaining) {
        http_client_pool* oldest = nullptr;
        std::optional<std::uint64_t> oldest_order;
        for (auto* pool : pools_) {
            const auto order = pool->least_recent_idle(multiplexed);
            if (order && (!oldest_order || *order < *oldest_order)) {
                oldest = pool;
                oldest_order = order;
            }
        }
        if (oldest == nullptr || !oldest->close_least_recent_idle(multiplexed)) {
            return;
        }
    }
}

bool http_client_worker_limits::try_begin_dns_refresh() noexcept {
    if (dns_refreshes_ == max_dns_refreshes_) {
        return false;
    }
    ++dns_refreshes_;
    return true;
}

void http_client_worker_limits::end_dns_refresh() noexcept {
    --dns_refreshes_;
}

http_client_worker_services make_http_client_worker_services(const http_client_worker_config& config) {
    return {.result_budget_ = std::make_shared<http_client_result_budget_domain>(config.result_budget_), .limits_ = std::make_shared<http_client_worker_limits>(config)};
}

client_request_policy::client_request_policy(const http_client_config_storage& config) :
    config_(config),
    jar_(config.scheme_, config.host_, {.max_cookies_ = config.max_cookies_, .max_cookie_bytes_ = config.max_cookie_bytes_}) {
    for (const auto& cookie : config.cookies_) {
        jar_.add_configured_cookie(cookie.name(), cookie.value());
    }
}

bool client_request_policy::addresses_origin(const http_client_request_storage& request) const noexcept {
    return (!request.is_tunnel() || request.is_upgrade()) && request.route().alias_origin_;
}

http_client_request_view client_request_policy::prepare_request(const http_client_request_storage& request, client_prepared_request_scratch& scratch) {
    const auto& input_headers = request.headers();
    const auto max_prepared_headers = input_headers.size() + 2;
    http_header_view* prepared_begin = scratch.inline_headers_.data();
    if (max_prepared_headers > scratch.inline_headers_.size()) {
        scratch.overflow_headers_.resize(max_prepared_headers);
        prepared_begin = scratch.overflow_headers_.data();
    }
    std::size_t index = 0;
    bool add_user_agent = !config_.user_agent_.empty();
    scratch.overflow_cookie_header_.clear();
    std::size_t inline_cookie_size = 0;
    bool cookie_overflowed = false;
    // RFC 6265 section 4.2.1: cookie-pairs are joined by "; ".
    const auto ensure_cookie_space = [&](std::size_t count) -> char* {
        const auto current_size = cookie_overflowed ? scratch.overflow_cookie_header_.size() : inline_cookie_size;
        const auto separator_bytes = current_size == 0 ? std::size_t{0} : std::size_t{2};
        const auto total_needed = separator_bytes + count;
        if (total_needed == 0) {
            return scratch.inline_cookie_header_.data();
        }
        if (!cookie_overflowed && total_needed <= scratch.inline_cookie_header_.size() - inline_cookie_size) {
            char* cursor = scratch.inline_cookie_header_.data() + inline_cookie_size;
            if (separator_bytes != 0) {
                *cursor++ = ';';
                *cursor++ = ' ';
            }
            inline_cookie_size += total_needed;
            return cursor;
        }
        if (!cookie_overflowed) {
            scratch.overflow_cookie_header_.reserve(std::max(inline_cookie_size + total_needed, scratch.inline_cookie_header_.size() * 2));
            scratch.overflow_cookie_header_.append(scratch.inline_cookie_header_.data(), inline_cookie_size);
            cookie_overflowed = true;
        }
        const auto old_size = scratch.overflow_cookie_header_.size();
        if (old_size + total_needed > scratch.overflow_cookie_header_.capacity()) {
            scratch.overflow_cookie_header_.reserve(std::max(old_size + total_needed, scratch.overflow_cookie_header_.capacity() * 2));
        }
        scratch.overflow_cookie_header_.resize(old_size + total_needed);
        char* cursor = scratch.overflow_cookie_header_.data() + old_size;
        if (separator_bytes != 0) {
            *cursor++ = ';';
            *cursor++ = ' ';
        }
        return cursor;
    };
    // Request storage preserves caller field-name spelling.
    for (const auto& field : input_headers) {
        const auto name = field.name();
        if (add_user_agent && name.size() == 10U && http_ascii_equals_ignore_case(name, "user-agent")) {
            add_user_agent = false;
        }
        if (name.size() == 6U && http_ascii_equals_ignore_case(name, "cookie")) {
            const auto value = field.value();
            std::ranges::copy(value, ensure_cookie_space(value.size()));
            continue;
        }
        prepared_begin[index++] = http_header_view{name, field.value()};
    }

    // RFC 9110 §9.3.8: TRACE reflects the request, so stored credentials stay
    // out; caller-written Cookie fields above remain the caller's choice.
    // Stored cookies belong to the fixed origin (RFC 6265 §5.4): a tunnel to
    // another authority carries none. Empty jars have no expiry or path
    // constraints to inspect.
    if (!jar_.empty() && request.method() != "TRACE" && addresses_origin(request)) {
        const bool include_received = config_.received_cookies_ != http_client_received_cookie_policy::ignore;
        jar_.visit_matching_cookies(request.target(), include_received, std::chrono::system_clock::now(), [&](std::string_view name, std::string_view value) {
            const auto pair_size = cookie_request_pair_size(name, value);
            (void)write_cookie_request_pair(ensure_cookie_space(pair_size), name, value);
        });
    }
    const auto cookie_header_view = cookie_overflowed ? std::string_view(scratch.overflow_cookie_header_) : std::string_view(scratch.inline_cookie_header_.data(), inline_cookie_size);
    if (add_user_agent) {
        prepared_begin[index++] = http_header_view{"user-agent", config_.user_agent_};
    }
    if (!cookie_header_view.empty()) {
        prepared_begin[index++] = http_header_view{"cookie", cookie_header_view};
    }
    http_client_request_view result;
    result.method_ = request.method();
    result.target_ = request.target();
    result.headers_ = std::span<const http_header_view>(prepared_begin, index);
    result.content_ = request.has_body() ? http_client_request_content_view::bytes(request.body()) : http_client_request_content_view::none();
    return result;
}

void client_request_policy::retain_response_cookies(std::string_view request_target, std::span<const http_header> headers) {
    if (config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
        return;
    }
    jar_.retain_response_cookies(request_target, headers);
}

namespace {

// An encoded body must be collected to FIN before any bytes can be returned
// to the consumer.
void configure_http_client_response_decoding(http_client_response_state& state_value) {
    state_value.body_decode_required_ = false;
    state_value.content_coding_.reset();
    const auto* plan = state_value.response_body_plan_ ? &*state_value.response_body_plan_ : nullptr;
    // A 206 body (single range or multipart/byteranges) holds slices of the
    // encoded representation that cannot be decoded alone (RFC 9110 §14,
    // §15.3.7); it is exposed as received, with Content-Encoding kept.
    if (!state_value.decode_content_ || plan == nullptr || plan->body_suppressed() || !plan->status_allows_body() || plan->content_semantics() != http_response_content_semantics::with_content || state_value.status_.status() == http_status::partial_content) {
        return;
    }
    // Parsed once with the final head; decoding reuses this result at FIN.
    auto& parsed_coding = state_value.content_coding_.emplace(parse_http_content_coding_headers(state_value.headers_));
    // Content codings are an open registry (RFC 9110 §8.4.1, §16.6.1), and a
    // request without Accept-Encoding accepts every coding (§12.5.3). A
    // syntactically valid stack this client cannot fully decode is therefore
    // exposed as received, with Content-Encoding kept, like a 206 body.
    if (const auto* unsupported = parsed_coding.unsupported()) {
        if (!unsupported->depth_exceeded()) {
            state_value.content_coding_.reset();
            return;
        }
    }
    const bool transforms_body = std::ranges::any_of(parsed_coding.codings(), [](http_content_coding coding) {
        return coding != http_content_coding::identity;
    });
    state_value.body_decode_required_ = parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr || transforms_body;
    if (state_value.body_decode_required_) {
        state_value.collect_all_ = true;
    } else {
        state_value.content_coding_.reset();
    }
}

// Uses the parser-computed body plan and keeps decoded output bounded by the
// response's configured byte limit.
void decode_http_client_response_body(http_client_response_state& state_value, bool content_semantics_present, std::size_t max_decoded_bytes) {
    if (!content_semantics_present || !state_value.body_decode_required_) {
        return;
    }
    try {
        const auto& parsed_coding = *state_value.content_coding_;
        if (parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid HTTP response Content-Encoding");
        }
        // Encoded bytes are never exposed, so the unread part of buffered_ and
        // pending_ together are the whole body. Decode in place when one holds it.
        const auto buffered_bytes = state_value.buffered_.size() - state_value.offset_;
        std::string joined;
        std::string_view encoded;
        if (state_value.pending_.empty()) {
            encoded = std::string_view(state_value.buffered_).substr(state_value.offset_);
        } else if (buffered_bytes == 0) {
            encoded = state_value.pending_;
        } else {
            if (state_value.pending_.size() > std::numeric_limits<std::size_t>::max() - buffered_bytes) {
                throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP response exceeds configured byte limit");
            }
            joined.reserve(buffered_bytes + state_value.pending_.size());
            joined.append(state_value.buffered_.data() + state_value.offset_, buffered_bytes);
            joined.append(state_value.pending_);
            encoded = joined;
        }

        const auto decoded_limit = std::min(max_decoded_bytes, state_value.buffered_limit_);
        auto decoded = decode_http_content(parsed_coding.codings(), encoded, {.max_decoded_bytes_ = decoded_limit});
        if (auto* decoded_body = std::get_if<std::string>(&decoded)) {
            std::string empty_buffered;
            std::string empty_pending;
            state_value.buffered_.swap(empty_buffered);
            state_value.pending_.swap(empty_pending);
            state_value.buffered_.swap(*decoded_body);
            state_value.offset_ = 0;
            state_value.body_decode_required_ = false;
            state_value.content_coding_.reset();
            std::erase_if(state_value.headers_, [](const http_header& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), "Content-Encoding") || http_ascii_equals_ignore_case(field.name(), "Content-Length");
            });
            state_value.notify_producer_space();
            return;
        }
        const auto* failure = std::get_if<http_content_decode_error>(&decoded);
        if (failure != nullptr && *failure == http_content_decode_error::decoded_size_exceeded) {
            throw http_client_error(make_error_code(http_client_errc::response_too_large), "HTTP response exceeds configured byte limit");
        }
        throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid HTTP response Content-Encoding");
    } catch (...) {
        // Encoded bytes were never exposed: the head selected collection before
        // waking any consumer. Release that retained input on every decode error.
        state_value.content_coding_.reset();
        state_value.discard_response_body();
        throw;
    }
}

[[noreturn]] void throw_body_decoding_timeout() {
    throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP response body decoding timed out");
}

}  // namespace

void accept_extended_connect_websocket_response(http_client_websocket_handshake& websocket, http_status_code status, std::span<const http_header> headers, bool stream_ended, std::string_view error_message) {
    if (!status.is_successful() || stream_ended) {
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected), error_message, status);
    }
    const auto negotiated = websocket.negotiation_.validate_fields(headers);
    const auto* selected = std::get_if<websocket_client_negotiation_result_view>(&negotiated);
    if (selected == nullptr) {
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected), error_message, status);
    }
    websocket.selected_subprotocol_.assign(selected->selected_subprotocol_);
    websocket.compression_ = selected->compression_;
}

void retain_http_client_binary_response_head(http_client_response_state& state_value, http_client_response_head&& head) {
    const auto status = head.status();
    state_value.content_length_ = head.content_length();
    auto fields = std::move(head).take_headers();
    state_value.headers_ = std::move(fields);
    const auto body_plan = plan_http_response_body(state_value.request_method_, status);
    state_value.framing_ = body_plan.content_semantics() == http_response_content_semantics::connect_tunnel
        ? http_client_response_framing::tunnel
        : body_plan.body_suppressed()
        ? http_client_response_framing::none
        : http_client_response_framing::stream;
}

void publish_http_client_final_head(http_client_response_state& state_value, client_request_policy& policy, const http_client_final_head& head) {
    if (state_value.upload_ && !state_value.upload_->output_.ended_) {
        const auto status = head.status_.status();
        const bool rewrite_upload_redirect = state_value.upload_->redirect_request_ != nullptr && (status == http_status::see_other || ((status == http_status::moved_permanently || status == http_status::found) && state_value.request_method_ == http_known_method::post));
        if (head.request_content_signal_ == http_client_request_content_signal::exchange_complete || rewrite_upload_redirect) {
            if (rewrite_upload_redirect) {
                state_value.upload_->abandoned_ = true;
            }
            state_value.upload_->output_.stop();
        }
    }
    state_value.status_ = head.status_;
    state_value.protocol_version_ = head.protocol_version_;
    state_value.response_body_plan_ = plan_http_response_body(state_value.request_method_, state_value.status_.status());
    // A received final head changes the jar before consumers can issue
    // another request, regardless of body completion or abandonment.
    if (head.origin_target_) {
        policy.retain_response_cookies(*head.origin_target_, state_value.headers_);
    }
#ifdef RUVIA_ENABLE_HTTP3
    if (state_value.route_ != nullptr && state_value.pool_ != nullptr) {
        state_value.pool_->observe_alt_svc(*state_value.route_, state_value.headers_);
    }
#endif
    auto* const tunnel = state_value.tunnel_ ? &*state_value.tunnel_ : nullptr;
    if (tunnel != nullptr) {
        if (head.opens_tunnel_ && tunnel->udp_ && validate_http_connect_udp_response(state_value.protocol_version_, state_value.status_.status().value(), state_value.headers_).has_value()) {
            throw http_client_error(make_error_code(http_client_errc::protocol_error), "invalid CONNECT-UDP response head");
        }
        tunnel->accepted_ = head.opens_tunnel_;
        if (!tunnel->accepted_) {
            tunnel->output_.stop();
        }
    }
    if (tunnel == nullptr || !tunnel->accepted_) {
        configure_http_client_response_decoding(state_value);
    }
    state_value.head_ready_ = true;
    state_value.head_signal_.notify();
}

void finish_http_client_response_body(http_client_response_state& state_value, const operation_deadline& timeout, bool content_semantics_present, std::size_t max_decoded_bytes) {
    if (timeout.expired()) {
        throw_body_decoding_timeout();
    }
    decode_http_client_response_body(state_value, content_semantics_present, max_decoded_bytes);
    if (timeout.expired()) {
        throw_body_decoding_timeout();
    }
}

class http_client_advertisement_memory final {
public:
    explicit http_client_advertisement_memory(const worker_ref& worker_value) :
        worker_(worker_value) {}

    void retain(std::size_t bytes_value) noexcept {
        require_current();
        if (references_ == std::numeric_limits<std::size_t>::max() || bytes_value > std::numeric_limits<std::size_t>::max() - retained_bytes_) {
            std::terminate();
        }
        ++references_;
        retained_bytes_ += bytes_value;
        active_ = true;
    }
    void release(std::size_t bytes_value) noexcept {
        require_current();
        if (bytes_value > retained_bytes_ || references_ <= 1) {
            std::terminate();
        }
        retained_bytes_ -= bytes_value;
        --references_;
        if (retired_ && references_ == 1) {
            destroy();
        }
    }
    void retire() noexcept {
        if (active_) {
            require_current();
        }
        if (retired_) {
            std::terminate();
        }
        retired_ = true;
        if (references_ == 1) {
            destroy();
        }
    }
    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        return retained_bytes_;
    }
    void require_current() const noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
    }

private:
    void destroy() noexcept {
        if (retained_bytes_ != 0 || references_ != 1) {
            std::terminate();
        }
        delete (this);
    }
    worker_ref worker_;
    std::size_t references_{1};
    std::size_t retained_bytes_{};
    bool active_{};
    bool retired_{};
};

struct http_client_advertisement_state final {
    http_client_advertisement_state(http_client_advertisement_memory& owner_value, std::size_t slot, http_protocol_version protocol, const http_origin_advertisement& origins, std::size_t bytes_value) :
        memory_(owner_value),
        slot_(slot),
        protocol_(protocol),
        bytes_(bytes_value),
        value_(std::in_place_type<http_origin_advertisement>) {
        auto& retained = std::get<http_origin_advertisement>(value_);
        retained.origins_.reserve(origins.origins_.size());
        for (const auto& origin : origins.origins_) {
            retained.origins_.emplace_back(origin);
        }
        memory_.retain(bytes_value);
    }
    http_client_advertisement_state(http_client_advertisement_memory& owner_value, std::size_t slot, const http_alternative_service_advertisement& service, std::size_t bytes_value) :
        memory_(owner_value),
        slot_(slot),
        protocol_(http_protocol_version::http2),
        bytes_(bytes_value),
        value_(std::in_place_type<http_alternative_service_advertisement>) {
        auto& retained = std::get<http_alternative_service_advertisement>(value_);
        retained.stream_id_ = service.stream_id_;
        retained.origin_.assign(service.origin_);
        retained.field_value_.assign(service.field_value_);
        memory_.retain(bytes_value);
    }
    http_client_advertisement_memory& memory_;
    std::size_t slot_;
    http_protocol_version protocol_;
    std::size_t bytes_;
    std::variant<http_origin_advertisement, http_alternative_service_advertisement> value_;
};

namespace {
void release_advertisement(http_client_advertisement_state* state_value) noexcept {
    if (state_value == nullptr) {
        return;
    }
    auto& memory = state_value->memory_;
    memory.require_current();
    const auto bytes_value = state_value->bytes_;
    delete (state_value);
    memory.release(bytes_value);
}
}  // namespace

http_client_advertisement_queue::http_client_advertisement_queue(const worker_ref& worker_value, http_client_advertisement_config config) :
    worker_(worker_value),
    config_(config),
    pending_() {
    if (!worker_value.valid() || config.max_queued_advertisements_ == 0 || config.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement queue requires worker and positive bounds");
    }
    if (config.receive_origins_ || config.receive_alternative_services_) {
        memory_ = new http_client_advertisement_memory(worker_value);
    }
}
http_client_advertisement_queue::~http_client_advertisement_queue() {
    pending_.clear();
    if (memory_ != nullptr) {
        memory_->retire();
    }
}
void http_client_advertisement_queue::require_current() const noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
}
bool http_client_advertisement_queue::admit(std::size_t bytes_value) noexcept {
    if (memory_ == nullptr || pending_.size() >= config_.max_queued_advertisements_ || bytes_value > config_.max_retained_bytes_ - std::min(config_.max_retained_bytes_, memory_->retained_bytes())) {
        ++dropped_;
        return false;
    }
    return true;
}
void http_client_advertisement_queue::retain(std::size_t slot, http_protocol_version protocol, const http_origin_advertisement& origins) {
    require_current();
    if (!config_.receive_origins_) {
        return;
    }
    if (origins.origins_.size() > (config_.max_retained_bytes_ / sizeof(std::string))) {
        ++dropped_;
        return;
    }
    auto bytes_value = sizeof(http_client_advertisement_state) + origins.origins_.size() * sizeof(std::string);
    for (const auto& origin : origins.origins_) {
        if (origin.size() > config_.max_retained_bytes_ - std::min(bytes_value, config_.max_retained_bytes_)) {
            ++dropped_;
            return;
        }
        bytes_value += origin.size();
    }
    if (!admit(bytes_value)) {
        return;
    }
    auto owned = std::make_unique<http_client_advertisement_state>(*memory_, slot, protocol, origins, bytes_value);
    http_client_advertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
}
void http_client_advertisement_queue::retain(std::size_t slot, const http_alternative_service_advertisement& service) {
    require_current();
    if (!config_.receive_alternative_services_) {
        return;
    }
    auto bytes_value = sizeof(http_client_advertisement_state);
    for (const auto size : {service.origin_.size(), service.field_value_.size()}) {
        if (size > config_.max_retained_bytes_ - std::min(bytes_value, config_.max_retained_bytes_)) {
            ++dropped_;
            return;
        }
        bytes_value += size;
    }
    if (!admit(bytes_value)) {
        return;
    }
    auto owned = std::make_unique<http_client_advertisement_state>(*memory_, slot, service, bytes_value);
    http_client_advertisement advertisement(owned.release());
    pending_.push_back(std::move(advertisement));
}
std::optional<http_client_advertisement> http_client_advertisement_queue::next() {
    require_current();
    if (pending_.empty()) {
        return std::nullopt;
    }
    std::optional<http_client_advertisement> advertisement(std::in_place, std::move(pending_.front()));
    pending_.pop_front();
    return advertisement;
}
void http_client_advertisement_queue::retire() noexcept {
    require_current();
    pending_.clear();
    if (auto* memory = std::exchange(memory_, nullptr)) {
        memory->retire();
    }
}

}  // namespace ruvia::detail

namespace ruvia {
http_client_advertisement::http_client_advertisement(http_client_advertisement&& other) noexcept :
    state_(std::exchange(other.state_, nullptr)) {}
http_client_advertisement& http_client_advertisement::operator=(http_client_advertisement&& other) noexcept {
    if (this != &other) {
        detail::release_advertisement(state_);
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
http_client_advertisement::~http_client_advertisement() {
    detail::release_advertisement(state_);
}
http_protocol_version http_client_advertisement::protocol_version() const noexcept {
    return state_ == nullptr ? http_protocol_version::http11 : state_->protocol_;
}
std::size_t http_client_advertisement::connection_slot() const noexcept {
    return state_ == nullptr ? 0 : state_->slot_;
}
const http_origin_advertisement* http_client_advertisement::origins() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<http_origin_advertisement>(&state_->value_);
}
const http_alternative_service_advertisement* http_client_advertisement::alternative_service() const& noexcept {
    return state_ == nullptr ? nullptr : std::get_if<http_alternative_service_advertisement>(&state_->value_);
}
}  // namespace ruvia
