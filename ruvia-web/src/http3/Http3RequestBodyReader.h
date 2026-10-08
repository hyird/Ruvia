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

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerSignal.h"

namespace ruvia::detail {

// Worker-affine HTTP/3 streaming request body storage. The HTTP/3 transport
// owner must copy incoming bytes here on this worker; this type has no UDP or
// QUIC dependency. A returned view stays valid until the next read() call.
// backlogLimit bounds the queued bytes, excluding the single active chunk
// borrowed by the consumer; it is not an allocator-capacity or QUIC memory cap.
class Http3RequestBodyReader final {
public:
    enum class Terminal : unsigned char { kOpen,
        kFin,
        kError,
        kCancelled,
        kShutdown };

    Http3RequestBodyReader(const WorkerHandle& worker, std::size_t backlogLimit,
        std::pmr::memory_resource* resource)
        : signal_(worker),
          spaceSignal_(worker),
          queued_(resourceOrDefault(resource)),
          active_(resourceOrDefault(resource)),
          backlogLimit_(backlogLimit) {}
    Http3RequestBodyReader(WorkerHandle&&, std::size_t, std::pmr::memory_resource*) = delete;
    Http3RequestBodyReader(const Http3RequestBodyReader&) = delete;
    Http3RequestBodyReader& operator=(const Http3RequestBodyReader&) = delete;
    ~Http3RequestBodyReader() {
        if (reading_ || waitingForSpace_) {
            std::terminate();
        }
    }

    // Enqueue is worker-affine, copies before returning, and checks capacity
    // before allocating or modifying the pending queue.
    [[nodiscard]] bool enqueue(std::span<const std::byte> bytes) {
        if (terminal_ != Terminal::kOpen || queued_.size() > backlogLimit_ ||
            bytes.size() > backlogLimit_ - queued_.size()) {
            return false;
        }
        if (!bytes.empty()) {
            queued_.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            signal_.notify();
        }
        return true;
    }
    [[nodiscard]] bool enqueue(std::string_view bytes) {
        return enqueue(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    }

    // FIN is a level-triggered state behind all already-enqueued bytes.
    [[nodiscard]] bool finish() noexcept {
        if (terminal_ != Terminal::kOpen) {
            return false;
        }
        terminal_ = Terminal::kFin;
        signal_.notify();
        spaceSignal_.notify();
        return true;
    }
    [[nodiscard]] bool fail(std::error_code error) noexcept {
        if (terminal_ != Terminal::kOpen) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::protocol_error);
        terminal_ = Terminal::kError;
        clear(queued_);
        signal_.notify();
        spaceSignal_.notify();
        return true;
    }
    [[nodiscard]] bool cancel() noexcept {
        return setTerminal(Terminal::kCancelled);
    }
    [[nodiscard]] bool shutdown() noexcept {
        return setTerminal(Terminal::kShutdown);
    }

    [[nodiscard]] Terminal terminal() const noexcept {
        return terminal_;
    }
    [[nodiscard]] std::size_t queuedBytes() const noexcept {
        return queued_.size();
    }
    [[nodiscard]] std::size_t availableCapacity() const noexcept {
        return backlogLimit_ - queued_.size();
    }
    // Single producer, lazy and worker-affine. True means queued capacity is
    // available; false means the receive side terminated. The consumer's
    // borrowed active chunk is separate from this backlog and remains valid.
    // Stop via finish/fail/cancel/shutdown, then join before destroying us.
    [[nodiscard]] Task<bool> waitForSpace(std::size_t minimumBytes = 1) & {
        if (!spaceSignal_.worker().isCurrent()) {
            throw std::logic_error("HTTP/3 producer must wait on its owner worker");
        }
        if (waitingForSpace_) {
            throw std::logic_error("HTTP/3 body reader already has a space waiter");
        }
        if (minimumBytes > backlogLimit_) {
            throw std::invalid_argument("HTTP/3 producer chunk exceeds backlog capacity");
        }
        waitingForSpace_ = true;
        struct WaitGuard final {
            bool& waiting;
            ~WaitGuard() {
                waiting = false;
            }
        } guard{waitingForSpace_};
        while (terminal_ == Terminal::kOpen && availableCapacity() < minimumBytes) {
            co_await spaceSignal_.wait();
        }
        co_return terminal_ == Terminal::kOpen;
    }
    Task<bool> waitForSpace(std::size_t = 1) && = delete;
    [[nodiscard]] Task<std::optional<std::span<const std::byte>>> read() & {
        if (reading_) {
            throw std::logic_error("HTTP/3 body reader already has an active read");
        }
        reading_ = true;
        struct ReadGuard final {
            bool& active;
            ~ReadGuard() {
                active = false;
            }
        } guard{reading_};

        for (;;) {
            // Retire the previous result only when the next read actually starts.
            clear(active_);
            // Error, cancellation and shutdown discard unread data; only a
            // verified FIN may drain queued bytes before reporting normal EOF.
            switch (terminal_) {
                case Terminal::kError:
                    throw std::system_error(error_);
                case Terminal::kCancelled:
                    throw std::system_error(std::make_error_code(std::errc::operation_canceled));
                case Terminal::kShutdown:
                    throw std::system_error(std::make_error_code(std::errc::owner_dead),
                        "HTTP/3 worker shutdown");
                case Terminal::kOpen:
                case Terminal::kFin:
                    break;
            }
            if (!queued_.empty()) {
                active_.swap(queued_);
                clear(queued_);
                spaceSignal_.notify();
                co_return std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(active_.data()), active_.size());
            }
            if (terminal_ == Terminal::kFin) {
                co_return std::nullopt;
            }
            co_await signal_.wait();
        }
    }
    Task<std::optional<std::span<const std::byte>>> read() && = delete;

private:
    static std::pmr::memory_resource* resourceOrDefault(std::pmr::memory_resource* resource) noexcept {
        return resource == nullptr ? std::pmr::get_default_resource() : resource;
    }
    static void clear(std::pmr::string& value) noexcept {
        value.clear();
        if (value.capacity() > 256) {
            std::pmr::string empty(value.get_allocator().resource());
            value.swap(empty);
        }
    }
    bool setTerminal(Terminal terminal) noexcept {
        if (terminal_ != Terminal::kOpen) {
            return false;
        }
        terminal_ = terminal;
        clear(queued_);
        signal_.notify();
        spaceSignal_.notify();
        return true;
    }

    WorkerSignal signal_;
    WorkerSignal spaceSignal_;
    bool waitingForSpace_{false};
    std::pmr::string queued_;
    std::pmr::string active_;
    const std::size_t backlogLimit_;
    Terminal terminal_{Terminal::kOpen};
    std::error_code error_;
    bool reading_{false};
};

}  // namespace ruvia::detail
