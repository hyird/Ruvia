#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/server_config.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"

namespace http2_file_flow_control_test {

constexpr std::size_t file_size = 65535;

std::filesystem::path& file_path() {
    static std::filesystem::path path;
    return path;
}

[[nodiscard]] const std::filesystem::path& empty_directory() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-http2-empty-file");
    return directory.path();
}

std::unique_ptr<ruvia::static_root> empty_root;

class file_routes final {
    RUVIA_ROUTES_BEGIN(file_routes)
    RUVIA_GET("/http2-file-flow-control", file);
    RUVIA_GET("/http2-file-mutated", mutated_file);
    RUVIA_GET("/http2-file-ok", ok_text);
    RUVIA_GET("/http2-indexed-empty", indexed_empty);
    RUVIA_GET("/http2-empty-mutated", mutated_empty);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        auto response = co_await context_value.file({.path_ = file_path()});
        // Make the exercised transport path observable, rather than assuming
        // that the file is above the context::file in-memory threshold.
        response.header("x-file-backed", response.file_body().has_value() ? "yes" : "no");
        co_return response;
    }

    ruvia::task<ruvia::http_response> mutated_file(ruvia::context& context_value) {
        {
            std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
            const std::string bytes(file_size, 'f');
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        // The client truncates the file once HEADERS arrive: the file was
        // validated before them, and the multipart boundary DATA is submitted
        // before the file range read fails and resets the stream.
        co_return co_await context_value.file({.path_ = file_path()});
    }

    ruvia::task<ruvia::http_response> mutated_empty(ruvia::context& context_value) {
        const auto path = empty_directory() / "body.bin";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
        }
        auto response = co_await context_value.file({.path_ = path});
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "changed";
        }
        co_return response;
    }

    ruvia::task<ruvia::http_response> indexed_empty(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*empty_root, {.relative_path_ = "body.bin"});
    }

    ruvia::task<ruvia::http_response> ok_text(ruvia::context& context_value) {
        co_return context_value.text("alive");
    }
};

struct test_file final {
    test_file() {
        file_path() = directory_.path() / "body.bin";
        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
        const std::string bytes(file_size, 'f');
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write HTTP/2 flow-control test file");
        }
    }

private:
    ruvia::test::scoped_temp_directory directory_{"ruvia-http2-file"};
};

using ruvia::test::append_frame;

std::string request_wire(bool exhaust_connection) {
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
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
    append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return wire;
}

using ruvia::test::received_frame;

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

void check_file_end(bool exhaust_connection) {
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool file_backed = false;
    std::string status{};
    std::string body{};
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
            const auto wire = request_wire(exhaust_connection);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            bool credited = false;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
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
                    EXPECT_TRUE((frame.header_.flags_ & 0x1) == 0);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    EXPECT_TRUE((frame.header_.flags_ & 0x8) == 0);
                    body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                    if (ended) {
                        EXPECT_TRUE(frame.payload_.empty());
                        EXPECT_TRUE(credited);
                    }
                }
                // The file exhausted one send window. The empty END_STREAM DATA
                // frame waits for that window to open (RFC 9113 section 6.9.1
                // permits sending it at zero window; nghttp2 does not), so one
                // byte of credit releases it.
                if (!ended && !credited && body.size() == file_size) {
                    credited = true;
                    std::string increment{};
                    append_frame(increment, ruvia::http2_frame_type::window_update, 0, exhaust_connection ? 0 : 1,
                        std::string_view("\0\0\0\1", 4));
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(increment), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
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
    std::move(root).get();
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(file_backed);
    EXPECT_EQ(body, std::string(file_size, 'f'));
    EXPECT_TRUE(ended);
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_file_flow_control_test

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_connection_window) {
    http2_file_flow_control_test::check_file_end(true);
}

TEST(Ruvia, http2_file_response_end_waits_for_exhausted_stream_window) {
    http2_file_flow_control_test::check_file_end(false);
}

