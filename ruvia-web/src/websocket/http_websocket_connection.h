#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_cancellation.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/core/worker_timer.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/websocket.h"

#include "http/http_stream_end.h"
#include "server/connection_scanner.h"
#include "server/inbound_buffer_budget.h"
#include "server/inbound_transport_error.h"
#include "websocket/http_websocket_liveness.h"
#include "websocket/websocket_state.h"

namespace ruvia::detail {

// RFC 6455 connection over an HTTP/1 socket or HTTP/2 stream transport.
// Frame reassembly, serialization, heartbeat and close policy are shared;
// stream transports provide executor/read_more/write_bytes/abort operations.
template <typename stream_type>
class websocket_session_connection final {
public:
    static constexpr bool native_stream = requires(stream_type& stream) { stream.get_executor(); };
    using stream_argument_type = std::conditional_t<native_stream, stream_type&, stream_type>;

    websocket_session_connection(stream_argument_type stream, const worker_ref& worker_value,
        connection_scanner::entry& scanner_entry, websocket_lifecycle_options lifecycle_options,
        protocol_byte_limit message_limit, std::string_view initial_bytes,
        websocket_connection_options protocol_options, std::string&& subprotocol_value,
        inbound_buffer_account* inbound = nullptr)
        : stream_(std::forward<stream_argument_type>(stream)),
          worker_(&worker_value),
          scanner_entry_(scanner_entry),
          lifecycle_options_(lifecycle_options),
          buffer_reservation_(inbound),
          protocol_(buffer_, [&] {
              protocol_options.message_limit_ = message_limit;
              return protocol_options;
          }()),
          subprotocol_(std::move(subprotocol_value)),
          write_lane_(worker_value),
          reader_done_signal_(worker_value),
          background_tasks_(worker_value) {
        try {
            inbound_append(buffer_, buffer_reservation_, initial_bytes);
        } catch (...) {
            close_stream();
            throw;
        }
        scanner_entry_.register_periodic_check(periodic_check_, this, &websocket_session_connection::heartbeat_tick_thunk);
    }

    websocket_session_connection(stream_argument_type, worker_ref&&, connection_scanner::entry&, websocket_lifecycle_options,
        protocol_byte_limit, std::string_view, websocket_connection_options,
        std::string&&, inbound_buffer_account* = nullptr) = delete;
    static void close_stream(stream_type& stream) noexcept {
        if constexpr (!native_stream) {
            stream.abort();
        } else {
            ruvia::close_socket(stream.lowest_layer());
        }
    }
    ~websocket_session_connection() {
        if (facade_) {
            facade_->operation_scope_.close();
        }
        if (cancellation_target_) {
            cancellation_target_->detach(*this);
        }
    }

    websocket_session_connection(const websocket_session_connection&) = delete;
    websocket_session_connection& operator=(const websocket_session_connection&) = delete;

    static void heartbeat_tick_thunk(void* target, std::int64_t now) noexcept {
        static_cast<websocket_session_connection*>(target)->heartbeat_tick(now);
    }

    // A cancelled or timed-out read ends only its own wait: buffered input,
    // the read lane and the session stay intact for the next read.
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options options = {});
    // A cancelled or timed-out output operation aborts the session, since a
    // partly written frame cannot be withdrawn.
    task<void> write(websocket_opcode opcode, std::string_view payload, bool compress, websocket_write_extent extent,
        operation_options options);
    // Claims only the write lane. A read already pending observes the peer
    // Close and returns nullopt; otherwise close() reads until it arrives.
    task<void> close(::ruvia::websocket_close_options options = {}, operation_options operation = {});

    // Delivered on this worker by an operation's stop token.
    void cancel_operation_by_id(std::uint64_t id) noexcept {
        if (id == 0) {
            return;
        }
        if (id == read_operation_id_) {
            interrupt(operation_lane::read, operation_interrupt::cancelled);
        } else if (id == write_operation_id_) {
            interrupt(operation_lane::write, operation_interrupt::cancelled);
        }
    }
    void abort() noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
        abort_transport(websocket_termination::aborted);
    }

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND {
        return *worker_;
    }
    const worker_ref& worker() const&& = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept {
        return peer_close_.code();
    }

    [[nodiscard]] std::string_view peer_close_reason() const& noexcept RUVIA_LIFETIMEBOUND {
        return peer_close_.reason();
    }
    std::string_view peer_close_reason() const&& = delete;

    // The protocol core is the single owner of the termination cause and the
    // wire byte counters.
    [[nodiscard]] const websocket_connection& protocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return protocol_;
    }
    const websocket_connection& protocol() const&& = delete;

    void require_current_worker() const noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
    }
    void bind_facade(const std::shared_ptr<websocket_state>& facade) noexcept {
        facade_ = facade;
    }
    [[nodiscard]] stream_type& transport() & noexcept {
        return stream_;
    }
    task<void> detach_and_drain_writes();
    // The failure of a heartbeat write. Those writes run detached
    // from any caller, so the session reports it once detach_and_drain_writes()
    // has aborted the transport and joined them.
    [[nodiscard]] std::exception_ptr take_background_failure() noexcept {
        return std::exchange(background_failure_, nullptr);
    }

