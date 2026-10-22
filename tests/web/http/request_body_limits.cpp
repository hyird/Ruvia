#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/streaming.h"

#include "context_body_decoding_fixture.h"
#include "http_server_fixture.h"
#include "test_io_context.h"

namespace request_body_limits_test {

struct body_lane_observation final {
    std::atomic<bool> first_read_{};
    std::atomic<bool> eof_{};
    std::atomic<bool> overlap_rejected_{};
    std::atomic<bool> multipart_eof_{};
    std::atomic<bool> multipart_overlap_rejected_{};
    std::atomic<bool> truncated_read_failed_{};
    std::atomic<bool> truncated_reread_rejected_{};
    std::atomic<bool> truncated_reread_yielded_{};
    std::atomic<unsigned> multipart_reader_parts_{};
};

const auto body_lane_observed = std::make_shared<body_lane_observation>();
[[maybe_unused]] const bool body_lane_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<body_lane_observation>>([] { return body_lane_observed; });
    return true;
}();

class body_lane_routes final {
    RUVIA_ROUTES_BEGIN(body_lane_routes)
    RUVIA_POST("/server-lazy-body-read", read, ruvia::stream_body);
    RUVIA_POST("/server-lazy-multipart-read", read_multipart, ruvia::stream_body);
    RUVIA_POST("/server-multipart-reader", read_multipart_reader, ruvia::stream_body);
    RUVIA_POST("/server-truncated-body-reread", reread_truncated, ruvia::stream_body);
    RUVIA_POST("/server-small-chunk-body", read_small_chunks, ruvia::stream_body);
    RUVIA_GET("/server-small-chunk-next", small_chunk_next);
    RUVIA_POST("/server-pipelined-echo", pipelined_echo);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> read_multipart_reader(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        auto reader = context_value.req().multipart_reader();
        std::pmr::string body(context_value.allocator<char>());
        while (auto part = co_await reader.read()) {
            observed.multipart_reader_parts_.fetch_add(1);
            body.append(part->body());
        }
        co_return context_value.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> read(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        auto& reader = context_value.req().body_reader();
        {
            auto discarded = reader.read();
            static_cast<void>(discarded);
        }
        auto first = reader.text();
        auto last = reader.read();
        ruvia::task_scope reads(context_value.worker());
        const auto receive = [&]() -> ruvia::task<void> {
            const auto chunk = co_await std::move(first);
            observed.first_read_ = chunk && *chunk == "body";
        };
        reads.spawn(receive());
        try {
            auto overlap = reader.read();
            static_cast<void>(co_await std::move(overlap));
        } catch (const std::logic_error&) {
            observed.overlap_rejected_ = true;
        }
        co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        co_await reads.join();
        observed.eof_ = !(co_await std::move(last));
        co_return context_value.text("read-complete");
    }

    ruvia::task<ruvia::http_response> read_multipart(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        ruvia::multipart_reader reader(context_value.req().body_reader(),
            {.boundary_ = ruvia::multipart_boundary("lazy"), .resource_ = context_value.pool()});
        auto first = reader.read();
        auto last = reader.read();
        std::pmr::string body(context_value.allocator<char>());
        ruvia::task_scope reads(context_value.worker());
        const auto receive = [&]() -> ruvia::task<void> {
            const auto part = co_await std::move(first);
            if (part) {
                body.append(part->name());
                body.push_back(':');
                body.append(part->body());
            }
        };
        reads.spawn(receive());
        try {
            auto overlap = reader.read();
            static_cast<void>(co_await std::move(overlap));
        } catch (const std::logic_error&) {
            observed.multipart_overlap_rejected_ = true;
        }
        co_await context_value.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        co_await reads.join();
        auto part = co_await std::move(last);
        while (part) {
            body.append(part->body());
            part = co_await reader.read();
        }
        observed.multipart_eof_ = true;
        co_return context_value.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> reread_truncated(ruvia::context& context_value) {
        auto& observed = *context_value.worker_state<std::shared_ptr<body_lane_observation>>();
        auto& reader = context_value.req().body_reader();
        try {
            static_cast<void>(co_await reader.read());
        } catch (const ruvia::http_protocol_error&) {
            observed.truncated_read_failed_ = true;
        }
        try {
            observed.truncated_reread_yielded_ = (co_await reader.read()).has_value();
        } catch (const std::logic_error&) {
            observed.truncated_reread_rejected_ = true;
        }
        co_return context_value.text("reread-observed");
    }

    ruvia::task<ruvia::http_response> read_small_chunks(ruvia::context& context_value) {
        auto& reader = context_value.req().body_reader();
        std::pmr::string body(context_value.allocator<char>());
        while (const auto chunk = co_await reader.text()) {
            body.append(*chunk);
        }
        body.push_back(';');
        body.append(context_value.req().trailer("X-Sum").value_or("missing"));
        co_return context_value.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> small_chunk_next(ruvia::context& context_value) {
        co_return context_value.text("next");
    }

    ruvia::task<ruvia::http_response> pipelined_echo(ruvia::context& context_value) {
        co_return context_value.text(co_await context_value.req().text());
    }
};

void exercise_lazy_body_reads(ruvia::testing::test_context& ruvia_ctx,
    std::string_view target, std::string_view body, std::string_view expected) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            const std::string head = "POST " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(client, asio::buffer(head), std::move(handler));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            std::string informational;
            char byte{};
            while (!informational.ends_with("\r\n\r\n")) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(&byte, 1), std::move(handler));
                });
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                informational.push_back(byte);
            }
            RUVIA_CHECK(informational.starts_with("HTTP/1.1 103 "));
            const auto body_sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(client, asio::buffer(body), std::move(handler));
            });
            if (body_sent.error_code()) {
                throw std::system_error(body_sent.error_code());
            }
            std::string response;
            std::array<char, 1024> buffer;
            for (;;) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(buffer), std::move(handler));
                });
                if (received.error_code() == asio::error::eof) {
                    break;
                }
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                response.append(buffer.data(), received.result());
            }
            RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
            RUVIA_CHECK(response.ends_with(expected));
        } catch (...) {
            failure = std::current_exception();
        }
        static_cast<void>(watchdog.cancel());
        ruvia::close_socket(client);
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    done.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// Writes `head`, waits for `100 Continue` when `await_continue` is set, writes
// `body`, half-closes when `shutdown_send` is set, and returns every byte the
// server sends until it closes the connection.
std::string exchange_raw_request(
    std::string_view head, std::string_view body, bool await_continue, bool shutdown_send) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::string response;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            const auto write_all = [&](std::string_view bytes) -> ruvia::task<void> {
                const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    asio::async_write(client, asio::buffer(bytes), std::move(handler));
                });
                if (sent.error_code()) {
                    throw std::system_error(sent.error_code());
                }
            };
            co_await write_all(head);
            if (await_continue) {
                std::string informational;
                char byte{};
                while (!informational.ends_with("\r\n\r\n")) {
                    const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                        client.async_read_some(asio::buffer(&byte, 1), std::move(handler));
                    });
                    if (received.error_code()) {
                        throw std::system_error(received.error_code());
                    }
                    informational.push_back(byte);
                }
                if (!informational.starts_with("HTTP/1.1 100 ")) {
                    throw std::runtime_error("expected 100 Continue before the request body");
                }
            }
            co_await write_all(body);
            if (shutdown_send) {
                client.shutdown(asio::ip::tcp::socket::shutdown_send);
            }
            std::array<char, 1024> buffer;
            for (;;) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(buffer), std::move(handler));
                });
                if (received.error_code() == asio::error::eof) {
                    break;
                }
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                response.append(buffer.data(), received.result());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        static_cast<void>(watchdog.cancel());
        ruvia::close_socket(client);
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    done.get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return response;
}

}  // namespace request_body_limits_test