TEST(Ruvia, http2_buffered_response_resumes_after_stream_window_update) {
    using namespace http2_file_flow_control_test;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool ended = false;
    bool credited = false;
    std::string status{};
    std::string body{};
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
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
            std::string wire(ruvia::http2_client_preface);
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0,
                std::string_view("\0\4\0\0\0\1", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(wire), std::move(handler));
            });
            if (written.error_code()) {
                throw std::system_error(written.error_code());
            }
            ruvia::hpack_decoder decoder;
            while (!ended) {
                const auto frame = co_await read_frame(socket);
                std::string reply;
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    append_frame(reply, ruvia::http2_frame_type::settings, 0x1, 0, {});
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::goaway) ||
                    frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    throw std::runtime_error("HTTP/2 buffered response aborted");
                }
                if (frame.header_.stream_id_ == 1) {
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        const auto decoded = decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                            if (name == ":status") {
                                status = value;
                            }
                            return true;
                        });
                        if (decoded.has_value()) {
                            throw std::runtime_error("invalid HTTP/2 buffered response headers");
                        }
                        ended = (frame.header_.flags_ & 0x1) != 0;
                    } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        body.append(frame.payload_);
                        ended = (frame.header_.flags_ & 0x1) != 0;
                        if (!credited && !body.empty()) {
                            EXPECT_EQ(body, std::string_view("a"));
                            EXPECT_FALSE(ended);
                            credited = true;
                            append_frame(reply, ruvia::http2_frame_type::window_update, 0, 1,
                                std::string_view("\0\0\0\4", 4));
                        }
                    }
                }
                if (!reply.empty()) {
                    const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        asio::async_write(socket, asio::buffer(reply), std::move(handler));
                    });
                    if (sent.error_code()) {
                        throw std::system_error(sent.error_code());
                    }
                }
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
    std::move(root).get();
    EXPECT_EQ(status, std::string_view("200"));
    EXPECT_EQ(body, std::string_view("alive"));
    EXPECT_TRUE(credited);
    EXPECT_TRUE(ended);
    EXPECT_FALSE(expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// A file body is validated before HEADERS but read only once the peer grants
// send window: bytes the file held before the window opened are never sent.
// Truncating the file between HEADERS and the first WINDOW_UPDATE therefore
// fails the read and resets the stream without a single DATA byte.
TEST(Ruvia, http2_file_body_is_read_only_after_send_window_is_granted) {
    using namespace http2_file_flow_control_test;
    const test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    bool reset = false;
    std::string status{};
    std::size_t data_bytes = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
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
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            std::string wire(ruvia::http2_client_preface);
            // INITIAL_WINDOW_SIZE = 0: the stream has no send window at all.
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\0", 6));
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-flow-control");
            append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            while (!reset) {
                const auto frame = co_await read_frame(socket);
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                    (frame.header_.flags_ & 0x1) == 0) {
                    std::string ack{};
                    append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                    co_await send_bytes(ack);
                }
                if (frame.header_.stream_id_ != 1) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            status = value;
                        }
                        return true;
                    });
                    {
                        std::ofstream output(file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    }
                    std::string update{};
                    append_frame(update, ruvia::http2_frame_type::window_update, 0, 1, std::string_view("\0\0\xff\xff", 4));
                    co_await send_bytes(update);
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    data_bytes += frame.payload_.size();
                    if ((frame.header_.flags_ & 0x1) != 0) {
                        break;
                    }
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                    reset = true;
                }
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
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(status, std::string("200"));
    EXPECT_TRUE(reset);
    EXPECT_EQ(data_bytes, std::size_t{0});
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_failed_file_writes_release_output_budget_slots) {
    const http2_file_flow_control_test::test_file file;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    std::string final_status{};
    std::string final_body{};
    std::size_t resets_seen = 0;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog(io, std::chrono::seconds(15));
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
            std::string wire(ruvia::http2_client_preface);
            // A 10-byte initial stream window splits the first multipart
            // boundary chunk (~80 bytes) across window updates, so DATA output
            // is in flight when the file range read fails and resets the stream.
            http2_file_flow_control_test::append_frame(
                wire, ruvia::http2_frame_type::settings, 0, 0, std::string_view("\0\4\0\0\0\x0a", 6));
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            co_await send_bytes(wire);
            ruvia::hpack_decoder decoder;
            // Five failed file responses exceed the 4 connection-wide DATA
            // output budget slots if any reset leaks its slot.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-mutated");
                ruvia::hpack_encoder::encode_header(block, "range", "bytes=0-3, 10-13");
                std::string req{};
                http2_file_flow_control_test::append_frame(
                    req, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(req);
                bool stream_done = false;
                while (!stream_done) {
                    const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        http2_file_flow_control_test::append_frame(
                            ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                        (void)decoder.decode(frame.payload_, [](std::string_view, std::string_view) { return true; });
                        std::ofstream output(http2_file_flow_control_test::file_path(), std::ios::binary | std::ios::trunc);
                        output << "x";
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                        std::string update{};
                        http2_file_flow_control_test::append_frame(
                            update, ruvia::http2_frame_type::window_update, 0, stream_id,
                            std::string_view("\0\0\x10\0", 4));
                        co_await send_bytes(update);
                    } else if (frame.header_.stream_id_ == stream_id &&
                               frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                        ++resets_seen;
                        stream_done = true;
                    }
                }
            }
            std::string block;
            ruvia::hpack_encoder::encode_header(block, ":method", "GET");
            ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
            ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
            ruvia::hpack_encoder::encode_header(block, ":path", "/http2-file-ok");
            std::string req{};
            http2_file_flow_control_test::append_frame(req, ruvia::http2_frame_type::headers, 0x5, 11, block);
            co_await send_bytes(req);
            bool ended = false;
            while (!ended) {
                const auto frame = co_await http2_file_flow_control_test::read_frame(socket);
                if (frame.header_.stream_id_ != 11) {
                    continue;
                }
                if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                    (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                        if (name == ":status") {
                            final_status = value;
                        }
                        return true;
                    });
                } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                    final_body.append(frame.payload_);
                    ended = (frame.header_.flags_ & 0x1) != 0;
                }
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
    std::move(root).get();
    EXPECT_TRUE(!expired);
    EXPECT_EQ(resets_seen, std::size_t{5});
    EXPECT_EQ(final_status, std::string("200"));
    EXPECT_EQ(final_body, std::string("alive"));
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http2_empty_file_validation_precedes_end_stream_headers) {
    using namespace http2_file_flow_control_test;
    const auto& directory = empty_directory();
    const auto path = directory / "body.bin";
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    std::exception_ptr failure;
    bool expired = false;
    auto run = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
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
            const auto send_bytes = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(socket, asio::buffer(bytes), std::move(handler));
                });
                if (written.error_code()) {
                    throw std::system_error(written.error_code());
                }
            };
            ruvia::hpack_decoder decoder;
            std::string preface(ruvia::http2_client_preface);
            append_frame(preface, ruvia::http2_frame_type::settings, 0, 0, {});
            co_await send_bytes(preface);
            // Valid empty, changed, removed, changed after framing, then a
            // healthy stream proves failed validation remains stream-local.
            for (std::uint32_t stream_id = 1; stream_id <= 9; stream_id += 2) {
                // Changed, removed, and changed after framing: each fails
                // validation before HEADERS and gets an error response.
                const bool invalid = stream_id == 3 || stream_id == 5 || stream_id == 7;
                if (stream_id <= 5) {
                    {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    }
                    empty_root = std::make_unique<ruvia::static_root>(directory,
                        ruvia::static_root_options{
                            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
                        });
                    if (stream_id == 3) {
                        std::ofstream output(path, std::ios::binary | std::ios::trunc);
                        output << "changed";
                    } else if (stream_id == 5) {
                        EXPECT_TRUE(std::filesystem::remove(path));
                    }
                }
                std::string block;
                ruvia::hpack_encoder::encode_header(block, ":method", "GET");
                ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
                ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
                ruvia::hpack_encoder::encode_header(block, ":path",
                    stream_id == 9 ? "/http2-file-ok" : stream_id == 7 ? "/http2-empty-mutated"
                                                                       : "/http2-indexed-empty");
                std::string request{};
                append_frame(request, ruvia::http2_frame_type::headers, 0x5, stream_id, block);
                co_await send_bytes(request);
                bool ended = false;
                bool reset = false;
                bool saw_head = false;
                std::string status{};
                std::string body{};
                while (!ended && !reset) {
                    const auto frame = co_await read_frame(socket);
                    if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::settings) &&
                        (frame.header_.flags_ & 0x1) == 0) {
                        std::string ack{};
                        append_frame(ack, ruvia::http2_frame_type::settings, 0x1, 0, {});
                        co_await send_bytes(ack);
                    } else if (frame.header_.stream_id_ == stream_id) {
                        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                            saw_head = true;
                            (void)decoder.decode(frame.payload_, [&](std::string_view name, std::string_view value) {
                                if (name == ":status") {
                                    status = value;
                                }
                                return true;
                            });
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data)) {
                            body.append(frame.payload_);
                            ended = (frame.header_.flags_ & 0x1) != 0;
                        } else if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream)) {
                            reset = true;
                        }
                    }
                }
                EXPECT_TRUE(saw_head);
                if (invalid) {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("500"));
                } else {
                    EXPECT_TRUE(ended);
                    EXPECT_TRUE(!reset);
                    EXPECT_EQ(status, std::string("200"));
                    EXPECT_EQ(body, stream_id == 9 ? std::string("alive") : std::string{});
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        (void)watchdog.cancel();
        server.finish();
        attachment.stop();
    };
    auto operation = attachment.loop().start(run());
    io.run();
    std::move(operation).get();
    empty_root.reset();
    EXPECT_TRUE(!expired);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace http2_client_upload_test {

enum class response_mode { ordinary,
    continued,
    rejected,
    reset,
    abandoned,
    abandoned_pending,
    buffered,
    streaming,
    disconnected,
    refused,
    refused_always,
    connection_lost,
    connection_lost_upload,
    goaway_upload };

constexpr std::size_t streaming_payload_bytes = 256 * 1024;

using ruvia::test::append_frame;

// A raw peer deliberately keeps the request half open after the final response.
// It advertises one stream and never supplies the optional server-side reset, so
// client retirement cannot be masked by a server request-lease destructor.
class early_final_peer final {
public:
    early_final_peer(asio::io_context& io, const ruvia::worker_ref& worker, response_mode mode)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          done_(worker),
          mode_(mode) {}
    early_final_peer(asio::io_context&, ruvia::worker_ref&&, response_mode) = delete;

    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            done_.notify();
        });
    }

    void stop() {
        stopping_ = true;
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
    }

    ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::string received_{};
    bool request_ended_{false};
    bool request_reset_{false};
    unsigned requests_{0};

