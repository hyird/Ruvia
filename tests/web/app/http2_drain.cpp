#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <mutex>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <asio/write.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "test_harness.h"

namespace http2_drain_test {
using namespace std::chrono_literals;
inline constexpr std::size_t response_bytes = 8 * 1024 * 1024;

struct observation final {
    std::binary_semaphore started_{0};
    std::atomic<bool> entered_{};
    std::atomic<bool> release_{};
    std::atomic<bool> cancelled_{};
    std::atomic<bool> forbidden_ran_{};
};
observation& observed() {
    static observation value;
    return value;
}

class drain_routes final {
public:
    RUVIA_ROUTES_BEGIN(drain_routes)
    RUVIA_GET("/held", held);
    RUVIA_GET("/forbidden", forbidden);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> held(ruvia::context& context) {
        auto& state = observed();
        state.entered_ = true;
        const auto stop = context.get_stop_token();
        while (!state.release_ && !stop.stop_requested()) {
            (void)co_await ruvia::sleep_for(context.worker(), 1ms, stop);
        }
        state.cancelled_ = stop.stop_requested();
        if (state.cancelled_) {
            co_return context.text("cancelled");
        }
        std::pmr::string body(response_bytes, 'd', context.arena());
        co_return context.text(std::move(body));
    }
    ruvia::task<ruvia::http_response> forbidden(ruvia::context& context) {
        observed().forbidden_ran_ = true;
        co_return context.text("must-not-run");
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
            observed().release_ = true;
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

template <typename predicate_type>
void await_observation(predicate_type predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("HTTP/2 drain observation timed out");
        }
        std::this_thread::sleep_for(1ms);
    }
}

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream)) {
        throw std::logic_error("invalid HTTP/2 test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}
std::string request(std::uint32_t stream, std::string_view path) {
    std::pmr::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", path);
    std::string wire;
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, stream, block);
    return wire;
}

struct frame final {
    ruvia::http2_frame_header header_;
    std::string payload_;
};
// Nonblocking reads bound every wire assertion, including a missing GOAWAY.
class peer final {
public:
    explicit peer(asio::ip::tcp::socket& socket)
        : socket_(socket) {
        socket_.non_blocking(true);
    }
    void read_exact(char* bytes, std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (count != 0) {
            asio::error_code error;
            const auto received = socket_.read_some(asio::buffer(bytes, count), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("HTTP/2 drain frame timed out");
                }
                std::this_thread::sleep_for(1ms);
                continue;
            }
            if (error) {
                throw std::system_error(error);
            }
            bytes += received;
            count -= received;
        }
    }
    frame next() {
        std::array<char, ruvia::http2_frame_header_bytes> bytes{};
        read_exact(bytes.data(), bytes.size());
        const auto header = ruvia::parse_http2_frame_header(bytes);
        if (!header || header->length_ > 64 * 1024) {
            throw std::runtime_error("invalid HTTP/2 drain response frame");
        }
        frame value{*header, std::string(header->length_, '\0')};
        read_exact(value.payload_.data(), value.payload_.size());
        return value;
    }
    bool ended() {
        asio::error_code error;
        char byte{};
        const auto count = socket_.read_some(asio::buffer(&byte, 1), error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            return false;
        }
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            return true;
        }
        if (error) {
            throw std::system_error(error);
        }
        (void)count;
        return false;
    }

private:
    asio::ip::tcp::socket& socket_;
};
std::uint32_t uint32(std::string_view bytes) {
    if (bytes.size() < 4) {
        throw std::runtime_error("truncated HTTP/2 drain frame");
    }
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[i]);
    }
    return value;
}

void check_drain(ruvia::testing::test_context& ruvia_ctx, bool deadline_expires) {
    auto& state = observed();
    state.entered_ = false;
    state.release_ = false;
    state.cancelled_ = false;
    state.forbidden_ran_ = false;
    auto& app = ruvia::app();
    static std::once_flag hooks;
    std::call_once(hooks, [&app] { app.on_start([] { observed().started_.release(); }); });
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = reservation.local_endpoint();
    reservation.close();
    app.server({.worker_count_ = 1, .drain_timeout_ = deadline_expires ? 200ms : 10s, .connection_scan_interval_ = 5ms});
    app.listen({.address_ = "127.0.0.1", .http_ = endpoint.port()});
    app_run run(app);
    if (!state.started_.try_acquire_for(3s)) {
        throw std::runtime_error("HTTP/2 drain app did not start");
    }
    asio::ip::tcp::socket socket(io);
    socket.open(endpoint.protocol());
    socket.set_option(asio::socket_base::receive_buffer_size(64 * 1024));
    socket.connect(endpoint);
    std::string opening(ruvia::http2_client_preface);
    // Permit the full response at the HTTP/2 layer so the deliberately slow
    // receiver backpressures the socket writer, not the stream send window.
    append_frame(opening, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\x00\x04\x01\x00\x00\x00", 6));
    append_frame(opening, ruvia::http2_frame_type::window_update, 0, 0, std::string_view("\x00\xff\x00\x01", 4));
    opening.append(request(1, "/held"));
    asio::write(socket, asio::buffer(opening));
    await_observation([&state] { return state.entered_.load(); });
    app.stop();
    peer client(socket);
    frame goaway;
    do {
        goaway = client.next();
    } while (goaway.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway));
    RUVIA_CHECK_EQ(uint32(goaway.payload_) & 0x7fffffffU, 1U);
    RUVIA_CHECK_EQ(uint32(std::string_view(goaway.payload_).substr(4)), 0U);
    RUVIA_CHECK(!state.cancelled_);
    if (!deadline_expires) {
        const auto forbidden_request = request(3, "/forbidden");
        asio::write(socket, asio::buffer(forbidden_request));
        frame refused;
        do {
            refused = client.next();
        } while (refused.header_.type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) || refused.header_.stream_id_ != 3);
        RUVIA_CHECK_EQ(uint32(refused.payload_), 7U);
        RUVIA_CHECK(!state.forbidden_ran_);
        RUVIA_CHECK(!state.cancelled_);
        state.release_ = true;
        // Give the response producer a head start without consuming its body:
        // the response exceeds normal TCP send/receive buffering.
        std::this_thread::sleep_for(100ms);
        std::string body;
        bool complete = false;
        while (!complete) {
            const auto received = client.next();
            if (received.header_.stream_id_ == 1) {
                if (received.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    body.append(received.payload_);
                }
                complete = (received.header_.flags_ & 0x1) != 0;
            }
        }
        RUVIA_CHECK_EQ(body.size(), response_bytes);
        RUVIA_CHECK(std::all_of(body.begin(), body.end(), [](char byte) { return byte == 'd'; }));
        RUVIA_CHECK(!state.cancelled_);
    }
    await_observation([&client] { return client.ended(); });
    run.join();
    RUVIA_CHECK_EQ(state.cancelled_.load(), deadline_expires);
    RUVIA_CHECK(!state.forbidden_ran_);
}
}  // namespace http2_drain_test

RUVIA_TEST(http2_app_stop_sends_goaway_refuses_new_streams_and_completes_admitted_request) {
    http2_drain_test::check_drain(ruvia_ctx, false);
}
RUVIA_TEST(http2_app_stop_cancels_admitted_request_at_drain_deadline) {
    http2_drain_test::check_drain(ruvia_ctx, true);
}
