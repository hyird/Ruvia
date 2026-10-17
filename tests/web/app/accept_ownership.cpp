#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/timer.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/streaming.h"

#include "test_harness.h"
#include "test_tls_identity.h"

namespace accept_ownership_test {
using namespace std::chrono_literals;

struct observations final {
    std::binary_semaphore started_{0};
    std::atomic<unsigned> live_{};
    std::atomic<unsigned> destroyed_{};
    std::atomic<unsigned> wrong_owner_{};
};

struct worker_state final {
    explicit worker_state(std::shared_ptr<observations> observed)
        : observed_(std::move(observed)),
          owner_(std::this_thread::get_id()) {
        ++observed_->live_;
    }
    worker_state(worker_state&&) noexcept = default;
    worker_state(const worker_state&) = delete;
    ~worker_state() {
        if (observed_) {
            if (owner_ != std::this_thread::get_id()) {
                ++observed_->wrong_owner_;
            }
            --observed_->live_;
            ++observed_->destroyed_;
        }
    }
    void check(const ruvia::context& context) const {
        if (owner_ != std::this_thread::get_id() || !context.worker().is_current()) {
            ++observed_->wrong_owner_;
            throw std::logic_error("connection resumed on another worker");
        }
    }
    std::shared_ptr<observations> observed_;
    std::thread::id owner_;
};

class routes final {
public:
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_POST_STREAM("/accept-owner", echo);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> echo(ruvia::context& context) {
        auto& owner = context.worker_state<worker_state>();
        owner.check(context);
        const auto worker_id = context.worker().id();
        std::pmr::string body(context.arena());
        auto& reader = context.req().get_body_reader();
        while (const auto chunk = co_await reader.text()) {
            owner.check(context);
            body.append(*chunk);
        }
        owner.check(context);
        (void)co_await ruvia::sleep_for(context.worker(), 1ms, context.get_stop_token());
        owner.check(context);
        context.header("X-Connection-Worker", std::to_string(worker_id));
        co_return context.text(std::move(body));
    }
};

class app_run final {
public:
    explicit app_run(ruvia::application& app)
        : app_(app),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  failure_ = std::current_exception();
              }
          }) {}
    ~app_run() {
        if (thread_.joinable()) {
            app_.stop();
            thread_.join();
        }
    }
    void join() {
        thread_.join();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    ruvia::application& app_;
    std::exception_ptr failure_;
    std::thread thread_;
};

std::string_view header(std::string_view head, std::string_view name) {
    while (!head.empty()) {
        const auto end = head.find("\r\n");
        const auto line = head.substr(0, end);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos &&
            ruvia::http_ascii_equals_ignore_case(line.substr(0, colon), name)) {
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            return value;
        }
        if (end == std::string_view::npos) {
            break;
        }
        head.remove_prefix(end + 2);
    }
    return {};
}

template <typename stream_type>
std::string exchange(stream_type& stream, std::string_view expected) {
    const auto request = std::string("POST /accept-owner HTTP/1.1\r\nHost: localhost\r\nContent-Length: ") +
                         std::to_string(expected.size()) + "\r\n\r\n";
    asio::write(stream, asio::buffer(request));
    asio::write(stream, asio::buffer(expected.substr(0, expected.size() / 2)));
    asio::write(stream, asio::buffer(expected.substr(expected.size() / 2)));
    std::string response;
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        asio::error_code error;
        const auto count = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), count);
        if (error && error != asio::error::would_block && error != asio::error::try_again) {
            throw std::system_error(error);
        }
        const auto head_end = response.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            const auto head = std::string_view(response).substr(0, head_end);
            const auto length_text = header(head, "Content-Length");
            if (length_text.empty()) {
                throw std::runtime_error("missing response content length");
            }
            std::size_t length{};
            const auto parsed = std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
            if (parsed.ec != std::errc{} || parsed.ptr != length_text.data() + length_text.size() || length != expected.size()) {
                throw std::runtime_error("missing response content length");
            }
            if (response.size() >= head_end + 4 + length) {
                if (!response.starts_with("HTTP/1.1 200 ") ||
                    std::string_view(response).substr(head_end + 4) != expected) {
                    throw std::runtime_error("incorrect affinity echo response");
                }
                const auto worker = header(head, "X-Connection-Worker");
                if (worker.empty()) {
                    throw std::runtime_error("missing connection owner identity");
                }
                return std::string(worker);
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("connection ownership response timed out");
        }
        if (count == 0) {
            std::this_thread::sleep_for(1ms);
        }
    }
}