private:
    asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 client preface");
        }
        if (mode_ == response_mode::connection_lost || mode_ == response_mode::connection_lost_upload ||
            mode_ == response_mode::goaway_upload) {
            std::string wire{};
            const std::array<char, 6> no_streams{0, 3, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {no_streams.data(), no_streams.size()});
            const std::array<char, 8> ping{};
            append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
            co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            for (;;) {
                std::array<char, ruvia::http2_frame_header_bytes> bytes;
                co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
                const auto header = ruvia::parse_http2_frame_header(bytes);
                if (!header) {
                    throw std::runtime_error("invalid client HTTP/2 frame");
                }
                std::string payload(header->length_, '\0');
                if (!payload.empty()) {
                    co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
                }
                const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
                if (type == ruvia::http2_frame_type::headers) {
                    throw std::runtime_error("request submitted despite zero stream limit");
                }
                if (type == ruvia::http2_frame_type::ping && (header->flags_ & 1U) != 0) {
                    break;
                }
            }
            if (mode_ == response_mode::goaway_upload) {
                wire.clear();
                const std::array<char, 8> goaway{};
                append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                std::array<char, 1024> remaining;
                std::error_code error;
                do {
                    co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
                } while (!error);
            }
            socket_.close();
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
            if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
                throw std::runtime_error("missing retried HTTP/2 client preface");
            }
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        if (mode_ == response_mode::disconnected) {
            const std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {zero_window.data(), zero_window.size()});
        }
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        ruvia::hpack_decoder decoder;
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid client HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            wire.clear();
            const auto type = static_cast<ruvia::http2_frame_type>(header->type_);
            if (type == ruvia::http2_frame_type::settings && (header->flags_ & 1U) == 0) {
                append_frame(wire, type, 1, 0, {});
            } else if (type == ruvia::http2_frame_type::headers) {
                std::string method{};
                std::string path{};
                const auto decoded = decoder.decode(payload, [&](std::string_view name, std::string_view value) {
                    if (name == ":method") {
                        method = value;
                    }
                    if (name == ":path") {
                        path = value;
                    }
                    return true;
                });
                if (decoded.has_value() || (header->flags_ & 4U) == 0) {
                    throw std::runtime_error("invalid client request fields");
                }
                ++requests_;
                if ((requests_ == 1 && mode_ == response_mode::refused) || mode_ == response_mode::refused_always) {
                    // RFC 9113 section 8.7: the request was not processed.
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected refused request");
                    }
                    first_stream = header->stream_id_;
                    request_ended_ = (header->flags_ & 1U) != 0;
                    const std::array<char, 4> refused_stream{0, 0, 0, 7};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream,
                        {refused_stream.data(), refused_stream.size()});
                    co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
                    continue;
                }
                if (requests_ == 1 && mode_ == response_mode::connection_lost) {
                    if (method != "GET" || path != "/next") {
                        throw std::runtime_error("unexpected retried request");
                    }
                    first_stream = header->stream_id_;
                } else if (requests_ == 1) {
                    if (method != "POST" || path != "/upload") {
                        throw std::runtime_error("unexpected initial request");
                    }
                    first_stream = header->stream_id_;
                    if (mode_ == response_mode::continued) {
                        std::string interim;
                        ruvia::hpack_encoder::encode_header(interim, ":status", "100");
                        append_frame(wire, type, 4, first_stream, interim);
                    }
                } else if (requests_ == 2) {
                    if (method != "GET" || path != "/next" || header->stream_id_ != first_stream + 2 ||
                        (!request_ended_ && !request_reset_ && mode_ != response_mode::reset)) {
                        throw std::runtime_error("previous request half was not retired on the reused connection");
                    }
                } else {
                    throw std::runtime_error("unexpected extra request");
                }
                const bool rejected = requests_ == 1 && mode_ == response_mode::rejected;
                const std::string_view body = requests_ == 2 || mode_ == response_mode::connection_lost ? "next" : rejected ? "no"
                                                                                                                            : "ok";
                std::string fields;
                const bool empty_final = requests_ == 1 &&
                                         (mode_ == response_mode::buffered || mode_ == response_mode::streaming);
                ruvia::hpack_encoder::encode_header(fields, ":status", empty_final ? "204" : rejected ? "413"
                                                                                                      : "200");
                if (!empty_final) {
                    ruvia::hpack_encoder::encode_header(fields, "content-length", std::to_string(body.size()));
                }
                append_frame(wire, type, empty_final ? 5 : 4, header->stream_id_, fields);
                if (!empty_final && (requests_ != 1 || mode_ != response_mode::abandoned_pending)) {
                    append_frame(wire, ruvia::http2_frame_type::data, 1, header->stream_id_, body);
                }
                if (requests_ == 1 && mode_ == response_mode::reset) {
                    const std::array<char, 4> no_error{};
                    append_frame(wire, ruvia::http2_frame_type::rst_stream, 0, first_stream, {no_error.data(), no_error.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::data) {
                received_.append(payload);
                request_ended_ = (header->flags_ & 1U) != 0;
                if (mode_ == response_mode::buffered && received_.size() == 65535 && !window_returned_) {
                    window_returned_ = true;
                    const std::string_view increment("\0\0\0\1", 4);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream, increment);
                } else if (mode_ == response_mode::streaming && !payload.empty()) {
                    const auto increment_value = static_cast<std::uint32_t>(payload.size());
                    const std::array<char, 4> increment{
                        static_cast<char>((increment_value >> 24U) & 0x7fU),
                        static_cast<char>((increment_value >> 16U) & 0xffU),
                        static_cast<char>((increment_value >> 8U) & 0xffU),
                        static_cast<char>(increment_value & 0xffU)};
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0,
                        {increment.data(), increment.size()});
                    append_frame(wire, ruvia::http2_frame_type::window_update, 0, first_stream,
                        {increment.data(), increment.size()});
                }
            } else if (header->stream_id_ == first_stream && type == ruvia::http2_frame_type::rst_stream) {
                request_reset_ = true;
            }
            if (!wire.empty()) {
                co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
            }
            if (requests_ == 2 || (requests_ == 1 && (mode_ == response_mode::disconnected ||
                                                         mode_ == response_mode::connection_lost ||
                                                         ((mode_ == response_mode::connection_lost_upload ||
                                                              mode_ == response_mode::goaway_upload) &&
                                                             request_ended_)))) {
                co_return;
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    ruvia::worker_signal done_;
    response_mode mode_;
    std::exception_ptr failure_;
    bool stopping_{false};
    bool finished_{false};
    bool window_returned_{false};
};

