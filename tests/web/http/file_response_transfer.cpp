#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/static_files.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// File bodies leave the server on the native path (sendfile/TransmitFile, or
// one buffered send for small files). Pipelined responses must still arrive
// complete and in order, whatever path each one takes.

namespace file_response_transfer_test {

namespace fs = std::filesystem;

// Below, around and above the in-memory threshold, and above one kernel
// transfer slice.
constexpr std::size_t small_size = 100;
constexpr std::size_t medium_size = 20 * 1024;
constexpr std::size_t large_size = 300 * 1024 + 17;

[[nodiscard]] const fs::path& transfer_dir() {
    static const fs::path dir = fs::temp_directory_path() / "ruvia_file_response_transfer";
    return dir;
}

[[nodiscard]] std::string file_bytes(std::size_t size, char seed) {
    std::string bytes(size, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<char>(seed + static_cast<char>(index % 23));
    }
    return bytes;
}

void write_file(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::unique_ptr<ruvia::static_root> indexed_root;

// A handler that framed its file response signals `held_response_framed`,
// then holds the response until the test reports the file rewritten
// (bounded at 5 s).
std::binary_semaphore held_response_framed{0};
std::atomic<bool> rewritten{false};

class file_transfer_routes final {
    RUVIA_ROUTES_BEGIN(file_transfer_routes)
    RUVIA_GET("/file-transfer/small", small);
    RUVIA_GET("/file-transfer/medium", medium);
    RUVIA_GET("/file-transfer/large", large);
    RUVIA_GET("/file-transfer/text", text);
    RUVIA_GET("/file-transfer/indexed-small", indexed_small);
    RUVIA_GET("/file-transfer/indexed-large", indexed_large);
    RUVIA_GET("/file-transfer/held/:name", held);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> small(ruvia::context& c) {
        co_return c.file({.path_ = transfer_dir() / "small.bin"});
    }

    ruvia::task<ruvia::http_response> medium(ruvia::context& c) {
        co_return c.file({.path_ = transfer_dir() / "medium.bin"});
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& c) {
        co_return c.file({.path_ = transfer_dir() / "large.bin"});
    }

    ruvia::task<ruvia::http_response> text(ruvia::context& c) {
        co_return c.text("between");
    }

    ruvia::task<ruvia::http_response> indexed_small(ruvia::context& c) {
        co_return c.static_file(*indexed_root, {.relative_path_ = "small.bin"});
    }

    ruvia::task<ruvia::http_response> indexed_large(ruvia::context& c) {
        co_return c.static_file(*indexed_root, {.relative_path_ = "large.bin"});
    }

    ruvia::task<ruvia::http_response> held(ruvia::context& c) {
        auto response = c.file({.path_ = transfer_dir() / std::string(c.req().param("name").value_or(""))});
        held_response_framed.release();
        for (int wait = 0; wait < 1000 && !rewritten.load(); ++wait) {
            (void)co_await ruvia::sleep_for(c.worker(), std::chrono::milliseconds(5));
        }
        co_return response;
    }
};

struct parsed_response final {
    std::string status_;
    std::string head_;
    std::string body_;
};

[[nodiscard]] std::optional<std::size_t> content_length(std::string_view head) {
    constexpr std::string_view name = "\r\nContent-Length: ";
    const auto position = head.find(name);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t value = 0;
    for (auto index = position + name.size(); index < head.size() && head[index] >= '0' && head[index] <= '9';
        ++index) {
        value = value * 10 + static_cast<std::size_t>(head[index] - '0');
    }
    return value;
}

// Splits a stream of Content-Length framed responses. `bodyless` marks the
// responses to HEAD requests, whose Content-Length carries no body.
[[nodiscard]] std::vector<parsed_response> split_responses(std::string_view stream, const std::vector<bool>& bodyless) {
    std::vector<parsed_response> responses;
    while (!stream.empty() && responses.size() < bodyless.size()) {
        const auto head_end = stream.find("\r\n\r\n");
        if (head_end == std::string_view::npos) {
            break;
        }
        parsed_response response;
        response.head_.assign(stream.substr(0, head_end + 2));
        response.status_.assign(stream.substr(9, 3));
        stream.remove_prefix(head_end + 4);
        const auto length = bodyless[responses.size()] ? std::optional<std::size_t>(0) : content_length(response.head_);
        if (!length.has_value() || *length > stream.size()) {
            break;
        }
        response.body_.assign(stream.substr(0, *length));
        stream.remove_prefix(*length);
        responses.push_back(std::move(response));
    }
    return responses;
}

[[nodiscard]] std::string exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view requests) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(requests));
    std::string output;
    std::error_code error;
    asio::read(socket, asio::dynamic_buffer(output), error);
    return output;
}

