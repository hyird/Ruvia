#pragma once

#include <cstddef>
#include <exception>
#include <stdexcept>

namespace ruvia::detail {

// Worker-affine accounting shared by buffered HTTP/3 request bodies and tunnel
// input across connections. It counts queued body bytes, not vector capacity,
// pool cache, TLS, or other QUIC memory. The owner is borrowed without synchronization and must
// outlive every session and request lease that can return a reservation.
class http3_server_body_budget final {
public:
    explicit http3_server_body_budget(std::size_t limit)
        : limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("HTTP/3 server body budget must be greater than zero");
        }
    }

    ~http3_server_body_budget() {
        if (used_ != 0) {
            std::terminate();
        }
    }

    http3_server_body_budget(const http3_server_body_budget&) = delete;
    http3_server_body_budget& operator=(const http3_server_body_budget&) = delete;
    http3_server_body_budget(http3_server_body_budget&&) = delete;
    http3_server_body_budget& operator=(http3_server_body_budget&&) = delete;

    [[nodiscard]] bool try_reserve(std::size_t bytes_value) noexcept {
        if (bytes_value > limit_ - used_) {
            return false;
        }
        used_ += bytes_value;
        return true;
    }

    void release(std::size_t bytes_value) noexcept {
        if (bytes_value > used_) {
            std::terminate();
        }
        used_ -= bytes_value;
    }

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
    const std::size_t limit_;
    std::size_t used_{0};
};

}  // namespace ruvia::detail