template <typename stream_type>
void check_connection(ruvia::testing::test_context& ruvia_ctx, stream_type& stream) {
    const auto first = exchange(stream, "first split body");
    for (unsigned request = 0; request != 3; ++request) {
        RUVIA_CHECK_EQ(exchange(stream, "next split body"), first);
    }
}

}  // namespace accept_ownership_test

RUVIA_TEST(app_multi_listener_tcp_tls_preserves_connection_worker_and_joins_pending_accepts) {
    using namespace accept_ownership_test;
    auto observed = std::make_shared<observations>();
    auto& app = ruvia::app();
    app.use_worker_state<worker_state>([observed] { return worker_state(observed); });
    app.on_start([observed] { observed->started_.release(); });
    ruvia::test::tls_identity identity("localhost");
    const auto key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write ownership test TLS key");
        }
    }
    asio::io_context io;
    asio::ip::tcp::acceptor http_reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::acceptor https_reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto http = http_reservation.local_endpoint();
    const auto https = https_reservation.local_endpoint();
    http_reservation.close();
    https_reservation.close();
    const auto configure = [&] {
        app.server({.worker_count_ = 3, .drain_timeout_ = 0ms, .connection_scan_interval_ = 5ms});
        app.listen({.address_ = "127.0.0.1", .http_ = http.port(), .https_ = https.port(), .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = key}});
    };
    // A pending stop must retire even a graph whose worker threads never ran.
    configure();
    app.stop();
    app.run();
    RUVIA_CHECK(app.workers().empty());
    RUVIA_CHECK_EQ(observed->live_.load(), 0U);
    const auto cold_destroyed = observed->destroyed_.load();
    for (unsigned cycle = 0; cycle != 3; ++cycle) {
        configure();
        app_run run(app);
        if (!observed->started_.try_acquire_for(5s)) {
            throw std::runtime_error("ownership test app did not start");
        }
        RUVIA_CHECK_EQ(observed->live_.load(), 3U);
        std::vector<asio::ip::tcp::socket> idle;
        if (cycle != 0) {
            for (unsigned connection = 0; connection != 12; ++connection) {
                idle.emplace_back(io);
                idle.back().connect(connection % 2 == 0 ? http : https);
            }
            for (unsigned connection = 0; connection != 12; ++connection) {
                asio::ip::tcp::socket aborted(io);
                aborted.connect(connection % 2 == 0 ? http : https);
                aborted.set_option(asio::socket_base::linger(true, 0));
                aborted.close();
            }
            for (unsigned connection = 0; connection != 6; ++connection) {
                asio::ip::tcp::socket plain(io);
                plain.connect(http);
                plain.non_blocking(true);
                check_connection(ruvia_ctx, plain);
                asio::ssl::context tls(asio::ssl::context::tls_client);
                tls.load_verify_file(identity.ca_file_.string());
                tls.set_verify_mode(asio::ssl::verify_peer);
                tls.set_verify_callback(asio::ssl::host_name_verification("localhost"));
                asio::ssl::stream<asio::ip::tcp::socket> secure(io, tls);
                secure.next_layer().connect(https);
                if (SSL_set_tlsext_host_name(secure.native_handle(), "localhost") != 1) {
                    throw std::runtime_error("cannot set ownership test TLS server name");
                }
                secure.handshake(asio::ssl::stream_base::client);
                secure.next_layer().non_blocking(true);
                check_connection(ruvia_ctx, secure);
            }
        }
        app.stop();
        run.join();
        RUVIA_CHECK_EQ(observed->live_.load(), 0U);
        RUVIA_CHECK_EQ(observed->destroyed_.load(), cold_destroyed + (cycle + 1) * 3U);
        RUVIA_CHECK_EQ(observed->wrong_owner_.load(), 0U);
        RUVIA_CHECK(app.workers().empty());
        for (const auto& endpoint : {http, https}) {
            asio::ip::tcp::socket closed(io);
            asio::error_code error;
            closed.connect(endpoint, error);
            RUVIA_CHECK(error == asio::error::connection_refused);
        }
    }
}
