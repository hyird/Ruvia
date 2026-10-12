#include <chrono>
#include <cstddef>
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

#include "ruvia/web/app.h"
#include "ruvia/web/server_config.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// A multi-worker application serves a document root that changes while it
// runs. This executable owns that application, so the root cannot leak into
// other suites.

namespace {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

constexpr std::size_t worker_count = 3;
constexpr std::chrono::milliseconds refresh_interval{20};
constexpr std::chrono::seconds observation_timeout{10};

[[nodiscard]] fs::path document_root_dir() {
    return fs::temp_directory_path() / "ruvia_document_root_refresh_test";
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

// Windows can briefly refuse a write or delete while a refresh pass inspects
// the tree, so filesystem mutations retry until the deadline.
[[nodiscard]] bool write_version(std::string_view body) {
    return retry_until_deadline([body] {
        std::error_code ignored;
        fs::create_directories(document_root_dir(), ignored);
        std::ofstream output(version_file(), std::ios::binary | std::ios::trunc);
        output << body;
        output.close();
        return !output.fail();
    });
}

[[nodiscard]] bool remove_document_root() {
    return retry_until_deadline([] {
        std::error_code ec;
        fs::remove_all(document_root_dir(), ec);
        return !ec && !fs::exists(document_root_dir(), ec) && !ec;
    });
}

// The application is shared by every case in this executable. It is configured
// once, with the root populated: startup builds the first index synchronously.
[[nodiscard]] steady_clock::time_point configure_application() {
    static const auto configured_at = [] {
        std::error_code ignored;
        fs::remove_all(document_root_dir(), ignored);
        fs::create_directories(document_root_dir());
        std::ofstream(version_file(), std::ios::binary) << "initial";

        ruvia::document_root_config config;
        config.root_ = document_root_dir();
        config.static_options_.file_types_ =
            ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind_type::all};
        config.runtime_.refresh_interval_ = refresh_interval;
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

// The acceptor hands fresh connections to workers round-robin, so worker_count
// consecutive connections reach every worker once. True once a full round of
// consecutive requests serves `body`.
[[nodiscard]] bool every_worker_serves(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view body) {
    std::size_t consecutive = 0;
    return retry_until_deadline([&] {
        const auto response = exchange(io, endpoint, "/version.txt");
        const bool served = response.starts_with("HTTP/1.1 200") && response.ends_with(body);
        consecutive = served ? consecutive + 1 : 0;
        return consecutive == worker_count;
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

RUVIA_TEST(document_root_refresh_failures_count_once_per_application) {
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

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

    // The refresher keeps running after failures and recovers once the root
    // is back.
    RUVIA_CHECK(write_version("restored"));
    RUVIA_CHECK(every_worker_serves(io, server.endpoint(), "restored"));
    server.finish();
}
