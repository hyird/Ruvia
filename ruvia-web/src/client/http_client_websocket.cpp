#include "ruvia/web/http_client_websocket.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/websocket_connection.h"

#include "client/http_client_pool.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_state.h"
#include "http/inline_http_header_views.h"
#include "tls/tls_random.h"
#include "util/operation_lane_lease.h"
#include "util/pmr_owned_storage.h"
#include "websocket/http_websocket_liveness.h"
#include "websocket/websocket_lifecycle_validation.h"

namespace ruvia::detail {

class http_client_websocket_state final {
public:
    http_client_websocket_state(http_client_tunnel tunnel, const worker_handle& worker,
        const http_client_websocket_handshake& handshake, websocket_client_deflate_offer deflate,
        operation_options options,
        websocket_lifecycle_options lifecycle, std::size_t message_limit,
        std::optional<std::chrono::milliseconds> read_timeout)
        : worker_(worker),
          tunnel_(std::move(tunnel)),
          transport_state_(*tunnel_.response_.state_),
          input_(transport_state_.resource_),
          protocol_(input_, {.message_limit_ = protocol_byte_limit::limited(message_limit),
                                .compression_ = handshake.compression_,
                                .role_ = websocket_connection_role::client,
                                .mask_key_generator_ = &mask_key,
                                .deflater_ = deflate.deflater_}),
          subprotocol_(handshake.selected_subprotocol_, transport_state_.resource_),
          options_(std::move(options)),
          lifecycle_(normalize_websocket_lifecycle_options(lifecycle)),
          read_timeout_(read_timeout),
          write_lane_(worker),
          heartbeat_signal_(worker),
          cancellation_(make_worker_cancellation_target(*this, worker)) {}

    ~http_client_websocket_state() {
        cancellation_->detach(*this);
    }

    void require_current() const {
        if (!worker_.is_current()) {
            throw std::logic_error("WebSocket requires its origin worker");
        }
    }

    void check() const {
        if (failure_) {
            throw http_client_error(*failure_, "WebSocket operation terminated");
        }
        if (ended_) {
            throw http_client_error(http_client_error::code_type::closing, "WebSocket is closed");
        }
    }

    void abort(http_client_error::code_type reason = http_client_error::code_type::cancelled) noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (!failure_ && !ended_) {
            failure_ = reason;
        }
        heartbeat_timer_.cancel();
        tunnel_.abort();
        heartbeat_signal_.notify();
        write_lane_.notify();
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept {
        if (id != 0 && (id == read_id_ || id == write_id_)) {
            abort();
        }
    }

    std::pmr::memory_resource* resource() noexcept {
        return transport_state_.resource_;
    }

    std::string_view subprotocol() const noexcept {
        return subprotocol_;
    }

    http_protocol_version protocol_version() const {
        return tunnel_.protocol_version();
    }
    bool connected() const noexcept {
        return !failure_ && !ended_ && !transport_state_.pool_closed_ &&
               !transport_state_.failure_ && !transport_state_.tunnel_->output_.stopped_ &&
               protocol_.liveness_mode() == websocket_liveness_mode::open;
    }
    std::optional<std::uint16_t> peer_close_code() const noexcept {
        return peer_close_.code();
    }
    std::string_view peer_close_reason() const noexcept {
        return peer_close_.reason();
    }
    const worker_handle& worker() const noexcept {
        return worker_;
    }

    static task<void> heartbeat(std::shared_ptr<http_client_websocket_state> state) {
        try {
            state->refresh_heartbeat();
            while (state->heartbeat_open()) {
                if (std::exchange(state->heartbeat_due_, false)) {
                    co_await state->write_lane_.wait_for_idle(false, [&] { state->check(); });
                    if (state->heartbeat_decision() == websocket_liveness_decision::send_ping) {
                        write_guard lane(*state);
                        const auto challenge = ++state->heartbeat_sequence_;
                        state->liveness_ = websocket_sending_ping(challenge);
                        const auto payload = websocket_heartbeat_payload(challenge);
                        if (state->protocol_.submit_frame(websocket_opcode::ping,
                                std::string_view(payload.data(), payload.size())) != websocket_frame_submit_status::accepted) {
                            throw http_client_error(http_client_error::code_type::protocol_error, "WebSocket heartbeat rejected");
                        }
                        co_await state->flush();
                        if (std::holds_alternative<websocket_sending_ping>(state->liveness_)) {
                            state->liveness_ = websocket_awaiting_pong(steady_now_ms(), challenge);
                        }
                        lane.commit();
                    }
                }
                state->refresh_heartbeat();
                if (state->heartbeat_open()) {
                    co_await state->heartbeat_signal_.wait();
                }
            }
        } catch (...) {
            state->abort(http_client_error::code_type::closing);
        }
        state->heartbeat_timer_.cancel();
    }

