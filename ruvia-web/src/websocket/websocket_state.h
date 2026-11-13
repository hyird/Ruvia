#pragma once

#include "ruvia/web/websocket.h"

namespace ruvia::detail {

// Address-stable lane leases and capability scope shared by both endpoint roles.
// The client also retains its transport owner; server transports remain borrowed.
class websocket_state final : public std::enable_shared_from_this<websocket_state> {
public:
    using read_type = task<std::optional<websocket_message>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, websocket_opcode, std::string_view, bool, websocket_write_extent, operation_options);
    using close_type = task<void> (*)(void*, websocket_close_options, operation_options);
    using abort_type = void (*)(void*) noexcept;
    using peer_close_code_type = std::optional<std::uint16_t> (*)(void*) noexcept;
    using peer_close_reason_type = std::string_view (*)(void*) noexcept;
    using protocol_type = const websocket_connection& (*)(void*) noexcept;

    websocket_state(std::pmr::memory_resource& resource, const worker_handle& worker, void* target,
        std::string_view subprotocol, read_type read, write_type write, close_type close,
        abort_type abort, peer_close_code_type peer_close_code, peer_close_reason_type peer_close_reason,
        protocol_type protocol) noexcept
        : resource_(&resource),
          worker_storage_(worker),
          worker_(&worker_storage_),
          target_(target),
          subprotocol_(subprotocol),
          read_(read),
          write_(write),
          close_(close),
          abort_(abort),
          peer_close_code_(peer_close_code),
          peer_close_reason_(peer_close_reason),
          protocol_(protocol) {
        operation_scope_.bind_worker(worker_storage_);
    }
    websocket_state(const websocket_state&) = delete;
    websocket_state& operator=(const websocket_state&) = delete;
    ~websocket_state() {
        if (operation_scope_.has_pending_operations() && !worker_->is_current()) {
            std::terminate();
        }
    }

    void require_active() const {
        if (!worker_->is_current()) {
            std::terminate();
        }
        if (!operation_scope_.active()) {
            throw std::logic_error("websocket lifetime has expired");
        }
    }
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options) &;
    task<void> text(std::string_view, websocket_send_options, operation_options) &;
    task<void> binary(std::string_view, websocket_send_options, operation_options) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options, operation_options) &;
    task<void> pong(std::string_view, operation_options) &;
    task<void> ping(std::string_view, operation_options) &;
    task<void> send(websocket_message&&, websocket_send_options, operation_options) &;
    task<void> close(websocket_close_options, operation_options) &;
    void abort() & noexcept;
    websocket_message_writer begin_text(websocket_send_options) &;
    websocket_message_writer begin_binary(websocket_send_options) &;
    websocket_message_writer begin_message(websocket_opcode, bool);
    task<void> write(websocket_opcode, std::string_view, bool, operation_options, websocket_write_extent = websocket_write_extent::message);
    task<void> write(websocket_opcode, std::pmr::string&&, bool, operation_options, websocket_write_extent = websocket_write_extent::message);

    [[nodiscard]] std::string_view subprotocol() const noexcept {
        if (!worker_->is_current()) {
            std::terminate();
        }
        return operation_scope_.active() ? subprotocol_ : std::string_view{};
    }
    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept;
    [[nodiscard]] std::string_view peer_close_reason() const& noexcept;
    [[nodiscard]] websocket_termination termination() const noexcept;
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;

    std::pmr::memory_resource* resource_;
    worker_handle worker_storage_;
    const worker_handle* worker_;
    void* target_;
    std::string_view subprotocol_;
    read_type read_;
    write_type write_;
    close_type close_;
    abort_type abort_;
    peer_close_code_type peer_close_code_;
    peer_close_reason_type peer_close_reason_;
    protocol_type protocol_;
    bool read_active_{false};
    bool write_active_{false};
    bool message_send_active_{false};
    bool fragmented_message_active_{false};
    std::uint64_t fragmented_message_sequence_{0};
    operation_options defaults_;
    std::optional<timeout_duration> read_timeout_;
    std::shared_ptr<void> owner_;
    void (*retire_)(void*) noexcept {nullptr};
    operation_scope operation_scope_;
};

}  // namespace ruvia::detail
