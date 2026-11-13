#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/server_config.h"

#include "integration/operation_interrupt.h"

namespace ruvia::detail {

// One worker's admitted-connection gauge. Only the owning worker counts; any
// thread reads it relaxed (stats(), and the Windows acceptor choosing the
// least-loaded worker).
class connection_count final {
public:
    explicit connection_count(connection_count* parent = nullptr) noexcept
        : parent_(parent) {}
    connection_count(const connection_count&) = delete;
    connection_count& operator=(const connection_count&) = delete;

    [[nodiscard]] std::size_t active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    // Worker thread: one admitted connection.
    void acquire() noexcept {
        active_.fetch_add(1, std::memory_order_relaxed);
        if (parent_) {
            parent_->acquire();
        }
    }

    // Worker thread: one closed connection.
    void release() noexcept {
        const auto previous = active_.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 0) {
            std::terminate();
        }
        if (parent_) {
            parent_->release();
        }
        if (previous == 1) {
            std::vector<std::weak_ptr<wait_state>> ready;
            {
                std::lock_guard lock(mutex_);
                ready.swap(waiters_);
            }
            for (auto& weak : ready) {
                if (auto state = weak.lock()) {
                    state->closed_.store(true, std::memory_order_release);
                    detail::worker_ref_access::defer_or_terminate(state->worker_, [state] { state->changed_.notify(); });
                }
            }
        }
    }

    // The caller retains the gauge; registrations never borrow its coroutine.
    static task<void> wait(std::shared_ptr<connection_count> count, worker_ref caller, operation_options options, stop_token host_stop) {
        if (!caller.is_current()) {
            throw std::logic_error("connection wait must run on its caller worker");
        }
        stop_group stops(options.stop_token_, host_stop);
        options.stop_token_ = stops.token();
        auto state = std::make_shared<wait_state>(caller);
        operation_interrupt_source source;
        operation_interrupt interrupt(source, caller, options, state.get(), [](void* target) noexcept { static_cast<wait_state*>(target)->changed_.notify(); }, "wait_connections_closed");
        {
            std::lock_guard lock(count->mutex_);
            if (count->active() != 0) {
                count->waiters_.push_back(state);
            } else {
                state->closed_.store(true, std::memory_order_relaxed);
            }
        }
        struct subscription final {
            connection_count& count_;
            wait_state* state_;
            ~subscription() {
                std::lock_guard lock(count_.mutex_);
                std::erase_if(count_.waiters_, [this](const auto& weak) {
                    const auto state = weak.lock();
                    return !state || state.get() == state_;
                });
            }
        } registration{*count, state.get()};
        interrupt.throw_if_interrupted();
        while (!state->closed_.load(std::memory_order_acquire)) {
            co_await state->changed_.wait();
            interrupt.throw_if_interrupted();
        }
        interrupt.throw_if_interrupted();
    }

private:
    std::atomic<std::size_t> active_{};
    struct wait_state final {
        explicit wait_state(worker_ref worker)
            : worker_(std::move(worker)),
              changed_(worker_) {}
        worker_ref worker_;
        worker_signal changed_;
        std::atomic<bool> closed_{false};
    };
    connection_count* parent_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<wait_state>> waiters_;
};

// One admitted connection's place in its worker's connection_count, as a
// linear value. Every transport's connection owns exactly one: a TCP session
// through accepted_connection_lease, a QUIC connection directly.
class connection_slot final {
public:
    explicit connection_slot(connection_count& count) noexcept
        : count_(&count) {
        count_->acquire();
    }

    connection_slot(const connection_slot&) = delete;
    connection_slot& operator=(const connection_slot&) = delete;

    connection_slot(connection_slot&& other) noexcept
        : count_(std::exchange(other.count_, nullptr)) {}

    connection_slot& operator=(connection_slot&&) = delete;

    ~connection_slot() {
        if (count_ != nullptr) {
            count_->release();
        }
    }

private:
    connection_count* count_;
};

// Merge endpoint gauges across workers and successive listener instances.
inline void append_listener_stats(http_server_stats& total, const http_server_stats& worker) {
    for (const auto& listener : worker.listeners_) {
        auto found = std::find_if(total.listeners_.begin(), total.listeners_.end(), [&](const auto& existing) {
            return existing.address_ == listener.address_ && existing.port_ == listener.port_;
        });
        if (found == total.listeners_.end()) {
            total.listeners_.push_back(listener);
        } else {
            found->active_connections_ += listener.active_connections_;
        }
    }
}

// Process-wide monotonically increasing IDs cannot alias across workers,
// hosts, or restarts. Exhaustion terminates rather than recycling a stale ID.
class connection_id_sequence final {
public:
    connection_id_sequence() noexcept = default;

    connection_id_sequence(const connection_id_sequence&) = delete;
    connection_id_sequence& operator=(const connection_id_sequence&) = delete;

    [[nodiscard]] std::uint64_t next() noexcept {
        const auto id = next_.fetch_add(1, std::memory_order_relaxed);
        if (id == 0) {
            std::terminate();
        }
        return id;
    }

private:
    inline static std::atomic<std::uint64_t> next_{1};
};

}  // namespace ruvia::detail
