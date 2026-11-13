#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/integer.h"

namespace ruvia::test {

// Stateful, isolated RESP store. All map access and connection teardown run on
// its own event loop. Expiry is not simulated: these cases do not test time.
class redis_session_store_peer final {
public:
    explicit redis_session_store_peer(
        std::pmr::map<std::pmr::string, std::pmr::string> store = std::pmr::map<std::pmr::string, std::pmr::string>(ruvia::process_resource()))
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          work_(asio::make_work_guard(io_)),
          store_(std::move(store)) {
        asio::co_spawn(io_, accept(), [this](std::exception_ptr failure) { fail(failure); });
        thread_ = std::thread([this] { io_.run(); });
    }

    ~redis_session_store_peer() {
        asio::post(io_, [this] {
            closing_ = true;
            std::error_code ignored;
            acceptor_.close(ignored);
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
            work_.reset();
        });
        thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    using socket_type = asio::ip::tcp::socket;

    void fail(std::exception_ptr failure) {
        if (failure) {
            failure_ = failure;
            std::error_code ignored;
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
        }
    }

    [[nodiscard]] bool closed(const std::error_code& error) const noexcept {
        return error == asio::error::eof || error == asio::error::connection_reset ||
               error == asio::error::connection_aborted ||
               (closing_ && (error == asio::error::operation_aborted || error == asio::error::bad_descriptor));
    }

    asio::awaitable<void> accept() {
        try {
            for (;;) {
                auto socket = std::make_shared<socket_type>(io_);
                co_await acceptor_.async_accept(*socket, asio::use_awaitable);
                sockets_.push_back(socket);
                asio::co_spawn(io_, serve(socket), [this](std::exception_ptr failure) { fail(failure); });
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    static asio::awaitable<std::pmr::string> line(socket_type& socket, std::pmr::string& buffer) {
        const auto size = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffer), "\r\n", asio::use_awaitable);
        auto result = std::pmr::string(std::string_view(buffer).substr(0, size - 2), ruvia::process_resource());
        buffer.erase(0, size);
        co_return result;
    }

    static asio::awaitable<std::pmr::vector<std::pmr::string>> command(socket_type& socket, std::pmr::string& buffer) {
        const auto header = co_await line(socket, buffer);
        if (header.empty() || header.front() != '*') {
            throw std::runtime_error("expected RESP array");
        }
        const auto count = std::get<0>(ruvia::parse_integer<std::size_t>(std::string_view(header).substr(1)));
        std::pmr::vector<std::pmr::string> args{ruvia::process_resource()};
        for (std::size_t index = 0; index < count; ++index) {
            const auto bulk = co_await line(socket, buffer);
            if (bulk.empty() || bulk.front() != '$') {
                throw std::runtime_error("expected RESP bulk string");
            }
            const auto size = std::get<0>(ruvia::parse_integer<std::size_t>(std::string_view(bulk).substr(1)));
            if (buffer.size() < size + 2) {
                co_await asio::async_read(socket, asio::dynamic_buffer(buffer),
                    asio::transfer_exactly(size + 2 - buffer.size()), asio::use_awaitable);
            }
            args.emplace_back(buffer.data(), size);
            buffer.erase(0, size + 2);
        }
        co_return args;
    }

    std::pmr::string execute(const std::pmr::vector<std::pmr::string>& args) {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (args.size() == 2 && args[0] == "GET") {
            const auto found = store_.find(args[1]);
            return found == store_.end() ? std::pmr::string("$-1\r\n", ruvia::process_resource()) : std::move(std::pmr::string("$", ruvia::process_resource()).append(std::to_string(found->second.size()))) + "\r\n" + found->second + "\r\n";
        }
        if (args.size() >= 3 && args[0] == "SET") {
            // Session creation uses NX; an existing key must remain unchanged.
            return std::pmr::string(store_.emplace(args[1], args[2]).second ? "+OK\r\n" : "$-1\r\n", ruvia::process_resource());
        }
        if (args.size() == 2 && args[0] == "DEL") {
            return std::move(std::pmr::string(":", ruvia::process_resource()).append(std::to_string(store_.erase(args[1])))) + "\r\n";
        }
        if (args.size() == 7 && args[0] == "EVAL" && args[2] == "2") {
            // Atomic compare-and-replace of an existing identity, not a canned
            // script response: missing sources and occupied targets cannot rotate.
            if (!store_.contains(args[3]) || !store_.emplace(args[4], args[5]).second) {
                return std::pmr::string(":0\r\n", ruvia::process_resource());
            }
            store_.erase(args[3]);
            return std::pmr::string(":1\r\n", ruvia::process_resource());
        }
        throw std::runtime_error("unsupported session store command");
    }

    asio::awaitable<void> serve(std::shared_ptr<socket_type> socket) {
        std::pmr::string buffer{ruvia::process_resource()};
        try {
            for (;;) {
                const auto args = co_await command(*socket, buffer);
                const auto reply = execute(args);
                co_await asio::async_write(*socket, asio::buffer(reply), asio::use_awaitable);
            }
        } catch (const std::system_error& error) {
            if (!closed(error.code())) {
                throw;
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::pmr::map<std::pmr::string, std::pmr::string> store_;
    std::pmr::vector<std::shared_ptr<socket_type>> sockets_{ruvia::process_resource()};
    std::exception_ptr failure_;
    bool closing_{false};
    std::thread thread_;
};

}  // namespace ruvia::test
