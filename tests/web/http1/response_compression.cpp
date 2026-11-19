#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/socket_base.hpp>
#include <asio/write.hpp>

#include "app.h"
#include "blocking_pool.h"
#include "context.h"
#include "controller.h"
#include "event_loop.h"
#include "http_client.h"
#include "http_content_codec.h"
#include "http_content_coding.h"
#include "http_content_negotiation.h"
#include "http_server.h"
#include "http_server_fixture.h"
#include "streaming.h"
#include "task_scope.h"
#include "worker_task_fixture.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "http2_frame.h"
#include "http2_hpack.h"
#endif
// Inline buffered-response compression on one worker, whose encoders are
// reused across responses. This executable owns its application, so the
// app-wide compression policy cannot change the bodies of other suites.

namespace {

// Every fixture passes the same configuration: whichever case runs first
// fixes the shared application's server_config.
[[nodiscard]] ruvia::server_config compressing_server() {
    return {.connection_scan_interval_ = std::chrono::milliseconds(5), .compression_ = ruvia::compression_config{.min_bytes_ = 16, .sync_bytes_ = 64 * 1024}};
}

[[nodiscard]] std::string make_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 3U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 6U) + ((i / 48U) % 5U));
    }
    return body;
}

// Cover both sides of the configured inline ceiling without changing policy between cases.
const std::string small_body(make_body(3'000, 1));
const std::string large_body(make_body(60'000, 2));
const std::string offloaded_body(make_body(128 * 1024, 3));

[[nodiscard]] const std::filesystem::path& file_dir() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-response-compression");
    return directory.path();
}

std::unique_ptr<ruvia::static_root> resident_root;
std::unique_ptr<ruvia::static_root> coded_root;

class response_compression_routes final {
    RUVIA_ROUTES_BEGIN(response_compression_routes)
    RUVIA_GET("/compression-small", small);
    RUVIA_GET("/compression-large", large);
    RUVIA_GET("/compression-offloaded", offloaded);
    RUVIA_GET("/compression-tiny", tiny);
    RUVIA_GET("/compression-digest", digest);
    RUVIA_GET("/compression-file", file);
    RUVIA_GET("/compression-static", static_file);
    RUVIA_GET("/compression-coded-static", coded_static_file);
    RUVIA_GET("/compression-head", small);
    RUVIA_HEAD("/compression-head", head_declared);
    RUVIA_GET("/compression-head-bare", small);
    RUVIA_HEAD("/compression-head-bare", head_bare);
    RUVIA_GET("/compression-empty-get", empty_get);
    RUVIA_GET("/compression-redirect", redirect_get);
    RUVIA_GET("/compression-zero-file", zero_file);
    RUVIA_GET("/compression-zero-static", zero_static_file);
    RUVIA_GET("/compression-head-comma", small);
    RUVIA_HEAD("/compression-head-comma", head_comma_declared);
    RUVIA_GET("/compression-head-zero", empty_get);
    RUVIA_HEAD("/compression-head-zero", head_zero_declared);
    RUVIA_GET("/compression-custom-coding", custom_coding);
    RUVIA_GET("/compression-mixed-rejected", mixed_rejected_coding);
    RUVIA_POST("/compression-echo-decoded", echo_decoded);
#if defined(RUVIA_ENABLE_HTTP2)
    RUVIA_POST("/compression-overlap", read_overlapping);
#endif
    RUVIA_GET("/compression-zero-declared", head_zero_declared);
    RUVIA_GET("/compression-stream", stream);
    RUVIA_GET("/compression-stream-held", hold_stream);
    RUVIA_GET("/compression-stream-custom", stream_custom_coding);
    RUVIA_GET("/compression-stream-mixed", stream_mixed_coding);
    RUVIA_GET("/compression-stream-204", stream_no_content);
    RUVIA_GET("/compression-stream-304", stream_not_modified);
    RUVIA_GET("/compression-stream-declared", stream_declared_length);
    RUVIA_GET("/compression-validators", validators);
    RUVIA_GET("/compression-media", media);
    RUVIA_GET_WS("/compression-websocket", refused_websocket);
    RUVIA_ROUTES_END

    // Never reached: the cases refuse the handshake.
    ruvia::task<void> refused_websocket(ruvia::context&) {
        co_return;
    }

    ruvia::task<ruvia::http_response> small(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(small_body));
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(large_body));
    }

    ruvia::task<ruvia::http_response> offloaded(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(offloaded_body));
    }

    ruvia::task<ruvia::http_response> head_declared(ruvia::context& context_value) {
        const auto length = std::to_string(small_body.size());
        const std::array headers{ruvia::http_header_view{"Content-Length", length}};
        co_return context_value.text("", {.headers_ = headers});
    }
    ruvia::task<ruvia::http_response> head_bare(ruvia::context& context_value) {
        co_return context_value.text("");
    }
    ruvia::task<ruvia::http_response> empty_get(ruvia::context& context_value) {
        const std::array headers{ruvia::http_header_view{"ETag", "\"empty-v1\""}};
        co_return context_value.text("", {.headers_ = headers});
    }
    ruvia::task<ruvia::http_response> redirect_get(ruvia::context& context_value) {
        co_return context_value.redirect({.location_ = "/compression-small"});
    }
    ruvia::task<ruvia::http_response> zero_file(ruvia::context& context_value) {
        const auto path = file_dir() / "zero.txt";
        co_return co_await context_value.file({.path_ = path});
    }
    ruvia::task<ruvia::http_response> zero_static_file(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*resident_root, {.relative_path_ = "zero.txt"});
    }
    ruvia::task<ruvia::http_response> head_comma_declared(ruvia::context& context_value) {
        const auto len = std::to_string(small_body.size());
        const auto length = len + ", " + len;
        const std::array headers{ruvia::http_header_view{"Content-Length", length}};
        co_return context_value.text("", {.headers_ = headers});
    }
    ruvia::task<ruvia::http_response> head_zero_declared(ruvia::context& context_value) {
        const std::array headers{ruvia::http_header_view{"ETag", "\"empty-v1\""}, ruvia::http_header_view{"Content-Length", "0"}};
        co_return context_value.text("", {.headers_ = headers});
    }
    ruvia::task<ruvia::http_response> custom_coding(ruvia::context& context_value) {
        auto resp = context_value.text("custom-encoded-payload");
        ruvia::http_accepted_encoding_quality quality;
        quality.update(context_value.request().header("Accept-Encoding").value_or(""), "x-custom");
        if (quality.accepts(false)) {
            resp.header("Content-Encoding", "x-custom");
        }
        co_return resp;
    }
    ruvia::task<ruvia::http_response> mixed_rejected_coding(ruvia::context& context_value) {
        auto resp = context_value.text("mixed-encoded-payload");
        resp.header("Content-Encoding", "br, x-custom");
        co_return resp;
    }
    ruvia::task<ruvia::http_response> echo_decoded(ruvia::context& context_value) {
        const ruvia::operation_options expired{.timeout_ = ruvia::operation_timeout(std::chrono::steady_clock::now())};
        try {
            (void)co_await context_value.request().body_text(expired);
            ADD_FAILURE() << "expired initial read returned the request body";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::errc::timed_out);
        }
        const auto bytes = co_await context_value.request().body_bytes();
        const auto body = co_await context_value.request().body_text();
        try {
            (void)co_await context_value.request().collect_body(expired);
            ADD_FAILURE() << "expired collection returned cached bytes";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::errc::timed_out);
        }
        try {
            co_await context_value.request().discard_body(expired);
            ADD_FAILURE() << "expired discard succeeded";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::errc::timed_out);
        }
        const auto owned = co_await context_value.request().collect_body();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), body);
        EXPECT_EQ(owned, body);
        if (!body.empty()) {
            EXPECT_NE(owned.data(), body.data());
        }
        co_return context_value.text(body);
    }

