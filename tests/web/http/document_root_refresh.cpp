#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/server_config.h"

#include "http_server_fixture.h"
#include "test_io_context.h"

// A multi-worker application serves a document root that changes while it
// runs. This executable owns that application, so the root cannot leak into
// other suites.

namespace document_root_refresh_test {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

constexpr std::size_t worker_count = 3;
constexpr std::chrono::milliseconds refresh_interval{20};
constexpr std::chrono::seconds observation_timeout{10};

std::atomic<std::size_t> next_worker_id{0};
std::atomic<bool> held_request_started{false};
std::atomic<bool> release_held_request{true};

struct root_worker_state final {
    std::string label_{std::to_string(next_worker_id.fetch_add(1, std::memory_order_relaxed))};
};

class root_request_observation final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        if (context_value.req().header("X-Hold-Root").has_value()) {
            held_request_started.store(true, std::memory_order_release);
            while (!release_held_request.load(std::memory_order_acquire)) {
                if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(1),
                        context_value.stop_token()) == ruvia::timer_sleep_result::stop_requested) {
                    co_return;
                }
            }
        }
        co_await next_value();
        context_value.edit_response().header(
            "X-Document-Root-Worker", context_value.worker_state<root_worker_state>().label_);
    }
};

[[nodiscard]] fs::path document_root_dir() {
    return fs::temp_directory_path() / "ruvia_document_root_refresh_test";
}

[[nodiscard]] fs::path retired_root_dir() {
    return fs::temp_directory_path() / "ruvia_document_root_refresh_test.retired";
}

// The application is configured through this symlink where the platform
// creates one unprivileged, as an atomic-deploy layout would.
[[nodiscard]] fs::path configured_root() {
#ifdef _WIN32
    return document_root_dir();
#else
    return fs::temp_directory_path() / "ruvia_document_root_refresh_link";
#endif
}

[[nodiscard]] fs::path version_file() {
    return document_root_dir() / "version.txt";
}

[[nodiscard]] ruvia::server_config multi_worker_config() {
    return {.worker_count_ = worker_count, .connection_scan_interval_ = std::chrono::milliseconds(5)};
}

template <typename fn_type>
[[nodiscard]] bool retry_until_deadline(fn_type&& attempt) {
    const auto deadline = steady_clock::now() + observation_timeout;
    while (!attempt()) {
        if (steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Publish each filesystem mutation atomically. A truncate or remove_all could
// otherwise expose a valid intermediate tree that a refresh may publish.
// Windows can briefly refuse a rename while a refresh pass inspects the tree.
[[nodiscard]] bool write_version(std::string_view body) {
    return retry_until_deadline([body] {
        std::error_code ec;
        fs::create_directories(document_root_dir(), ec);
        if (ec) {
            return false;
        }
        const auto staged = document_root_dir() / ".version.txt.next";
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output << body;
        output.close();
        if (output.fail()) {
            return false;
        }
        fs::rename(staged, version_file(), ec);
        return !ec;
    });
}

[[nodiscard]] bool remove_document_root() {
    return retry_until_deadline([] {
        std::error_code ec;
        fs::rename(document_root_dir(), retired_root_dir(), ec);
        return !ec;
    });
}

// The application is shared by every case in this executable. It is configured
// once, with the root populated: startup builds the first index synchronously.
[[nodiscard]] steady_clock::time_point configure_application() {
    static const auto configured_at = [] {
        std::error_code ignored;
        fs::remove_all(document_root_dir(), ignored);
        fs::remove_all(retired_root_dir(), ignored);
        fs::create_directories(document_root_dir());
        std::ofstream(version_file(), std::ios::binary) << "initial";
#ifndef _WIN32
        fs::remove(configured_root(), ignored);
        fs::create_directory_symlink(document_root_dir(), configured_root());
#endif

        ruvia::document_root_config config;
        config.root_ = configured_root();
        config.static_options_ =
            {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}};
        config.refresh_interval_ = refresh_interval;
        ruvia::app().use_worker_state<root_worker_state>().use<root_request_observation>();
        ruvia::app().document_root(std::move(config));
        return steady_clock::now();
    }();
    return configured_at;
}

// Sends one request on a fresh connection and returns the complete response.
[[nodiscard]] std::string exchange(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    asio::write(socket, asio::buffer(request));
    std::string output;
    std::error_code error;
    asio::read(socket, asio::dynamic_buffer(output), error);
    return output;
}

// Fresh connections let the OS distribute requests. Observe a successful new
// version from every identified worker, not merely whichever one accepts first.
[[nodiscard]] bool eventually_serves(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view body) {
    std::set<std::string> observed_workers;
    return retry_until_deadline([&] {
        const auto response = exchange(io, endpoint, "/version.txt");
        if (!response.starts_with("HTTP/1.1 200") || !response.ends_with(body)) {
            return false;
        }
        constexpr std::string_view worker_header = "\r\nX-Document-Root-Worker: ";
        const auto header = response.find(worker_header);
        if (header == std::string::npos) {
            return false;
        }
        const auto label = header + worker_header.size();
        const auto end = response.find("\r\n", label);
        if (end == std::string::npos) {
            return false;
        }
        observed_workers.emplace(response.substr(label, end - label));
        return observed_workers.size() == worker_count;
    });
}

[[nodiscard]] std::size_t refresh_failures() {
    return ruvia::app().http_stats().document_root_refresh_failures_;
}

}  // namespace document_root_refresh_test

using namespace document_root_refresh_test;

TEST(Ruvia, document_root_refresh_updates_multi_worker_application) {
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());
    EXPECT_EQ(ruvia::app().workers().size(), worker_count);

    // Every version has a distinct size, so a change is indexed even where the
    // filesystem's modification time is coarse.
    EXPECT_TRUE(write_version("first version"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "first version"));

    EXPECT_TRUE(write_version("second, longer version"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "second, longer version"));
    server.finish();
}