    static task<void> write(std::shared_ptr<http_client_websocket_state> state,
        websocket_opcode opcode, std::pmr::string payload, websocket_send_options options,
        operation_options operation_options_value) {
        state->require_current();
        auto active = claim_operation_lane(state->write_active_, "concurrent WebSocket sends are not supported");
        operation_guard operation(*state, state->write_id_, std::move(operation_options_value));
        try {
            co_await state->write_lane_.wait_for_idle(false, [&] { state->check(); });
            write_guard lane(*state);
            const auto submitted = state->protocol_.submit_frame(opcode, payload, options.compress_);
            std::pmr::string(payload.get_allocator()).swap(payload);
            switch (submitted) {
                case websocket_frame_submit_status::accepted:
                    break;
                case websocket_frame_submit_status::message_too_large:
                    throw http_client_error(http_client_error::code_type::message_too_large, "WebSocket message exceeds configured limit");
                case websocket_frame_submit_status::not_open:
                    throw http_client_error(http_client_error::code_type::closing, "WebSocket is closing");
                default:
                    throw http_client_error(http_client_error::code_type::protocol_error, "invalid WebSocket message");
            }
            co_await state->flush();
            lane.commit();
        } catch (...) {
            state->abort();
            if (state->failure_ == http_client_error::code_type::timeout) {
                state->check();
            }
            throw;
        }
    }

    static task<std::optional<websocket_message>> receive(std::shared_ptr<http_client_websocket_state> state,
        operation_options options) {
        state->require_current();
        auto active = claim_operation_lane(state->read_active_, "concurrent WebSocket receives are not supported");
        if (state->ended_ && !state->failure_) {
            co_return std::nullopt;
        }
        operation_guard operation(*state, state->read_id_, std::move(options));
        try {
            state->resume_pong_deadline();
            for (;;) {
                const auto event = co_await state->next_event();
                if (const auto* message = event.message()) {
                    co_return *message;
                }
                if (event.close() || event.transport_end()) {
                    co_await state->finish_close();
                    co_return std::nullopt;
                }
            }
        } catch (...) {
            state->abort();
            if (state->failure_ == http_client_error::code_type::timeout) {
                state->check();
            }
            throw;
        }
    }

    static task<void> close(std::shared_ptr<http_client_websocket_state> state,
        websocket_close_code code, std::pmr::string reason, operation_options options) {
        state->require_current();
        auto read_active = claim_operation_lane(state->read_active_, "WebSocket close cannot overlap receive");
        auto write_active = claim_operation_lane(state->write_active_, "WebSocket close cannot overlap send");
        if (state->ended_ && !state->failure_) {
            co_return;
        }
        operation_guard operation(*state, state->read_id_, std::move(options));
        worker_timer_registration close_timer;
        if (const auto bound = state->lifecycle_.close_handshake_timeout_) {
            (void)state->worker_.schedule_timer(close_timer, worker_timer_deadline_after(*bound),
                [state = state.get()](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        state->abort(http_client_error::code_type::timeout);
                    }
                });
        }
        try {
            {
                co_await state->write_lane_.wait_for_idle(false, [&] { state->check(); });
                write_guard lane(*state);
                if (state->protocol_.submit_close(code, reason) != websocket_close_submit_status::accepted) {
                    throw http_client_error(http_client_error::code_type::invalid_request, "invalid WebSocket close");
                }
                std::pmr::string(reason.get_allocator()).swap(reason);
                co_await state->flush();
                lane.commit();
            }
            while (!state->ended_) {
                const auto event = co_await state->next_event();
                if (event.close() || event.transport_end()) {
                    close_timer.cancel();
                    co_await state->finish_close();
                    break;
                }
            }
        } catch (...) {
            state->abort();
            if (state->failure_ == http_client_error::code_type::timeout) {
                state->check();
            }
            throw;
        }
    }

private:
    class operation_guard final {
    public:
        operation_guard(http_client_websocket_state& state, std::uint64_t& identity,
            operation_options options)
            : cancellation_(state.cancellation_, identity) {
            const auto effective = override_operation_options(state.options_, std::move(options));
            const auto timeout = operation_timeout(effective.timeout_);
            if (const auto deadline = timeout.deadline()) {
                (void)state.worker_.schedule_timer(timer_, *deadline, [&state](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        state.abort(http_client_error::code_type::timeout);
                    }
                });
            }
            cancellation_.arm(effective.stop_token_);
            state.check();
        }
        operation_guard(const operation_guard&) = delete;
        operation_guard& operator=(const operation_guard&) = delete;

