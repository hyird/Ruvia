#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory_resource>
#include <new>
#include <optional>
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
#include <asio/read_until.hpp>
#include <asio/write.hpp>

#ifdef _WIN32
#include <malloc.h>
#endif

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

// Inbound buffer budgets on the streamed request body and CONNECT tunnel
// paths, and the worker's memory trim, which runs only after a scan interval
// without request exchanges. This executable owns its application (small
// budgets) and replaces the global aligned operator new/delete: worker memory
// takes its cached large blocks from them, so a block handed back to them is
// one the worker has trimmed.

namespace {

std::atomic<void*> watched_block{nullptr};
std::atomic<bool> watched_block_freed{false};
// Set on the worker while a handler allocates the block to watch.
thread_local bool capturing_block = false;
constexpr std::size_t watched_block_bytes = std::size_t{200} * 1024;

void* aligned_allocate(std::size_t bytes, std::size_t alignment) noexcept {
    const auto size = bytes == 0 ? alignment : (bytes + alignment - 1) / alignment * alignment;
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    return std::aligned_alloc(alignment, size);
#endif
}

void aligned_release(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    if (pointer == watched_block.load()) {
        watched_block_freed = true;
    }
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void* aligned_new(std::size_t bytes, std::align_val_t alignment) {
    auto* const pointer = aligned_allocate(bytes, static_cast<std::size_t>(alignment));
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    if (capturing_block && bytes >= watched_block_bytes) {
        watched_block = pointer;
    }
    return pointer;
}

}  // namespace

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return aligned_new(bytes, alignment);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return aligned_new(bytes, alignment);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return aligned_new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return aligned_new(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* pointer, std::align_val_t) noexcept {
    aligned_release(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
    aligned_release(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    aligned_release(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    aligned_release(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    aligned_release(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    aligned_release(pointer);
}

namespace {

constexpr auto scan_interval = std::chrono::milliseconds(10);
// A tunnel's reused read window takes a little over 4 KiB: one fits the
// connection budget with room for a small chunk, two fit the worker budget,
// three do not. A streamed body's 8 KiB read buffer fits neither.
constexpr std::size_t connection_budget = 6000;
constexpr std::size_t worker_budget = 10000;

[[nodiscard]] ruvia::server_config inbound_memory_config() {
    return {
        .worker_count_ = 1,
        .connection_scan_interval_ = scan_interval,
        .max_inbound_buffer_bytes_per_worker_ = worker_budget,
        .max_inbound_buffer_bytes_per_connection_ = connection_budget,
    };
}

class inbound_memory_routes final {
    RUVIA_ROUTES_BEGIN(inbound_memory_routes)
    RUVIA_POST("/inbound-stream", stream_body, ruvia::stream_body);
    RUVIA_GET("/inbound-trim", trim);
    RUVIA_CONNECT("inbound-tunnel.test:443", tunnel);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> stream_body(ruvia::context& context_value) {
        auto& reader = context_value.req().body_reader();
        std::size_t received = 0;
        while (const auto chunk = co_await reader.read()) {
            received += chunk->size();
        }
        co_return context_value.text(std::pmr::string(std::to_string(received), context_value.pool()));
    }

    // Caches one large freed block in the worker's memory, then keeps the
    // exchange running for many scan intervals before reporting whether the
    // block was trimmed meanwhile.
    ruvia::task<ruvia::http_response> trim(ruvia::context& context_value) {
        auto* const pool = context_value.pool();
        capturing_block = true;
        auto* const block = pool->allocate(watched_block_bytes);
        capturing_block = false;
        pool->deallocate(block, watched_block_bytes);
        if (watched_block.load() == nullptr) {
            co_return context_value.text("uncached\n");
        }
        co_await ruvia::sleep_for(context_value.worker(), 30 * scan_interval);
        co_return context_value.text(std::string_view(watched_block_freed ? "freed\n" : "kept\n"));
    }

    // Keeps every chunk until the peer ends, acknowledging each, and reports
    // whether a read ran out of inbound budget.
    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& tunnel_value = context_value.tunnel();
        std::vector<std::pmr::string> retained;
        bool limited = false;
        for (;;) {
            std::optional<std::pmr::string> chunk;
            try {
                chunk = co_await tunnel_value.read();
            } catch (const std::bad_alloc&) {
                limited = true;
            }
            if (!chunk) {
                break;
            }
            retained.push_back(std::move(*chunk));
            co_await tunnel_value.write("ok\n");
        }
        retained.clear();
        co_await tunnel_value.write(limited ? "limited\n" : "end\n");
    }
};

class test_client final {
public:
    test_client(asio::io_context& io, const asio::ip::tcp::endpoint& server_endpoint)
        : io_(io),
          socket_(io) {
        socket_.connect(server_endpoint);
    }

    void send(std::string_view bytes) {
        asio::write(socket_, asio::buffer(bytes));
    }

    void finish_sending() {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
    }

    // Everything up to and including `delimiter`, or nullopt when the stream
    // ends or 5 s pass first.
    [[nodiscard]] std::optional<std::string> read_until(std::string_view delimiter) {
        std::optional<std::size_t> found;
        asio::async_read_until(socket_, asio::dynamic_buffer(input_), delimiter,
            [&found](const std::error_code& error, std::size_t bytes) {
                if (!error) {
                    found = bytes;
                }
            });
        run();
        if (!found) {
            return std::nullopt;
        }
        std::string result = input_.substr(0, *found);
        input_.erase(0, *found);
        return result;
    }

    // Everything until the server ends the stream (or 5 s pass).
    [[nodiscard]] std::string read_to_end() {
        asio::async_read(socket_, asio::dynamic_buffer(input_), [](const std::error_code&, std::size_t) {});
        run();
        return std::exchange(input_, {});
    }

    void close() {
        std::error_code ignored;
        socket_.close(ignored);
    }

private:
    void run() {
        io_.restart();
        io_.run_for(std::chrono::seconds(5));
        if (!io_.stopped()) {
            std::error_code ignored;
            socket_.cancel(ignored);
            io_.restart();
            io_.run();
        }
    }

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    std::string input_;
};

constexpr std::string_view connect_request =
    "CONNECT inbound-tunnel.test:443 HTTP/1.1\r\nHost: inbound-tunnel.test:443\r\n\r\n";

[[nodiscard]] bool open_tunnel(test_client& client) {
    client.send(connect_request);
    const auto head = client.read_until("\r\n\r\n");
    return head && head->starts_with("HTTP/1.1 200 ");
}

}  // namespace

RUVIA_TEST(streamed_request_body_reads_count_against_the_connection_inbound_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    {
        // A body that arrives with its head needs no reader buffer.
        test_client client(io, server.endpoint());
        client.send(
            "POST /inbound-stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16\r\nConnection: close\r\n\r\n"
            "0123456789abcdef");
        const auto response = client.read_to_end();
        RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
        RUVIA_CHECK(response.ends_with("\r\n\r\n16"));
        client.close();
    }
    {
        // The reader's 8 KiB socket buffer exceeds the connection budget.
        test_client client(io, server.endpoint());
        client.send(
            "POST /inbound-stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20000\r\nConnection: close\r\n\r\n" +
            std::string(20000, 'b'));
        const auto response = client.read_to_end();
        RUVIA_CHECK(response.starts_with("HTTP/1.1 503 "));
        RUVIA_CHECK(response.find("inbound_buffer_limit") != std::string::npos);
        client.close();
    }
    server.finish();
}

RUVIA_TEST(tunnel_reads_count_against_the_connection_inbound_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    test_client client(io, server.endpoint());
    RUVIA_CHECK(open_tunnel(client));
    client.send(std::string(20000, 't'));
    client.finish_sending();
    const auto output = client.read_to_end();
    RUVIA_CHECK(output.ends_with("limited\n"));
    client.close();
    server.finish();
}

RUVIA_TEST(tunnel_reads_count_against_the_worker_inbound_budget) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    // Two tunnels each hold their read window, within their own connection
    // budgets; a third tunnel's first read exceeds the worker's.
    test_client first(io, server.endpoint());
    test_client second(io, server.endpoint());
    for (auto* const client : {&first, &second}) {
        RUVIA_CHECK(open_tunnel(*client));
        client->send("a");
        RUVIA_CHECK(client->read_until("\n") == std::optional<std::string>("ok\n"));
    }
    test_client third(io, server.endpoint());
    RUVIA_CHECK(open_tunnel(third));
    RUVIA_CHECK(third.read_to_end() == "limited\n");
    third.close();
    for (auto* const client : {&first, &second}) {
        client->finish_sending();
        RUVIA_CHECK(client->read_to_end() == "end\n");
        client->close();
    }
    server.finish();
}

RUVIA_TEST(worker_memory_is_trimmed_only_after_an_idle_scan_interval) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    test_client client(io, server.endpoint());
    client.send("GET /inbound-trim HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto head = client.read_until("\r\n\r\n");
    RUVIA_CHECK(head && head->starts_with("HTTP/1.1 200 "));
    // Thirty scan intervals passed while the exchange ran: none trimmed.
    RUVIA_CHECK(client.read_until("\n") == std::optional<std::string>("kept\n"));
    // The connection stays open but idle; a later scan trims the block.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!watched_block_freed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(scan_interval);
    }
    RUVIA_CHECK(watched_block_freed);
    client.close();
    server.finish();
}