#if defined(RUVIA_ENABLE_HTTP2)
    ruvia::task<ruvia::http_response> read_overlapping(ruvia::context& ctx) {
        auto* const pool = ctx.worker().blocking_pool();
        if (pool == nullptr) {
            throw std::logic_error("test requires a blocking pool");
        }
        std::promise<void> unblock;
        const auto gate = unblock.get_future().share();
        const auto wait_for_release = [gate] {
            gate.wait();
        };
        for (std::size_t i = 0; i < pool->thread_count(); ++i) {
            if (!pool->post(wait_for_release).accepted()) {
                throw std::logic_error("could not occupy the blocking pool");
            }
        }

        ruvia::stop_source stop;
        const auto first_read = [&]() -> ruvia::task<void> {
            try {
                (void)co_await ctx.request().body_bytes({.stop_token_ = stop.token()});
                ADD_FAILURE() << "cancelled read returned the body";
            } catch (const std::system_error& error) {
                EXPECT_EQ(error.code(), std::errc::operation_canceled);
            }
        };
        ruvia::task_scope reads(ctx.worker());
        reads.spawn(first_read());
        bool rejected = false;
        std::exception_ptr failure;
        try {
            (void)co_await ctx.request().collect_body({.timeout_ = std::chrono::milliseconds(100)});
        } catch (const std::logic_error&) {
            rejected = true;
        } catch (...) {
            failure = std::current_exception();
        }
        stop.request_stop();
        co_await reads.join();
        unblock.set_value();
        EXPECT_TRUE(rejected);
        if (failure) {
            std::rethrow_exception(failure);
        }
        const auto text = co_await ctx.request().body_text();
        const auto bytes = co_await ctx.request().body_bytes();
        EXPECT_EQ(bytes.data(), reinterpret_cast<const std::byte*>(text.data()));
        co_return ctx.text(text);
    }
