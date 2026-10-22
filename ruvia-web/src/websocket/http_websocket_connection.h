#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_protocol.h"
#include "ruvia/web/websocket.h"

#include "server/connection_scanner.h"
#include "tls/tls_stream_end.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

// RFC 6455 connection over an HTTP/1 socket or HTTP/2 stream transport.
// Frame reassembly, serialization, heartbeat and close policy are shared;
// stream transports provide executor/read_more/write_bytes/abort operations.
template <typename stream_type>
class server_websocket_connection final {
public:
    static constexpr bool native_stream = requires(stream_type& stream) { stream.get_executor(); };
    using stream_argument_type = std::conditional_t<native_stream, stream_type&, stream_type>;

    server_websocket_connection(stream_argument_type stream, const worker_handle& worker_value,
        connection_scanner::entry& scanner_entry, websocket_lifecycle_options lifecycle_options,
        protocol_byte_limit message_limit, std::pmr::memory_resource* resource, std::string_view initial_bytes,
        websocket_connection_options protocol_options)
        : stream_(std::forward<stream_argument_type>(stream)),
          worker_(&worker_value),
          scanner_entry_(scanner_entry),
          lifecycle_options_(lifecycle_options),
          buffer_(std::pmr::polymorphic_allocator<char>(resource)),
          protocol_(buffer_, [&] {
              protocol_options.message_limit_ = message_limit;
              return protocol_options;
          }()),
          background_write_signal_(worker_value),
          reader_done_signal_(worker_value) {
        buffer_.append(initial_bytes);
        scanner_entry_.register_periodic_check(periodic_check_, this, &server_websocket_connection::heartbeat_tick_thunk,
            &server_websocket_connection::drain_thunk);
    }

    server_websocket_connection(stream_argument_type, worker_handle&&, connection_scanner::entry&, websocket_lifecycle_options,
        protocol_byte_limit, std::pmr::memory_resource*, std::string_view, websocket_connection_options) = delete;

    ~server_websocket_connection() = default;

    server_websocket_connection(const server_websocket_connection&) = delete;
    server_websocket_connection& operator=(const server_websocket_connection&) = delete;

    static void heartbeat_tick_thunk(void* target, std::int64_t now) noexcept {
        static_cast<server_websocket_connection*>(target)->heartbeat_tick(now);
    }

    static void drain_thunk(void* target) noexcept {
        static_cast<server_websocket_connection*>(target)->start_going_away();
    }

    [[nodiscard]] task<std::optional<websocket_message>> read();
    task<void> write(websocket_opcode opcode, std::string_view payload, bool compress);
    task<void> close(::ruvia::websocket_close_options options = {});
    void abort() noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
        abort_transport();
    }

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return *worker_;
    }

    void require_current_worker() const noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
    }
    task<void> detach_and_drain_writes();
    // The failure of a heartbeat or Going Away write. Those writes run detached
    // from any caller, so the session reports it once detach_and_drain_writes()
    // has aborted the transport and joined them.
    [[nodiscard]] std::exception_ptr take_background_failure() noexcept {
        return std::exchange(background_failure_, nullptr);
    }

