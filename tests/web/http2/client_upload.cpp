#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/http_client.h"

#include "test_harness.h"
#include "test_io_context.h"

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
    refused };

constexpr std::size_t streaming_payload_bytes = 256 * 1024;

void append_frame(std::string& wire, ruvia::http2_frame_type type, std::uint8_t flags,
    std::uint32_t stream_id, std::string_view payload) {
    std::array<char, ruvia::http2_frame_header_bytes> header;
    if (!ruvia::encode_http2_frame_header(header, static_cast<std::uint32_t>(payload.size()), type, flags, stream_id)) {
        throw std::logic_error("invalid scripted HTTP/2 frame");
    }
    wire.append(header.data(), header.size());
    wire.append(payload);
}

// A raw peer deliberately keeps the request half open after the final response.
// It advertises one stream and never supplies the optional server-side reset, so
// client retirement cannot be masked by a server request-lease destructor.
class early_final_peer final {
public:
    early_final_peer(asio::io_context& io, const ruvia::worker_handle& worker, response_mode mode)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          done_(worker),
          mode_(mode) {}
    early_final_peer(asio::io_context&, ruvia::worker_handle&&, response_mode) = delete;

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

    std::string received_;
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
        std::string wire;
        const std::array<char, 6> one_stream{0, 3, 0, 0, 0, 1};
        append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {one_stream.data(), one_stream.size()});
        if (mode_ == response_mode::disconnected) {
            const std::array<char, 6> zero_window{0, 4, 0, 0, 0, 0};
            append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {zero_window.data(), zero_window.size()});
        }
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::pmr::unsynchronized_pool_resource memory;
        ruvia::hpack_decoder decoder({.resource_ = &memory});
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
                std::string method;
                std::string path;
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
                if (requests_ == 1 && mode_ == response_mode::refused) {
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
                if (requests_ == 1) {
                    if (method != "POST" || path != "/upload") {
                        throw std::runtime_error("unexpected initial request");
                    }
                    first_stream = header->stream_id_;
                    if (mode_ == response_mode::continued) {
                        std::pmr::string interim(&memory);
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
                const std::string_view body = requests_ == 2 ? "next" : rejected ? "no"
                                                                                 : "ok";
                std::pmr::string fields(&memory);
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
            if (requests_ == 2 || (requests_ == 1 && mode_ == response_mode::disconnected)) {
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

void exercise(ruvia::testing::test_context& ruvia_ctx, response_mode mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    // The peer's signal borrows this handle through completion and join.
    const auto worker = attachment.loop().handle();
    early_final_peer peer(io, worker, mode);
    peer.start();
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = peer.port(),
                                                         .connection_count_ = 1,
                                                         .request_timeout_ = std::chrono::seconds(2),
                                                         .protocol_ = ruvia::http_client_protocol::http2_only});
        try {
            if (mode == response_mode::refused) {
                // The refused request is replayed instead of failing.
            } else if (mode == response_mode::abandoned_pending) {
                std::optional<ruvia::http_client_response> retained;
                {
                    auto exchange = co_await client.handle().open_request(
                        {.method_ = "POST", .target_ = "/upload"}, {.content_length_ = 7});
                    retained.emplace(co_await exchange.response());
                    RUVIA_CHECK(retained->status() == ruvia::http_status::ok);
                    RUVIA_CHECK(!retained->body().complete());
                }
                bool cancelled = false;
                try {
                    (void)co_await retained->body().read_all();
                } catch (const ruvia::http_client_error& error) {
                    cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
                }
                RUVIA_CHECK(cancelled);
            } else if (mode == response_mode::buffered) {
                const std::string payload(65536, 'b');
                auto response = co_await client.handle().send({.method_ = "POST", .target_ = "/upload", .content_ = ruvia::http_client_request_content_view::bytes(payload)});
                RUVIA_CHECK(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.body().read_all();
                RUVIA_CHECK(bytes.bytes().empty());
            } else if (mode == response_mode::streaming) {
                const std::string payload(streaming_payload_bytes, 's');
                auto exchange = co_await client.handle().open_request({.method_ = "POST", .target_ = "/upload"},
                    {.content_length_ = payload.size(), .max_chunk_bytes_ = payload.size()});
                auto response = co_await exchange.response();
                RUVIA_CHECK(response.status() == ruvia::http_status::no_content);
                const auto bytes = co_await response.body().read_all();
                RUVIA_CHECK(bytes.bytes().empty());
                co_await exchange.body().write(payload);
                co_await exchange.body().end();
                RUVIA_CHECK(exchange.body().complete());
            } else {
                const bool expect = mode == response_mode::continued || mode == response_mode::rejected;
                auto exchange = co_await client.handle().open_request({.method_ = "POST", .target_ = "/upload"},
                    {.content_length_ = 7, .expectation_ = expect ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none});
                auto response = co_await exchange.response();
                RUVIA_CHECK_EQ(response.status().value(), mode == response_mode::rejected ? 413 : 200);
                const auto bytes = co_await response.body().read_all();
                const auto body = bytes.bytes();
                RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()),
                    mode == response_mode::rejected ? "no" : "ok");
                RUVIA_CHECK(response.body().complete());
                if (mode == response_mode::disconnected) {
                    // Start a write with no stream credit, then close the peer.
                    // The write must report its I/O error without replacing a
                    // response EOF that the application has already observed.
                    ruvia::worker_signal started(worker);
                    auto waiting = started.wait();
                    std::optional<ruvia::http_client_error::code_type> write_error;
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
                    RUVIA_CHECK(write_error == ruvia::http_client_error::code_type::io_error);
                    co_await client.shutdown();
                    RUVIA_CHECK(!(co_await response.body().text()).has_value());
                    RUVIA_CHECK(response.body().complete());
                } else if (mode == response_mode::ordinary || mode == response_mode::continued) {
                    co_await exchange.body().write("payload");
                    co_await exchange.body().end();
                    RUVIA_CHECK(exchange.body().complete());
                } else if (mode != response_mode::abandoned) {
                    bool cancelled = false;
                    try {
                        co_await exchange.body().write("payload");
                    } catch (const ruvia::http_client_error& error) {
                        cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
                    }
                    RUVIA_CHECK(cancelled);
                }
            }
            if (mode != response_mode::disconnected) {
                auto next = co_await client.handle().send({.target_ = "/next"});
                const auto bytes = co_await next.body().read_all();
                const auto body = bytes.bytes();
                RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()), "next");
                co_await peer.join();
                RUVIA_CHECK_EQ(peer.requests_, 2U);
                if (mode == response_mode::ordinary || mode == response_mode::continued ||
                    mode == response_mode::buffered || mode == response_mode::streaming) {
                    const auto expected = mode == response_mode::buffered
                                              ? std::string(65536, 'b')
                                          : mode == response_mode::streaming
                                              ? std::string(streaming_payload_bytes, 's')
                                              : std::string("payload");
                    RUVIA_CHECK_EQ(peer.received_, expected);
                    RUVIA_CHECK(peer.request_ended_);
                } else if (mode == response_mode::refused) {
                    RUVIA_CHECK(peer.received_.empty());
                    RUVIA_CHECK(!peer.request_reset_);
                } else {
                    RUVIA_CHECK(peer.received_.empty());
                    RUVIA_CHECK(mode == response_mode::reset || peer.request_reset_);
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
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http2_client_upload_test

RUVIA_TEST(http2_client_early_final_keeps_ordinary_upload_open) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::ordinary);
}
RUVIA_TEST(http2_client_early_final_after_continue_keeps_upload_open) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::continued);
}
RUVIA_TEST(http2_client_early_final_before_continue_retires_stream) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::rejected);
}
RUVIA_TEST(http2_client_early_final_no_error_reset_preserves_response) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::reset);
}
RUVIA_TEST(http2_client_early_final_abandoned_upload_retires_stream) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::abandoned);
}
RUVIA_TEST(http2_client_abandoned_upload_cancels_its_retained_incomplete_response) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::abandoned_pending);
}

RUVIA_TEST(http2_client_early_final_drains_window_blocked_buffered_upload_before_reuse) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::buffered);
}

RUVIA_TEST(http2_client_streaming_upload_drains_one_large_chunk_across_flow_control_windows) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::streaming);
}

RUVIA_TEST(http2_client_upload_transport_failure_preserves_completed_response) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::disconnected);
}

RUVIA_TEST(http2_client_retries_request_refused_with_refused_stream) {
    http2_client_upload_test::exercise(ruvia_ctx, http2_client_upload_test::response_mode::refused);
}