#endif

    ruvia::task<void> stream(ruvia::context& context_value) {
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        const std::string_view body(small_body);
        co_await writer.write(body.substr(0, body.size() / 2));
        co_await writer.write(body.substr(body.size() / 2));
        co_await writer.finish();
    }
    ruvia::task<void> hold_stream(ruvia::context& context_value) {
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write(std::string_view(small_body));
        static_cast<void>(co_await writer.sleep(std::chrono::hours(1)));
    }
    ruvia::task<void> stream_declared_length(ruvia::context& context_value) {
        const auto length = std::to_string(small_body.size());
        const std::array headers{ruvia::http_header_view{"Content-Length", length}};
        auto& writer = context_value.stream_text({.headers_ = headers});
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write(std::string_view(small_body));
        co_await writer.finish();
    }

    ruvia::task<void> stream_custom_coding(ruvia::context& context_value) {
        const std::array headers{ruvia::http_header_view{"Content-Encoding", "x-custom"}};
        auto& writer = context_value.stream_text({.headers_ = headers});
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("custom-stream-payload");
        co_await writer.finish();
    }
    ruvia::task<void> stream_mixed_coding(ruvia::context& context_value) {
        const std::array headers{ruvia::http_header_view{"Content-Encoding", "br, x-custom"}};
        auto& writer = context_value.stream_text({.headers_ = headers});
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("mixed-stream-payload");
        co_await writer.finish();
    }
    ruvia::task<void> stream_no_content(ruvia::context& context_value) {
        co_await context_value.stream_text({.status_ = ruvia::http_status::no_content}).finish();
    }
    ruvia::task<void> stream_not_modified(ruvia::context& context_value) {
        co_await context_value.stream_text({.status_ = ruvia::http_status::not_modified}).finish();
    }
    ruvia::task<ruvia::http_response> tiny(ruvia::context& context_value) {
        co_return context_value.text("hello");
    }

    ruvia::task<ruvia::http_response> validators(ruvia::context& context_value) {
        auto resp = context_value.text(std::string_view(small_body));
        resp.header("ETag", context_value.request().query("etag").value_or(""));
        resp.header("Accept-Ranges", "bytes");
        co_return resp;
    }

    ruvia::task<ruvia::http_response> media(ruvia::context& context_value) {
        auto resp = context_value.text(std::string_view(small_body));
        const auto type = context_value.request().query("type").value_or("");
        if (type.empty()) {
            resp.remove_header("Content-Type");
        } else {
            resp.header("Content-Type", type);
        }
        co_return resp;
    }

    ruvia::task<ruvia::http_response> digest(ruvia::context& context_value) {
        const auto field = context_value.request().query("field").value_or("");
        const std::array headers{ruvia::http_header_view{field, "sha-256=:AAAA:"}};
        co_return context_value.text(std::string_view(small_body), {.headers_ = headers});
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        static const std::filesystem::path path = file_dir() / "file.txt";
        co_return co_await context_value.file({.path_ = path});
    }

    ruvia::task<ruvia::http_response> static_file(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*resident_root, {.relative_path_ = "resident.txt"});
    }

    ruvia::task<ruvia::http_response> coded_static_file(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*coded_root, {.relative_path_ = "asset.txt"});
    }
};

// Sends one request and returns every byte the server wrote before closing,
// or nullopt when the connection is still open after 5 s.
[[nodiscard]] std::optional<std::string> fetch_response(const asio::ip::tcp::endpoint& server_endpoint, std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(server_endpoint);
    asio::write(socket, asio::buffer(request));
    std::string output{};
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output), [&closed](const std::error_code&, std::size_t) {
        closed = true;
    });
    io.run_for(std::chrono::seconds(5));
    if (!closed) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] std::string lower_head(std::string_view response) {
    std::string head(response.substr(0, response.find("\r\n\r\n")));
    std::ranges::transform(head, head.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return head;
}

// The response head's field lines without Date, which may tick between two
// requests.
[[nodiscard]] std::vector<std::string> head_fields(std::string_view response) {
    std::vector<std::string> fields{};
    const auto head = lower_head(response);
    for (auto begin = head.find("\r\n"); begin != std::string::npos;) {
        begin += 2;
        const auto end = head.find("\r\n", begin);
        const auto line = std::string_view(head).substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.starts_with("date:")) {
            fields.emplace_back(line);
        }
        begin = end;
    }
    std::ranges::sort(fields);
    return fields;
}

[[nodiscard]] bool has_field(const std::vector<std::string>& fields, std::string_view prefix) {
    return std::ranges::any_of(fields, [prefix](const std::string& line) {
        return line.starts_with(prefix);
    });
}

[[nodiscard]] std::string expected_encoding(ruvia::http_content_coding coding, std::string_view body) {
    const auto result_value = ruvia::encode_http_content(coding, body, {.max_encoded_bytes_ = body.size() - 1});
    return result_value.has_value() ? std::string(*result_value) : std::string();
}

[[nodiscard]] std::string_view response_body(std::string_view response) {
    const auto separator = response.find("\r\n\r\n");
    return separator == std::string_view::npos ? std::string_view{} : response.substr(separator + 4);
}

[[nodiscard]] std::string head_request(std::string_view target, std::string_view fields) {
    return "HEAD " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" + std::string(fields) + "Connection: close\r\n\r\n";
}

[[nodiscard]] std::string get_request(std::string_view target, std::string_view fields) {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" + std::string(fields) + "Connection: close\r\n\r\n";
}

}  // namespace

#if defined(RUVIA_ENABLE_HTTP2)
// A WebSocket handshake that fails validation is answered with a generated
// error, which does not inherit the negotiated coding promise: HTTP/2 leaves
// it unencoded, as HTTP/1.1 and HTTP/3 do, even for a client that refuses
// identity -- a refusal that would encode a body of any size. The typed
// client senders always offer version 13, so the request is written as frames.
TEST(Ruvia, http2_websocket_handshake_failure_response_is_not_encoded) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    asio::ip::tcp::socket socket(io);
    socket.connect(server.endpoint());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string wire(ruvia::http2_client_preface);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    asio::write(socket, asio::buffer(wire));
    for (;;) {
        const auto frame = ruvia::test::read_frame(socket, deadline);
        if (frame.header_.type_ == static_cast<unsigned>(ruvia::http2_frame_type::settings) && (frame.header_.flags_ & 1U) == 0) {
            break;
        }
    }
    wire.clear();
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 1, 0, {});
    std::string block;
    for (const auto& field : std::array<std::pair<std::string_view, std::string_view>, 7>{{{":method", "CONNECT"}, {":protocol", "websocket"}, {":scheme", "http"}, {":authority", "localhost"}, {":path", "/compression-websocket"}, {"sec-websocket-version", "12"}, {"accept-encoding", "gzip, identity;q=0"}}}) {
        ruvia::hpack_encoder::encode_header(block, field.first, field.second);
    }
    // END_HEADERS without END_STREAM, on the first client stream.
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, 1, block);
    asio::write(socket, asio::buffer(wire));

    block.clear();
    for (bool complete = false; !complete;) {
        const auto frame = ruvia::test::read_frame(socket, deadline);
        if (frame.header_.stream_id_ != 1) {
            continue;
        }
        const auto type = frame.header_.type_;
        ASSERT_TRUE(type == static_cast<unsigned>(ruvia::http2_frame_type::headers) || type == static_cast<unsigned>(ruvia::http2_frame_type::continuation));
        block.append(frame.payload_);
        complete = (frame.header_.flags_ & 4U) != 0;
    }
    std::string status;
    bool version = false;
    bool encoded = false;
    ruvia::hpack_decoder decoder;
    const auto error = decoder.decode(block, [&](std::string_view name, std::string_view value) {
        if (name == ":status") {
            status = value;
        } else if (name == "sec-websocket-version") {
            version = value == "13";
        } else if (name == "content-encoding") {
            encoded = true;
        }
        return true;
    });
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(status, "400");
    EXPECT_TRUE(version);
    EXPECT_FALSE(encoded);
    std::error_code ignored;
    socket.close(ignored);
    server.finish();
}