[[nodiscard]] std::string get(std::string_view target, std::string_view extra = {}) {
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: localhost\r\n");
    request.append(extra);
    request.append("\r\n");
    return request;
}

}  // namespace file_response_transfer_test

using namespace file_response_transfer_test;

RUVIA_TEST(pipelined_file_responses_arrive_complete_and_in_order) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    const auto small_bytes = file_bytes(small_size, 'a');
    const auto medium_bytes = file_bytes(medium_size, 'b');
    const auto large_bytes = file_bytes(large_size, 'c');
    write_file(transfer_dir() / "small.bin", small_bytes);
    write_file(transfer_dir() / "medium.bin", medium_bytes);
    write_file(transfer_dir() / "large.bin", large_bytes);
    indexed_root = std::make_unique<ruvia::static_root>(transfer_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::all},
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);

    std::string requests;
    requests += get("/file-transfer/small");
    requests += get("/file-transfer/large");
    requests += get("/file-transfer/text");
    requests += get("/file-transfer/medium");
    requests += get("/file-transfer/large", "Range: bytes=10-19\r\n");
    requests += get("/file-transfer/large", "Range: bytes=100000-299999\r\n");
    requests += get("/file-transfer/medium", "Range: bytes=0-4,10-14\r\n");
    requests += "HEAD /file-transfer/large HTTP/1.1\r\nHost: localhost\r\n\r\n";
    requests += get("/file-transfer/indexed-small");
    requests += get("/file-transfer/indexed-large", "Connection: close\r\n");
    const auto stream = exchange(io, server.endpoint(), requests);
    const auto responses =
        split_responses(stream, {false, false, false, false, false, false, false, true, false, false});

    RUVIA_CHECK_EQ(responses.size(), std::size_t{10});
    if (responses.size() == 10) {
        RUVIA_CHECK_EQ(responses[0].status_, std::string("200"));
        RUVIA_CHECK(responses[0].body_ == small_bytes);
        RUVIA_CHECK_EQ(responses[1].status_, std::string("200"));
        RUVIA_CHECK(responses[1].body_ == large_bytes);
        RUVIA_CHECK_EQ(responses[2].body_, std::string("between"));
        RUVIA_CHECK(responses[3].body_ == medium_bytes);
        RUVIA_CHECK_EQ(responses[4].status_, std::string("206"));
        RUVIA_CHECK(responses[4].body_ == large_bytes.substr(10, 10));
        RUVIA_CHECK_EQ(responses[5].status_, std::string("206"));
        RUVIA_CHECK(responses[5].body_ == large_bytes.substr(100000, 200000));
        RUVIA_CHECK_EQ(responses[6].status_, std::string("206"));
        RUVIA_CHECK(responses[6].head_.find("multipart/byteranges") != std::string::npos);
        RUVIA_CHECK(responses[6].body_.find(medium_bytes.substr(0, 5)) != std::string::npos);
        RUVIA_CHECK(responses[6].body_.find(medium_bytes.substr(10, 5)) != std::string::npos);
        RUVIA_CHECK_EQ(responses[7].status_, std::string("200"));
        RUVIA_CHECK_EQ(content_length(responses[7].head_), std::optional<std::size_t>(large_size));
        RUVIA_CHECK(responses[8].body_ == small_bytes);
        RUVIA_CHECK(responses[9].body_ == large_bytes);
    }
    // Nothing follows the last response: every body was exactly its length.
    std::size_t framed = 0;
    for (const auto& response : responses) {
        framed += response.head_.size() + 2 + response.body_.size();
    }
    RUVIA_CHECK_EQ(framed, stream.size());

    server.finish();
    indexed_root.reset();
    fs::remove_all(transfer_dir(), ignored);
}

