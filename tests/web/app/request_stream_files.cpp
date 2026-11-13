#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_server.h"
#include "ruvia/web/server_config.h"

#include "scoped_temp_directory.h"
#include "tcp_port_reservation.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace request_stream_files_test {
using namespace std::chrono_literals;
namespace fs = std::filesystem;

template <typename root_type>
concept accepts_static_root = requires(ruvia::request_stream& stream, root_type&& root) {
    stream.static_file(std::forward<root_type>(root), ruvia::static_file_response_options{});
};

template <typename root_type>
concept accepts_default_static_root = requires(ruvia::request_stream& stream, root_type&& root) {
    stream.static_file(std::forward<root_type>(root));
};

static_assert(accepts_static_root<ruvia::static_root&>);
static_assert(accepts_static_root<const ruvia::static_root&>);
static_assert(!accepts_static_root<ruvia::static_root>);
static_assert(!accepts_static_root<const ruvia::static_root>);
static_assert(accepts_default_static_root<ruvia::static_root&>);
static_assert(accepts_default_static_root<const ruvia::static_root&>);
static_assert(!accepts_default_static_root<ruvia::static_root>);
static_assert(!accepts_default_static_root<const ruvia::static_root>);

void write_file(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string representation() {
    std::string bytes(100 * 1024 + 17, '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>('a' + index % 23);
    }
    return bytes;
}

template <typename check_type>
void with_server(ruvia::request_stream_callback callback, check_type check) {
    asio::io_context io;
    ruvia::blocking_pool pool({.thread_count_ = 2, .queue_capacity_ = 16});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &pool});
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = std::move(callback)});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            co_await check(client, server);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await server.shutdown();
        EXPECT_EQ(server.stats().active_connections_, 0);
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    pool.stop();
    pool.join();
    EXPECT_EQ(pool.stats().queued_, 0);
    EXPECT_EQ(pool.stats().running_, 0);
}

ruvia::task<void> expect_file_responses(ruvia::http_client& client, std::string_view bytes, std::string_view target) {
    auto full = co_await client.client().get(target).send();
    EXPECT_EQ(full.status(), ruvia::http_status::ok);
    EXPECT_EQ(full.header("content-type"), std::optional<std::string_view>("application/octet-stream"));
    EXPECT_EQ(full.header("content-length"), std::optional<std::string_view>(std::to_string(bytes.size())));
    EXPECT_FALSE(full.header("content-range").has_value());
    EXPECT_EQ(full.header("x-file"), std::optional<std::string_view>("retained"));
    EXPECT_EQ(full.header("set-cookie"), std::optional<std::string_view>("file=retained; Path=/"));
    const std::string etag(full.header("etag").value_or(""));
    EXPECT_FALSE(etag.empty());
    const auto body = co_await full.text();
    EXPECT_EQ(body.text(), bytes);

    auto head = co_await client.client().head(target).send();
    EXPECT_EQ(head.status(), ruvia::http_status::ok);
    EXPECT_EQ(head.header("content-type"), full.header("content-type"));
    EXPECT_EQ(head.header("etag"), std::optional<std::string_view>(etag));
    EXPECT_EQ(head.header("content-length"), std::optional<std::string_view>(std::to_string(bytes.size())));
    EXPECT_TRUE((co_await head.text()).empty());

    auto unchanged = co_await client.client().get(target).header("if-none-match", etag).send();
    EXPECT_EQ(unchanged.status(), ruvia::http_status::not_modified);
    EXPECT_EQ(unchanged.header("etag"), std::optional<std::string_view>(etag));
    EXPECT_FALSE(unchanged.header("content-type").has_value());
    EXPECT_FALSE(unchanged.header("content-range").has_value());
    EXPECT_TRUE((co_await unchanged.text()).empty());

    auto partial = co_await client.client().get(target).header("range", "bytes=7-19").send();
    EXPECT_EQ(partial.status(), ruvia::http_status::partial_content);
    EXPECT_EQ(partial.header("content-type"), std::optional<std::string_view>("application/octet-stream"));
    EXPECT_EQ(partial.header("content-length"), std::optional<std::string_view>("13"));
    EXPECT_EQ(partial.header("content-range"),
        std::optional<std::string_view>("bytes 7-19/" + std::to_string(bytes.size())));
    const auto partial_body = co_await partial.text();
    EXPECT_EQ(partial_body.text(), bytes.substr(7, 13));

    auto multipart = co_await client.client().get(target).header("range", "bytes=3-8,90000-90009").send();
    EXPECT_EQ(multipart.status(), ruvia::http_status::partial_content);
    EXPECT_TRUE(multipart.header("content-type").value_or("").starts_with("multipart/byteranges; boundary="));
    EXPECT_FALSE(multipart.header("content-range").has_value());
    const auto parts = co_await multipart.text();
    EXPECT_EQ(multipart.header("content-length"), std::optional<std::string_view>(std::to_string(parts.text().size())));
    EXPECT_NE(parts.text().find("Content-Type: application/octet-stream\r\n"), std::string_view::npos);
    EXPECT_NE(parts.text().find("Content-Range: bytes 3-8/102417"), std::string_view::npos);
    EXPECT_NE(parts.text().find("Content-Range: bytes 90000-90009/102417"), std::string_view::npos);
    EXPECT_NE(parts.text().find(bytes.substr(3, 6)), std::string_view::npos);
    EXPECT_NE(parts.text().find(bytes.substr(90000, 10)), std::string_view::npos);
}

