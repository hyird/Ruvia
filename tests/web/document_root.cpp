#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"
// A document root shares paths with a guarded wildcard route. This executable
// owns its application, so the document root cannot leak into other suites.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <semaphore>
#include <set>
#include <thread>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/middleware.h"
namespace {

namespace fs = std::filesystem;

class document_root_guard final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        if (!c.req().header("X-Test-Credential").has_value()) {
            throw ruvia::http_error({.status_ = ruvia::http_status::unauthorized});
        }
        co_await next_value();
    }
};

class document_root_routes final {
    RUVIA_ROUTES_BEGIN(document_root_routes)
    RUVIA_GET("/private/*", private_file, document_root_guard);
    RUVIA_GET("/admin/panel/*", private_file, document_root_guard);
    // Makes PROPFIND a method this server knows, for the 405-vs-501 split.
    RUVIA_METHOD("PROPFIND", "/dav", private_file);
    RUVIA_POST("/encoded/:slug", post_slug);
    RUVIA_GET("/a%20b.txt", get_space_route);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> private_file(ruvia::context& c) {
        co_return c.text("guarded");
    }

    ruvia::task<ruvia::http_response> post_slug(ruvia::context& c) {
        co_return c.text("posted:" + std::string(c.req().param("slug").value_or("")));
    }

    ruvia::task<ruvia::http_response> get_space_route(ruvia::context& c) {
        co_return c.text("from-route");
    }
};

void populate_document_root(const fs::path& dir) {
    fs::create_directories(dir / "private");
    fs::create_directories(dir / "admin" / "panel");
    fs::create_directories(dir / "encoded");
    std::ofstream(dir / "private" / "file.txt") << "secret";
    std::ofstream(dir / "admin" / "panel" / "secret.txt") << "secret";
    std::ofstream(dir / "public.txt") << "public";
    std::ofstream(dir / "my report.txt") << "spaced";
    std::ofstream(dir / "encoded" / "readme.txt") << "readme-from-disk";
    std::ofstream(dir / "encoded" / "hello world.txt") << "hello-world-from-disk";
    std::ofstream(dir / "a%20b.txt") << "literal-percent";
}

// Sends one request on a fresh connection and returns the complete response.
[[nodiscard]] std::string exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    std::string_view target, std::string_view method = "GET") {
    std::string request(method);
    request.append(" ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    return ruvia::test::exchange(io, endpoint, request);
}

[[nodiscard]] bool has_status(std::string_view response, std::string_view status) {
    return response.starts_with("HTTP/1.1 ") && response.substr(9).starts_with(status);
}

}  // namespace