private:
    enum class write_claim_type : std::uint8_t {
        acquire,
        adopt,
    };

    enum class read_phase_type : std::uint8_t {
        idle,
        reserved,
        active,
    };

    enum class operation_lane : std::uint8_t {
        read,
        write,
    };

    enum class operation_interrupt : std::uint8_t {
        cancelled,
        timed_out,
    };

    // Unwinds an interrupted operation to the frame that owns its lane; it
    // never leaves this connection.
    struct operation_interruption final {};

    using cancellation_target_type = worker_cancellation_target<websocket_session_connection>;

    // One operation's deadline and stop token. Both deliver on this worker
    // and only flag the interruption; the operation unwinds at its next wait.
    class operation_interrupt_registration final {
    public:
        operation_interrupt_registration(
            websocket_session_connection& connection, operation_lane lane, const operation_options& options) {
            if (options.timeout_.has_value()) {
                (void)connection.worker_->schedule_timer(timer_, options.timeout_->deadline_from(std::chrono::steady_clock::now()),
                    [&connection, lane](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            connection.interrupt(lane, operation_interrupt::timed_out);
                        }
                    });
            }
            if (options.stop_token_.stoppable()) {
                cancellation_.emplace(connection.cancellation_target(),
                    lane == operation_lane::read ? connection.read_operation_id_ : connection.write_operation_id_);
                cancellation_->arm(options.stop_token_);
            }
        }

        operation_interrupt_registration(const operation_interrupt_registration&) = delete;
        operation_interrupt_registration& operator=(const operation_interrupt_registration&) = delete;

    private:
        worker_timer_registration timer_;
        // Declared last: its reset joins a pending cancellation first.
        std::optional<worker_cancellation_registration<cancellation_target_type>> cancellation_;
    };

    class write_guard_type final {
    public:
        explicit write_guard_type(websocket_session_connection& connection,
            write_claim_type claim = write_claim_type::acquire, bool restart_pong = true)
            : connection_(connection),
              restart_pong_(restart_pong) {
            if (claim == write_claim_type::acquire) {
                if (connection_.write_lane_.write_in_progress()) {
                    throw std::logic_error("concurrent websocket writes are not supported");
                }
                connection_.write_lane_.begin();
            } else if (!connection_.write_lane_.write_in_progress()) {
                std::terminate();
            }
        }

        void note_write() noexcept {
            restart_pong_ = true;
        }

        ~write_guard_type() {
            connection_.finish_write(restart_pong_);
        }

        write_guard_type(const write_guard_type&) = delete;
        write_guard_type& operator=(const write_guard_type&) = delete;

    private:
        websocket_session_connection& connection_;
        bool restart_pong_{true};
    };
    class write_operation_lease_type final {
    public:
        explicit write_operation_lease_type(websocket_session_connection& connection)
            : connection_(&connection) {
            if (connection_->write_active_) {
                connection_ = nullptr;
                throw std::logic_error("concurrent websocket writes are not supported");
            }
            connection_->write_active_ = true;
        }

        write_operation_lease_type(const write_operation_lease_type&) = delete;
        write_operation_lease_type& operator=(const write_operation_lease_type&) = delete;
        write_operation_lease_type(write_operation_lease_type&& other) noexcept
            : connection_(std::exchange(other.connection_, nullptr)) {}
        write_operation_lease_type& operator=(write_operation_lease_type&&) = delete;

        ~write_operation_lease_type() {
            if (connection_ != nullptr) {
                if (!connection_->worker_->is_current()) {
                    std::terminate();
                }
                connection_->write_active_ = false;
                connection_->notify_write_idle();
            }
        }

    private:
        websocket_session_connection* connection_;
    };

    // Submits an application payload by reference. Its owner is the caller of
    // write_owned(), which releases it once that write ends, so unless
    // release() records that the flush took the payload off the protocol, the
    // guard aborts protocol and transport: every exit after the submit,
    // exceptions and frame destruction included, leaves no view the protocol
    // could still copy or send.
    class borrowed_frame_guard_type final {
    public:
        borrowed_frame_guard_type(websocket_session_connection& connection, websocket_opcode opcode,
            std::string_view payload, bool compress, websocket_write_extent extent)
            : connection_(&connection) {
            const websocket_frame_options options{
                .final_ = extent != websocket_write_extent::fragment, .compress_ = compress};
            const auto submit = [&](bool borrow) {
                return borrow ? connection.protocol_.submit_frame(opcode, websocket_borrowed_payload(payload), options)
                              : connection.protocol_.submit_frame(opcode, payload, options);
            };
            const auto status = [&] {
                if constexpr (native_stream) {
                    if constexpr (requires(stream_type& value) { value.next_layer(); }) {
                        if (payload.size() + 14 <= websocket_retained_buffer_bytes) {
                            return submit(false);
                        }
                    }
                    return submit(true);
                } else {
                    return submit(false);
                }
            }();
            if (!connection.accept_submitted_frame(status)) {
                connection_ = nullptr;
            }
        }

        borrowed_frame_guard_type(const borrowed_frame_guard_type&) = delete;
        borrowed_frame_guard_type& operator=(const borrowed_frame_guard_type&) = delete;

        ~borrowed_frame_guard_type() {
            if (connection_ != nullptr) {
                connection_->abort_transport(websocket_termination::aborted);
            }
        }

        // False when the protocol was no longer open and queued nothing.
        [[nodiscard]] bool submitted() const noexcept {
            return connection_ != nullptr;
        }

        // A completed flush drained the payload, or ended the protocol so it
        // never reads the view again.
        void release() noexcept {
            connection_ = nullptr;
        }

    private:
        websocket_session_connection* connection_;
    };

    class read_guard_type final {
    public:
        explicit read_guard_type(websocket_session_connection& connection)
            : connection_(&connection) {
            if (connection_->read_phase_ != read_phase_type::idle) {
                connection_ = nullptr;
                throw std::logic_error("concurrent websocket reads are not supported");
            }
            connection_->read_phase_ = read_phase_type::reserved;
        }

        read_guard_type(const read_guard_type&) = delete;
        read_guard_type& operator=(const read_guard_type&) = delete;
        read_guard_type(read_guard_type&& other) noexcept
            : connection_(std::exchange(other.connection_, nullptr)) {}
        read_guard_type& operator=(read_guard_type&&) = delete;

        void start() {
            if (connection_ == nullptr || connection_->read_phase_ != read_phase_type::reserved) {
                std::terminate();
            }
            connection_->read_phase_ = read_phase_type::active;
            connection_->resume_pong_window_on_read_start();
        }

        ~read_guard_type() {
            if (connection_ != nullptr) {
                if (!connection_->worker_->is_current()) {
                    std::terminate();
                }
                connection_->read_phase_ = read_phase_type::idle;
                connection_->read_interrupt_.reset();
                connection_->reader_done_signal_.notify();
            }
        }

    private:
        websocket_session_connection* connection_;
    };

    void finish_write(bool restart_pong = true) noexcept;
    void resume_pong_window_on_read_start() noexcept;
    [[nodiscard]] bool pong_observable() const noexcept;
    void heartbeat_tick(std::int64_t now) noexcept;
    task<std::optional<websocket_message>> read_owned(read_guard_type read_guard, operation_options options);
    task<void> write_owned(websocket_opcode opcode, std::string_view payload, write_operation_lease_type write_lease,
        bool compress, websocket_write_extent extent, operation_options options);
    task<void> close_owned(::ruvia::websocket_close_options options, operation_options operation,
        write_operation_lease_type write_lease);
    task<void> write_heartbeat_ping();
    // Maps a rejected submit to its exception; false when the protocol is no
    // longer open and nothing was queued.
    bool accept_submitted_frame(websocket_frame_submit_status status);
    task<void> flush_protocol_output_now();
    // Reads more transport bytes into buffer_. Between frames the read is
    // sized by recent traffic, so an idle connection holds little read storage.
    task<bool> read_more();
    [[nodiscard]] const std::shared_ptr<cancellation_target_type>& cancellation_target();
    // Flags the operation that owns `lane`. A read wakes at its next wait and
    // keeps the session; an output operation aborts the session.
    void interrupt(operation_lane lane, operation_interrupt reason) noexcept;
    void throw_if_read_interrupted() const {
        if (read_interrupt_.has_value()) {
            throw operation_interruption{};
        }
    }
    void throw_if_write_interrupted() const {
        if (write_interrupt_.has_value()) {
            throw operation_interruption{};
        }
    }
    [[noreturn]] static void throw_interrupted(operation_interrupt reason, const char* operation_name);
    // A termination is recorded: this side ended the session (abort, a
    // heartbeat, close or operation timeout) or its close handshake finished.
    [[nodiscard]] bool session_ended() const noexcept {
        return protocol_.termination() != websocket_termination::none;
    }
    task<std::error_code> write_adapted(const websocket_output_plan& plan);
    auto write_bytes(const websocket_output_plan& plan);
    void close_stream() noexcept;
    // `cause` is recorded unless an earlier termination cause was.
    void abort_transport(websocket_termination cause, bool force_transport = false) noexcept;
    // Aborts the transport and retains the first background write failure.
    void fail_background(std::exception_ptr failure) noexcept;
    void record_peer_close(const websocket_close_event& close_event) noexcept {
        peer_close_.record(close_event);
    }
    void notify_write_idle() noexcept;
    [[nodiscard]] bool has_operations_to_drain() const noexcept {
        return read_phase_ != read_phase_type::idle || write_active_ || write_lane_.write_in_progress();
    }

    std::shared_ptr<websocket_state> facade_;
    stream_argument_type stream_;
    const worker_ref* worker_;
    connection_scanner::entry& scanner_entry_;
    websocket_lifecycle_options lifecycle_options_{};
    http_io_buffer buffer_;
    inbound_buffer_reservation buffer_reservation_{};
    websocket_connection protocol_;
    std::string subprotocol_;
    websocket_peer_close_info peer_close_{};
    websocket_write_lane write_lane_;
    worker_signal reader_done_signal_;
    bool write_active_{false};
    read_phase_type read_phase_{read_phase_type::idle};
    std::optional<operation_interrupt> read_interrupt_;
    std::optional<operation_interrupt> write_interrupt_;
    std::uint64_t read_operation_id_{0};
    std::uint64_t write_operation_id_{0};
    // Cancels only the transport read of an interrupted read().
    asio::cancellation_signal read_cancellation_;
    // Created by the first operation with a stop token; detached on destruction.
    std::shared_ptr<cancellation_target_type> cancellation_target_;
    websocket_liveness_state_type liveness_state_{websocket_liveness_idle{}};
    std::exception_ptr background_failure_;
    std::uint64_t heartbeat_sequence_{0};
    // Whether the last read between frames received less than
    // websocket_idle_read_bytes: the next such read then waits in a buffer of
    // that size.
    bool small_frames_{true};
    task_scope background_tasks_;
    // Declared last so destruction unregisters before any callback target state
    // starts to disappear.
    connection_scanner::periodic_check_registration periodic_check_;
};