void metadata(ruvia::request_stream& stream) {
    // A default-status draft must not replace the file factory's 206 or 304.
    stream.draft().header("x-file", "retained");
    stream.draft().header("set-cookie", "file=retained; Path=/");
}

}  // namespace request_stream_files_test

using namespace request_stream_files_test;

TEST(Ruvia, independent_request_stream_submits_complete_file_and_multipart_responses) {
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-file");
    const auto path = directory.path() / "file.bin";
    const auto bytes = representation();
    write_file(path, bytes);
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        metadata(stream);
        auto caller_path = path;
        std::string type = "application/octet-stream";
        auto pending = stream.file({.path_ = caller_path, .content_type_ = type});
        caller_path = directory.path() / "missing";
        type = "invalid/later";
        auto response = co_await std::move(pending);
        if (response.status() == ruvia::http_status::not_modified) {
            EXPECT_FALSE(response.header("Content-Type").has_value());
        } else if (response.has_multipart_file_body()) {
            EXPECT_TRUE(response.header("Content-Type").value_or("").starts_with("multipart/byteranges; boundary="));
        } else {
            EXPECT_EQ(response.header("Content-Type"), std::optional<std::string_view>("application/octet-stream"));
        }
        stream.respond(std::move(response));
        EXPECT_FALSE(stream.response().committed()); }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> { co_await expect_file_responses(client, bytes, "/file"); });
}

TEST(Ruvia, independent_request_stream_static_root_rejects_paths_and_changed_identity) {
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-static");
    const auto path = directory.path() / "file.bin";
    const auto replacement = directory.path() / "replacement.bin";
    const auto bytes = representation();
    write_file(path, bytes);
    write_file(replacement, std::string(bytes.size(), 'z'));
    ruvia::static_root root(directory.path(), {.default_content_type_ = "application/octet-stream",
                                                  .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        metadata(stream);
        const auto target = stream.request().path();
        if (target == "/deny") {
            stream.respond(co_await stream.static_file(root, {.relative_path_ = "../file.bin"}));
        } else if (target == "/absolute") {
            stream.respond(co_await stream.static_file(root, {.relative_path_ = "/file.bin"}));
        } else if (target == "/unindexed") {
            stream.respond(co_await stream.static_file(root, {.relative_path_ = "later.bin"}));
        } else {
            std::string relative = "file.bin";
            auto pending = stream.static_file(root, {.relative_path_ = relative});
            relative = "missing.bin";
            stream.respond(co_await std::move(pending));
        } }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        co_await expect_file_responses(client, bytes, "/static");
        for (const auto target : {"/deny", "/absolute"}) {
            auto denied = co_await client.client().get(target).send();
            EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
            (void)co_await denied.text();
        }
        write_file(directory.path() / "later.bin", "not indexed");
        auto missing = co_await client.client().get("/unindexed").send();
        EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
        (void)co_await missing.text();
        fs::rename(path, directory.path() / "original.bin");
        fs::rename(replacement, path);
        auto changed = co_await client.client().head("/static").send();
        EXPECT_EQ(changed.status(), ruvia::http_status::internal_server_error);
        EXPECT_TRUE((co_await changed.text()).empty());
        auto stale = co_await client.client().get("/static").send();
        EXPECT_EQ(stale.status(), ruvia::http_status::internal_server_error);
        (void)co_await stale.text(); });
}