TEST(Ruvia, http2_buffered_request_body_rejects_overlapping_decode_and_allows_retry) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    auto attachment = ruvia::event_loop_attachment(io);
    const std::string plain(96 * 1024, 'x');
    auto encoded = ruvia::encode_http_content(ruvia::http_content_coding::gzip, plain, {.max_encoded_bytes_ = plain.size()});
    ASSERT_TRUE(encoded);
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(server.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = url, .versions_ = ruvia::http_client_versions{.http1_ = false}});
        std::exception_ptr failure;
        try {
            auto response = co_await client.ref().post("/compression-overlap").header("Content-Encoding", "gzip").body(std::move(*encoded)).send();
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            const auto received = co_await response.collect_body();
            EXPECT_EQ(received.text(), plain);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    server.finish();
}
#endif

TEST(Ruvia, http1_streaming_head_preserves_coding_before_following_encoded_get) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::identity, ruvia::http_content_coding::gzip, ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    for (const auto coding : codings) {
        const auto token = ruvia::http_content_coding_token(coding);
        std::string fields("Accept-Encoding: ");
        fields.append(token).append("\r\n");
        const auto check_coding = [coding, token](std::string_view response) {
            const auto headers = head_fields(response);
            if (coding == ruvia::http_content_coding::identity) {
                EXPECT_FALSE(has_field(headers, "content-encoding:"));
            } else {
                std::string expected("content-encoding: ");
                expected.append(token);
                EXPECT_TRUE(has_field(headers, expected));
            }
        };
        const auto head = fetch_response(server.endpoint(), head_request("/compression-stream", fields));
        ASSERT_TRUE(head.has_value());
        EXPECT_TRUE(head->starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response_body(*head).empty());
        check_coding(*head);

        // HTTP/1.0 close delimiting isolates content encoding from chunk framing.
        std::string request("GET /compression-stream HTTP/1.0\r\nHost: localhost\r\n");
        request.append(fields).append("\r\n");
        const auto get = fetch_response(server.endpoint(), request);
        ASSERT_TRUE(get.has_value());
        EXPECT_NE(std::string_view(*get).substr(0, get->find("\r\n")).find(" 200 "), std::string_view::npos);
        check_coding(*get);
        if (coding == ruvia::http_content_coding::identity) {
            EXPECT_EQ(response_body(*get), std::string_view(small_body));
        } else {
            const auto decoded = ruvia::decode_http_content(std::array{coding}, response_body(*get), {.max_decoded_bytes_ = small_body.size()});
            const auto* body = std::get_if<std::string>(&decoded);
            ASSERT_NE(body, nullptr);
            EXPECT_EQ(*body, small_body);
        }
    }
    server.finish();
}

// Compression removes the declared Content-Length, so a length-delimited
// stream falls back to chunked framing of the encoded bytes.
TEST(Ruvia, http1_compressed_stream_with_declared_length_falls_back_to_chunked) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const auto response = fetch_response(server.endpoint(), get_request("/compression-stream-declared", "Accept-Encoding: gzip\r\n"));
    server.finish();
    ASSERT_TRUE(response.has_value());
    const auto headers = head_fields(*response);
    EXPECT_TRUE(response->starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(headers, "content-encoding: gzip"));
    EXPECT_TRUE(has_field(headers, "transfer-encoding: chunked"));
    EXPECT_FALSE(has_field(headers, "content-length:"));
}

TEST(Ruvia, http1_compressed_stream_peer_reset_allows_following_response) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const auto failures_before = ruvia::app().stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        constexpr std::string_view request = "GET /compression-stream-held HTTP/1.0\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::string prefix;
        asio::read_until(client, asio::dynamic_buffer(prefix), "\r\n\r\n");
        ASSERT_TRUE(has_field(head_fields(prefix), "content-encoding: gzip"));
        if (prefix.size() == prefix.find("\r\n\r\n") + 4) {
            char byte;
            asio::read(client, asio::buffer(&byte, 1));
        }
        client.set_option(asio::socket_base::linger(true, 0));
        client.close();
    }
    server.finish();

    const auto response = fetch_response(server.endpoint(), "GET /compression-stream HTTP/1.0\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n");
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(has_field(head_fields(*response), "content-encoding: gzip"));
    const auto decoded = ruvia::decode_http_content(std::array{ruvia::http_content_coding::gzip}, response_body(*response), {.max_decoded_bytes_ = small_body.size()});
    const auto* body = std::get_if<std::string>(&decoded);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(*body, small_body);
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before);
}

