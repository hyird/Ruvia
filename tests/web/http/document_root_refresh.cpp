#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

#include "ruvia/http/http_ascii.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/next.h"
#include "ruvia/web/server_config.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// A multi-worker application serves a document root that changes while it
// runs. This executable owns that application, so the root cannot leak into
// other suites.

namespace ruvia::test {

class document_root_worker_header final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    task<void> handle(context& value, next& next_value) {
        co_await next_value();
        value.header("X-Document-Worker", std::to_string(value.worker().id()));
    }
};

}  // namespace ruvia::test

namespace {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

constexpr std::size_t worker_count = 3;
constexpr std::chrono::milliseconds refresh_interval{20};
constexpr std::chrono::seconds observation_timeout{10};

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
            {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::all}};
        config.refresh_interval_ = refresh_interval;
        ruvia::app().use<ruvia::test::document_root_worker_header>().document_root(std::move(config));
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

// Independent listeners do not promise round-robin distribution. Count only
// distinct workers that actually return the requested version.
[[nodiscard]] bool every_worker_serves(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view body) {
    std::array<std::uint64_t, worker_count> workers{};
    std::size_t observed = 0;
    return retry_until_deadline([&] {
        const auto response = exchange(io, endpoint, "/version.txt");
        const auto head_end = response.find("\r\n\r\n");
        if (!response.starts_with("HTTP/1.1 200") || !response.ends_with(body) ||
            head_end == std::string::npos) {
            return false;
        }
        auto head = std::string_view(response).substr(0, head_end);
        while (!head.empty()) {
            const auto end = head.find("\r\n");
            const auto line = head.substr(0, end);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos &&
                ruvia::http_ascii_equals_ignore_case(line.substr(0, colon), "X-Document-Worker")) {
                auto worker = line.substr(colon + 1);
                while (!worker.empty() && (worker.front() == ' ' || worker.front() == '\t')) {
                    worker.remove_prefix(1);
                }
                if (!worker.empty()) {
                    std::uint64_t worker_id{};
                    const auto parsed = std::from_chars(worker.data(), worker.data() + worker.size(), worker_id);
                    const auto known_end = workers.begin() + static_cast<std::ptrdiff_t>(observed);
                    if (parsed.ec == std::errc{} && parsed.ptr == worker.data() + worker.size() &&
                        std::find(workers.begin(), known_end, worker_id) == known_end &&
                        observed < workers.size()) {
                        workers[observed++] = worker_id;
                    }
                }
                break;
            }
            if (end == std::string_view::npos) {
                break;
            }
            head.remove_prefix(end + 2);
        }
        return observed == worker_count;
    });
}

[[nodiscard]] std::size_t refresh_failures() {
    return ruvia::app().http_stats().document_root_refresh_failures_;
}

}  // namespace

RUVIA_TEST(document_root_refresh_reaches_every_worker) {
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    // Every version has a distinct size, so a change is indexed even where the
    // filesystem's modification time is coarse.
    RUVIA_CHECK(write_version("first version"));
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "first version"));

    RUVIA_CHECK(write_version("second, longer version"));
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "second, longer version"));
    server.finish();
}

#ifndef _WIN32
RUVIA_TEST(document_root_refresh_follows_a_retargeted_symlink_root) {
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
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "retargeted release"));

    retarget(document_root_dir());
    RUVIA_CHECK(write_version("back on the original root"));
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "back on the original root"));
    fs::remove_all(release, ignored);
    server.finish();
}
#endif

RUVIA_TEST(document_root_refresh_failures_count_once_per_application) {
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    RUVIA_CHECK(write_version("cached before removal"));
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "cached before removal"));
    const auto failures_before = refresh_failures();
    const auto removed_at = steady_clock::now();
    RUVIA_CHECK(remove_document_root());

    // Observe a window of several intervals with the root missing. The
    // application's one refresher sleeps a full interval before every pass, so
    // it can fail at most once per interval in that window plus one pass that
    // was already sleeping, however many workers serve the root.
    std::size_t failures = 0;
    RUVIA_CHECK(retry_until_deadline([&] {
        failures = refresh_failures() - failures_before;
        return failures != 0 && steady_clock::now() - removed_at >= 10 * refresh_interval;
    }));
    const auto window_intervals =
        static_cast<std::size_t>((steady_clock::now() - removed_at) / refresh_interval);
    RUVIA_CHECK(failures >= 1);
    RUVIA_CHECK(failures <= window_intervals + 1);
    // Configuring static_options_ without resident limits still keeps the
    // application's small-file snapshots. Failed refreshes must not turn
    // the last complete snapshot into an unreadable file-backed response.
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "cached before removal"));

    // The refresher keeps running after failures and recovers once the root
    // is back.
    RUVIA_CHECK(write_version("restored"));
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "restored"));
    RUVIA_CHECK(retry_until_deadline([] {
        std::error_code ec;
        fs::remove_all(retired_root_dir(), ec);
        return !ec;
    }));
    server.finish();
}
