#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/http_client.h"

#include "test_harness.h"
#include "test_io_context.h"

namespace happy_eyeballs_tests {

using namespace std::chrono_literals;
using tcp = asio::ip::tcp;

[[nodiscard]] std::optional<bool> localhost_first_is_v6() {
    asio::io_context io;
    tcp::resolver resolver(io);
    std::error_code error;
    const auto addresses = resolver.resolve("localhost", "80", error);
    if (error || addresses.empty()) {
        return std::nullopt;
    }
    bool has_v4 = false;
    bool has_v6 = false;
    for (const auto& entry : addresses) {
        has_v4 = has_v4 || entry.endpoint().address().is_v4();
        has_v6 = has_v6 || entry.endpoint().address().is_v6();
    }
    if (!has_v4 || !has_v6) {
        return std::nullopt;
    }
    return addresses.begin()->endpoint().address().is_v6();
}

[[nodiscard]] tcp::endpoint loopback_endpoint(bool v6, std::uint16_t port) {
    return v6 ? tcp::endpoint(asio::ip::address_v6::loopback(), port)
              : tcp::endpoint(asio::ip::address_v4::loopback(), port);
}

void open_acceptor(tcp::acceptor& acceptor, bool v6) {
    acceptor.open(v6 ? tcp::v6() : tcp::v4());
    if (v6) {
        acceptor.set_option(asio::ip::v6_only(true));
    }
}

// The first localhost family is bound but does not listen. The other family
// answers HTTP, so success requires the client to launch the later family
// while the first attempt remains unresolved or refused.
class opposite_family_peer final {
public:
    opposite_family_peer(asio::io_context& io, const ruvia::worker_handle& worker, bool first_is_v6)
        : io_(io),
          listening_(io),
          unavailable_(io),
          socket_(io),
          done_(worker) {
        const bool listening_is_v6 = !first_is_v6;
        open_acceptor(listening_, listening_is_v6);
        listening_.bind(loopback_endpoint(listening_is_v6, 0));
        listening_.listen();

        open_acceptor(unavailable_, first_is_v6);
        unavailable_.bind(loopback_endpoint(first_is_v6, port()));
    }

    [[nodiscard]] std::uint16_t port() const {
        return listening_.local_endpoint().port();
    }

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = std::move(failure);
            }
            complete_ = true;
            done_.notify();
        });
    }

    void stop() noexcept {
        stopping_ = true;
        std::error_code ignored;
        listening_.close(ignored);
        unavailable_.close(ignored);
        socket_.close(ignored);
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] asio::awaitable<void> serve() {
        co_await listening_.async_accept(socket_, asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket_, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        co_await asio::async_write(socket_, asio::buffer(response), asio::use_awaitable);
    }

    asio::io_context& io_;
    tcp::acceptor listening_;
    tcp::acceptor unavailable_;
    tcp::socket socket_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool stopping_{false};
    bool complete_{false};
};

[[nodiscard]] ruvia::task<void> exercise(ruvia::event_loop_attachment& attachment,
    opposite_family_peer& peer, ruvia::testing::test_context& ruvia_ctx, std::string_view host,
    std::chrono::milliseconds connect_timeout) {
    ruvia::http_client client(attachment.loop(),
        {.scheme_ = ruvia::http_scheme::http,
            .host_ = host,
            .port_ = peer.port(),
            .connect_timeout_ = connect_timeout,
            .request_timeout_ = 3s,
            .protocol_ = ruvia::http_client_protocol::http1_only});
    std::exception_ptr failure;
    try {
        auto response = co_await client.handle().send({.target_ = "/happy-eyeballs"});
        const auto body = co_await response.body().read_all();
        const std::string_view body_text(reinterpret_cast<const char*>(body.bytes().data()), body.size());
        RUVIA_CHECK(body_text == "ok");
    } catch (...) {
        failure = std::current_exception();
        peer.stop();
    }
    try {
        co_await client.shutdown();
        co_await peer.wait();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace happy_eyeballs_tests

#if defined(_WIN32)
RUVIA_TEST(http_client_tries_next_address_family_when_first_is_unavailable) {
#else
RUVIA_TEST(http_client_fast_address_failure_immediately_starts_the_next_family) {
#endif
    using namespace happy_eyeballs_tests;
    const auto first_is_v6 = localhost_first_is_v6();
    if (!first_is_v6) {
        // A single-family host has no cross-family race to exercise.
        return;
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto loop = attachment.loop();
    auto worker = loop.handle();
    opposite_family_peer peer(io, worker, *first_is_v6);
    peer.start();
    // Windows can leave a connect to a bound, non-listening socket pending
    // instead of refusing it immediately. Allow the 250 ms stagger there;
    // the shorter deadline elsewhere still checks immediate failure handling.
#if defined(_WIN32)
    constexpr auto connect_timeout = 2s;
#else
    constexpr auto connect_timeout = 150ms;
#endif
    auto root = loop.start(exercise(attachment, peer, ruvia_ctx, "localhost", connect_timeout));
    attachment.run();
    root.get();
}

RUVIA_TEST(http_client_connects_to_an_ipv6_literal) {
    using namespace happy_eyeballs_tests;
    if (!localhost_first_is_v6()) {
        return;
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto loop = attachment.loop();
    auto worker = loop.handle();
    opposite_family_peer peer(io, worker, false);
    peer.start();
    auto root = loop.start(exercise(attachment, peer, ruvia_ctx, "::1", 3s));
    attachment.run();
    root.get();
}
