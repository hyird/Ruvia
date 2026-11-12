#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

namespace ruvia::detail {

// One worker's admitted-connection gauge. Only the owning worker counts; any
// thread reads it relaxed (stats(), and the Windows acceptor choosing the
// least-loaded worker).
class connection_count final {
public:
    connection_count() = default;
    connection_count(const connection_count&) = delete;
    connection_count& operator=(const connection_count&) = delete;

    [[nodiscard]] std::size_t active() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    // Worker thread: one admitted connection.
    void acquire() noexcept {
        active_.fetch_add(1, std::memory_order_relaxed);
    }

    // Worker thread: one closed connection.
    void release() noexcept {
        if (active_.fetch_sub(1, std::memory_order_relaxed) == 0) {
            std::terminate();
        }
    }

private:
    std::atomic<std::size_t> active_{};
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