TEST(Ruvia, http1_request_decoding_offloads_small_and_large_encoded_inputs) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const bool large_encoded : {false, true}) {
        const auto plain = large_encoded ? make_body(512 * 1024, 5) : std::string(96 * 1024, 'x');
        const auto encoded = ruvia::encode_http_content(ruvia::http_content_coding::gzip, plain, {.max_encoded_bytes_ = plain.size() + 4096});
        ASSERT_TRUE(encoded.has_value());
        EXPECT_EQ(encoded->size() > 64 * 1024, large_encoded);
        std::string request("POST /compression-echo-decoded HTTP/1.1\r\nHost: localhost\r\nContent-Encoding: gzip\r\nContent-Length: ");
        request.append(std::to_string(encoded->size()));
        request.append("\r\nConnection: close\r\n\r\n");
        request.append(*encoded);
        const auto response = fetch_response(server.endpoint(), request);
        ASSERT_TRUE(response.has_value());
        EXPECT_TRUE(response->starts_with("HTTP/1.1 200 "));
        EXPECT_EQ(response_body(*response), plain);
    }
    server.finish();
}

TEST(Ruvia, http1_inline_compression_reuses_worker_encoders_with_identical_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::gzip, ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    // Each encoder serves several responses of alternating sizes in a row.
    for (int round = 0; round < 2; ++round) {
        for (const auto coding : codings) {
            for (const bool large : {true, false, true}) {
                const std::string_view path = large ? "/compression-large" : "/compression-small";
                const std::string_view body = large ? large_body : small_body;
                const auto token = ruvia::http_content_coding_token(coding);
                const auto output = fetch_response(server.endpoint(), "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: " + std::string(token) + "\r\nConnection: close\r\n\r\n");
                EXPECT_TRUE(output.has_value());
                const auto text = (output ? std::string_view(*output) : std::string_view{});
                EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
                EXPECT_TRUE(lower_head(text).find("\r\ncontent-encoding: " + std::string(token) + "\r\n") != std::string::npos);
                const auto separator = text.find("\r\n\r\n");
                EXPECT_TRUE(separator != std::string::npos);
                if (separator != std::string::npos) {
                    const auto expected = expected_encoding(coding, body);
                    EXPECT_TRUE(!expected.empty());
                    EXPECT_TRUE(std::string_view(text).substr(separator + 4) == expected);
                }
            }
        }
    }
    server.finish();
}

// context::file and static_file serve fixed file representations: a small
// file's bytes travel in memory on GET but still are not runtime-compressed,
// so GET and HEAD carry the same identity fields (RFC 9110 9.3.2).
TEST(Ruvia, http1_file_representations_are_not_runtime_compressed) {
    namespace fs = std::filesystem;
    std::error_code ignored;
    fs::remove_all(file_dir(), ignored);
    fs::create_directories(file_dir());
    for (const auto* name : {"file.txt", "resident.txt"}) {
        std::ofstream(file_dir() / name, std::ios::binary) << small_body;
    }
    resident_root = std::make_unique<ruvia::static_root>(file_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 16 * 1024,
            .resident_budget_bytes_ = 16 * 1024,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view path : {"/compression-file", "/compression-static"}) {
        const auto fetch = [&](std::string_view method) {
            return fetch_response(server.endpoint(),
                std::string(method) + " " + std::string(path) +
                    " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: "
                    "gzip\r\nConnection: close\r\n\r\n")
                .value_or(std::string{});
        };
        const auto get = fetch("GET");
        const auto head = fetch("HEAD");
        EXPECT_TRUE(get.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
        const auto get_fields = head_fields(get);
        EXPECT_TRUE(get_fields == head_fields(head));
        EXPECT_TRUE(!has_field(get_fields, "content-encoding:"));
        EXPECT_TRUE(has_field(get_fields, "content-length: " + std::string(std::to_string(small_body.size()))));
        EXPECT_TRUE(has_field(get_fields, "etag: \""));
        // static_file negotiates indexed precompressed variants; context::file
        // has no coding dimension at all.
        EXPECT_EQ(has_field(get_fields, "vary: accept-encoding"), path == "/compression-static");
        const auto separator = get.find("\r\n\r\n");
        EXPECT_TRUE(separator != std::string::npos);
        if (separator != std::string::npos) {
            EXPECT_TRUE(std::string_view(get).substr(separator + 4) == small_body);
        }
    }
    server.finish();
    resident_root.reset();
    fs::remove_all(file_dir(), ignored);
}

// RFC 9530 §2/§3: Content-Digest and Repr-Digest are computed over the content
// as sent, including its content coding, so the framework must not encode a
// body whose integrity fields the application already computed.
TEST(Ruvia, http1_compression_keeps_responses_with_integrity_fields_identity) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view field : {"Content-Digest", "Repr-Digest", "Content-MD5"}) {
        const auto text = fetch_response(server.endpoint(), get_request("/compression-digest?field=" + std::string(field), "Accept-Encoding: gzip\r\n")).value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        const auto fields = head_fields(text);
        EXPECT_TRUE(!has_field(fields, "content-encoding:"));
        EXPECT_TRUE(has_field(fields, "content-length: " + std::string(std::to_string(small_body.size()))));
        EXPECT_TRUE(response_body(text) == small_body);
    }
    server.finish();
}