#ifndef _WIN32
// As in the shared file-transfer regression, moving a parent directory leaves
// the opened file's change time intact; moving the file itself does not.
// Windows does not permit this directory move while the file is open.
TEST(Ruvia, independent_request_stream_reuses_open_file_after_path_replacement) {
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-open");
    const auto bytes = representation();
    const auto single = directory.path() / "single" / "file.bin";
    const auto partial = directory.path() / "partial" / "file.bin";
    const auto multiple = directory.path() / "multiple" / "file.bin";
    fs::create_directory(single.parent_path());
    fs::create_directory(partial.parent_path());
    fs::create_directory(multiple.parent_path());
    write_file(single, bytes);
    write_file(partial, bytes);
    write_file(multiple, bytes);
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        const auto target = stream.request().path();
        const auto path = target == "/single" ? single : target == "/partial" ? partial : multiple;
        auto response = co_await stream.file({.path_ = path});
        EXPECT_TRUE(response.has_file_representation());
        const auto replaced = co_await stream.run_blocking([path, bytes] {
            fs::rename(path.parent_path(), fs::path(path.parent_path()).concat(".opened"));
            fs::create_directory(path.parent_path());
            write_file(path, std::string(bytes.size(), 'Z'));
        });
        EXPECT_EQ(replaced.status(), ruvia::blocking_status::completed);
        replaced.value();
        stream.respond(std::move(response)); }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        auto single_response = co_await client.client().get("/single").send();
        EXPECT_EQ(single_response.status(), ruvia::http_status::ok);
        const auto single_body = co_await single_response.text();
        EXPECT_EQ(single_body.text(), bytes);
        auto partial_response = co_await client.client().get("/partial").header("range", "bytes=7-19").send();
        EXPECT_EQ(partial_response.status(), ruvia::http_status::partial_content);
        EXPECT_EQ(partial_response.header("content-range"), std::optional<std::string_view>("bytes 7-19/102417"));
        const auto partial_body = co_await partial_response.text();
        EXPECT_EQ(partial_body.text(), std::string_view(bytes).substr(7, 13));
        auto multiple_response = co_await client.client().get("/multiple").header("range", "bytes=0-9,90000-90009").send();
        EXPECT_EQ(multiple_response.status(), ruvia::http_status::partial_content);
        const auto parts = co_await multiple_response.text();
        EXPECT_NE(parts.text().find(bytes.substr(0, 10)), std::string_view::npos);
        EXPECT_NE(parts.text().find(bytes.substr(90000, 10)), std::string_view::npos);
        EXPECT_EQ(parts.text().find(std::string(10, 'Z')), std::string_view::npos); });
}
#endif

