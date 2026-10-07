#pragma once

#include <cstddef>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/memory/ProcessResource.h"

namespace ruvia {

class buffer_pool;
class buffer_lease;

// A linear return token. It must reach its pool's owner before destruction.
// Moving transfers the obligation; abandoning a live token is terminal.
class buffer_credit final {
public:
    buffer_credit() noexcept = default;
    buffer_credit(const buffer_credit&) = delete;
    buffer_credit& operator=(const buffer_credit&) = delete;
    buffer_credit(buffer_credit&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)),
          index_(other.index_) {}
    buffer_credit& operator=(buffer_credit&& other) noexcept {
        if (this != &other) {
            if (owner_) {
                std::terminate();
            }
            owner_ = std::exchange(other.owner_, nullptr);
            index_ = other.index_;
        }
        return *this;
    }
    ~buffer_credit() {
        if (owner_) {
            std::terminate();
        }
    }

    [[nodiscard]] std::size_t index() const noexcept {
        if (!owner_) {
            std::terminate();
        }
        return index_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

private:
    buffer_credit(buffer_pool& owner, std::size_t index) noexcept
        : owner_(&owner),
          index_(index) {}

    buffer_pool* owner_{nullptr};
    std::size_t index_{0};
    friend class buffer_pool;
    friend class buffer_lease;
};

// The callback and its context must outlive the lease. A cross-thread callback
// publishes only a credit into its bounded single-producer return channel; it
// must not touch the pool's owner-affine bookkeeping or memory resource.
struct return_callback final {
    void* context{nullptr};
    void (*function)(void*, buffer_credit) noexcept {nullptr};
};

class buffer_lease final {
public:
    buffer_lease() noexcept = default;
    buffer_lease(const buffer_lease&) = delete;
    buffer_lease& operator=(const buffer_lease&) = delete;
    buffer_lease(buffer_lease&& other) noexcept
        : credit_(std::move(other.credit_)),
          bytes_(std::exchange(other.bytes_, {})),
          callback_(other.callback_) {}
    buffer_lease& operator=(buffer_lease&& other) noexcept {
        if (this != &other) {
            reset();
            credit_ = std::move(other.credit_);
            bytes_ = std::exchange(other.bytes_, {});
            callback_ = other.callback_;
        }
        return *this;
    }
    ~buffer_lease() {
        reset();
    }

    [[nodiscard]] std::span<std::byte> bytes() noexcept {
        return bytes_;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] std::size_t index() const noexcept {
        return credit_.index();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(credit_);
    }

    // Only the current linear holder may rebind its return destination.
    void set_return_callback(return_callback callback) noexcept {
        if (!credit_ || !callback.function) {
            std::terminate();
        }
        callback_ = callback;
    }

    // Invalidates every byte borrow. Empty/moved-from leases reset harmlessly.
    void reset() noexcept {
        if (credit_) {
            auto credit = release_credit();
            callback_.function(callback_.context, std::move(credit));
        }
    }

    // The caller must have a destination for this credit. No pool access occurs.
    // Releasing an empty/already released lease is a contract violation.
    [[nodiscard]] buffer_credit release_credit() noexcept {
        if (!credit_) {
            std::terminate();
        }
        bytes_ = {};
        return std::move(credit_);
    }

private:
    buffer_lease(buffer_pool& owner, std::size_t index, std::span<std::byte> bytes, return_callback callback) noexcept
        : credit_(owner, index),
          bytes_(bytes),
          callback_(callback) {}

    buffer_credit credit_;
    std::span<std::byte> bytes_;
    return_callback callback_;
    friend class buffer_pool;
};

// Stable authority for preallocated blocks and free indices, shared by local
// and cross-thread channels. Construction/acquire/reclaim/queries/destruction
// are owner-affine. Only leases and credits travel to the other endpoint.
// No allocation, resource access, locks, or shared ownership on acquire/return.
// The pool, PMR resource, and callback contexts outlive all leases and credits.
// Closing a channel must still drain credits; destroy only after outstanding=0.
class buffer_pool final {
public:
    buffer_pool(std::size_t count, std::size_t bytes, std::pmr::memory_resource* resource = nullptr)
        : bytes_(resource ? resource : detail::processResource()),
          free_(bytes_.get_allocator().resource()),
          busy_(bytes_.get_allocator().resource()),
          slot_bytes_(bytes) {
        if (count == 0 || bytes == 0 || count > std::numeric_limits<std::size_t>::max() / bytes) {
            throw std::invalid_argument("buffer pool requires nonzero, representable storage dimensions");
        }
        bytes_.resize(count * bytes);
        free_.reserve(count);
        busy_.resize(count, false);
        for (std::size_t index = count; index != 0; --index) {
            free_.push_back(index - 1);
        }
    }

    buffer_pool(const buffer_pool&) = delete;
    buffer_pool& operator=(const buffer_pool&) = delete;
    buffer_pool(buffer_pool&&) = delete;
    buffer_pool& operator=(buffer_pool&&) = delete;
    ~buffer_pool() {
        if (outstanding() != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] std::optional<buffer_lease> try_acquire(return_callback callback = {}) noexcept {
        if (free_.empty()) {
            return std::nullopt;
        }
        if (!callback.function) {
            if (callback.context) {
                std::terminate();
            }
            callback = {this, owner_reclaim};
        }
        const auto index = free_.back();
        free_.pop_back();
        busy_[index] = true;
        return buffer_lease(*this, index, std::span<std::byte>(bytes_).subspan(index * slot_bytes_, slot_bytes_), callback);
    }

    void reclaim(buffer_credit&& credit) noexcept {
        if (credit.owner_ != this || credit.index_ >= capacity() || !busy_[credit.index_]) {
            std::terminate();
        }
        const auto index = credit.index_;
        credit.owner_ = nullptr;
        busy_[index] = false;
        free_.push_back(index);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return busy_.size();
    }
    [[nodiscard]] std::size_t available() const noexcept {
        return free_.size();
    }
    [[nodiscard]] std::size_t outstanding() const noexcept {
        return capacity() - available();
    }

private:
    static void owner_reclaim(void* context, buffer_credit credit) noexcept {
        static_cast<buffer_pool*>(context)->reclaim(std::move(credit));
    }

    std::pmr::vector<std::byte> bytes_;
    std::pmr::vector<std::size_t> free_;
    std::pmr::vector<bool> busy_;
    std::size_t slot_bytes_;
};

}  // namespace ruvia
