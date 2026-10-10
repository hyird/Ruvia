#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/core/pmr_string.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/websocket_server_protocol.h"
#include "ruvia/web/websocket.h"

#include "http/http_stream_read_result.h"
#include "websocket/http_websocket_liveness.h"

namespace ruvia::detail {

// Transport-agnostic websocket connection (RFC 6455). All protocol behavior,
// including frame reassembly, write serialization, heartbeats, and close,
// lives here; the HTTP/1.1 and HTTP/2 transports differ only in the Transport
// policy, which supplies four transport-specific operations:
//   asio-executor executor() const;
//   task<http_stream_read_result> read_more(std::pmr::string& buffer);
//   task<std::error_code> write_bytes(std::string_view, websocket_transport_disposition);
//   void abort() noexcept;  // abort this websocket transport, not an unrelated h2 stream
template <typename transport_type>
class websocket_connection final {
public:
    websocket_connection(transport_type transport, const worker_handle& worker_value,
        ruvia::connection_scanner::entry_type& scanner_entry, websocket_lifecycle_options lifecycle_options,
        protocol_byte_limit message_limit, std::pmr::memory_resource* resource,
        std::string_view initial_bytes = {},
        websocket_compression compression = (websocket_compression{}),
        int compression_level = 6)
        : transport_(std::move(transport)),
          worker_(&worker_value),
          scanner_entry_(scanner_entry),
          lifecycle_options_(lifecycle_options),
          buffer_(pmr_resource_or_default(resource)),
          protocol_(buffer_, message_limit, websocket_server_protocol_options{compression, compression_level}),
          background_write_signal_(worker_value),
          reader_done_signal_(worker_value) {
        buffer_.append(initial_bytes.data(), initial_bytes.size());
        scanner_entry_.register_periodic_check(
            periodic_check_, this, &websocket_connection::heartbeat_tick_thunk);
    }

    websocket_connection(transport_type, worker_handle&&, ruvia::connection_scanner::entry_type&,
        websocket_lifecycle_options, protocol_byte_limit, std::pmr::memory_resource*,
        std::string_view = {}, websocket_compression = (websocket_compression{}), int = 6) = delete;

    ~websocket_connection() = default;

    websocket_connection(const websocket_connection&) = delete;
    websocket_connection& operator=(const websocket_connection&) = delete;

    static void heartbeat_tick_thunk(void* target, std::int64_t now) noexcept {
        static_cast<websocket_connection*>(target)->heartbeat_tick(now);
    }

    [[nodiscard]] task<std::optional<websocket_message>> read();
    task<void> write(websocket_opcode opcode, std::string_view payload, bool compress = true);
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

private:
    enum class write_phase_type : std::uint8_t {
        idle,
        application,
        heartbeat,
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
        write_guard_type(websocket_connection& connection, write_phase_type phase,
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
        websocket_connection& connection_;
        write_phase_type phase_;
    };

    class write_operation_lease_type final {
    public:
        explicit write_operation_lease_type(websocket_connection& connection)
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
        websocket_connection* connection_;
    };

    class read_guard_type final {
    public:
        explicit read_guard_type(websocket_connection& connection)
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
                const bool started = connection_->read_phase_ == read_phase_type::active;
                connection_->read_phase_ = read_phase_type::idle;
                if (started || connection_->worker_->is_current()) {
                    connection_->reader_done_signal_.notify();
                }
            }
        }

    private:
        websocket_connection* connection_;
    };

    void finish_write(write_phase_type phase) noexcept;
    void heartbeat_tick(std::int64_t now) noexcept;
    task<std::optional<websocket_message>> read_owned(read_guard_type read_guard);
    task<void> write_owned(
        websocket_opcode opcode, std::string_view payload, write_operation_lease_type write_lease, bool compress);
    task<void> close_owned(::ruvia::websocket_close_options options, write_operation_lease_type write_lease);
    task<void> write_heartbeat_ping();
    task<void> wait_for_write_idle();
    task<void> write_exclusive(websocket_opcode opcode, std::string_view payload, bool compress = true);
    task<void> write_frame_now(websocket_opcode opcode, std::string_view payload, bool compress = true);
    task<void> flush_protocol_output_exclusive();
    task<void> flush_protocol_output_now();
    void abort_transport(bool force_transport = false) noexcept;
    void notify_write_idle() noexcept;
    [[nodiscard]] bool has_operations_to_drain() const noexcept {
        return read_phase_ != read_phase_type::idle || write_active_ || write_phase_ != write_phase_type::idle;
    }

    transport_type transport_;
    const worker_handle* worker_;
    ruvia::connection_scanner::entry_type& scanner_entry_;
    websocket_lifecycle_options lifecycle_options_{};
    std::pmr::string buffer_;
    websocket_server_protocol protocol_;
    worker_signal background_write_signal_;
    worker_signal reader_done_signal_;
    write_phase_type write_phase_{write_phase_type::idle};
    bool write_active_{false};
    read_phase_type read_phase_{read_phase_type::idle};
    websocket_liveness_state_type liveness_state_{websocket_liveness_idle{}};
    std::uint64_t heartbeat_sequence_{0};
    // Declared last so destruction unregisters before any callback target state
    // starts to disappear.
    ruvia::connection_scanner::periodic_check_registration_type periodic_check_;
};

}  // namespace ruvia::detail

#include "websocket/http_websocket_connection_heartbeat.inl"
#include "websocket/http_websocket_connection_read.inl"
#include "websocket/http_websocket_connection_write.inl"