template <typename stream_type>
bool websocket_session_connection<stream_type>::pong_observable() const noexcept {
    return !write_lane_.write_in_progress() && read_phase_ == read_phase_type::active;
}

template <typename stream_type>
void websocket_session_connection<stream_type>::resume_pong_window_on_read_start() noexcept {
    resume_websocket_pong_deadline(lifecycle_options_, liveness_state_, pong_observable());
}

template <typename stream_type>
void websocket_session_connection<stream_type>::finish_write(bool restart_pong) noexcept {
    write_lane_.release();
    if (restart_pong && std::holds_alternative<websocket_awaiting_pong>(liveness_state_)) {
        restart_websocket_pong_window(liveness_state_, steady_now_ms());
    }
    write_lane_.notify();
}

template <typename stream_type>
void websocket_session_connection<stream_type>::heartbeat_tick(std::int64_t now) noexcept {
    const bool observable = pong_observable();
    if (!observable) {
        restart_websocket_pong_window(liveness_state_, now);
    }
    switch (evaluate_websocket_liveness(lifecycle_options_, protocol_.liveness_mode(), liveness_state_,
        write_lane_.write_in_progress(), observable, scanner_entry_.last_active_ms(), now)) {
        case websocket_liveness_decision::idle:
            return;
        case websocket_liveness_decision::abort_transport:
            // A heartbeat or close timeout closes the connection.
            abort_transport(protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof
                                ? protocol_.termination()
                                : websocket_termination::timeout,
                true);
            return;
        case websocket_liveness_decision::send_ping:
            break;
    }
    if (write_lane_.reader_waiting()) {
        // Let an already blocked reader poll its buffered control frames
        // before a scanner heartbeat claims the next message boundary.
        return;
    }

    liveness_state_ = websocket_sending_ping{++heartbeat_sequence_};
    write_lane_.begin();
    try {
        background_tasks_.spawn(write_heartbeat_ping());
    } catch (...) {
        finish_write();
        fail_background(std::current_exception());
        return;
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::write_heartbeat_ping() {
    write_guard_type write_guard(*this, write_claim_type::adopt);
    try {
        const auto* sending = std::get_if<websocket_sending_ping>(&liveness_state_);
        if (sending == nullptr) {
            co_return;
        }
        const auto challenge = sending->challenge_;
        const auto payload_value = websocket_heartbeat_payload(challenge);
        if (accept_submitted_frame(protocol_.submit_frame(
                websocket_opcode::ping, std::string_view(payload_value.data(), payload_value.size())))) {
            co_await flush_protocol_output_now();
        }
        // Ordinary I/O shares the scanner activity timestamp and may update it
        // while this coroutine is suspended. The Pong deadline belongs to this
        // completed heartbeat write, so use its own timestamp.
        const auto ping_sent_at_ms = steady_now_ms();
        if (protocol_.liveness_mode() == websocket_liveness_mode::open &&
            std::holds_alternative<websocket_sending_ping>(liveness_state_)) {
            liveness_state_ = websocket_awaiting_pong{ping_sent_at_ms, challenge};
        }
    } catch (...) {
        fail_background(std::current_exception());
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::fail_background(std::exception_ptr failure) noexcept {
    abort_transport(websocket_termination::aborted);
    if (background_failure_ == nullptr) {
        background_failure_ = std::move(failure);
    }
}

template <typename stream_type>
task<std::optional<websocket_message>> websocket_session_connection<stream_type>::read(operation_options options) {
    require_current_worker();
    return read_owned(read_guard_type(*this), std::move(options));
}

template <typename stream_type>
const std::shared_ptr<typename websocket_session_connection<stream_type>::cancellation_target_type>&
websocket_session_connection<stream_type>::cancellation_target() {
    if (!cancellation_target_) {
        cancellation_target_ = make_worker_cancellation_target(*this, *worker_);
    }
    return cancellation_target_;
}

template <typename stream_type>
void websocket_session_connection<stream_type>::interrupt(operation_lane lane, operation_interrupt reason) noexcept {
    if (lane == operation_lane::read) {
        if (read_phase_ != read_phase_type::active || read_interrupt_.has_value()) {
            return;
        }
        read_interrupt_ = reason;
        read_cancellation_.emit(asio::cancellation_type::terminal);
        // Wakes a reader waiting for its turn at the next message boundary.
        write_lane_.notify();
        return;
    }
    if (!write_active_ || write_interrupt_.has_value()) {
        return;
    }
    write_interrupt_ = reason;
    abort_transport(reason == operation_interrupt::timed_out ? websocket_termination::timeout
                                                             : websocket_termination::aborted,
        true);
}

template <typename stream_type>
void websocket_session_connection<stream_type>::throw_interrupted(operation_interrupt reason, const char* operation_name) {
    if (reason == operation_interrupt::timed_out) {
        throw std::system_error(std::make_error_code(std::errc::timed_out), operation_name);
    }
    throw std::system_error(std::make_error_code(std::errc::operation_canceled), operation_name);
}

template <typename stream_type>
task<std::optional<websocket_message>> websocket_session_connection<stream_type>::read_owned(
    read_guard_type read_guard_value, operation_options options) {
    require_current_worker();
    std::optional<operation_interrupt> interrupted;
    {
        read_guard_type active_read(std::move(read_guard_value));
        active_read.start();
        if (options.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt::cancelled, "websocket read cancelled");
        }
        const operation_interrupt_registration interrupt_registration(*this, operation_lane::read, options);
        const auto check_interrupt = [this] { throw_if_read_interrupted(); };
        try {
            for (;;) {
                std::optional<websocket_event> event;
                {
                    // RFC 6455 sections 5.5.1/5.5.2: poll() may queue an
                    // automatic Close/Pong. Reserve the next message boundary
                    // and retain the lane through that control output flush.
                    if (write_lane_.write_in_progress()) {
                        co_await write_lane_.wait_for_idle(true, check_interrupt);
                    }
                    write_guard_type write_guard(*this, write_claim_type::acquire, false);
                    event = protocol_.next_event();
                    if (event.has_value()) {
                        if (event->ping() != nullptr) {
                            write_guard.note_write();
                            co_await flush_protocol_output_now();
                        } else if (event->close() != nullptr || event->protocol_error() != nullptr || event->transport_end() != nullptr) {
                            if (const auto* close_event = event->close()) {
                                record_peer_close(*close_event);
                            }
                            liveness_state_ = event->close() != nullptr
                                                  ? websocket_liveness_state_type(websocket_awaiting_peer_close{steady_now_ms()})
                                                  : websocket_liveness_state_type(websocket_liveness_idle{});
                            write_guard.note_write();
                            co_await flush_protocol_output_now();
                        }
                    }
                }
                if (!event.has_value()) {
                    if (!(co_await read_more())) {
                        // EOF is an abnormal websocket close when no peer Close was
                        // received. The core discards unsent WS output and plans the
                        // transport end.
                        if (write_lane_.write_in_progress()) {
                            co_await write_lane_.wait_for_idle(true, check_interrupt);
                        }
                        write_guard_type write_guard(*this);
                        protocol_.notify_transport_eof();
                        co_await flush_protocol_output_now();
                        close_stream();
                        co_return std::nullopt;
                    }
                    scanner_entry_.touch();
                    continue;
                }

                if (const auto* message = event->message()) {
                    co_return *message;
                }
                if (event->ping() != nullptr) {
                    continue;
                }
                if (event->pong() != nullptr) {
                    if (websocket_heartbeat_pong_matches(liveness_state_, event->pong()->payload())) {
                        liveness_state_ = websocket_liveness_idle{};
                        scanner_entry_.touch();
                    }
                    continue;
                }
                if (event->close() != nullptr || event->protocol_error() != nullptr || event->transport_end() != nullptr) {
                    // The poll's exclusive lane already flushed the protocol's
                    // Close/end-transport plan before this read completes.
                    if (protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof) {
                        continue;
                    }
                    co_return std::nullopt;
                }
                throw std::logic_error("unexpected WebSocket protocol event");
            }
        } catch (const operation_interruption&) {
            interrupted = read_interrupt_;
        } catch (...) {
            abort_transport(websocket_termination::aborted, true);
            throw;
        }
    }
    throw_interrupted(*interrupted, "websocket read");
}

template <typename stream_type>
task<bool> websocket_session_connection<stream_type>::read_more() {
    throw_if_read_interrupted();
    if constexpr (!native_stream) {
        try {
            const bool more = co_await stream_.read_more(
                buffer_, [this] { throw_if_read_interrupted(); }, read_cancellation_.slot());
            buffer_reservation_.resize_to(buffer_.capacity());
            co_return more;
        } catch (const inbound_transport_error&) {
            if (protocol_.liveness_mode() != websocket_liveness_mode::inactive) {
                abort_transport(websocket_termination::io_error, true);
                throw;
            }
        }
        // This side already ended the session and aborted the stream.
        co_return false;
    } else {
        constexpr std::size_t min_read_bytes = 4096;
        constexpr std::size_t max_read_bytes = 64 * 1024;
        const bool between_frames = buffer_.empty();
        std::span<char> read_space;
        if (between_frames) {
            // Preserve the native socket's small idle read allocation and
            // retained storage for steady mid-size messages.
            if (buffer_.capacity() > websocket_retained_buffer_bytes) {
                inbound_shrink_to(buffer_, buffer_reservation_, websocket_retained_buffer_bytes);
            }
            const auto min_idle_bytes = small_frames_ ? websocket_idle_read_bytes : min_read_bytes;
            if (buffer_.capacity() < min_idle_bytes) {
                read_space = inbound_prepare(buffer_, buffer_reservation_, min_idle_bytes);
            } else {
                read_space = buffer_.prepare_spare();
                if (small_frames_ && read_space.size() > websocket_idle_read_bytes) {
                    read_space = read_space.first(websocket_idle_read_bytes);
                } else if (read_space.size() > max_read_bytes) {
                    read_space = read_space.first(max_read_bytes);
                }
            }
        } else {
            // Use the writable tail before growing an incomplete frame.
            read_space = buffer_.prepare_spare();
            if (read_space.empty()) {
                const auto needed = std::clamp(protocol_.needed_input_bytes(), min_read_bytes, max_read_bytes);
                read_space = inbound_prepare(buffer_, buffer_reservation_, needed);
            } else if (read_space.size() > max_read_bytes) {
                read_space = read_space.first(max_read_bytes);
            }
        }
        // Only this read is cancelled: a TLS stream shields its own record
        // writes from the slot, so the engine resumes cleanly on the next read.
        auto read_completion = co_await ruvia::async_asio<std::size_t>([this, read_space](auto handler) mutable {
            stream_.async_read_some(asio::buffer(read_space),
                asio::bind_cancellation_slot(read_cancellation_.slot(), std::move(handler)));
        });
        const auto ec = read_completion.error_code();
        const auto bytes_read = read_completion.result();
        // The protocol maps EOF without a Close frame to abnormal closure.
        if (is_stream_read_end(ec) || (!ec && bytes_read == 0)) {
            co_return false;
        }
        if (ec) {
            // This side already ended the session and closed the stream.
            if (protocol_.liveness_mode() == websocket_liveness_mode::inactive) {
                co_return false;
            }
            if (read_interrupt_.has_value() && ec == asio::error::operation_aborted) {
                throw operation_interruption{};
            }
            abort_transport(websocket_termination::io_error, true);
            throw inbound_transport_error(ec, "failed to read websocket bytes");
        }
        buffer_.commit(bytes_read);
        if (between_frames) {
            small_frames_ = bytes_read < websocket_idle_read_bytes;
        }
        co_return true;
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::write(websocket_opcode opcode, std::string_view payload_value,
    bool compress, websocket_write_extent extent, operation_options options) {
    require_current_worker();
    return write_owned(opcode, payload_value, write_operation_lease_type(*this), compress, extent, std::move(options));
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::write_owned(websocket_opcode opcode,
    std::string_view payload_value, write_operation_lease_type write_lease, bool compress, websocket_write_extent extent,
    operation_options options) {
    require_current_worker();
    std::optional<operation_interrupt> interrupted;
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        if (options.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt::cancelled, "websocket write");
        }
        write_interrupt_.reset();
        const operation_interrupt_registration interrupt_registration(*this, operation_lane::write, options);
        try {
            if (write_lane_.blocked_for_writer()) {
                co_await write_lane_.wait_for_idle(false, [this] { throw_if_write_interrupted(); });
            }
            write_guard_type write_guard(*this);
            // The caller keeps the payload alive until this write completes, so an
            // uncompressed frame is gathered from it instead of being copied.
            borrowed_frame_guard_type borrowed_frame(*this, opcode, payload_value, compress, extent);
            if (borrowed_frame.submitted()) {
                co_await flush_protocol_output_now();
                borrowed_frame.release();
            }
        } catch (...) {
            if (!write_interrupt_.has_value()) {
                throw;
            }
            interrupted = write_interrupt_;
        }
    }
    if (interrupted.has_value()) {
        throw_interrupted(*interrupted, "websocket write");
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::close(
    ::ruvia::websocket_close_options options, operation_options operation) {
    require_current_worker();
    return close_owned(options, std::move(operation), write_operation_lease_type(*this));
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::close_owned(
    ::ruvia::websocket_close_options options, operation_options operation, write_operation_lease_type write_lease) {
    require_current_worker();
    std::optional<operation_interrupt> interrupted;
    bool handshake_timed_out = false;
    {
        write_operation_lease_type active_write(std::move(write_lease));
        static_cast<void>(active_write);
        if (operation.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt::cancelled, "websocket close");
        }
        write_interrupt_.reset();
        const operation_interrupt_registration interrupt_registration(*this, operation_lane::write, operation);
        try {
            if (write_lane_.blocked_for_writer()) {
                co_await write_lane_.wait_for_idle(false, [this] { throw_if_write_interrupted(); });
            }
            const auto reason = options.reason_.view();
            bool flush_output = false;
            {
                write_guard_type write_guard(*this);
                switch (protocol_.submit_close(options.code_, reason)) {
                    case websocket_close_submit_status::accepted:
                        // The deadline covers the Close write too: a peer that stops
                        // reading must not hold the flush below open past the timeout.
                        liveness_state_ = websocket_awaiting_peer_close{steady_now_ms()};
                        flush_output = true;
                        break;
                    case websocket_close_submit_status::already_closing:
                        flush_output = true;
                        break;
                    case websocket_close_submit_status::closed:
                        break;
                    case websocket_close_submit_status::invalid_code:
                        throw std::invalid_argument("invalid websocket close code");
                    case websocket_close_submit_status::invalid_reason:
                        throw std::invalid_argument("invalid websocket close reason");
                    case websocket_close_submit_status::reason_too_large:
                        throw std::invalid_argument("websocket close reason is too large");
                }
                if (flush_output) {
                    co_await flush_protocol_output_now();
                }
            }

            // RFC 6455 section 7.1.1: sending Close starts, but does not complete,
            // the handshake. A pending read keeps parsing input until the peer
            // Close arrives (or EOF/timeout aborts this transport) and then
            // returns nullopt; without one, close() reads itself. The core
            // suppresses application messages in this phase while still
            // validating frames and handling control traffic.
            const bool awaiting_peer_close = protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_close;
            while (protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_close ||
                   protocol_.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof) {
                if (read_phase_ != read_phase_type::idle) {
                    co_await reader_done_signal_.wait();
                } else {
                    (void)co_await read();
                }
            }
            handshake_timed_out = awaiting_peer_close && protocol_.termination() == websocket_termination::timeout;
        } catch (...) {
            if (!write_interrupt_.has_value()) {
                throw;
            }
        }
        interrupted = write_interrupt_;
    }
    if (interrupted.has_value()) {
        throw_interrupted(*interrupted, "websocket close");
    }
    if (handshake_timed_out) {
        // The peer never answered: an inbound connection end, not a handler fault.
        throw std::system_error(
            std::make_error_code(std::errc::timed_out), "websocket close handshake timed out");
    }
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::detach_and_drain_writes() {
    require_current_worker();
    if (facade_ && !facade_->owner_) {
        facade_->operation_scope_.expire();
    }
    periodic_check_.reset();
    // Teardown first cancels the transport operation that owns a suspended
    // write, then joins that write before the connection storage disappears.
    // Application writes can outlive the handler through the public facade just
    // as heartbeat writes can outlive the scanner callback, so both phases are
    // part of the same structured drain. Force a transport abort when active
    // I/O remains even if the protocol already committed its normal close.
    const bool has_active_io = read_phase_ == read_phase_type::active || write_lane_.write_in_progress();
    abort_transport(websocket_termination::aborted, has_active_io);
    while (has_operations_to_drain()) {
        if (read_phase_ != read_phase_type::idle) {
            co_await reader_done_signal_.wait();
        } else {
            co_await write_lane_.wait();
        }
    }
    co_await background_tasks_.join();
    if (facade_ && !facade_->owner_) {
        auto facade = std::exchange(facade_, nullptr);
        co_await facade->operation_scope_.close_and_join();
    } else {
        facade_.reset();
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::notify_write_idle() noexcept {
    write_lane_.notify();
}

template <typename stream_type>
bool websocket_session_connection<stream_type>::accept_submitted_frame(websocket_frame_submit_status status) {
    switch (status) {
        case websocket_frame_submit_status::accepted:
            return true;
        case websocket_frame_submit_status::not_open:
            return false;
        case websocket_frame_submit_status::invalid_opcode:
            throw std::logic_error("invalid outbound websocket opcode");
        case websocket_frame_submit_status::message_too_large:
            throw std::invalid_argument("websocket message is too large");
        case websocket_frame_submit_status::invalid_text_payload:
            throw std::invalid_argument("websocket text payload is not valid UTF-8");
        case websocket_frame_submit_status::control_frame_too_large:
            throw std::invalid_argument("websocket control frame is too large");
        case websocket_frame_submit_status::message_in_progress:
            throw std::logic_error("a fragmented websocket message is in progress");
    }
    throw std::logic_error("unexpected websocket frame submit status");
}

template <typename stream_type>
auto websocket_session_connection<stream_type>::write_bytes(const websocket_output_plan& plan) {
    if constexpr (!native_stream) {
        return write_adapted(plan);
    } else {
        // One gathered write: pending protocol output, then a borrowed payload.
        const std::array<asio::const_buffer, 2> buffers{
            asio::buffer(plan.bytes().data(), plan.bytes().size()),
            asio::buffer(plan.borrowed_bytes().data(), plan.borrowed_bytes().size())};
        return ruvia::async_asio([this, buffers](auto handler) mutable {
            // Partial progress refreshes heartbeat transport activity and the
            // write_timeout inactivity clock.
            asio::async_write(stream_, buffers, connection_write_progress(scanner_entry_), std::move(handler));
        });
    }
}

template <typename stream_type>
task<std::error_code> websocket_session_connection<stream_type>::write_adapted(const websocket_output_plan& plan) {
    // The websocket protocol's transport disposition maps onto the stream end
    // at this one boundary; the multiplexed transports only speak http_stream_end.
    const auto end = plan.disposition() == websocket_transport_disposition::end_transport ? http_stream_end::end
                                                                                          : http_stream_end::keep_open;
    if (plan.borrowed_bytes().empty()) {
        co_return co_await stream_.write_bytes(plan.bytes(), end);
    }
    if (const auto error = co_await stream_.write_bytes(plan.bytes(), http_stream_end::keep_open)) {
        co_return error;
    }
    co_return co_await stream_.write_bytes(plan.borrowed_bytes(), end);
}

template <typename stream_type>
task<void> websocket_session_connection<stream_type>::flush_protocol_output_now() {
    struct flush_abort_guard final {
        websocket_session_connection* connection_;
        websocket_termination cause_{websocket_termination::aborted};
        ~flush_abort_guard() noexcept {
            if (connection_ != nullptr) {
                connection_->abort_transport(cause_, true);
            }
        }
    } guard{this};
    for (;;) {
        const auto plan = protocol_.output_plan();
        const auto disposition = plan.disposition();
        const auto size = plan.size();
        if (size == 0 && disposition == websocket_transport_disposition::keep_open) {
            guard.connection_ = nullptr;
            co_return;
        }
        if (size != 0 || (!native_stream && disposition == websocket_transport_disposition::end_transport)) {
            std::error_code ec;
            if constexpr (native_stream) {
                // The server write_timeout_ bounds a native write stalled by a
                // peer that stopped reading, whatever the reader's phase. An
                // HTTP/2-adapted stream is bounded by its session's writer.
                const connection_scanner::entry::write_scope write_stall_clock(scanner_entry_);
                const auto write_result = co_await write_bytes(plan);
                ec = write_result.error_code();
            } else {
                ec = co_await write_bytes(plan);
            }
            if (ec) {
                guard.cause_ = websocket_termination::io_error;
                if (protocol_.termination() == websocket_termination::timeout) {
                    throw std::system_error(std::make_error_code(std::errc::timed_out), "websocket write timed out");
                }
                throw inbound_transport_error(ec, "failed to write websocket bytes");
            }
        }
        if (protocol_.consume_output(size) != websocket_output_consume_status::drained) {
            std::terminate();
        }
        scanner_entry_.touch();
        if (disposition == websocket_transport_disposition::end_transport) {
            protocol_.commit_transport_end();
            guard.connection_ = nullptr;
            co_return;
        }
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::abort_transport(websocket_termination cause, bool force_transport) noexcept {
    liveness_state_ = websocket_liveness_idle{};
    const auto disposition = protocol_.abort(cause);
    if (force_transport || disposition == websocket_abort_disposition::abort_transport) {
        close_stream();
        notify_write_idle();
    }
}

template <typename stream_type>
void websocket_session_connection<stream_type>::close_stream() noexcept {
    close_stream(stream_);
}

}  // namespace ruvia::detail