TEST(Ruvia, document_root_never_serves_targets_rejected_by_routing) {
    const ruvia::test::scoped_temp_directory directory("ruvia-document-root");
    const auto& dir = directory.path();
    populate_document_root(dir);
    ruvia::document_root_config config;
    config.root_ = dir;
    config.static_options_.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    ruvia::app().document_root(std::move(config));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);

    // The document root still answers paths no route claims.
    const auto public_file = exchange(io, server.endpoint(), "/public.txt");
    EXPECT_TRUE(has_status(public_file, "200"));
    EXPECT_TRUE(public_file.ends_with("public"));

    // The guarded route owns its prefix, under every equivalent spelling
    // (RFC 3986 6.2.2.2): an encoded unreserved character reaches the same
    // route and its guard.
    for (const std::string_view target : {"/private/file.txt", "/private/file.txt/", "/admin/panel/secret.txt",
             "/%70rivate/file.txt", "/admin/%70anel/secret.txt"}) {
        EXPECT_TRUE(has_status(exchange(io, server.endpoint(), target), "401"));
    }

    // Dot-segment and empty-segment ("//", trailing '/') spellings of guarded
    // routes must not fall back to the document root.
    for (const std::string_view target :
        {"/private/./file.txt", "/private/%2E/file.txt", "/private/%2e/file.txt", "/private/x/../file.txt",
            "//private/file.txt", "/admin//panel/secret.txt", "//public.txt", "/public.txt/"}) {
        const auto response = exchange(io, server.endpoint(), target);
        EXPECT_TRUE(has_status(response, "404"));
        EXPECT_TRUE(response.find("secret") == std::string::npos);
        EXPECT_TRUE(response.find("public") == std::string::npos);
    }
    // Another method on an encoded spelling answers for the route, never the file.
    const auto encoded_post = exchange(io, server.endpoint(), "/%70rivate/file.txt", "POST");
    EXPECT_TRUE(has_status(encoded_post, "405"));
    EXPECT_TRUE(encoded_post.find("secret") == std::string::npos);
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/%70rivate/file.txt", "OPTIONS"), "204"));

    // The request path is decoded exactly once into the file name, and no
    // escape may reveal a separator routing never split on.
    const auto spaced = exchange(io, server.endpoint(), "/my%20report.txt");
    EXPECT_TRUE(has_status(spaced, "200"));
    EXPECT_TRUE(spaced.ends_with("spaced"));
    for (const std::string_view target : {"/my%2520report.txt", "/private%2Ffile.txt", "/private%5Cfile.txt"}) {
        const auto response = exchange(io, server.endpoint(), target);
        EXPECT_TRUE(has_status(response, "404"));
        EXPECT_TRUE(response.find("secret") == std::string::npos);
    }

    // RFC 9110 15.5.6 and 9.3.7: a document-root file exists for every method,
    // so OPTIONS describes it and other methods are 405 with Allow, while a
    // missing file stays 404 and an unregistered token stays 501.
    const auto options = exchange(io, server.endpoint(), "/public.txt", "OPTIONS");
    EXPECT_TRUE(has_status(options, "204"));
    EXPECT_TRUE(options.find("\r\nAllow: GET, HEAD, OPTIONS\r\n") != std::string::npos);
    const auto post = exchange(io, server.endpoint(), "/my%20report.txt", "POST");
    EXPECT_TRUE(has_status(post, "405"));
    EXPECT_TRUE(post.find("\r\nAllow: GET, HEAD, OPTIONS\r\n") != std::string::npos);
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/missing.txt", "POST"), "404"));
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/missing.txt", "OPTIONS"), "404"));
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/public.txt", "BREW"), "501"));
    // A registered extension token is known here, so the file answers 405 too.
    const auto propfind = exchange(io, server.endpoint(), "/public.txt", "PROPFIND");
    EXPECT_TRUE(has_status(propfind, "405"));
    EXPECT_TRUE(propfind.find("\r\nAllow: GET, HEAD, OPTIONS\r\n") != std::string::npos);

    // Percent-encoded document-root paths alongside a sibling method route on the same path pattern.
    const auto encoded_readme = exchange(io, server.endpoint(), "/encoded/%72eadme.txt", "GET");
    EXPECT_TRUE(has_status(encoded_readme, "200"));
    EXPECT_TRUE(encoded_readme.ends_with("readme-from-disk"));
    const auto encoded_space = exchange(io, server.endpoint(), "/encoded/hello%20world.txt", "GET");
    EXPECT_TRUE(has_status(encoded_space, "200"));
    EXPECT_TRUE(encoded_space.ends_with("hello-world-from-disk"));
    const auto encoded_options = exchange(io, server.endpoint(), "/encoded/%72eadme.txt", "OPTIONS");
    EXPECT_TRUE(has_status(encoded_options, "204"));
    EXPECT_TRUE(encoded_options.find("\r\nAllow: GET, POST, HEAD, OPTIONS\r\n") != std::string::npos);
    const auto delete_encoded = exchange(io, server.endpoint(), "/encoded/%72eadme.txt", "DELETE");
    EXPECT_TRUE(has_status(delete_encoded, "405"));
    EXPECT_TRUE(delete_encoded.find("\r\nAllow: GET, POST, HEAD, OPTIONS\r\n") != std::string::npos);
    const auto literal_percent = exchange(io, server.endpoint(), "/a%2520b.txt", "GET");
    EXPECT_TRUE(has_status(literal_percent, "200"));
    EXPECT_TRUE(literal_percent.ends_with("literal-percent"));
    server.finish();
}

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
    root_worker_state() {
        label_.append(std::to_string(next_worker_id.fetch_add(1, std::memory_order_relaxed)));
    }
    std::string label_{};
};

