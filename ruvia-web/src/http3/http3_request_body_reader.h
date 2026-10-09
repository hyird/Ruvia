#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/core/worker_signal.h"

namespace ruvia::detail {

// Worker-affine HTTP/3 streaming request body storage. The HTTP/3 transport
// owner must copy incoming bytes here on this worker; this type has no UDP or
// QUIC dependency. A returned view stays valid until the next read() call.
// backlog_limit bounds the queued bytes, excluding the single active chunk
// borrowed by the consumer; it is not an allocator-capacity or QUIC memory cap.
class http3_request_body_reader final {
public:
    enum class terminal_type : unsigned char { open,
        fin,
        error,
        cancelled,
        shutdown };

    http3_request_body_reader(const worker_handle& worker_value, std::size_t backlog_limit,
        std::pmr::memory_resource* resource)
        : signal_(worker_value),
          space_signal_(worker_value),
          queued_(resource_or_default(resource)),
          active_(resource_or_default(resource)),
          backlog_limit_(backlog_limit) {}
    http3_request_body_reader(worker_handle&&, std::size_t, std::pmr::memory_resource*) = delete;
    http3_request_body_reader(const http3_request_body_reader&) = delete;
    http3_request_body_reader& operator=(const http3_request_body_reader&) = delete;
    ~http3_request_body_reader() {
        if (reading_ || waiting_for_space_) {
            std::terminate();
        }
    }

    // Enqueue is worker-affine, copies before returning, and checks capacity
    // before allocating or modifying the pending queue.
    [[nodiscard]] bool enqueue(std::span<const std::byte> bytes_value) {
        if (terminal_ != terminal_type::open || queued_.size() > backlog_limit_ ||
            bytes_value.size() > backlog_limit_ - queued_.size()) {
            return false;
        }
        if (!bytes_value.empty()) {
            queued_.append(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size());
            signal_.notify();
        }
        return true;
    }
    [[nodiscard]] bool enqueue(std::string_view bytes_value) {
        return enqueue(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(bytes_value.data()), bytes_value.size()));
    }

    // FIN is a level-triggered state behind all already-enqueued bytes.
    [[nodiscard]] bool finish() noexcept {
        if (terminal_ != terminal_type::open) {
            return false;
        }
        terminal_ = terminal_type::fin;
        signal_.notify();
        space_signal_.notify();
        return true;
    }
    [[nodiscard]] bool fail(std::error_code error) noexcept {
        if (terminal_ != terminal_type::open) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::protocol_error);
        terminal_ = terminal_type::error;
        clear(queued_);
        signal_.notify();
        space_signal_.notify();
        return true;
    }
    [[nodiscard]] bool cancel() noexcept {
        return set_terminal(terminal_type::cancelled);
    }
    [[nodiscard]] bool shutdown() noexcept {
        return set_terminal(terminal_type::shutdown);
    }

    [[nodiscard]] terminal_type terminal() const noexcept {
        return terminal_;
    }
    [[nodiscard]] std::size_t queued_bytes() const noexcept {
        return queued_.size();
    }
    [[nodiscard]] std::size_t available_capacity() const noexcept {
        return backlog_limit_ - queued_.size();
    }
    // Single producer, lazy and worker-affine. True means queued capacity is
    // available; false means the receive side terminated. The consumer's
    // borrowed active chunk is separate from this backlog and remains valid.
    // Stop via finish/fail/cancel/shutdown, then join before destroying us.
    [[nodiscard]] task<bool> wait_for_space(std::size_t minimum_bytes = 1) & {
        if (!space_signal_.worker().is_current()) {
            throw std::logic_error("HTTP/3 producer must wait on its owner worker");
        }
        if (waiting_for_space_) {
            throw std::logic_error("HTTP/3 body reader already has a space waiter");
        }
        if (minimum_bytes > backlog_limit_) {
            throw std::invalid_argument("HTTP/3 producer chunk exceeds backlog capacity");
        }
        waiting_for_space_ = true;
        struct wait_guard final {
            bool& waiting_;
            ~wait_guard() {
                waiting_ = false;
            }
        } guard_value{waiting_for_space_};
        while (terminal_ == terminal_type::open && available_capacity() < minimum_bytes) {
            co_await space_signal_.wait();
        }
        co_return terminal_ == terminal_type::open;
    }
    task<bool> wait_for_space(std::size_t = 1) && = delete;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() & {
        if (reading_) {
            throw std::logic_error("HTTP/3 body reader already has an active read");
        }
        reading_ = true;
        struct read_guard final {
            bool& active_;
            ~read_guard() {
                active_ = false;
            }
        } guard_value{reading_};

        for (;;) {
            // Retire the previous result only when the next read actually starts.
            clear(active_);
            // Error, cancellation and shutdown discard unread data; only a
            // verified FIN may drain queued bytes before reporting normal EOF.
            switch (terminal_) {
                case terminal_type::error:
                    throw std::system_error(error_);
                case terminal_type::cancelled:
                    throw std::system_error(std::make_error_code(std::errc::operation_canceled));
                case terminal_type::shutdown:
                    throw std::system_error(std::make_error_code(std::errc::owner_dead),
                        "HTTP/3 worker shutdown");
                case terminal_type::open:
                case terminal_type::fin:
                    break;
            }
            if (!queued_.empty()) {
                active_.swap(queued_);
                clear(queued_);
                space_signal_.notify();
                co_return std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(active_.data()), active_.size());
            }
            if (terminal_ == terminal_type::fin) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }
    task<std::optional<std::span<const std::byte>>> read() && = delete;

private:
    static std::pmr::memory_resource* resource_or_default(std::pmr::memory_resource* resource) noexcept {
        return resource == nullptr ? std::pmr::get_default_resource() : resource;
    }
    static void clear(std::pmr::string& value) noexcept {
        value.clear();
        if (value.capacity() > 256) {
            std::pmr::string empty(value.get_allocator().resource());
            value.swap(empty);
        }
    }
    bool set_terminal(terminal_type terminal) noexcept {
        if (terminal_ != terminal_type::open) {
            return false;
        }
        terminal_ = terminal;
        clear(queued_);
        signal_.notify();
        space_signal_.notify();
        return true;
    }

    worker_signal signal_;
    worker_signal space_signal_;
    bool waiting_for_space_{false};
    std::pmr::string queued_;
    std::pmr::string active_;
    const std::size_t backlog_limit_;
    terminal_type terminal_{terminal_type::open};
    std::error_code error_;
    bool reading_{false};
};

}  // namespace ruvia::detail
