#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace http2_file_flow_control_test {

constexpr std::size_t file_size = 65535;

std::filesystem::path& file_path() {
    static std::filesystem::path path;
    return path;
}

class file_routes final {
    RUVIA_ROUTES_BEGIN(file_routes)
    RUVIA_GET("/http2-file-flow-control", file);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        auto response = context_value.file({.path_ = file_path()});
        // Make the exercised transport path observable, rather than assuming
        // that the file is above the context::file in-memory threshold.
        response.header("x-file-backed", response.file_body().has_value() ? "yes" : "no");
        co_return response;
    }
};

struct test_file final {
    test_file() {
        std::random_device random;
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            directory_ = std::filesystem::temp_directory_path() /
                         ("ruvia-http2-file-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(directory_)) {
                break;
            }
            directory_.clear();
        }
        if (directory_.empty()) {
            throw std::runtime_error("cannot create HTTP/2 flow-control test directory");
        }
        try {
            file_path() = directory_ / "body.bin";
            std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
            const std::string bytes(file_size, 'f');
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) {
                throw std::runtime_error("cannot write HTTP/2 flow-control test file");
            }
        } catch (...) {
            std::filesystem::remove_all(directory_);
            throw;
        }
    }

    ~test_file() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

private:
    std::filesystem::path directory_;
};

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header{};
    if (!ruvia::encode_http2_frame_header(
            header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid HTTP/2 flow-control test frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

std::string request_wire(bool exhaust_connection, std::pmr::memory_resource* resource) {
    std::string wire(ruvia::http2_client_preface);
    // In each case exactly one window reaches zero after the file. Keep the
    // stream window positive when testing the connection window, and vice versa.
    const std::string_view settings = exhaust_connection
                                          ? std::string_view("\0\4\0\1\xff\xfe", 6)   // INITIAL_WINDOW_SIZE = 131070
                                          : std::string_view("\0\4\0\0\xff\xff", 6);  // INITIAL_WINDOW_SIZE = 65535
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
    if (!exhaust_connection) {
        append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
            std::string_view("\0\0\xff\xff", 4));
    }
    std::pmr::string block(resource);
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

struct received_frame final {
    ruvia::http2_frame_header header_;
    std::string payload_;
};

ruvia::task<received_frame> read_frame(asio::ip::tcp::socket& socket) {
    std::array<char, ruvia::http2_frame_header_bytes> header_bytes{};
    const auto head = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_read(socket, asio::buffer(header_bytes), std::move(handler));
    });
    if (head.error_code()) {
        throw std::system_error(head.error_code());
    }
    const auto header = ruvia::parse_http2_frame_header(header_bytes);
    if (!header) {
        throw std::logic_error("invalid HTTP/2 flow-control response frame");
    }
    received_frame frame{*header, std::string(header->length_, '\0')};
    if (!frame.payload_.empty()) {
        const auto body = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read(socket, asio::buffer(frame.payload_), std::move(handler));
        });
        if (body.error_code()) {
            throw std::system_error(body.error_code());
        }
    }
    co_return frame;
}

void check_file_end(ruvia::testing::test_context& ruvia_ctx, bool exhaust_connection) {
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool file_backed = false;
    std::string status;
    std::string body;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        // Bound missing END_STREAM without a sleeping thread or a race between
        // reading the final file bytes and deciding whether to return credit.
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                expired = true;
                ruvia::close_socket(socket);
            }
        });
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) {
                socket.async_connect(server.endpoint(), std::move(handler));
            });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            std::pmr::unsynchronized_pool_resource resource;
            const auto wire = request_wire(exhaust_connection, &resource);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder({.resource_ = &resource});
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack;
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    const auto acknowledged = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(ack), std::move(handler));
                    });
                    if (acknowledged.error_code()) {
                        throw std::system_error(acknowledged.error_code());
                    }
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 file response aborted");
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        } else if (name == "x-file-backed") {
                            file_backed = value == "yes";
                        }
                        return true;
                    });
                    if (decoded.has_value()) {
                        throw std::runtime_error("invalid HTTP/2 file response headers");
                    }
                    RUVIA_CHECK((frame.header_.flags_ & 0x1) == 0);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    RUVIA_CHECK((frame.header_.flags_ & 0x8) == 0);
                    body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                    if (ended) {
                        RUVIA_CHECK(frame.payload_.empty());
                    }
                }
                // Deliberately never send WINDOW_UPDATE: a zero-length final
                // DATA frame needs no credit (RFC 9113 section 6.9.1).
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    RUVIA_CHECK_EQ(status, std::string("200"));
    RUVIA_CHECK(file_backed);
    RUVIA_CHECK_EQ(body, std::string(file_size, 'f'));
    RUVIA_CHECK(ended);
    RUVIA_CHECK(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_file_flow_control_test

RUVIA_TEST(http2_file_response_ends_with_exhausted_connection_window) {
    http2_file_flow_control_test::check_file_end(ruvia_ctx, true);
}

RUVIA_TEST(http2_file_response_ends_with_exhausted_stream_window) {
    http2_file_flow_control_test::check_file_end(ruvia_ctx, false);
}
