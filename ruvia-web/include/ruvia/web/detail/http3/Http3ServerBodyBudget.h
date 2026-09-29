#pragma once

#include <cstddef>
#include <exception>
#include <stdexcept>

namespace ruvia::detail {

// Worker-affine accounting shared by buffered HTTP/3 request bodies and tunnel
// input across connections. It counts queued body bytes, not vector capacity,
// pool cache, TLS, or other QUIC memory. The owner is borrowed without synchronization and must
// outlive every session and request lease that can return a reservation.
class Http3ServerBodyBudget final {
public:
    explicit Http3ServerBodyBudget(std::size_t limit)
        : limit_(limit) {
        if (limit_ == 0) {
            throw std::invalid_argument("HTTP/3 server body budget must be greater than zero");
        }
    }

    ~Http3ServerBodyBudget() {
        if (used_ != 0) {
            std::terminate();
        }
    }

    Http3ServerBodyBudget(const Http3ServerBodyBudget&) = delete;
    Http3ServerBodyBudget& operator=(const Http3ServerBodyBudget&) = delete;
    Http3ServerBodyBudget(Http3ServerBodyBudget&&) = delete;
    Http3ServerBodyBudget& operator=(Http3ServerBodyBudget&&) = delete;

    [[nodiscard]] bool tryReserve(std::size_t bytes) noexcept {
        if (bytes > limit_ - used_) {
            return false;
        }
        used_ += bytes;
        return true;
    }

    void release(std::size_t bytes) noexcept {
        if (bytes > used_) {
            std::terminate();
        }
        used_ -= bytes;
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
