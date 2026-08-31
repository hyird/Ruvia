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
#include <asio/post.hpp>
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
        : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          socket_(io_),
          response_("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + std::string(body)),
          thread_([this] { serve(); }) {}

    ~OneShotOrigin() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
        });
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    void serve() noexcept {
        acceptor_.async_accept(socket_, [this](std::error_code error) {
            if (error) {
                return;
            }
            asio::async_read_until(
                socket_, request_, "\r\n\r\n", [this](std::error_code readError, std::size_t) {
                    if (readError) {
                        return;
                    }
                    asio::async_write(
                        socket_, asio::buffer(response_), [](std::error_code, std::size_t) {});
                });
        });
        io_.run();
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::streambuf request_;
    std::string response_;
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

ruvia::Task<int> send(
    ruvia::HttpClient& client, ruvia::WorkerId worker, std::string_view expected) {
    auto response = co_await client.send({.target = "/worker"});
    {
        auto discarded = response.body().read();
        static_cast<void>(discarded);
    }
    auto operation = response.body().readAll();
    bool competingReadRejected = false;
    try {
        auto competing = response.body().read();
        static_cast<void>(competing);
    } catch (const std::logic_error&) {
        competingReadRejected = true;
    }
    auto moved = std::move(response);
    const auto body = co_await std::move(operation);
    const bool valid = competingReadRejected && client.worker().isCurrent() &&
                       client.worker().id() == worker && client.host() == "127.0.0.1" &&
                       moved.status() == ruvia::http_status::kOk && body == expected;
    co_return valid ? 0 : 1;
}

ruvia::Task<void> shutdownClient(ruvia::HttpClient& client) {
    auto cold = client.send({.target = "/never"});
    static_cast<void>(cold);
    co_await client.shutdown();
}

void start(const ruvia::EventLoop& loop, ruvia::HttpClient& client, std::string_view expected,
    std::promise<int>& completion) {
    try {
        ruvia::detail::asyncStartTask(send(client, loop.id(), expected),
            asio::bind_executor(
                loop.executor(), [&completion](ruvia::detail::TaskCompletionResult<int> result) {
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
    auto shutdownTask = loop.start(shutdownClient(client));
    shutdownTask.get();
    loops.stop();
    loops.join();
    return result;
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
    auto shutdownTask = loop.start(shutdownClient(client));
    shutdownTask.get();
    attachment.stop();
    thread.join();
    return result;
}

bool closeBeforeDispatch() {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    ruvia::HttpClient client(loop, configFor(80));

    // Construction has not driven the event loop yet. close() must safely
    // enqueue worker teardown, and shutdown() must later observe that close
    // task rather than leave an unjoined client state behind.
    client.close();
    loops.start();
    try {
        auto shutdownTask = loop.start(client.shutdown());
        shutdownTask.get();
    } catch (...) {
        loops.stop();
        loops.join();
        return false;
    }
    loops.stop();
    loops.join();
    return true;
}

}  // namespace

int main() {
    try {
        try {
            ruvia::HttpClient invalid(ruvia::EventLoop{}, configFor(80));
            return 1;
        } catch (const std::invalid_argument&) {
        }
        if (const auto result = pooledWorker(); result != 0) {
            return 10 + result;
        }
        if (const auto result = attachedWorker(); result != 0) {
            return 20 + result;
        }
        if (!closeBeforeDispatch()) {
            return 30;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "standalone HTTP client test failed: %s\n", error.what());
        return 100;
    }
}