class root_request_observation final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
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

[[nodiscard]] const fs::path& test_directory() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-document-root-refresh");
    return directory.path();
}

[[nodiscard]] fs::path document_root_dir() {
    return test_directory() / "root";
}

[[nodiscard]] fs::path retired_root_dir() {
    return test_directory() / "retired";
}

// The application is configured through this symlink where the platform
// creates one unprivileged, as an atomic-deploy layout would.
[[nodiscard]] fs::path configured_root() {
#ifdef _WIN32
    return document_root_dir();
#else
    return test_directory() / "link";
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
    ruvia::test::wait_until(std::forward<fn_type>(attempt), observation_timeout);
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
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    return ruvia::test::exchange(io, endpoint, request);
}

// Observe distinct workers rather than whichever one accepts first. Windows
// routes fresh connections away from a worker that holds a long-lived request.
[[nodiscard]] bool eventually_serves(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view body,
    std::size_t required_workers = worker_count) {
    std::set<std::string> observed_workers{};
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
        observed_workers.emplace(std::string_view(response).substr(label, end - label));
        return observed_workers.size() >= required_workers;
    });
}

[[nodiscard]] std::size_t refresh_failures() {
    return ruvia::app().stats().document_root_refresh_failures_;
}

}  // namespace document_root_refresh_test

TEST(Ruvia, document_root_refresh_updates_multi_worker_application) {
    using namespace document_root_refresh_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
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
    using namespace document_root_refresh_test;
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
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "new snapshot with a request still held", worker_count - 1));
    EXPECT_TRUE(write_version("another publication before the old request finishes"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "another publication before the old request finishes", worker_count - 1));

    release_held_request.store(true, std::memory_order_release);
    std::string response{};
    std::error_code error;
    asio::read(held, asio::dynamic_buffer(response), error);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(response.ends_with("old leased snapshot"));
    held.close();
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "another publication before the old request finishes"));
    server.finish();
}

#ifndef _WIN32
TEST(Ruvia, document_root_refresh_follows_a_retargeted_symlink_root) {
    using namespace document_root_refresh_test;
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    const auto release = test_directory() / "release";
    const auto staged_link = test_directory() / "link.next";
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
    using namespace document_root_refresh_test;
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
            if (co_await ruvia::sleep_for(context_value.worker(), 1h, context_value.stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
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
    },
        5s);
}

}  // namespace document_root_shutdown_test

TEST(Ruvia, document_root_shutdown_cancels_old_snapshot_lease_and_restarts_refresh) {
    using namespace document_root_shutdown_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
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
    app.server({.worker_count_ = 3, .connection_scan_interval_ = 5ms})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port()})
        .use<hold_root_request>()
        .document_root(std::move(config))
        .on_start([] { started.release(); });
    constexpr std::string_view get_version =
        "GET /version.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started.try_acquire_for(5s));
        asio::ip::tcp::socket held(io);
        held.connect(endpoint);
        constexpr std::string_view request =
            "GET /version.txt HTTP/1.1\r\nHost: localhost\r\nX-Hold-Root: yes\r\nConnection: close\r\n\r\n";
        asio::write(held, asio::buffer(request));
        ruvia::test::wait_until([] {
            return held_request_started.load(std::memory_order_acquire);
        },
            5s);
        replace_version(directory.path(), "published while the old request remains held");
        ruvia::test::wait_until([&] {
            const auto response = ruvia::test::exchange(io, endpoint, get_version);
            return response.starts_with("HTTP/1.1 200") &&
                   response.ends_with("published while the old request remains held");
        },
            5s);
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
        },
            5s);
        app.stop();
        restarted.join();
    }
}
