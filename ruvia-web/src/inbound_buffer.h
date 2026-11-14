#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"

// Live inbound buffer bytes charged against a per-connection limit and the
// worker budget together; exhaustion throws inbound_buffer_limit_error, which
// servers answer with 503. Every growth helper charges before it allocates and
// settles the reservation on the capacity actually held.
namespace ruvia::detail {

class inbound_buffer_limit_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "inbound buffer memory budget exhausted";
    }
};

class inbound_buffer_account;
class inbound_buffer_reservation;

// A worker's live inbound buffer bytes, shared by the inbound_buffer_account of
// each of its connections. Worker-affine, no synchronization. This owner must
// outlive every connection account.
class inbound_buffer_budget final {
public:
    explicit inbound_buffer_budget(std::size_t limit) :
        limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("inbound buffer budget requires a positive limit");
        }
    }
    ~inbound_buffer_budget() {
        if (used_ != 0) {
            std::terminate();
        }
    }
    inbound_buffer_budget(const inbound_buffer_budget&) = delete;
    inbound_buffer_budget& operator=(const inbound_buffer_budget&) = delete;

private:
    friend class inbound_buffer_account;
    const std::size_t limit_;
    std::size_t used_{};
};

// One connection's inbound buffers. Each reservation is checked against the
// connection's limit and its worker's budget, both charged together. Every
// reservation must be returned before this owner is destroyed.
class inbound_buffer_account final {
public:
    inbound_buffer_account(inbound_buffer_budget& worker, std::size_t limit) :
        worker_(worker),
        limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("inbound buffer resource requires a positive limit");
        }
    }
    ~inbound_buffer_account() {
        if (used_ != 0) {
            std::terminate();
        }
    }
    inbound_buffer_account(const inbound_buffer_account&) = delete;
    inbound_buffer_account& operator=(const inbound_buffer_account&) = delete;

    [[nodiscard]] inbound_buffer_reservation reserve(std::size_t bytes_value);

private:
    friend class inbound_buffer_reservation;

    void charge_bytes(std::size_t bytes_value) {
        if (bytes_value > limit_ - used_ || bytes_value > worker_.limit_ - worker_.used_) {
            throw inbound_buffer_limit_error();
        }
        used_ += bytes_value;
        worker_.used_ += bytes_value;
    }

    void release_bytes(std::size_t bytes_value) noexcept {
        if (bytes_value > used_ || bytes_value > worker_.used_) {
            std::terminate();
        }
        used_ -= bytes_value;
        worker_.used_ -= bytes_value;
    }

    inbound_buffer_budget& worker_;
    const std::size_t limit_;
    std::size_t used_{};
};

// Move-only RAII reservation of live inbound buffer capacity against an
// inbound_buffer_account (no-op when unbound).
class inbound_buffer_reservation final {
public:
    constexpr inbound_buffer_reservation() noexcept = default;
    explicit constexpr inbound_buffer_reservation(inbound_buffer_account* account) noexcept :
        account_(account) {}
    inbound_buffer_reservation(inbound_buffer_account& account, std::size_t bytes_value) :
        account_(&account) {
        if (bytes_value != 0) {
            account_->charge_bytes(bytes_value);
            bytes_ = bytes_value;
        }
    }
    ~inbound_buffer_reservation() {
        release();
    }

    inbound_buffer_reservation(const inbound_buffer_reservation&) = delete;
    inbound_buffer_reservation& operator=(const inbound_buffer_reservation&) = delete;

    inbound_buffer_reservation(inbound_buffer_reservation&& other) noexcept :
        account_(std::exchange(other.account_, nullptr)),
        bytes_(std::exchange(other.bytes_, 0)) {}