    private:
        worker_timer_registration timer_;
        worker_cancellation_registration<worker_cancellation_target<http_client_websocket_state>> cancellation_;
    };

    class write_guard final {
    public:
        explicit write_guard(http_client_websocket_state& state)
            : state_(state) {
            state_.check();
            state_.write_lane_.begin();
        }
        ~write_guard() {
            if (!committed_) {
                state_.abort();
            }
            state_.finish_write();
        }
        write_guard(const write_guard&) = delete;
        write_guard& operator=(const write_guard&) = delete;

        void commit() noexcept {
            committed_ = true;
        }

    private:
        http_client_websocket_state& state_;
        bool committed_{false};
    };

    static bool mask_key(void*, websocket_mask_key_type& key) noexcept {
        return fill_random_bytes(std::span<unsigned char>(reinterpret_cast<unsigned char*>(key.data()), key.size()));
    }

    [[nodiscard]] http_client_output_queue& output() noexcept {
        return transport_state_.tunnel_->output_;
    }

    [[nodiscard]] bool heartbeat_open() noexcept {
        return !failure_ && !ended_ && !output().stopped_ && !transport_state_.pool_closed_ &&
               protocol_.liveness_mode() == websocket_liveness_mode::open;
    }

    [[nodiscard]] websocket_liveness_decision heartbeat_decision() noexcept {
        return evaluate_websocket_liveness(lifecycle_, protocol_.liveness_mode(), liveness_,
            write_lane_.write_in_progress(), read_active_ && !write_lane_.write_in_progress(),
            last_active_ms_, steady_now_ms());
    }

    void refresh_heartbeat() {
        heartbeat_timer_.cancel();
        if (!lifecycle_.heartbeat_.ping_interval_ || !heartbeat_open() ||
            (!read_active_ && !write_lane_.write_in_progress() &&
                std::holds_alternative<websocket_awaiting_pong>(liveness_))) {
            return;
        }
        const auto delay = websocket_heartbeat_delay(lifecycle_, liveness_,
            write_lane_.write_in_progress(), last_active_ms_, steady_now_ms());
        (void)worker_.schedule_timer(heartbeat_timer_, worker_timer_deadline_after(delay),
            [this](worker_timer_outcome outcome) noexcept {
                if (outcome != worker_timer_outcome::expired) {
                    return;
                }
                try {
                    const auto decision = heartbeat_decision();
                    if (decision == websocket_liveness_decision::abort_transport) {
                        abort(http_client_error::code_type::timeout);
                        return;
                    }
                    heartbeat_due_ = decision == websocket_liveness_decision::send_ping;
                    heartbeat_signal_.notify();
                    // A blocked Ping/application write cannot run the background
                    // coroutine; its timer still enforces the independent stall bound.
                    if (!heartbeat_due_) {
                        refresh_heartbeat();
                    }
                } catch (...) {
                    abort(http_client_error::code_type::closing);
                }
            });
    }

    void touch_activity() {
        last_active_ms_ = steady_now_ms();
        refresh_heartbeat();
    }

    void resume_pong_deadline() {
        if (auto* pong = std::get_if<websocket_awaiting_pong>(&liveness_);
            pong && read_active_ && !write_lane_.write_in_progress()) {
            const auto now = steady_now_ms();
            if (now - pong->sent_at_ms() >= lifecycle_.heartbeat_.pong_timeout_->count()) {
                liveness_ = websocket_awaiting_pong(now, pong->challenge());
            }
        }
        refresh_heartbeat();
    }

    void finish_write() noexcept {
        write_lane_.finish();
        try {
            refresh_heartbeat();
        } catch (...) {
            abort(http_client_error::code_type::closing);
        }
    }

    task<void> flush() {
        refresh_heartbeat();
        for (;;) {
            check();
            const auto plan = protocol_.output_plan();
            if (!plan.bytes().empty()) {
                constexpr std::size_t chunk_bytes = 16384;
                const auto bytes = plan.bytes().substr(0, chunk_bytes);
                co_await tunnel_.write(bytes);
                touch_activity();
                if (protocol_.consume_output(bytes.size()) == websocket_output_consume_status::out_of_range) {
                    std::terminate();
                }
                continue;
            }
            if (plan.disposition() == websocket_transport_disposition::end_transport) {
                co_await tunnel_.finish();
                protocol_.commit_transport_end();
                ended_ = true;
                heartbeat_timer_.cancel();
                heartbeat_signal_.notify();
                output().data_.notify();
                write_lane_.notify();
            }
            co_return;
        }
    }

    task<websocket_event> next_event() {
        for (;;) {
            std::optional<websocket_event> event;
            {
                co_await write_lane_.wait_for_idle(true, [this] { check(); });
                write_guard lane(*this);
                event = protocol_.next_event();
                if (event && event->close()) {
                    peer_close_.record(*event->close());
                }
                if (event && event->pong() && websocket_heartbeat_pong_matches(liveness_, event->pong()->payload())) {
                    liveness_ = websocket_liveness_idle{};
                    touch_activity();
                }
                if (event && (event->ping() || event->close() || event->transport_end() || event->protocol_error())) {
                    worker_timer_registration close_timer;
                    if (event->close() && lifecycle_.close_handshake_timeout_) {
                        (void)worker_.schedule_timer(close_timer,
                            worker_timer_deadline_after(*lifecycle_.close_handshake_timeout_),
                            [this](worker_timer_outcome outcome) noexcept {
                                if (outcome == worker_timer_outcome::expired) {
                                    abort(http_client_error::code_type::timeout);
                                }
                            });
                    }
                    try {
                        co_await flush();
                    } catch (const http_client_error& error) {
                        // A peer may drop its transport after sending malformed
                        // input. Preserve that protocol error if its Close reply
                        // cannot be delivered, but never hide cancellation/timeouts.
                        if (!event->protocol_error() ||
                            (error.code() != http_client_error::code_type::io_error &&
                                error.code() != http_client_error::code_type::tls_failed)) {
                            throw;
                        }
                    }
                }
                lane.commit();
            }
            if (event) {
                if (event->protocol_error()) {
                    throw http_client_error(http_client_error::code_type::protocol_error, "invalid WebSocket frame");
                }
                if (!event->ping()) {
                    co_return *event;
                }
                continue;
            }
            worker_timer_registration read_timer;
            if (read_timeout_) {
                (void)worker_.schedule_timer(read_timer, worker_timer_deadline_after(*read_timeout_),
                    [this](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            abort(http_client_error::code_type::timeout);
                        }
                    });
            }
            const auto bytes = co_await tunnel_.read();
            read_timer.cancel();
            if (!bytes) {
                protocol_.notify_transport_eof();
                throw http_client_error(http_client_error::code_type::protocol_error, "WebSocket ended before peer Close");
            }
            input_.append(as_chars(*bytes));
            touch_activity();
        }
    }

    task<void> finish_close() {
        if (ended_) {
            co_return;
        }
        // RFC 6455 section 7.1.1: the server closes first after its Close.
        // A bounded silence after a completed handshake is a normal retirement.
        bool expired = false;
        worker_timer_registration timer;
        if (const auto timeout = lifecycle_.close_handshake_timeout_) {
            (void)worker_.schedule_timer(timer, worker_timer_deadline_after(*timeout),
                [this, &expired](worker_timer_outcome outcome) noexcept {
                    if (outcome == worker_timer_outcome::expired) {
                        expired = true;
                        tunnel_.abort();
                    }
                });
        }
        std::exception_ptr failure;
        try {
            for (;;) {
                worker_timer_registration read_timer;
                if (!lifecycle_.close_handshake_timeout_ && read_timeout_) {
                    (void)worker_.schedule_timer(read_timer, worker_timer_deadline_after(*read_timeout_),
                        [this](worker_timer_outcome outcome) noexcept {
                            if (outcome == worker_timer_outcome::expired) {
                                abort(http_client_error::code_type::timeout);
                            }
                        });
                }
                const auto bytes = co_await tunnel_.read();
                read_timer.cancel();
                if (!bytes) {
                    break;
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        timer.cancel();
        if (failure_ || (failure && !expired)) {
            check();
            std::rethrow_exception(failure);
        }
        if (expired) {
            ended_ = true;
            heartbeat_signal_.notify();
            write_lane_.notify();
            co_return;
        }
        protocol_.notify_transport_eof();
        co_await write_lane_.wait_for_idle(true, [this] { check(); });
        write_guard lane(*this);
        co_await flush();
        lane.commit();
    }

    worker_handle worker_;
    http_client_tunnel tunnel_;
    http_client_response_state& transport_state_;
    http_io_buffer input_;
    websocket_connection protocol_;
    std::pmr::string subprotocol_;
    operation_options options_;
    websocket_lifecycle_options lifecycle_;
    std::optional<std::chrono::milliseconds> read_timeout_;
    websocket_write_lane write_lane_;
    worker_signal heartbeat_signal_;
    websocket_peer_close_info peer_close_;
    std::optional<http_client_error::code_type> failure_;
    bool ended_{};
    bool read_active_{};
    bool write_active_{};
    std::uint64_t read_id_{};
    std::uint64_t write_id_{};
    websocket_liveness_state_type liveness_{websocket_liveness_idle{}};
    std::int64_t last_active_ms_{steady_now_ms()};
    std::uint64_t heartbeat_sequence_{};
    bool heartbeat_due_{};
    std::shared_ptr<worker_cancellation_target<http_client_websocket_state>> cancellation_;
    worker_timer_registration heartbeat_timer_;
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {
detail::http_client_websocket_state& require_websocket(
    const std::shared_ptr<detail::http_client_websocket_state>& state) {
    if (!state) {
        throw std::logic_error("WebSocket owner has moved");
    }
    state->require_current();
    return *state;
}
}  // namespace

http_client_websocket::http_client_websocket(std::shared_ptr<detail::http_client_websocket_state> state) noexcept
    : state_(std::move(state)) {}
http_client_websocket::http_client_websocket(http_client_websocket&&) noexcept = default;
http_client_websocket& http_client_websocket::operator=(http_client_websocket&& other) noexcept {
    if (this != &other) {
        abort();
        state_ = std::move(other.state_);
    }
    return *this;
}
http_client_websocket::~http_client_websocket() {
    abort();
}
void http_client_websocket::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
task<void> http_client_websocket::write(websocket_opcode opcode, std::string_view payload,
    websocket_send_options options, operation_options operation) {
    auto& state = require_websocket(state_);
    validate_operation_options(operation);
    return detail::http_client_websocket_state::write(
        state_, opcode, std::pmr::string(payload, state.resource()), options, std::move(operation));
}
task<void> http_client_websocket::write_owned(websocket_opcode opcode, std::pmr::string&& payload,
    websocket_send_options options, operation_options operation) {
    auto& state = require_websocket(state_);
    validate_operation_options(operation);
    return detail::http_client_websocket_state::write(
        state_, opcode, detail::rebind_owned_storage(payload, state.resource()), options, std::move(operation));
}
task<void> http_client_websocket::send(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::text, payload, options, std::move(operation));
}
task<void> http_client_websocket::send(std::span<const std::byte> payload, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::binary, as_chars(payload), options, std::move(operation));
}
task<void> http_client_websocket::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    return write(message.opcode(), message.payload(), options, std::move(operation));
}
task<void> http_client_websocket::ping(std::string_view payload, operation_options operation) & {
    return write(websocket_opcode::ping, payload, {}, std::move(operation));
}
task<void> http_client_websocket::pong(std::string_view payload, operation_options operation) & {
    return write(websocket_opcode::pong, payload, {}, std::move(operation));
}
task<std::optional<websocket_message>> http_client_websocket::receive(operation_options operation) & {
    (void)require_websocket(state_);
    validate_operation_options(operation);
    return detail::http_client_websocket_state::receive(state_, std::move(operation));
}
task<void> http_client_websocket::close(websocket_close_options options, operation_options operation) & {
    auto& state = require_websocket(state_);
    validate_operation_options(operation);
    return detail::http_client_websocket_state::close(
        state_, options.code_, std::pmr::string(options.reason_.view(), state.resource()), std::move(operation));
}
std::string_view http_client_websocket::subprotocol() const& {
    return require_websocket(state_).subprotocol();
}
http_protocol_version http_client_websocket::protocol_version() const {
    return require_websocket(state_).protocol_version();
}
bool http_client_websocket::connected() const {
    return require_websocket(state_).connected();
}
std::optional<std::uint16_t> http_client_websocket::peer_close_code() const {
    return require_websocket(state_).peer_close_code();
}
std::string_view http_client_websocket::peer_close_reason() const& {
    return require_websocket(state_).peer_close_reason();
}
const worker_handle& http_client_websocket::worker() const& {
    return require_websocket(state_).worker();
}

