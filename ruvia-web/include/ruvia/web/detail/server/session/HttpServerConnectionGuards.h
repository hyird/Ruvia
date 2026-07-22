#pragma once

#include "ruvia/core/detail/io/SocketUtils.h"
#include "ruvia/web/detail/server/session/HttpConnectionState.h"

#include <asio/ip/tcp.hpp>

#include <cstddef>
#include <exception>
#include <utility>

namespace ruvia::detail {

// Owns one admitted socket and its per-worker connection-count lease as one
// linear value. The lease is acquired only after socket configuration succeeds,
// then moves into the lazy session Task before co_spawn initiation. If coroutine
// allocation or co_spawn throws, destroying that cold ownership chain closes the
// socket first and releases the count second.
class AcceptedConnectionLease final {
public:
    AcceptedConnectionLease(
        asio::ip::tcp::socket socket,
        std::size_t& count) noexcept
        : socket_(std::move(socket)),
          count_(&count) {
        ++*count_;
    }

    AcceptedConnectionLease(const AcceptedConnectionLease&) = delete;
    AcceptedConnectionLease& operator=(const AcceptedConnectionLease&) = delete;

    AcceptedConnectionLease(AcceptedConnectionLease&& other) noexcept
        : socket_(std::move(other.socket_)),
          count_(std::exchange(other.count_, nullptr)) {}

    AcceptedConnectionLease& operator=(AcceptedConnectionLease&&) = delete;

    ~AcceptedConnectionLease() {
        if (count_ == nullptr) {
            return;
        }
        closeSocket(socket_);
        if (*count_ == 0) {
            std::terminate();
        }
        --*count_;
    }

    [[nodiscard]] asio::ip::tcp::socket& socket() & noexcept {
        return socket_;
    }
    asio::ip::tcp::socket& socket() && = delete;

private:
    asio::ip::tcp::socket socket_;
    std::size_t* count_;
};

// Returns a connection's borrowed work set to the per-worker pool on scope exit.
// Tracks the pointer variable by reference so explicit idle-gap releases are not
// double-released.
class WorkSetReturn final {
public:
    WorkSetReturn(ConnectionWorkSetPool& pool, ConnectionWorkSet*& workSet) noexcept
        : pool_(&pool), workSet_(&workSet) {}

    WorkSetReturn(const WorkSetReturn&) = delete;
    WorkSetReturn& operator=(const WorkSetReturn&) = delete;

    ~WorkSetReturn() {
        if (*workSet_ != nullptr) {
            pool_->release(*workSet_);
        }
    }

private:
    ConnectionWorkSetPool* pool_;
    ConnectionWorkSet** workSet_;
};

}  // namespace ruvia::detail