// RFC 9110 §12.5.3: a client that refuses identity accepts a listed coding, so a
// body below min_bytes_ is encoded instead of answered with 406.
TEST(Ruvia, http1_compression_encodes_small_bodies_when_identity_is_refused) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const auto refused = fetch_response(server.endpoint(), get_request("/compression-tiny", "Accept-Encoding: gzip, identity;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(refused.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(refused), "content-encoding: gzip"));
    const auto decoded = ruvia::decode_http_content(std::array{ruvia::http_content_coding::gzip}, response_body(refused), {.max_decoded_bytes_ = 64});
    const auto* plain = std::get_if<std::string>(&decoded);
    EXPECT_TRUE(plain != nullptr && std::string_view(*plain) == "hello");

    // With identity acceptable, the size threshold still keeps it identity.
    const auto accepted = fetch_response(server.endpoint(), get_request("/compression-tiny", "Accept-Encoding: gzip\r\n")).value_or(std::string{});
    EXPECT_TRUE(accepted.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(!has_field(head_fields(accepted), "content-encoding:"));
    EXPECT_TRUE(response_body(accepted) == "hello");
    server.finish();
}

// RFC 9110 §8.8.3 / §14.3: a coded representation weakens a strong ETag, drops
// a value that is not an entity-tag, and stops advertising identity ranges.
TEST(Ruvia, http1_compression_rewrites_validators_and_range_support) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const struct {
        std::string_view query_;
        std::optional<std::string_view> expected_;
    } cases[] = {
        {"%22v1%22", "etag: w/\"v1\""},
        {"W%2F%22v2%22", "etag: w/\"v2\""},
        {"abc", std::nullopt},
        {"%22unterminated", std::nullopt},
    };
    for (const auto& test : cases) {
        const auto text = fetch_response(server.endpoint(), get_request("/compression-validators?etag=" + std::string(test.query_), "Accept-Encoding: gzip\r\n")).value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        const auto fields = head_fields(text);
        EXPECT_TRUE(has_field(fields, "content-encoding: gzip"));
        EXPECT_TRUE(!has_field(fields, "accept-ranges:"));
        if (test.expected_.has_value()) {
            EXPECT_TRUE(has_field(fields, *test.expected_));
        } else {
            EXPECT_TRUE(!has_field(fields, "etag:"));
        }
    }

    // Identity keeps both fields untouched.
    const auto identity = fetch_response(server.endpoint(), get_request("/compression-validators?etag=%22v1%22", "Accept-Encoding: identity\r\n")).value_or(std::string{});
    EXPECT_TRUE(has_field(head_fields(identity), "etag: \"v1\""));
    EXPECT_TRUE(has_field(head_fields(identity), "accept-ranges: bytes"));
    server.finish();
}

// One allowlist decides compressibility: already-compressed and unknown media
// types, and a response without Content-Type, stay identity.
TEST(Ruvia, http1_compression_encodes_only_compressible_media_types) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const struct {
        std::string_view query_;
        bool encoded_;
    } cases[] = {
        {"application%2Fjson", true},
        {"application%2Fld%2Bjson", true},
        {"image%2Fsvg%2Bxml", true},
        {"font%2Fwoff2", false},
        {"application%2Fepub%2Bzip", false},
        {"application%2Fx-7z-compressed", false},
        {"application%2Fx-unknown", false},
        {"", false},
    };
    for (const auto& test : cases) {
        const auto text = fetch_response(server.endpoint(), get_request("/compression-media?type=" + std::string(test.query_), "Accept-Encoding: gzip\r\n")).value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        EXPECT_EQ(has_field(head_fields(text), "content-encoding: gzip"), test.encoded_);
        if (!test.encoded_) {
            EXPECT_TRUE(response_body(text) == small_body);
        }
    }
    server.finish();
}

// RFC 9110 §12.5.5: an application-encoded body is sent or refused (406)
// depending on Accept-Encoding, so the 200 varies on it like a framework coding.
TEST(Ruvia, http1_application_encoded_responses_vary_on_accept_encoding) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view path : {std::string_view("/compression-custom-coding"), std::string_view("/compression-stream-custom")}) {
        const auto text = fetch_response(server.endpoint(), get_request(path, "Accept-Encoding: x-custom\r\n")).value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        const auto fields = head_fields(text);
        EXPECT_TRUE(has_field(fields, "content-encoding: x-custom"));
        EXPECT_TRUE(has_field(fields, "vary: accept-encoding"));
    }
    server.finish();
}