TEST(Ruvia, independent_request_stream_revalidates_open_files_before_sending_their_head) {
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-changed");
    const auto bytes = representation();
    for (const auto name : {"file-full.bin", "file-multiple.bin", "static-full.bin", "static-multiple.bin"}) {
        write_file(directory.path() / name, bytes);
    }
    ruvia::static_root root(directory.path(), {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        const std::string relative(stream.request().path().substr(1));
        const auto path = directory.path() / relative;
        auto response = relative.starts_with("static-")
                            ? co_await stream.static_file(root, {.relative_path_ = relative})
                            : co_await stream.file({.path_ = path});
        EXPECT_TRUE(response.has_file_representation());
        // Keep the opened inode, but invalidate its framed size. Even a range
        // that still fits must fail validation before any multipart boundary
        // or success head leaves the server.
        const auto changed = co_await stream.run_blocking([path, size = bytes.size()] {
            write_file(path, std::string(size + 1, 'Z'));
        });
        EXPECT_EQ(changed.status(), ruvia::blocking_status::completed);
        changed.value();
        stream.respond(std::move(response)); }, [](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        for (const auto target : {"/file-full.bin", "/file-multiple.bin", "/static-full.bin", "/static-multiple.bin"}) {
            auto request = client.client().get(target);
            if (std::string_view(target).find("multiple") != std::string_view::npos) {
                request.header("range", "bytes=0-9,90000-90009");
            }
            auto response = co_await request.send();
            EXPECT_EQ(response.status(), ruvia::http_status::internal_server_error);
            EXPECT_FALSE(response.header("content-range").has_value());
            const auto body = co_await response.text();
            EXPECT_NE(body.text().find("\"code\":\"file_read_failed\""), std::string_view::npos);
            EXPECT_EQ(body.text().find(std::string(10, 'Z')), std::string_view::npos);
        } });
}

TEST(Ruvia, independent_request_stream_response_forms_are_mutually_exclusive_and_failures_replace_buffered_selection) {
    with_server([](ruvia::request_stream& stream) -> ruvia::task<void> {
        const auto target = stream.request().path();
        if (target == "/stream-first") {
            (void)co_await stream.response().commit();
            ruvia::http_response rejected;
            EXPECT_THROW(stream.respond(std::move(rejected)), std::logic_error);
            co_await stream.response().write(std::string_view("streamed"));
            co_return;
        }
        ruvia::http_response response;
        response.status(ruvia::http_status::conflict);
        response.header("x-selected", "retained");
        response.body("buffered");
        stream.respond(std::move(response));
        EXPECT_FALSE(stream.response().committed());
        if (target == "/failure") {
            throw ruvia::http_error({.status_ = ruvia::http_status::forbidden, .message_ = "replace selected response"});
        }
        bool refused = false;
        try {
            if (target == "/end") {
                co_await stream.response().end();
            } else if (target == "/commit") {
                (void)co_await stream.response().commit();
            } else if (target == "/tunnel") {
                (void)co_await stream.accept_tunnel();
            } else if (target == "/websocket") {
                (void)co_await stream.accept_websocket();
            } else {
                ruvia::http_response second;
                stream.respond(std::move(second));
            }
        } catch (const std::logic_error&) {
            refused = true;
        }
        EXPECT_TRUE(refused);
        EXPECT_FALSE(stream.response().committed()); }, [](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        for (const auto target : {"/commit", "/end", "/twice", "/tunnel", "/websocket"}) {
            auto request = client.client().get(target);
            if (std::string_view(target) == "/tunnel" || std::string_view(target) == "/websocket") {
                request.header("connection", "Upgrade");
                request.header("upgrade", std::string_view(target) == "/websocket" ? "websocket" : "test");
                request.header("sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
                request.header("sec-websocket-version", "13");
            }
            auto response = co_await request.send();
            EXPECT_EQ(response.status(), ruvia::http_status::conflict);
            EXPECT_EQ(response.header("x-selected"), std::optional<std::string_view>("retained"));
            const auto body = co_await response.text();
            EXPECT_EQ(body.text(), "buffered");
        }
        auto failure = co_await client.client().get("/failure").send();
        EXPECT_EQ(failure.status(), ruvia::http_status::forbidden);
        EXPECT_FALSE(failure.header("x-selected").has_value());
        (void)co_await failure.text();
        auto streamed = co_await client.client().get("/stream-first").send();
        EXPECT_EQ(streamed.status(), ruvia::http_status::ok);
        const auto streamed_body = co_await streamed.text();
        EXPECT_EQ(streamed_body.text(), "streamed"); });
}

TEST(Ruvia, independent_request_stream_retires_unstarted_file_and_accept_tasks_with_callback) {
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-cold");
    const auto path = directory.path() / "cold.bin";
    write_file(path, "cold");
    ruvia::static_root root(directory.path(), {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});
    std::array<std::optional<ruvia::task<ruvia::http_response>>, 2> files;
    std::optional<ruvia::task<ruvia::http_tunnel*>> tunnel;
    std::optional<ruvia::task<ruvia::websocket_accept_result>> websocket;
    with_server([&](ruvia::request_stream& stream) -> ruvia::task<void> {
        files[0].emplace(stream.file({.path_ = path}));
        files[1].emplace(stream.static_file(root, {.relative_path_ = "cold.bin"}));
        tunnel.emplace(stream.accept_tunnel());
        websocket.emplace(stream.accept_websocket());
        ruvia::http_response response;
        response.body("no cold operation started");
        stream.respond(std::move(response));
        co_return; }, [&](ruvia::http_client& client, ruvia::http_server&) -> ruvia::task<void> {
        auto response = co_await client.client().get("/cold").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "no cold operation started");
        for (auto& file : files) {
            bool expired = false;
            try {
                (void)co_await std::move(*file);
            } catch (const std::logic_error&) {
                expired = true;
            }
            EXPECT_TRUE(expired);
            file.reset();
        }
        bool tunnel_expired = false;
        try {
            (void)co_await std::move(*tunnel);
        } catch (const std::logic_error&) {
            tunnel_expired = true;
        }
        EXPECT_TRUE(tunnel_expired);
        tunnel.reset();
        bool websocket_expired = false;
        try {
            (void)co_await std::move(*websocket);
        } catch (const std::logic_error&) {
            websocket_expired = true;
        }
        EXPECT_TRUE(websocket_expired);
        websocket.reset(); });
}

