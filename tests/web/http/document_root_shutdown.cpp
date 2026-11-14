#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <semaphore>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"
namespace document_root_shutdown_test {

using namespace std::chrono_literals;

std::atomic<bool> held_request_started{false};
std::atomic<bool> held_request_cancelled{false};

class hold_root_request final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
        if (context_value.req().header("X-Hold-Root").has_value()) {
            held_request_started.store(true, std::memory_order_release);
            if (co_await ruvia::sleep_for(context_value.worker(), 1h, context_value.stop_token()) == ruvia::timer_sleep_result::stop_requested) {
                held_request_cancelled.store(true, std::memory_order_release);
                context_value.respond(context_value.text("cancelled"));
                co_return;
            }
        }
        co_await next_value();
    }
};

void replace_version(const std::filesystem::path& root, std::string_view bytes) {
    ruvia::test::wait_until([&] {
        const auto staged = root / ".version.next";
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output << bytes;
        output.close();
        if (output.fail()) {
            return false;
        }
        std::error_code error;
        std::filesystem::rename(staged, root / "version.txt", error);
        return !error;
    }, 5s);
}

}  // namespace document_root_shutdown_test

TEST(Ruvia, document_root_shutdown_cancels_old_snapshot_lease_and_restarts_refresh) {
    using namespace document_root_shutdown_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-document-root-shutdown");
    replace_version(directory.path(), "old leased snapshot");
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    static std::binary_semaphore started{0};
    auto& app = ruvia::app();
    ruvia::document_root_config config;
    config.root_ = directory.path();
    config.refresh_interval_ = 20ms;
    app.server({.worker_count_ = 3, .connection_scan_interval_ = 5ms}).listen({.address_ = "127.0.0.1", .http_ = endpoint.port()}).use<hold_root_request>().document_root(std::move(config)).on_start([] {
        started.release();
    });
    constexpr std::string_view get_version = "GET /version.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started.try_acquire_for(5s));
        asio::ip::tcp::socket held(io);
        held.connect(endpoint);
        constexpr std::string_view request = "GET /version.txt HTTP/1.1\r\nHost: localhost\r\nX-Hold-Root: yes\r\nConnection: close\r\n\r\n";
        asio::write(held, asio::buffer(request));
        ruvia::test::wait_until([] {
            return held_request_started.load(std::memory_order_acquire);
        }, 5s);
        replace_version(directory.path(), "published while the old request remains held");
        ruvia::test::wait_until([&] {
            const auto response = ruvia::test::exchange(io, endpoint, get_version);
            return response.starts_with("HTTP/1.1 200") && response.ends_with("published while the old request remains held");
        }, 5s);
        app.stop();
        runner.join();
        EXPECT_TRUE(held_request_cancelled.load(std::memory_order_acquire));
        std::error_code error;
        const auto response = ruvia::test::read_to_end(held, error);
        EXPECT_TRUE(response.empty());
        EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    }
    {
        ruvia::test::app_run restarted(app);
        ASSERT_TRUE(started.try_acquire_for(5s));
        replace_version(directory.path(), "refreshed after cancellation");
        ruvia::test::wait_until([&] {
            const auto response = ruvia::test::exchange(io, endpoint, get_version);
            return response.starts_with("HTTP/1.1 200") && response.ends_with("refreshed after cancellation");
        }, 5s);
        app.stop();
        restarted.join();
    }
}