// RFC 9110 §8.4 / §14.6: a multipart/byteranges body cannot carry the coding of
// the representation it slices, so several ranges of a precompressed variant
// are answered with the full coded representation; one range stays a 206.
TEST(Ruvia, http1_precompressed_variant_answers_multiple_ranges_in_full) {
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia-response-compression-coded");
    const auto& dir = directory.path();
    const std::string sidecar("gz-sidecar-bytes");
    std::ofstream(dir / "asset.txt", std::ios::binary) << "plain-asset-body";
    std::ofstream(dir / "asset.txt.gz", std::ios::binary) << sidecar;
    coded_root = std::make_unique<ruvia::static_root>(dir,
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .range_requests_ = ruvia::static_range_request_policy::honor,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const auto multi = fetch_response(server.endpoint(), get_request("/compression-coded-static", "Accept-Encoding: gzip\r\nRange: bytes=0-1,4-5\r\n")).value_or(std::string{});
    EXPECT_TRUE(multi.starts_with("HTTP/1.1 200 "));
    const auto multi_fields = head_fields(multi);
    EXPECT_TRUE(has_field(multi_fields, "content-encoding: gzip"));
    EXPECT_TRUE(!has_field(multi_fields, "content-type: multipart/byteranges"));
    EXPECT_TRUE(response_body(multi) == sidecar);

    const auto single = fetch_response(server.endpoint(), get_request("/compression-coded-static", "Accept-Encoding: gzip\r\nRange: bytes=0-1\r\n")).value_or(std::string{});
    EXPECT_TRUE(single.starts_with("HTTP/1.1 206 "));
    const auto single_fields = head_fields(single);
    EXPECT_TRUE(has_field(single_fields, "content-encoding: gzip"));
    EXPECT_TRUE(has_field(single_fields, "content-range: bytes 0-1/" + std::string(std::to_string(sidecar.size()))));
    EXPECT_TRUE(response_body(single) == "gz");

    // RFC 9110 §15.5.17: the empty 416 does not describe coded or typed content.
    for (const std::string_view coding : {"gzip", "gzip, identity;q=0"}) {
        const auto unsatisfied = fetch_response(server.endpoint(), get_request("/compression-coded-static", "Accept-Encoding: " + std::string(coding) + "\r\nRange: bytes=100-200\r\n")).value_or(std::string{});
        EXPECT_TRUE(unsatisfied.starts_with("HTTP/1.1 416 "));
        const auto unsatisfied_fields = head_fields(unsatisfied);
        EXPECT_TRUE(!has_field(unsatisfied_fields, "content-encoding:"));
        EXPECT_TRUE(!has_field(unsatisfied_fields, "content-type:"));
        EXPECT_TRUE(has_field(unsatisfied_fields, "content-range: bytes */" + std::string(std::to_string(sidecar.size()))));
        EXPECT_TRUE(has_field(unsatisfied_fields, "accept-ranges: bytes"));
        EXPECT_TRUE(has_field(unsatisfied_fields, "etag:"));
        EXPECT_TRUE(has_field(unsatisfied_fields, "vary: accept-encoding"));
    }

    // The identity representation still answers several ranges with multipart.
    const auto identity = fetch_response(server.endpoint(), get_request("/compression-coded-static", "Accept-Encoding: identity\r\nRange: bytes=0-1,4-5\r\n")).value_or(std::string{});
    EXPECT_TRUE(identity.starts_with("HTTP/1.1 206 "));
    EXPECT_TRUE(has_field(head_fields(identity), "content-type: multipart/byteranges"));
    EXPECT_TRUE(identity.find("\r\nContent-Encoding:") == std::string::npos);
    server.finish();
    coded_root.reset();
}

// RFC 9110 §8.6: a HEAD response's Content-Length equals the GET content length
// or is absent. An explicit HEAD route that only declares the GET length must
// get GET's coding without the encoded length of its own empty body.
TEST(Ruvia, http1_explicit_head_route_matches_get_coding_without_false_length) {
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view fields : {"Accept-Encoding: gzip\r\n", "Accept-Encoding: gzip, identity;q=0\r\n"}) {
        const auto get = fetch_response(server.endpoint(), get_request("/compression-head", fields)).value_or(std::string{});
        EXPECT_TRUE(has_field(head_fields(get), "content-encoding: gzip"));
        const auto head = fetch_response(server.endpoint(), head_request("/compression-head", fields)).value_or(std::string{});
        EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
        const auto head_line_fields = head_fields(head);
        EXPECT_TRUE(has_field(head_line_fields, "content-encoding: gzip"));
        EXPECT_TRUE(!has_field(head_line_fields, "content-length:"));
        EXPECT_TRUE(response_body(head).empty());
    }
    // Without a declared length, identity stays and no length is claimed.
    const auto bare = fetch_response(server.endpoint(), head_request("/compression-head-bare", "Accept-Encoding: gzip\r\n")).value_or(std::string{});
    EXPECT_TRUE(bare.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(!has_field(head_fields(bare), "content-encoding:"));
    EXPECT_TRUE(!has_field(head_fields(bare), "content-length:"));
    const auto refused = fetch_response(server.endpoint(), head_request("/compression-head-bare", "Accept-Encoding: gzip, identity;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(refused.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(refused), "content-encoding: gzip"));
    EXPECT_TRUE(!has_field(head_fields(refused), "content-length:"));
    // Identity accepted with an identity GET keeps the declared length.
    const auto identity = fetch_response(server.endpoint(), head_request("/compression-head", "Accept-Encoding: identity\r\n")).value_or(std::string{});
    EXPECT_TRUE(has_field(head_fields(identity), "content-length: " + std::string(std::to_string(small_body.size()))));
    server.finish();
}

TEST(Ruvia, http1_head_fallback_on_empty_get_files_and_redirects_matches_get_headers) {
    namespace fs = std::filesystem;
    std::error_code ignored;
    fs::remove_all(file_dir(), ignored);
    fs::create_directories(file_dir());
    {
        std::ofstream(file_dir() / "zero.txt", std::ios::binary);
        std::ofstream(file_dir() / "file.txt", std::ios::binary) << small_body;
    }
    resident_root = std::make_unique<ruvia::static_root>(file_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 16 * 1024,
            .resident_budget_bytes_ = 16 * 1024,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());

    // 1. HEAD fallback on 0-byte GET route matches GET under both identity and refused identity.
    for (const std::string_view fields : {std::string_view("Accept-Encoding: identity\r\n"), std::string_view("Accept-Encoding: gzip, identity;q=0\r\n")}) {
        const auto get = fetch_response(server.endpoint(), get_request("/compression-empty-get", fields)).value_or(std::string{});
        const auto head = fetch_response(server.endpoint(), head_request("/compression-empty-get", fields)).value_or(std::string{});
        EXPECT_TRUE(get.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(head_fields(get) == head_fields(head));
        EXPECT_TRUE(response_body(head).empty());
    }

    // 2. 0-byte file, 0-byte static_file, and redirect emit Content-Length: 0 on both GET and HEAD.
    for (const std::string_view path : {std::string_view("/compression-zero-file"), std::string_view("/compression-zero-static"), std::string_view("/compression-redirect")}) {
        const auto get = fetch_response(server.endpoint(), get_request(path, "")).value_or(std::string{});
        const auto head = fetch_response(server.endpoint(), head_request(path, "")).value_or(std::string{});
        EXPECT_TRUE(head_fields(get) == head_fields(head));
        EXPECT_TRUE(has_field(head_fields(head), "content-length: 0"));
        EXPECT_TRUE(response_body(head).empty());
    }

    // 3. Explicit HEAD route with comma-separated identical Content-Length ("3000, 3000").
    const auto comma_head = fetch_response(server.endpoint(), head_request("/compression-head-comma", "Accept-Encoding: gzip\r\n")).value_or(std::string{});
    EXPECT_TRUE(comma_head.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(comma_head), "content-encoding: gzip"));
    EXPECT_TRUE(!has_field(head_fields(comma_head), "content-length:"));

    // 4. Explicit HEAD route with Content-Length: 0 keeps Content-Length: 0 when identity is accepted.
    const auto zero_declared_get = fetch_response(server.endpoint(), get_request("/compression-head-zero", "Accept-Encoding: gzip\r\n")).value_or(std::string{});
    const auto zero_declared_head = fetch_response(server.endpoint(), head_request("/compression-head-zero", "Accept-Encoding: gzip\r\n")).value_or(std::string{});
    EXPECT_TRUE(head_fields(zero_declared_get) == head_fields(zero_declared_head));
    EXPECT_TRUE(has_field(head_fields(zero_declared_head), "content-length: 0"));
    for (const std::string_view path : {std::string_view("/compression-head-zero"), std::string_view("/compression-zero-declared")}) {
        const auto refused_zero_get = fetch_response(server.endpoint(), get_request(path, "Accept-Encoding: gzip, identity;q=0\r\n")).value_or(std::string{});
        const auto refused_zero_head = fetch_response(server.endpoint(), head_request(path, "Accept-Encoding: gzip, identity;q=0\r\n")).value_or(std::string{});
        EXPECT_TRUE(head_fields(refused_zero_get) == head_fields(refused_zero_head));
        EXPECT_TRUE(has_field(head_fields(refused_zero_head), "content-encoding: gzip"));
        EXPECT_TRUE(has_field(head_fields(refused_zero_head), "content-length: 20"));
    }

    // 5. Custom Content-Encoding negotiation and mixed rejected coding stacks.
    const auto custom_ok = fetch_response(server.endpoint(), get_request("/compression-custom-coding", "Accept-Encoding: x-custom, identity;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(custom_ok.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(custom_ok), "content-encoding: x-custom"));

    const auto mixed_rejected = fetch_response(server.endpoint(), get_request("/compression-mixed-rejected", "Accept-Encoding: br;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(mixed_rejected.starts_with("HTTP/1.1 406 "));
    const auto stream_custom_ok = fetch_response(server.endpoint(), get_request("/compression-stream-custom", "Accept-Encoding: x-custom, identity;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(stream_custom_ok.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(stream_custom_ok), "content-encoding: x-custom"));

    const auto stream_custom_rejected = fetch_response(server.endpoint(), get_request("/compression-stream-custom", "Accept-Encoding: gzip\r\n")).value_or(std::string{});
    EXPECT_TRUE(stream_custom_rejected.starts_with("HTTP/1.1 406 "));

    const auto stream_mixed_rejected = fetch_response(server.endpoint(), get_request("/compression-stream-mixed", "Accept-Encoding: br, x-custom;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(stream_mixed_rejected.starts_with("HTTP/1.1 406 "));

    const auto stream_204 = fetch_response(server.endpoint(), get_request("/compression-stream-204", "Accept-Encoding: *;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(stream_204.starts_with("HTTP/1.1 204 "));

    const auto stream_304 = fetch_response(server.endpoint(), get_request("/compression-stream-304", "Accept-Encoding: *;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(stream_304.starts_with("HTTP/1.1 304 "));

    const auto range_416_custom = fetch_response(server.endpoint(), get_request("/compression-file", "Range: bytes=10000-20000\r\nAccept-Encoding: x-custom, identity;q=0\r\n")).value_or(std::string{});
    EXPECT_TRUE(range_416_custom.starts_with("HTTP/1.1 416 "));

    // 6. Request Content-Encoding with >8 identity tokens does not fail with 415.
    const auto id9 = fetch_response(server.endpoint(),
        "POST /compression-echo-decoded HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Encoding: identity, identity, identity, identity, identity, identity, identity, identity, identity\r\n"
        "Content-Length: 5\r\nConnection: close\r\n\r\nhello")
                         .value_or(std::string{});
    EXPECT_TRUE(id9.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response_body(id9) == "hello");

    const auto gz_hello = *ruvia::encode_http_content(ruvia::http_content_coding::gzip, "hello", {.max_encoded_bytes_ = 1024});
    const auto id8_gz = fetch_response(server.endpoint(),
        "POST /compression-echo-decoded HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Encoding: identity, identity, identity, identity, identity, identity, identity, identity, gzip\r\n"
        "Content-Length: " +
            std::string(std::to_string(gz_hello.size())) + "\r\nConnection: close\r\n\r\n" + std::string(gz_hello))
                            .value_or(std::string{});
    EXPECT_TRUE(id8_gz.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response_body(id8_gz) == "hello");

    server.finish();
    resident_root.reset();
    fs::remove_all(file_dir(), ignored);
}
