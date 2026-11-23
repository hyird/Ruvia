#pragma once

#include <cstddef>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

// Two-level real-time inbound memory accounting: per-connection account
// (`inbound_buffer_account`) charged simultaneously against a worker-wide
// budget (`inbound_buffer_budget`). Exhaustion throws `inbound_buffer_limit_error`
// (derived from `std::bad_alloc`), which Ruvia servers translate to HTTP 503.
namespace ruvia {

class inbound_buffer_limit_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "inbound buffer memory budget exhausted";
    }
};

class inbound_buffer_account;
class inbound_buffer_reservation;

// A worker's live inbound buffer bytes, shared by the `inbound_buffer_account`
// of each of its connections. Worker-affine, no synchronization. Must outlive
// every connection account bound to it.
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

    [[nodiscard]] std::size_t limit() const noexcept {
        return limit_;
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return used_;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return limit_ - used_;
    }

private:
    friend class inbound_buffer_account;
    const std::size_t limit_;
    std::size_t used_{0};
};

// One connection's inbound buffers. Each reservation is checked against both
// the connection's limit and its worker's budget. Every reservation must be
// released before this account is destroyed.
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

    [[nodiscard]] std::size_t limit() const noexcept {
        return limit_;
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return used_;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        const auto conn_avail = limit_ - used_;
        const auto worker_avail = worker_.available();
        return conn_avail < worker_avail ? conn_avail : worker_avail;
    }

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
    std::size_t used_{0};
};

// Move-only RAII reservation of live inbound buffer capacity against an
// `inbound_buffer_account` (no-op when unbound).
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

namespace detail {
using ::ruvia::inbound_buffer_account;
using ::ruvia::inbound_buffer_budget;
using ::ruvia::inbound_buffer_limit_error;
using ::ruvia::inbound_buffer_reservation;
}  // namespace detail

}  // namespace ruvia
