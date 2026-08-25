#include <cstdio>
#include <exception>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/EventLoopPool.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/HttpClient.h"

namespace {

class OneShotOrigin final {
public:
    explicit OneShotOrigin(std::string_view body)
        : body_(body),
          acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          thread_([this] { serve(); }) {}

    ~OneShotOrigin() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    void serve() noexcept {
        try {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket);
            asio::streambuf request;
            std::error_code error;
            (void)asio::read_until(socket, request, "\r\n\r\n", error);
            if (error) return;
            auto response = std::string("HTTP/1.1 200 OK\r\nContent-Length: ");
            response += std::to_string(body_.size());
            response += "\r\nConnection: close\r\n\r\n";
            response += body_;
            (void)asio::write(socket, asio::buffer(response), error);
        } catch (...) {
        }
    }

    std::string body_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

[[nodiscard]] ruvia::HttpClientConfig configFor(std::uint16_t port) {
    auto config = ruvia::HttpClientConfig{
        .scheme = ruvia::HttpScheme::kHttp,
        .host = "127.0.0.1",
    };
    config.port = port;
    config.protocol = ruvia::HttpClientProtocol::kHttp1Only;
    return config;
}

ruvia::Task<int> send(ruvia::HttpClient& client, ruvia::WorkerId worker, std::string_view expected) {
    auto response = co_await client.send({.target = "/worker"});
    const auto body = co_await response.body().readAll();
    const bool valid =
        client.worker().isCurrent() &&
        client.worker().id() == worker &&
        client.host() == "127.0.0.1" &&
        response.status() == ruvia::http_status::kOk &&
        body == expected;
    co_return valid ? 0 : 1;
}

void start(const ruvia::EventLoop& loop, ruvia::HttpClient& client, std::string_view expected, std::promise<int>& completion) {
    try {
        ruvia::detail::asyncStartTask(send(client, loop.id(), expected), asio::bind_executor(loop.executor(), [&completion](ruvia::detail::TaskCompletionResult<int> result) {
            if (auto* success = result.success()) {
                completion.set_value(std::move(*success).takeValue());
            } else {
                completion.set_exception(result.failure()->exception());
            }
        }));
    } catch (...) {
        completion.set_exception(std::current_exception());
    }
}

int pooledWorker() {
    OneShotOrigin origin("pooled");
    ruvia::EventLoopPool loops({.loopCount = 1});
    auto loop = loops.loop(0);
    ruvia::HttpClient client(loop, configFor(origin.port()));
    loops.start();

    std::promise<int> completion;
    auto future = completion.get_future();
    const auto posted = loop.post([&] { start(loop, client, "pooled", completion); });
    if (posted != ruvia::PostStatus::kAccepted) {
        loops.stop();
        loops.join();
        return 2;
    }
    const auto result = future.get();
    const auto closing = loop.post([&] { client.close(); });
    loops.stop();
    loops.join();
    return closing == ruvia::PostStatus::kAccepted ? result : 3;
}

int attachedWorker() {
    OneShotOrigin origin("attached");
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    auto loop = attachment.loop();
    ruvia::HttpClient client(loop, configFor(origin.port()));
    std::thread thread([&] { io.run(); });

    std::promise<int> completion;
    auto future = completion.get_future();
    const auto posted = loop.post([&] { start(loop, client, "attached", completion); });
    if (posted != ruvia::PostStatus::kAccepted) {
        attachment.stop();
        thread.join();
        return 2;
    }
    const auto result = future.get();
    const auto closing = loop.post([&] { client.close(); });
    attachment.stop();
    thread.join();
    return closing == ruvia::PostStatus::kAccepted ? result : 3;
}

}  // namespace

int main() {
    try {
        try {
            ruvia::HttpClient invalid(ruvia::EventLoop{}, configFor(80));
            return 1;
        } catch (const std::invalid_argument&) {
        }
        if (const auto result = pooledWorker(); result != 0) return 10 + result;
        if (const auto result = attachedWorker(); result != 0) return 20 + result;
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "standalone HTTP client test failed: %s\n", error.what());
        return 100;
    }
}