#ifndef _WIN32
namespace request_stream_files_test {
int releases{};
void release_counted_file(ruvia::http_response_file_descriptor::native_type native) noexcept {
    ++releases;
    ::close(static_cast<int>(native));
}

ruvia::http_response counted_file(ruvia::request_stream& stream, const fs::path& path) {
    const auto native = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (native < 0) {
        throw std::runtime_error("cannot open counted response file");
    }
    ruvia::http_response_file_descriptor opened(static_cast<std::uintptr_t>(native), &release_counted_file);
    ruvia::http_response response;
    response.file_body(path.native(), ruvia::http_content_length(4), ruvia::http_content_length(0),
        ruvia::http_content_length(4), ruvia::http_response_file_identity::unchecked(), std::move(opened));
    return response;
}
}  // namespace request_stream_files_test

TEST(Ruvia, independent_request_stream_releases_selected_file_exactly_once_on_error_and_shutdown) {
    const ruvia::test::scoped_temp_directory directory("ruvia-request-stream-release");
    const auto path = directory.path() / "owned.bin";
    write_file(path, "file");
    releases = 0;
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::tcp_port_reservation port(io);
    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::worker_signal selected(worker);
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& stream) -> ruvia::task<void> {
            stream.respond(counted_file(stream, path));
            if (stream.request().path() == "/error") {
                throw ruvia::http_error({.status_ = ruvia::http_status::conflict, .message_ = "release file"});
            }
            selected.notify();
            EXPECT_EQ(co_await ruvia::sleep_for(stream.worker(), 10s, stream.stop_token()), ruvia::timer_sleep_result::stop_requested);
        }});
        server.listen({.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        const std::string origin = "http://127.0.0.1:" + std::to_string(port.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = origin, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            co_await server.start();
            auto error = co_await client.client().get("/error").send();
            EXPECT_EQ(error.status(), ruvia::http_status::conflict);
            (void)co_await error.text();
            EXPECT_EQ(releases, 1);
            asio::ip::tcp::socket peer(io);
            auto connected = co_await ruvia::async_asio([&](auto completion) {
                peer.async_connect({asio::ip::address_v4::loopback(), port.endpoint().port()}, std::move(completion));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            const std::string request = "GET /cancel HTTP/1.1\r\nHost: localhost\r\n\r\n";
            auto sent = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
                asio::async_write(peer, asio::buffer(request), std::move(completion));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            co_await selected.wait();
            EXPECT_EQ(releases, 1);
            co_await server.shutdown();
            EXPECT_EQ(releases, 2);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        co_await server.shutdown();
        EXPECT_EQ(server.stats().active_connections_, 0);
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    EXPECT_EQ(releases, 2);
}
#endif
