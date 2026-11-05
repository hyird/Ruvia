#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "test_harness.h"

// A file body that fails before its head is sent is answered with an error
// response, and only real faults reach the connection failure channel. A read
// the response waits on is bounded by write_timeout_: a pool that cannot run
// it (here: its single thread held by another request) does not pin the
// connection. This executable owns its application because the write timeout,
// the one-thread blocking pool and the failure hook are application-wide.

namespace file_response_failure_test {

using namespace std::chrono_literals;

constexpr auto write_timeout = 300ms;

[[nodiscard]] const std::filesystem::path& failure_dir() {
    static const std::filesystem::path dir = std::filesystem::current_path() / "ruvia_file_response_failure";
    return dir;
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::atomic<bool> pool_held{false};
std::atomic<bool> release_pool{false};
std::atomic<std::size_t> reported_failures{0};
std::atomic<std::size_t> reported_timeouts{0};

class file_failure_routes final {
    RUVIA_ROUTES_BEGIN(file_failure_routes)
    RUVIA_GET("/file-failure/hold-pool", hold_pool);
    RUVIA_GET("/file-failure/file", file);
    RUVIA_GET("/file-failure/changed", changed);
    RUVIA_ROUTES_END

    // Occupies the pool's only thread until the test releases it (bounded).
    ruvia::task<ruvia::http_response> hold_pool(ruvia::context& c) {
        auto held = co_await c.run_blocking([] {
            pool_held.store(true);
            for (int wait = 0; wait < 10000 && !release_pool.load(); ++wait) {
                std::this_thread::sleep_for(1ms);
            }
            return 0;
        });
        static_cast<void>(std::move(held).value());
        co_return c.text("released");
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& c) {
        const auto path = failure_dir() / "body.bin";
        co_return c.file({.path_ = path});
    }

    // Framed from the file's snapshot, then truncated before the transport
    // reads it.
    ruvia::task<ruvia::http_response> changed(ruvia::context& c) {
        const auto path = failure_dir() / "changed.bin";
        write_file(path, std::string(4096, 'c'));
        auto response = c.file({.path_ = path});
        write_file(path, "x");
        co_return response;
    }
};

void record_failure(const ruvia::connection_failure_record& record) noexcept {
    reported_failures.fetch_add(1);
    try {
        std::rethrow_exception(record.exception());
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::timed_out) {
            reported_timeouts.fetch_add(1);
        }
    } catch (...) {
    }
}

std::string get(std::string_view target) {
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    return request;
}

// Reads until the server closes the connection; nullopt when that takes
// longer than `limit` (the socket is then shut down to end the read).
[[nodiscard]] std::optional<std::string> read_to_close(asio::ip::tcp::socket& socket, std::chrono::milliseconds limit) {
    auto reader = std::async(std::launch::async, [&socket] {
        std::string wire;
        std::array<char, 4096> buffer{};
        std::error_code ec;
        while (!ec) {
            const auto count = socket.read_some(asio::buffer(buffer), ec);
            wire.append(buffer.data(), count);
        }
        return wire;
    });
    if (reader.wait_for(limit) != std::future_status::ready) {
        std::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        static_cast<void>(reader.get());
        return std::nullopt;
    }
    return reader.get();
}

template <typename predicate_type>
[[nodiscard]] bool wait_for(predicate_type predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

}  // namespace file_response_failure_test

using namespace file_response_failure_test;

RUVIA_TEST(file_failures_before_the_head_get_an_error_response_and_stalled_reads_time_out) {
    std::error_code ignored;
    std::filesystem::remove_all(failure_dir(), ignored);
    std::filesystem::create_directories(failure_dir());
    write_file(failure_dir() / "body.bin", std::string(4096, 'b'));

    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .write_timeout_ = write_timeout})
        .blocking_pool({.thread_count_ = 1, .queue_capacity_ = 4})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port()})
        .on_connection_failure(&record_failure)
        .on_start([&started] { started.release(); });
    std::exception_ptr failure;
    std::thread runner([&app, &failure] {
        try {
            app.run();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    const bool ready = started.try_acquire_for(10s);
    RUVIA_CHECK(ready);
    if (ready) {
        // A file changed after framing is an expected deployment race: the
        // client gets a 500 that closes the connection, and nothing is
        // reported.
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get("/file-failure/changed")));
            const auto wire = read_to_close(socket, 5s);
            RUVIA_CHECK(wire.has_value());
            if (wire.has_value()) {
                RUVIA_CHECK(wire->starts_with("HTTP/1.1 500 "));
                RUVIA_CHECK(wire->find(std::string(64, 'c')) == std::string::npos);
            }
            RUVIA_CHECK_EQ(reported_failures.load(), std::size_t{0});
        }

        // The pool's only thread is held, so the file read queues behind it.
        // The connection still ends within write_timeout_ (with a 503 unless
        // the write-stall scan closed the socket first), and the stalled read
        // is reported as a fault.
        asio::ip::tcp::socket holder(io);
        holder.connect(endpoint);
        asio::write(holder, asio::buffer(get("/file-failure/hold-pool")));
        RUVIA_CHECK(wait_for([] { return pool_held.load(); }));
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get("/file-failure/file")));
            const auto wire = read_to_close(socket, 5s);
            RUVIA_CHECK(wire.has_value());
            if (wire.has_value()) {
                RUVIA_CHECK(wire->empty() || wire->starts_with("HTTP/1.1 503 "));
            }
            RUVIA_CHECK(wait_for([] { return reported_timeouts.load() != 0; }));
        }
        release_pool.store(true);
        const auto released = read_to_close(holder, 5s);
        RUVIA_CHECK(released.has_value() && released->starts_with("HTTP/1.1 200 "));

        // With the pool free again the same file is served.
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get("/file-failure/file")));
            const auto wire = read_to_close(socket, 5s);
            RUVIA_CHECK(wire.has_value() && wire->starts_with("HTTP/1.1 200 ") &&
                        wire->ends_with(std::string(4096, 'b')));
        }
        RUVIA_CHECK(wait_for([&app] { return app.http_stats().active_connections_ == 0; }));
    }
    release_pool.store(true);
    app.stop();
    runner.join();
    RUVIA_CHECK(failure == nullptr);
    std::filesystem::remove_all(failure_dir(), ignored);
}