private:
    enum class write_phase_type : std::uint8_t {
        idle,
        application,
        heartbeat,
        going_away,
    };

    enum class write_claim_type : std::uint8_t {
        acquire,
        adopt,
    };

    enum class read_phase_type : std::uint8_t {
        idle,
        reserved,
        active,
    };

    class write_guard_type final {
    public:
        write_guard_type(server_websocket_connection& connection, write_phase_type phase,
            write_claim_type claim = write_claim_type::acquire)
            : connection_(connection),
              phase_(phase) {
            if (phase_ == write_phase_type::idle) {
                std::terminate();
            }
            if (claim == write_claim_type::acquire) {
                if (connection_.write_phase_ != write_phase_type::idle) {
                    throw std::logic_error("concurrent websocket writes are not supported");
                }
                connection_.write_phase_ = phase_;
            } else if (connection_.write_phase_ != phase_) {
                std::terminate();
            }
        }

        ~write_guard_type() {
            connection_.finish_write(phase_);
        }

        write_guard_type(const write_guard_type&) = delete;
        write_guard_type& operator=(const write_guard_type&) = delete;

    private:
        server_websocket_connection& connection_;
        write_phase_type phase_;
    };

    class write_operation_lease_type final {
    public:
        explicit write_operation_lease_type(server_websocket_connection& connection)
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
        server_websocket_connection* connection_;
    };

    // Submits an application payload by reference. Its owner is the caller of
    // write_owned(), which releases it once that write ends, so unless
    // release() records that the flush took the payload off the protocol, the
    // guard aborts protocol and transport: every exit after the submit,
    // exceptions and frame destruction included, leaves no view the protocol
    // could still copy or send.
    class borrowed_frame_guard_type final {
    public:
        borrowed_frame_guard_type(
            server_websocket_connection& connection, websocket_opcode opcode, std::string_view payload, bool compress)
            : connection_(&connection) {
            if (!connection.accept_submitted_frame(connection.protocol_.submit_borrowed_frame(opcode, payload, compress))) {
                connection_ = nullptr;
            }
        }

        borrowed_frame_guard_type(const borrowed_frame_guard_type&) = delete;
        borrowed_frame_guard_type& operator=(const borrowed_frame_guard_type&) = delete;

        ~borrowed_frame_guard_type() {
            if (connection_ != nullptr) {
                connection_->abort_transport();
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
        server_websocket_connection* connection_;
    };

    class read_guard_type final {
    public:
        explicit read_guard_type(server_websocket_connection& connection)
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
        }

        ~read_guard_type() {
            if (connection_ != nullptr) {
                if (!connection_->worker_->is_current()) {
                    std::terminate();
                }
                connection_->read_phase_ = read_phase_type::idle;
                connection_->reader_done_signal_.notify();
            }
        }

    private:
        server_websocket_connection* connection_;
    };

    void finish_write(write_phase_type phase) noexcept;
    [[nodiscard]] bool pong_observable() const noexcept;
    void restart_pong_window(std::int64_t now) noexcept;
    void heartbeat_tick(std::int64_t now) noexcept;
    // A draining server starts the close handshake with 1001 Going Away as
    // soon as the write lane is free; the close-handshake timeout bounds it.
    void start_going_away() noexcept;
    task<void> write_going_away_close();
    task<std::optional<websocket_message>> read_owned(read_guard_type read_guard);
    task<void> write_owned(
        websocket_opcode opcode, std::string_view payload, write_operation_lease_type write_lease, bool compress);
    task<void> close_owned(::ruvia::websocket_close_options options, write_operation_lease_type write_lease);
    task<void> write_heartbeat_ping();
    // Callers check write_phase_ first, so an idle lane costs no coroutine frame.
    task<void> wait_for_write_idle();
    // Maps a rejected submit to its exception; false when the protocol is no
    // longer open and nothing was queued.
    bool accept_submitted_frame(websocket_frame_submit_status status);
    task<void> flush_protocol_output_exclusive();
    task<void> flush_protocol_output_now();
    // Reads more transport bytes into buffer_. Between frames the read is
    // sized by recent traffic, so an idle connection holds little read storage.
    task<bool> read_more();
    task<std::error_code> write_adapted(const websocket_output_plan& plan);
    [[nodiscard]] auto executor() const {
        if constexpr (native_stream) {
            return stream_.get_executor();
        } else {
            return stream_.executor();
        }
    }
    auto write_bytes(const websocket_output_plan& plan);
    void close_stream() noexcept;
    void abort_transport(bool force_transport = false) noexcept;
    // Aborts the transport and retains the first background write failure.
    void fail_background(std::exception_ptr failure) noexcept;
    void notify_write_idle() noexcept;
    [[nodiscard]] bool has_operations_to_drain() const noexcept {
        return read_phase_ != read_phase_type::idle || write_active_ || write_phase_ != write_phase_type::idle;
    }

    stream_argument_type stream_;
    const worker_handle* worker_;
    connection_scanner::entry& scanner_entry_;
    websocket_lifecycle_options lifecycle_options_{};
    http_io_buffer buffer_;
    websocket_connection protocol_;
    worker_signal background_write_signal_;
    worker_signal reader_done_signal_;
    write_phase_type write_phase_{write_phase_type::idle};
    bool write_active_{false};
    read_phase_type read_phase_{read_phase_type::idle};
    websocket_liveness_state_type liveness_state_{websocket_liveness_idle{}};
    bool going_away_pending_{false};
    std::exception_ptr background_failure_;
    std::uint64_t heartbeat_sequence_{0};
    // Whether the last read between frames received less than
    // websocket_idle_read_bytes: the next such read then waits in a buffer of
    // that size.
    bool small_frames_{true};
    // Declared last so destruction unregisters before any callback target state
    // starts to disappear.
    connection_scanner::periodic_check_registration periodic_check_;
};

}  // namespace ruvia::detail

#include "websocket/http_websocket_connection_heartbeat.inl"
#include "websocket/http_websocket_connection_read.inl"
#include "websocket/http_websocket_connection_write.inl"