class draining_peer final {
public:
    draining_peer(asio::io_context& io, const ruvia::worker_ref& worker)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          start_drain_(io),
          ready_(worker),
          done_(worker) {
        start_drain_.expires_at(asio::steady_timer::time_point::max());
    }
    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            ready_.notify();
            done_.notify();
        });
    }
    void drain() {
        start_drain_.expires_at(asio::steady_timer::time_point::min());
    }
    void stop() {
        stopping_ = true;
        std::error_code ignored;
        start_drain_.cancel();
        socket_.close(ignored);
        acceptor_.close(ignored);
    }
    [[nodiscard]] ruvia::task<void> wait_ready() {
        while (!draining_ && !finished_) {
            co_await ready_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    [[nodiscard]] ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        if (std::string_view(preface.data(), preface.size()) != ruvia::http2_client_preface) {
            throw std::runtime_error("missing HTTP/2 preface");
        }
        std::string wire{};
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::uint32_t first_stream = 0;
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                first_stream = header->stream_id_;
                break;
            }
        }
        wire.clear();
        const std::array<char, 1> ok{static_cast<char>(0x88)};
        append_frame(wire, ruvia::http2_frame_type::headers, 4, first_stream, {ok.data(), ok.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::error_code error;
        co_await start_drain_.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (stopping_) {
            co_return;
        }
        wire.clear();
        const std::array<char, 8> goaway{0, 0, 0, static_cast<char>(first_stream), 0, 0, 0, 0};
        const std::array<char, 8> ping{};
        append_frame(wire, ruvia::http2_frame_type::goaway, 0, 0, {goaway.data(), goaway.size()});
        append_frame(wire, ruvia::http2_frame_type::ping, 0, 0, {ping.data(), ping.size()});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        for (;;) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::headers) {
                throw std::runtime_error("second request was submitted despite stream limit");
            }
            if (static_cast<ruvia::http2_frame_type>(header->type_) == ruvia::http2_frame_type::ping &&
                (header->flags_ & 1U) != 0) {
                draining_ = true;
                ready_.notify();
                break;
            }
        }
        std::array<char, 1024> remaining;
        do {
            co_await socket_.async_read_some(asio::buffer(remaining), asio::redirect_error(asio::use_awaitable, error));
        } while (!error);
    }
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer start_drain_;
    ruvia::worker_signal ready_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool draining_{false};
    bool stopping_{false};
    bool finished_{false};
};