http_client_websocket_request::http_client_websocket_request(const http_client_handle& client, std::string_view path)
    : client_(client),
      path_(path, process_resource()),
      headers_(process_resource()),
      subprotocols_(process_resource()) {
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
    headers_.emplace_back(name, value, headers_.get_allocator().resource());
    return *this;
}
http_client_websocket_request& http_client_websocket_request::headers(std::span<const http_header_view> fields) {
    require_open();
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value(), headers_.get_allocator().resource());
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
task<http_client_websocket> http_client_websocket_request::connect() {
    require_open();
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), connect_owned(std::move(*this)));
}
task<http_client_websocket> http_client_websocket_request::connect_owned(http_client_websocket_request request) {
    request.client_.registration_.require_active();
    auto& pool = *request.client_.pool_;
    auto* resource = pool.resource();
    std::shared_ptr<detail::http_client_websocket_handshake> handshake;
    std::optional<detail::http_client_request_storage> stored;
    {
        const detail::inline_http_header_views headers(request.headers_, resource);
        validate_http1_websocket_client_handshake_configuration(headers.view());
        std::pmr::vector<std::string_view> protocols(resource);
        protocols.reserve(request.subprotocols_.size());
        for (const auto& protocol : request.subprotocols_) {
            protocols.emplace_back(protocol);
        }
        handshake = std::allocate_shared<detail::http_client_websocket_handshake>(
            std::pmr::polymorphic_allocator<detail::http_client_websocket_handshake>(resource),
            websocket_client_negotiation_config_view{.subprotocols_ = std::span<const std::string_view>(protocols), .deflate_ = request.deflate_}, resource);
        stored.emplace("GET", request.path_, headers.view(), resource);
        stored->set_tunnel(pool.config().authority_, "websocket");
        stored->set_websocket(handshake);
    }
    std::pmr::string(request.path_.get_allocator()).swap(request.path_);
    std::pmr::vector<http_header>(request.headers_.get_allocator()).swap(request.headers_);
    std::pmr::vector<std::pmr::string>(request.subprotocols_.get_allocator()).swap(request.subprotocols_);
    // The opening deadline covers admission, transport establishment and every
    // handshake byte, but must not become the established tunnel's lifetime.
    // After opening, user cancellation belongs to each operation's snapshot.
    stop_source opening_stop;
    auto user_stop = request.client_.options_.stop_token_.register_callback(
        [&opening_stop] { opening_stop.request_stop(); });
    bool opening_expired = false;
    worker_timer_registration opening_timer;
    (void)pool.worker().schedule_timer(opening_timer, worker_timer_deadline_after(pool.config().connect_timeout_),
        [&opening_stop, &opening_expired](worker_timer_outcome outcome) noexcept {
            if (outcome == worker_timer_outcome::expired) {
                opening_expired = true;
                opening_stop.request_stop();
            }
        });
    auto opening_options = request.client_.options_;
    opening_options.stop_token_ = opening_stop.token();
    std::optional<http_client_tunnel_result> opened;
    try {
        opened.emplace(co_await pool.open_tunnel(std::move(*stored), {}, std::move(opening_options)));
    } catch (...) {
        opening_timer.cancel();
        if (opening_expired) {
            throw http_client_error(http_client_error::code_type::timeout, "WebSocket opening handshake timed out");
        }
        throw;
    }
    opening_timer.cancel();
    user_stop.reset();
    if (opening_expired) {
        throw http_client_error(http_client_error::code_type::timeout, "WebSocket opening handshake timed out");
    }
    auto* tunnel = opened->tunnel();
    if (!tunnel) {
        throw http_client_error(http_client_error::code_type::handshake_rejected,
            "WebSocket handshake rejected", opened->response()->status());
    }
    auto state = std::allocate_shared<detail::http_client_websocket_state>(
        std::pmr::polymorphic_allocator<detail::http_client_websocket_state>(process_resource()),
        std::move(*tunnel), pool.worker(), *handshake, request.deflate_, request.client_.options_,
        request.lifecycle_, request.max_message_bytes_, pool.config().read_timeout_);
    if (request.lifecycle_.heartbeat_.ping_interval_) {
        pool.tasks().spawn(detail::http_client_websocket_state::heartbeat(state));
    }
    co_return http_client_websocket(std::move(state));
}
http_client_websocket_request http_client_handle::websocket(std::string_view path) const {
    registration_.require_active();
    return http_client_websocket_request(*this, path);
}

}  // namespace ruvia
