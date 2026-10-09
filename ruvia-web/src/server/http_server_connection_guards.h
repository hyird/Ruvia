#pragma once

#include <cstddef>
#include <exception>
#include <utility>

#include <asio/ip/tcp.hpp>

#include "ruvia/core/socket.h"

#include "server/http_connection_state.h"

namespace ruvia::detail {

// Owns one admitted socket and its per-worker connection-count lease as one
// linear value. The lease is acquired only after socket configuration succeeds,
// then moves into the lazy session task before co_spawn initiation. If coroutine
// allocation or co_spawn throws, destroying that cold ownership chain closes the
// socket first and releases the count second.
class accepted_connection_lease final {
public:
    accepted_connection_lease(asio::ip::tcp::socket socket, std::atomic<std::size_t>& count) noexcept
        : socket_(std::move(socket)),
          count_(&count) {
        count_->fetch_add(1, std::memory_order_relaxed);
    }

    accepted_connection_lease(const accepted_connection_lease&) = delete;
    accepted_connection_lease& operator=(const accepted_connection_lease&) = delete;

    accepted_connection_lease(accepted_connection_lease&& other) noexcept
        : socket_(std::move(other.socket_)),
          count_(std::exchange(other.count_, nullptr)) {}

    accepted_connection_lease& operator=(accepted_connection_lease&&) = delete;

    ~accepted_connection_lease() {
        if (count_ == nullptr) {
            return;
        }
        ruvia::close_socket(socket_);
        if (count_->load(std::memory_order_relaxed) == 0) {
            std::terminate();
        }
        count_->fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] asio::ip::tcp::socket& socket() & noexcept {
        return socket_;
    }
    asio::ip::tcp::socket& socket() && = delete;

private:
    asio::ip::tcp::socket socket_;
    std::atomic<std::size_t>* count_;
};

// Returns a connection's borrowed work set to the per-worker pool on scope exit.
// Tracks the pointer variable by reference so explicit idle-gap releases are not
// double-released.
class work_set_return final {
public:
    work_set_return(connection_work_set_pool& pool, connection_work_set*& work_set) noexcept
        : pool_(&pool),
          work_set_(&work_set) {}

    work_set_return(const work_set_return&) = delete;
    work_set_return& operator=(const work_set_return&) = delete;

    ~work_set_return() {
        if (*work_set_ != nullptr) {
            pool_->release(*work_set_);
        }
    }

private:
    connection_work_set_pool* pool_;
    connection_work_set** work_set_;
};

}  // namespace ruvia::detail