    inbound_buffer_reservation& operator=(inbound_buffer_reservation&& other) noexcept {
        if (this != &other) {
            release();
            account_ = std::exchange(other.account_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    [[nodiscard]] constexpr inbound_buffer_account* account() const noexcept {
        return account_;
    }

    [[nodiscard]] constexpr std::size_t bytes() const noexcept {
        return bytes_;
    }

    void resize_to(std::size_t target_bytes) {
        if (account_ == nullptr || target_bytes == bytes_) {
            return;
        }
        if (target_bytes > bytes_) {
            account_->charge_bytes(target_bytes - bytes_);
        } else {
            account_->release_bytes(bytes_ - target_bytes);
        }
        bytes_ = target_bytes;
    }

    void shrink_to(std::size_t target_bytes) noexcept {
        if (account_ != nullptr && target_bytes < bytes_) {
            account_->release_bytes(bytes_ - target_bytes);
            bytes_ = target_bytes;
        }
    }

    void clear() noexcept {
        if (account_ != nullptr && bytes_ != 0) {
            account_->release_bytes(bytes_);
            bytes_ = 0;
        }
    }

    void release() noexcept {
        clear();
        account_ = nullptr;
    }

    void swap(inbound_buffer_reservation& other) noexcept {
        std::swap(account_, other.account_);
        std::swap(bytes_, other.bytes_);
    }

private:
    inbound_buffer_account* account_{nullptr};
    std::size_t bytes_{0};
};

inline inbound_buffer_reservation inbound_buffer_account::reserve(std::size_t bytes_value) {
    return inbound_buffer_reservation(*this, bytes_value);
}

[[nodiscard]] inline std::size_t inbound_string_heap_bytes(const std::string& value) noexcept {
    static const std::size_t sso_capacity = std::string{}.capacity();
    return value.capacity() > sso_capacity ? value.capacity() : 0;
}

inline void inbound_reserve(std::string& target, inbound_buffer_reservation& reservation, std::size_t capacity) {
    if (capacity <= target.capacity()) {
        return;
    }
    static const std::size_t sso_capacity = std::string{}.capacity();
    if (capacity > sso_capacity) {
        reservation.resize_to(capacity);
    }
    try {
        target.reserve(capacity);
    } catch (...) {
        reservation.shrink_to(inbound_string_heap_bytes(target));
        throw;
    }
    reservation.resize_to(inbound_string_heap_bytes(target));
}

inline void inbound_resize(std::string& target, inbound_buffer_reservation& reservation, std::size_t new_size) {
    if (new_size > target.capacity()) {
        static const std::size_t sso_capacity = std::string{}.capacity();
        if (new_size > sso_capacity) {
            reservation.resize_to(new_size);
        }
        try {
            target.resize(new_size);
        } catch (...) {
            reservation.shrink_to(inbound_string_heap_bytes(target));
            throw;
        }
        reservation.resize_to(inbound_string_heap_bytes(target));
    } else {
        target.resize(new_size);
        reservation.shrink_to(inbound_string_heap_bytes(target));
    }
}

inline void inbound_append(std::string& target, inbound_buffer_reservation& reservation, std::string_view data) {
    if (data.empty()) {
        return;
    }
    if (data.size() > target.max_size() - target.size()) {
        throw std::length_error("inbound buffer length overflow");
    }
    const auto needed = target.size() + data.size();
    if (needed > target.capacity()) {
        const auto doubled = target.capacity() > target.max_size() / 2 ? target.max_size() : target.capacity() * 2;
        inbound_reserve(target, reservation, (std::max)(needed, doubled));
    }
    target.append(data.data(), data.size());
    reservation.resize_to(inbound_string_heap_bytes(target));
}

inline void inbound_clear_retaining_small(std::string& target, inbound_buffer_reservation& reservation, std::size_t retained_bytes = http_retained_string_bytes) noexcept {
    http_clear_string_retaining_small(target, retained_bytes);
    reservation.shrink_to(inbound_string_heap_bytes(target));
}

inline void inbound_discard(std::string& target, inbound_buffer_reservation& reservation) noexcept {
    std::string{}.swap(target);
    reservation.clear();
}

[[nodiscard]] inline std::span<char> inbound_prepare(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t bytes) {
    const auto spare = buffer.prepare_spare();
    if (spare.size() >= bytes) {
        return spare.first(bytes);
    }
    constexpr std::size_t max_bytes = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
    if (bytes > max_bytes - buffer.size()) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    const auto doubled = buffer.capacity() > max_bytes / 2 ? max_bytes : buffer.capacity() * 2;
    const auto next_capacity = (std::max)(buffer.size() + bytes, doubled);
    if (next_capacity > buffer.capacity()) {
        reservation.resize_to(next_capacity);
    }
    try {
        const auto prepared = buffer.prepare(bytes);
        reservation.resize_to(buffer.capacity());
        return prepared;
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
}

inline void inbound_reserve(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t capacity) {
    if (capacity <= buffer.capacity()) {
        return;
    }
    reservation.resize_to(capacity);
    try {
        buffer.reserve(capacity);
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
    reservation.resize_to(buffer.capacity());
}

inline void inbound_append(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    const auto spare = buffer.prepare_spare();
    if (spare.size() < bytes.size()) {
        constexpr std::size_t max_bytes = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
        if (bytes.size() > max_bytes - buffer.size()) {
            throw std::length_error("http_io_buffer size exceeds the addressable range");
        }
        const auto doubled = buffer.capacity() > max_bytes / 2 ? max_bytes : buffer.capacity() * 2;
        const auto next_capacity = (std::max)(buffer.size() + bytes.size(), doubled);
        if (next_capacity > buffer.capacity()) {
            reservation.resize_to(next_capacity);
        }
    }
    try {
        buffer.append(bytes);
        reservation.resize_to(buffer.capacity());
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
}

inline void inbound_shrink_to(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t bytes) {
    buffer.shrink_to(bytes);
    reservation.shrink_to(buffer.capacity());
}

}  // namespace ruvia::detail