using request_body_limits_test::body_lane_observed;
using request_body_limits_test::exchange_raw_request;
using request_body_limits_test::exercise_lazy_body_reads;

RUVIA_TEST(web_request_decode_accepts_gzip_content) {
    const std::string plain(2048, 'x');
    const std::string encoded = gzip_compress(plain);
    RUVIA_CHECK(!encoded.empty());
    const auto observation_value = read_context_gzip_body(encoded);
    RUVIA_CHECK(!observation_value.error_status_.has_value());
    RUVIA_CHECK_EQ(observation_value.body_, plain);
}

RUVIA_TEST(web_request_decode_accepts_empty_encoded_representation) {
    const auto observation_value = read_context_gzip_body({});
    RUVIA_CHECK(!observation_value.error_status_.has_value());
    RUVIA_CHECK(observation_value.body_.empty());
}

RUVIA_TEST(context_request_cold_operation_rejects_after_request_scope_closes) {
    auto operation = make_expired_context_text_read();
    bool rejected = false;
    asio::io_context io(1);
    auto future = asio::co_spawn(io,
        ruvia::as_awaitable(await_expired_context_text_read(*operation, rejected)),
        asio::use_future);
    io.run();
    future.get();
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(streamed_request_cold_reads_share_a_lane_only_when_started) {
    body_lane_observed->first_read_ = false;
    body_lane_observed->eof_ = false;
    body_lane_observed->overlap_rejected_ = false;
    exercise_lazy_body_reads(ruvia_ctx, "/server-lazy-body-read", "body", "read-complete");
    RUVIA_CHECK(body_lane_observed->first_read_);
    RUVIA_CHECK(body_lane_observed->eof_);
    RUVIA_CHECK(body_lane_observed->overlap_rejected_);
}

RUVIA_TEST(multipart_request_cold_reads_preserve_parser_state_after_started_overlap) {
    body_lane_observed->multipart_eof_ = false;
    body_lane_observed->multipart_overlap_rejected_ = false;
    constexpr std::string_view body = "--lazy\r\nContent-Disposition: form-data; name=field\r\n\r\nvalue\r\n--lazy--\r\n";
    exercise_lazy_body_reads(ruvia_ctx, "/server-lazy-multipart-read", body, "field:value");
    RUVIA_CHECK(body_lane_observed->multipart_eof_);
    RUVIA_CHECK(body_lane_observed->multipart_overlap_rejected_);
}

RUVIA_TEST(streamed_request_body_stays_failed_after_truncated_read) {
    body_lane_observed->truncated_read_failed_ = false;
    body_lane_observed->truncated_reread_rejected_ = false;
    body_lane_observed->truncated_reread_yielded_ = false;
    const auto response = exchange_raw_request(
        "POST /server-truncated-body-reread HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n", {},
        false, true);
    RUVIA_CHECK(body_lane_observed->truncated_read_failed_);
    RUVIA_CHECK(body_lane_observed->truncated_reread_rejected_);
    RUVIA_CHECK(!body_lane_observed->truncated_reread_yielded_);
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    // The body was never consumed, so the connection cannot be reused.
    RUVIA_CHECK(response.find("\r\nConnection: close\r\n") != std::string::npos);
    RUVIA_CHECK(response.ends_with("reread-observed"));
}

RUVIA_TEST(streamed_chunked_body_of_small_chunks_keeps_trailers_and_pipeline) {
    // The body follows 100 Continue, so every chunk arrives through socket
    // reads after the head, with chunks split across read boundaries.
    constexpr std::size_t chunk_count = 3000;
    std::string expected;
    std::string body;
    for (std::size_t index = 0; index < chunk_count; ++index) {
        const auto value = static_cast<char>('a' + index % 26);
        expected.push_back(value);
        body.append("1\r\n");
        body.push_back(value);
        body.append("\r\n");
    }
    body.append("0\r\nX-Sum: ok\r\n\r\n");
    body.append("GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    const auto response = exchange_raw_request(
        "POST /server-small-chunk-body HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n"
        "Expect: 100-continue\r\n\r\n",
        body, true, false);
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    const auto first_end = response.find(expected + ";ok");
    RUVIA_CHECK(first_end != std::string::npos);
    const auto second = response.find("HTTP/1.1 200 ", first_end);
    RUVIA_CHECK(second != std::string::npos);
    RUVIA_CHECK(response.ends_with("next"));
}

RUVIA_TEST(buffered_routes_answer_a_pipelined_burst_in_order) {
    // One write carries every request, so each buffered-route response is
    // prepared while the next requests still sit behind it in the read buffer,
    // and the echoed bodies borrow that buffer until the response owns them.
    const auto response = exchange_raw_request(
        "GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "POST /server-pipelined-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 6\r\n\r\nfirst!"
        "POST /server-pipelined-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 7\r\n\r\nsecond!"
        "GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
        {}, false, false);
    std::size_t position = 0;
    for (const std::string_view body : {"next", "first!", "second!", "next"}) {
        position = response.find("HTTP/1.1 200 ", position);
        RUVIA_CHECK(position != std::string::npos);
        if (position == std::string::npos) {
            return;
        }
        position = response.find("\r\n\r\n", position);
        RUVIA_CHECK(position != std::string::npos);
        if (position == std::string::npos) {
            return;
        }
        position += 4;
        RUVIA_CHECK_EQ(std::string_view(response).substr(position, body.size()), body);
    }
    RUVIA_CHECK_EQ(position + 4, response.size());
}

RUVIA_TEST(buffered_known_length_body_read_from_socket_keeps_the_pipelined_request) {
    // The body follows 100 Continue, so all of it arrives through socket reads
    // after the head, across several reads and capacity growth steps, with the
    // next request directly behind its last byte in the same write.
    std::string body(200 * 1024 + 7, '\0');
    for (std::size_t index = 0; index < body.size(); ++index) {
        body[index] = static_cast<char>('a' + index % 23);
    }
    const auto response = exchange_raw_request(
        "POST /server-pipelined-echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nExpect: 100-continue\r\n\r\n",
        body + "GET /server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", true, false);
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    const auto echoed = response.find("\r\n\r\n");
    RUVIA_CHECK(echoed != std::string::npos);
    if (echoed == std::string::npos) {
        return;
    }
    RUVIA_CHECK(std::string_view(response).substr(echoed + 4, body.size()) == body);
    const auto second = response.find("HTTP/1.1 200 ", echoed + 4 + body.size());
    RUVIA_CHECK_EQ(second, echoed + 4 + body.size());
    RUVIA_CHECK(response.ends_with("next"));
}

RUVIA_TEST(server_rejects_absolute_form_targets_outside_http_schemes) {
    // A front proxy that only recognises http(s) absolute URIs must not see a
    // different target than the one routed here, so other schemes are a 400.
    for (const std::string_view head : {
             "GET ftp://evil/server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n",
             "GET javascript:/server-small-chunk-next HTTP/1.1\r\nHost: localhost\r\n\r\n",
             "GET x:80 HTTP/1.1\r\nHost: localhost\r\n\r\n"}) {
        const auto response = exchange_raw_request(head, {}, false, false);
        RUVIA_CHECK(response.starts_with("HTTP/1.1 400 "));
        RUVIA_CHECK(response.find("next") == std::string::npos);
    }
    const auto accepted = exchange_raw_request(
        "GET http://localhost/server-small-chunk-next HTTP/1.1\r\nHost: other\r\nConnection: close\r\n\r\n", {},
        false, false);
    RUVIA_CHECK(accepted.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(accepted.ends_with("next"));
}

RUVIA_TEST(web_request_decodes_content_coding_stacks_in_reverse_order) {
    constexpr std::array codings{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate};
    const std::string plain(8192, 'm');
    auto encoded = ruvia::encode_http_content(codings, plain, {.max_encoded_bytes_ = plain.size()});
    const auto* encoded_bytes = std::get_if<std::pmr::string>(&encoded);
    RUVIA_CHECK(encoded_bytes != nullptr);
    if (encoded_bytes == nullptr) {
        return;
    }
    (void)context_request_test::with_context(
        ruvia::test_request::post("/").header("Content-Encoding", "gzip, deflate").body(*encoded_bytes),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
            const auto body = co_await context_value.req().text();
            RUVIA_CHECK_EQ(body, std::string_view(plain));
        });
}

// The streaming multipart reader decodes no content coding, so it refuses a
// coded body before any part reaches the handler, advertising only identity
// (RFC 9110 §15.5.16); a malformed Content-Encoding field is a 400.
RUVIA_TEST(streamed_multipart_reader_refuses_content_coded_bodies) {
    constexpr std::string_view body = "--lazy\r\nContent-Disposition: form-data; name=field\r\n\r\nvalue\r\n--lazy--\r\n";
    const auto exchange = [](std::string_view coding_field, std::string_view payload) {
        const std::string head =
            "POST /server-multipart-reader HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: multipart/form-data; boundary=lazy\r\n" +
            std::string(coding_field) + "Content-Length: " + std::to_string(payload.size()) +
            "\r\nConnection: close\r\n\r\n";
        return exchange_raw_request(head, payload, false, false);
    };
    body_lane_observed->multipart_reader_parts_ = 0;
    const auto identity = exchange("Content-Encoding: identity\r\n", body);
    RUVIA_CHECK(identity.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(identity.ends_with("\r\n\r\nvalue"));
    RUVIA_CHECK_EQ(body_lane_observed->multipart_reader_parts_.load(), 1U);

    const auto gzip = exchange("Content-Encoding: gzip\r\n", gzip_compress(std::string(body)));
    RUVIA_CHECK(gzip.starts_with("HTTP/1.1 415 "));
    RUVIA_CHECK(gzip.find("\r\nAccept-Encoding: identity\r\n") != std::string::npos);
    const auto unknown = exchange("Content-Encoding: x-unknown\r\n", body);
    RUVIA_CHECK(unknown.starts_with("HTTP/1.1 415 "));
    RUVIA_CHECK(unknown.find("\r\nAccept-Encoding: identity\r\n") != std::string::npos);
    RUVIA_CHECK(exchange("Content-Encoding: gzip;level=9\r\n", body).starts_with("HTTP/1.1 400 "));
    RUVIA_CHECK_EQ(body_lane_observed->multipart_reader_parts_.load(), 1U);
}