TEST(Ruvia, document_root_refresh_keeps_old_requests_across_multiple_publications) {
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    EXPECT_TRUE(write_version("old leased snapshot"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "old leased snapshot"));
    held_request_started.store(false, std::memory_order_relaxed);
    release_held_request.store(false, std::memory_order_release);

    asio::ip::tcp::socket held(io);
    held.connect(server.endpoint());
    constexpr std::string_view request =
        "GET /version.txt HTTP/1.1\r\nHost: a\r\nX-Hold-Root: yes\r\nConnection: close\r\n\r\n";
    asio::write(held, asio::buffer(request));
    EXPECT_TRUE(retry_until_deadline([] {
        return held_request_started.load(std::memory_order_acquire);
    }));

    EXPECT_TRUE(write_version("new snapshot with a request still held"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "new snapshot with a request still held"));
    EXPECT_TRUE(write_version("another publication before the old request finishes"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "another publication before the old request finishes"));

    release_held_request.store(true, std::memory_order_release);
    std::string response;
    std::error_code error;
    asio::read(held, asio::dynamic_buffer(response), error);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(response.ends_with("old leased snapshot"));
    held.close();
    server.finish();
}

#ifndef _WIN32
TEST(Ruvia, document_root_refresh_follows_a_retargeted_symlink_root) {
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    const auto release = fs::temp_directory_path() / "ruvia_document_root_refresh_release";
    const auto staged_link = fs::temp_directory_path() / "ruvia_document_root_refresh_link.next";
    std::error_code ignored;
    fs::remove_all(release, ignored);
    fs::remove(staged_link, ignored);
    fs::create_directories(release);
    std::ofstream(release / "version.txt", std::ios::binary) << "retargeted release";

    // Swap the link atomically, as `ln -sfn` deploys do.
    const auto retarget = [&](const fs::path& target) {
        fs::create_directory_symlink(target, staged_link);
        fs::rename(staged_link, configured_root());
    };
    retarget(release);
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "retargeted release"));

    retarget(document_root_dir());
    EXPECT_TRUE(write_version("back on the original root"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "back on the original root"));
    fs::remove_all(release, ignored);
    server.finish();
}
#endif

TEST(Ruvia, document_root_refresh_failures_count_once_per_application) {
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    EXPECT_TRUE(write_version("cached before removal"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "cached before removal"));
    const auto failures_before = refresh_failures();
    const auto removed_at = steady_clock::now();
    EXPECT_TRUE(remove_document_root());

    // Observe a window of several intervals with the root missing. The
    // application's one refresher sleeps a full interval before every pass, so
    // it can fail at most once per interval in that window plus one pass that
    // was already sleeping, however many workers serve the root.
    std::size_t failures = 0;
    EXPECT_TRUE(retry_until_deadline([&] {
        failures = refresh_failures() - failures_before;
        return failures != 0 && steady_clock::now() - removed_at >= 10 * refresh_interval;
    }));
    const auto window_intervals =
        static_cast<std::size_t>((steady_clock::now() - removed_at) / refresh_interval);
    EXPECT_TRUE(failures >= 1);
    EXPECT_TRUE(failures <= window_intervals + 1);
    // Configuring static_options_ without resident limits still keeps the
    // application's small-file snapshots. Failed refreshes must not turn
    // the last complete snapshot into an unreadable file-backed response.
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "cached before removal"));

    // The refresher keeps running after failures and recovers once the root
    // is back.
    EXPECT_TRUE(write_version("restored"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "restored"));
    EXPECT_TRUE(retry_until_deadline([] {
        std::error_code ec;
        fs::remove_all(retired_root_dir(), ec);
        return !ec;
    }));
    server.finish();
}