void exercise_abandon_during_drain() {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    draining_peer peer(io, worker);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            auto held = co_await client.client().get("/hold").send();
            EXPECT_TRUE(held.status() == ruvia::http_status::ok);
            {
                auto exchange = co_await client.client().post("/abandon").upload({.content_length_ = 7});
                // The peer's single stream and max_open_ leave the upload
                // buffered for admission, its head never submitted.
                for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 1; ++i) {
                    (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
                }
                EXPECT_EQ(client.client().stats().buffered_requests_, 1U);
                EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
                peer.drain();
                co_await peer.wait_ready();
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(50));
            }
            for (unsigned i = 0; i != 100 && client.client().stats().buffered_requests_ != 0; ++i) {
                (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
            }
            EXPECT_EQ(client.client().stats().in_flight_requests_, 1U);
            EXPECT_EQ(client.client().stats().buffered_requests_, 0U);
            EXPECT_TRUE(!held.stream().complete());
        } catch (...) {
            failure = std::current_exception();
        }
        peer.stop();
        try {
            co_await client.shutdown();
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void exercise(response_mode mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    // The peer's signal borrows this handle through completion and join.
    const auto worker = attachment.loop().worker();
    early_final_peer peer(io, worker, mode);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string client_url("http://127.0.0.1:" + std::string(std::to_string(peer.port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_limits_ = {.max_open_ = 1},
                                                         .request_timeout_ = std::chrono::seconds(2),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        try {
            if (mode == response_mode::refused_always) {
                bool rejected = false;
                try {
                    (void)co_await client.client().get("/next").send();
                } catch (const ruvia::http_client_error& error) {
                    rejected = true;
                    EXPECT_EQ(error.code(), ruvia::http_client_errc::protocol_error);
                    EXPECT_TRUE(error.request_written());
                    EXPECT_NE(error.connection_info(), nullptr);
                }
                EXPECT_TRUE(rejected);
                EXPECT_EQ(peer.requests_, 2U);
            } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                // These cases start with the request below.
            } else if (mode == response_mode::connection_lost_upload || mode == response_mode::goaway_upload) {
                auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                co_await exchange.body().write("payload");
                co_await exchange.body().end();
                auto response = co_await exchange.response();
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "ok");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, 1U);
                EXPECT_EQ(peer.received_, "payload");
                EXPECT_TRUE(peer.request_ended_);
            } else if (mode == response_mode::abandoned_pending) {
                std::optional<ruvia::http_client_response> retained;
                {
                    auto exchange = co_await client.client().post("/upload").upload({.content_length_ = 7});
                    retained.emplace(co_await exchange.response());
                    EXPECT_TRUE(retained->status() == ruvia::http_status::ok);
                    EXPECT_TRUE(!retained->stream().complete());
                }
                bool cancelled = false;
                try {
                    (void)co_await retained->stream().read_all();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(cancelled);
                bool completion_cancelled = false;
                try {
                    (void)retained->stream().complete();
                } catch (const ruvia::http_client_error& error) {
                    completion_cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                }
                EXPECT_TRUE(completion_cancelled);
            } else if (mode == response_mode::buffered) {
                const std::string payload(65536, 'b');
                auto response = co_await client.client().post("/upload").body(payload).send();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
            } else if (mode == response_mode::streaming) {
                const std::string payload(streaming_payload_bytes, 's');
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = payload.size(), .max_chunk_bytes_ = payload.size()});
                auto response = co_await exchange.response();
                EXPECT_TRUE(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.stream().read_all();
                EXPECT_TRUE(bytes.bytes().empty());
                EXPECT_TRUE(response.stream().complete());
                co_await exchange.body().write(payload);
                co_await exchange.body().end();
                EXPECT_TRUE(exchange.body().complete());
            } else {
                const bool expect = mode == response_mode::continued || mode == response_mode::rejected;
                auto exchange = co_await client.client().request("POST", "/upload").upload({.content_length_ = 7, .expectation_ = expect ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none});
                auto response = co_await exchange.response();
                EXPECT_EQ(response.status().value(), mode == response_mode::rejected ? 413 : 200);
                const auto bytes = co_await response.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()),
                    mode == response_mode::rejected ? "no" : "ok");
                EXPECT_TRUE(response.stream().complete());
                if (mode == response_mode::disconnected) {
                    // Start a write with no stream credit, then close the peer.
                    // The write must report its I/O error without replacing a
                    // response EOF that the application has already observed.
                    ruvia::worker_signal started(worker);
                    auto waiting = started.wait();
                    std::optional<std::error_code> write_error;
                    ruvia::task_scope writers(worker);
                    const auto write = [&]() -> ruvia::task<void> {
                        started.notify();
                        try {
                            co_await exchange.body().write("payload");
                        } catch (const ruvia::http_client_error& error) {
                            write_error = error.code();
                        }
                    };
                    writers.spawn(write());
                    co_await std::move(waiting);
                    peer.stop();
                    co_await writers.join();
                    EXPECT_TRUE(write_error == ruvia::make_error_code(ruvia::http_client_errc::io_error));
                    co_await client.shutdown();
                    EXPECT_TRUE(!(co_await response.stream().text()).has_value());
                    EXPECT_TRUE(response.stream().complete());
                } else if (mode == response_mode::ordinary || mode == response_mode::continued) {
                    co_await exchange.body().write("payload");
                    co_await exchange.body().end();
                    EXPECT_TRUE(exchange.body().complete());
                } else if (mode != response_mode::abandoned) {
                    bool cancelled = false;
                    try {
                        co_await exchange.body().write("payload");
                    } catch (const ruvia::http_client_error& error) {
                        cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
                    }
                    EXPECT_TRUE(cancelled);
                }
            }
            if (mode != response_mode::disconnected && mode != response_mode::connection_lost_upload &&
                mode != response_mode::goaway_upload && mode != response_mode::refused_always) {
                auto next = co_await client.client().get("/next").send();
                const auto bytes = co_await next.stream().read_all();
                const auto body = bytes.bytes();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "next");
                co_await peer.join();
                EXPECT_EQ(peer.requests_, mode == response_mode::connection_lost ? 1U : 2U);
                if (mode == response_mode::ordinary || mode == response_mode::continued ||
                    mode == response_mode::buffered || mode == response_mode::streaming) {
                    const auto expected = mode == response_mode::buffered
                                              ? std::string(65536, 'b')
                                          : mode == response_mode::streaming
                                              ? std::string(streaming_payload_bytes, 's')
                                              : std::string("payload");
                    EXPECT_EQ(peer.received_, expected);
                    EXPECT_TRUE(peer.request_ended_);
                } else if (mode == response_mode::refused || mode == response_mode::connection_lost) {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(!peer.request_reset_);
                } else {
                    EXPECT_TRUE(peer.received_.empty());
                    EXPECT_TRUE(mode == response_mode::reset || peer.request_reset_);
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.stop();
        try {
            co_await peer.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_client_upload_test

TEST(Ruvia, http2_client_early_final_keeps_ordinary_upload_open) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::ordinary);
}
TEST(Ruvia, http2_client_early_final_after_continue_keeps_upload_open) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::continued);
}
TEST(Ruvia, http2_client_early_final_before_continue_retires_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::rejected);
}
TEST(Ruvia, http2_client_early_final_no_error_reset_preserves_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::reset);
}
TEST(Ruvia, http2_client_early_final_abandoned_upload_retires_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned);
}
TEST(Ruvia, http2_client_abandoned_upload_cancels_its_retained_incomplete_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::abandoned_pending);
}

TEST(Ruvia, http2_client_early_final_drains_window_blocked_buffered_upload_before_reuse) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::buffered);
}

TEST(Ruvia, http2_client_streaming_upload_drains_one_large_chunk_across_flow_control_windows) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::streaming);
}

TEST(Ruvia, http2_client_upload_transport_failure_preserves_completed_response) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::disconnected);
}

TEST(Ruvia, http2_client_retries_request_refused_with_refused_stream) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused);
}

TEST(Ruvia, http2_client_retries_unsubmitted_request_after_connection_loss) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_connection_loss) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::connection_lost_upload);
}

TEST(Ruvia, http2_client_retries_unsubmitted_streaming_upload_after_goaway) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::goaway_upload);
}

TEST(Ruvia, http2_abandoned_unsubmitted_upload_retires_while_older_stream_drains) {
    http2_client_upload_test::exercise_abandon_during_drain();
}

TEST(Ruvia, http2_client_refused_request_preserves_possible_delivery) {
    http2_client_upload_test::exercise(http2_client_upload_test::response_mode::refused_always);
}