RUVIA_TEST(changed_indexed_file_is_never_sent_under_a_stale_head) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    write_file(transfer_dir() / "small.bin", file_bytes(small_size, 'a'));
    write_file(transfer_dir() / "large.bin", file_bytes(large_size, 'c'));
    indexed_root = std::make_unique<ruvia::static_root>(transfer_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind_type::all},
        });
    // The index framed both files at their old size and identity.
    write_file(transfer_dir() / "small.bin", file_bytes(small_size + 1, 'x'));
    write_file(transfer_dir() / "large.bin", file_bytes(large_size + 1, 'y'));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const std::string_view target : {"/file-transfer/indexed-small", "/file-transfer/indexed-large"}) {
        const auto response = exchange(io, server.endpoint(), get(target, "Connection: close\r\n"));
        // The file is checked before the head is sent, so the connection
        // closes without committing a 200 for bytes it cannot honour.
        RUVIA_CHECK(!response.starts_with("HTTP/1.1 200 "));
    }

    server.finish();
    indexed_root.reset();
    fs::remove_all(transfer_dir(), ignored);
}

RUVIA_TEST(file_changed_in_place_after_framing_is_never_sent_under_its_head) {
    std::error_code ignored;
    fs::remove_all(transfer_dir(), ignored);
    fs::create_directories(transfer_dir());
    write_file(transfer_dir() / "small.bin", file_bytes(small_size, 'a'));
    write_file(transfer_dir() / "large.bin", file_bytes(large_size, 'c'));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    for (const auto& [name, size] : {std::pair<std::string_view, std::size_t>{"small.bin", small_size},
             std::pair<std::string_view, std::size_t>{"large.bin", large_size}}) {
        rewritten = false;
        asio::ip::tcp::socket socket(io);
        socket.connect(server.endpoint());
        const auto request = get("/file-transfer/held/" + std::string(name), "Connection: close\r\n");
        asio::write(socket, asio::buffer(request));
        // The handler framed the response from the file's current size and
        // identity and still holds it.
        const bool held = held_response_framed.try_acquire_for(std::chrono::seconds(5));
        RUVIA_CHECK(held);
        // Outlast coarse change-time clocks, then rewrite the bytes in place:
        // same size, same file, so only its change time tells them apart.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto replacement = file_bytes(size, 'x');
        {
            std::fstream output(transfer_dir() / std::string(name), std::ios::binary | std::ios::in | std::ios::out);
            output.write(replacement.data(), static_cast<std::streamsize>(replacement.size()));
        }
        rewritten = true;
        std::string response;
        std::error_code error;
        asio::read(socket, asio::dynamic_buffer(response), error);
        // Bytes paired with the head are always the framed version: a small
        // body was read from the framing handle before the rewrite and is
        // sent as framed; a larger handle is checked before the head is sent,
        // so the connection closes without committing a 200.
        RUVIA_CHECK(response.find(replacement.substr(0, 64)) == std::string::npos);
        if (size == small_size) {
            RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
            RUVIA_CHECK(response.ends_with(file_bytes(size, 'a')));
        } else {
            RUVIA_CHECK(!response.starts_with("HTTP/1.1 200 "));
        }
    }

    server.finish();
    fs::remove_all(transfer_dir(), ignored);
}
