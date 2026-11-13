// Streaming: streaming request bodies, typed multipart chunk phases, chunked
// response streaming and server-sent events.
// Run ruvia_example_streaming on port 8082. Try:
// curl -N http://127.0.0.1:8082/streaming/events
// curl --data-binary @file.bin http://127.0.0.1:8082/streaming/upload/raw
// curl -F file=@file.bin http://127.0.0.1:8082/streaming/upload/multipart
// A chunk borrows reusable reader storage; consume it before the next read.

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "ruvia/core/event_loop.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "backend_config.h"
class streaming_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/streaming")

    RUVIA_ROUTES_BEGIN(streaming_controller)
    RUVIA_POST("/upload/raw", upload_raw, ruvia::stream_body);
    RUVIA_POST("/upload/multipart", upload_multipart, ruvia::stream_body);
    RUVIA_GET("/chunks", chunks);
    RUVIA_GET("/events", events_value);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> upload_raw(ruvia::context& c) {
        std::size_t bytes_value = 0;
        auto& reader_value = c.req().body_reader();
        // Each read waits at most 30 s for the next chunk; a timeout throws
        // std::system_error(std::errc::timed_out) and keeps the body readable.
        while (auto chunk = co_await reader_value.read({.timeout_ = std::chrono::seconds(30)})) {
            bytes_value += chunk->size();
        }

        std::string body;
        body.append("uploaded bytes=");
        ruvia::http_append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<ruvia::http_response> upload_multipart(ruvia::context& c) {
        std::size_t parts = 0;
        std::size_t bytes_value = 0;
        auto reader_value = c.req().multipart_reader();
        while (auto part = co_await reader_value.read()) {
            if (part->phase() == ruvia::multipart_chunk_phase::first ||
                part->phase() == ruvia::multipart_chunk_phase::complete) {
                ++parts;
            }
            bytes_value += part->body().size();
        }

        std::string body;
        body.append("multipart parts=");
        ruvia::http_append_unsigned(body, parts);
        body.append(" bytes=");
        ruvia::http_append_unsigned(body, bytes_value);
        body.push_back('\n');
        co_return c.text(std::move(body));
    }

    ruvia::task<void> chunks(ruvia::context& c) {
        auto& stream = c.stream_text();
        if (co_await stream.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await stream.write("part 1\n");
        co_await stream.writeln("part 2");
        if (co_await stream.sleep(std::chrono::milliseconds(20)) ==
            ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!stream.aborted()) {
            co_await stream.write("part 3\n");
        }
    }

    ruvia::task<void> events_value(ruvia::context& c) {
        auto events_value = c.stream_sse();
        if (co_await events_value.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        // 客户端断开（FIN/RST）会停止 c.stop_token()；把它传给业务订阅等待。
        const auto stop = c.stop_token();
        co_await events_value.write({.data_ = "connected", .event_ = "open", .id_ = "1"});
        if (co_await ruvia::sleep_for(c.worker(), std::chrono::milliseconds(20), stop) ==
            ruvia::timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!events_value.aborted()) {
            co_await events_value.write({.data_ = "heartbeat",
                .event_ = "tick",
                .id_ = "2",
                .retry_ = std::chrono::milliseconds{3000}});
        }
    }
};

int main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8082})
            .server({
                .worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                .max_buffered_body_bytes_ = 16 * 1024 * 1024,
                .max_stream_body_bytes_ = std::nullopt,
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
}

// HTTP/1, HTTP/2 and HTTP/3 server features through the Web API.
// Run ruvia_example_http_features, then query it with any HTTP/1.1, HTTP/2 or HTTP/3 client.
// Plain HTTP listens on 127.0.0.1:8093 and accepts HTTP/2 prior knowledge.
// Set RUVIA_TLS_CERT and RUVIA_TLS_KEY to enable HTTPS and HTTP/3 on port 8444;
// the certificate must cover localhost.

namespace http_features_example {

class feature_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/features")
    RUVIA_ROUTES_BEGIN(feature_controller)
    RUVIA_GET("/info", info);
    RUVIA_GET("/asset", asset);
    RUVIA_GET("/safe", safe);
    RUVIA_GET("/redirect", redirect);
    RUVIA_GET("/download", download);
    RUVIA_POST("/upload", upload, ruvia::stream_body, ruvia::body_limit<1024 * 1024>);
    RUVIA_GET_WS("/ws", websocket);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> info(ruvia::context& c) {
        const auto connection = c.conn();
        const std::array<ruvia::http_header_view, 1> hints{{
            {"link", "</features/asset>; rel=preload; as=style"},
        }};
        // An interim head is separate from the final response. The Web API
        // copies its input, but the operation must still finish in this request.
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints, hints));
        c.draft().priority({.urgency_ = 2, .incremental_ = true});
        const auto protocol = c.req().protocol_version();
        if ((protocol == ruvia::http_protocol_version::http2 ||
                protocol == ruvia::http_protocol_version::http3) &&
            connection.tls()) {
            // Advertisements never change the client's trusted origin.
            const std::array<std::string_view, 1> origins{"https://localhost:8444"};
            c.advertise_origins(origins);
            if (protocol == ruvia::http_protocol_version::http2) {
                c.advertise_alternative_service("h2=\":8444\"; ma=60");
            }
        }
        if (protocol == ruvia::http_protocol_version::http2) {
            // Peer settings or capacity can decline push; the asset remains
            // available through an ordinary request.
            const bool pushed = c.push({
                .scheme_ = connection.tls() ? "https" : "http",
                .authority_ = c.req().header("host").value_or(connection.tls() ? "localhost:8444" : "localhost:8093"),
                .path_ = "/features/asset",
            });
            c.draft().header("X-Push-Accepted", pushed ? "yes" : "no");
        }
        co_return c.text("interim responses, priorities, advertisements, and push\n");
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& c) {
        c.draft().header("content-type", "text/css");
        co_return c.body(ruvia::static_text("body { color: #234; }\n"));
    }

    ruvia::task<ruvia::http_response> safe(ruvia::context& c) {
        co_return c.text("read-only resource\n");
    }

    ruvia::task<ruvia::http_response> redirect(ruvia::context& c) {
        co_return c.redirect({.location_ = "/features/safe", .status_ = ruvia::http_status::see_other});
    }

    ruvia::task<void> download(ruvia::context& c) {
        auto& output = c.stream_text();
        c.draft().header("Trailer", "X-Example-Complete");
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await output.write("first\n");
        // Await each write for bounded backpressure. No subsequent header or
        // cookie mutation is allowed once the first write publishes the head.
        co_await output.write("second\n");
        const std::array<ruvia::http_header_view, 1> trailers{{{"X-Example-Complete", "yes"}}};
        co_await output.end(trailers);
    }

    ruvia::task<ruvia::http_response> upload(ruvia::context& c) {
        std::size_t bytes_value = 0;
        auto& body = c.req().body_reader();
        while (const auto chunk = co_await body.read()) {
            bytes_value += chunk->size();
        }
        // Request trailers are available only after the request body reaches EOF.
        std::string result_value;
        result_value.append("uploaded=").append(std::to_string(bytes_value));
        result_value.append(" trailers=").append(std::to_string(c.req().trailers().size()));
        result_value.push_back('\n');
        co_return c.text(std::move(result_value));
    }

    ruvia::task<void> websocket(ruvia::context& c) {
        auto& socket = c.websocket();
        while (auto message = co_await socket.read()) {
            // Borrow the payload for the echo; send() holds the read lane until
            // the frame is written, so the next read cannot replace it early.
            co_await socket.send(std::move(*message));
        }
    }
};

}  // namespace http_features_example

[[maybe_unused]] int run_http_features_main() {
    using namespace http_features_example;
    try {
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        ruvia::listen_config listener_value{.address_ = "127.0.0.1", .http_ = 8093};
        const auto certificate = env_value.get("RUVIA_TLS_CERT");
        const auto key = env_value.get("RUVIA_TLS_KEY");
        if (certificate.has_value() != key.has_value()) {
            throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
        }
        if (certificate && key) {
            listener_value.https_ = 8444;
#if defined(RUVIA_ENABLE_HTTP3)
            listener_value.http3_ = 8444;
#endif
            listener_value.tls_.identity_ = {
                .certificate_chain_file_ = std::filesystem::path(*certificate),
                .private_key_file_ = std::filesystem::path(*key),
            };
        }
        app.server({.worker_count_ = 2,
                       .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                       .compression_ = ruvia::compression_config{.min_bytes_ = 16}})
            .listen(std::move(listener_value))
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// HTTP/1 and HTTP/2 CONNECT byte streams and CONNECT-UDP capsules.
// Run ruvia_example_tunnels for the server on port 8094; RUVIA_TLS_CERT/KEY
// enables TLS on port 8445. Any CONNECT or CONNECT-UDP (RFC 9298) client can
// drive it. The server echoes locally and does not forward to an arbitrary host.

namespace tunnels_example {

class tunnel_controller final {
public:
    RUVIA_ROUTES_BEGIN(tunnel_controller)
    RUVIA_GET("/health", health);
    // datagrams_ declares the Capsule Protocol (Capsule-Protocol: ?1 on the
    // 2xx, and the tunnel's udp()/datagrams()/capsules() facades); connect-udp
    // implies it, so here it only documents the route's protocol.
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::seconds(2), .datagrams_ = true};
    RUVIA_CONNECT("echo.example:443", bytes);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/udp/:host/:port", udp, options);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.text("ready\n");
    }

    ruvia::task<void> bytes(ruvia::context& c) {
        auto& tunnel = c.tunnel();
        while (auto chunk = co_await tunnel.read()) {
            // 借用块保持有效至下一次 read；write 在返回任务前拥有化输入。
            co_await tunnel.write(*chunk);
        }
        co_await tunnel.finish();
    }

    ruvia::task<void> udp(ruvia::context& c) {
        // This protocol route has already negotiated CONNECT-UDP. Capsule
        // limits can be passed directly to udp(http_capsule_config).
        auto stream = c.tunnel().udp();
        while (auto packet = co_await stream.read()) {
            // The adapter handles context ID 0 and ignores unknown contexts.
            // A zero-byte payload is a valid UDP packet, not an EOF marker.
            co_await stream.send(packet->payload());
        }
        co_await stream.finish();
    }
};

}  // namespace tunnels_example

[[maybe_unused]] int run_tunnels_main() {
    using namespace tunnels_example;
    try {
        std::cout << std::unitbuf;
        auto& app = ruvia::app();
        app.load_dotenv();
        const example::environment env_value(&app.env());
        ruvia::listen_config listener_value{.address_ = "127.0.0.1", .http_ = 8094};
        const auto cert = env_value.get("RUVIA_TLS_CERT");
        const auto key = env_value.get("RUVIA_TLS_KEY");
        if (cert.has_value() != key.has_value()) {
            throw std::invalid_argument("set both RUVIA_TLS_CERT and RUVIA_TLS_KEY");
        }
        if (cert && key) {
            listener_value.https_ = 8445;
            listener_value.tls_.identity_ = {
                .certificate_chain_file_ = std::filesystem::path(*cert),
                .private_key_file_ = std::filesystem::path(*key),
            };
        }
        app.server({.worker_count_ = 2,
                       .process_signal_handlers_ = ruvia::process_signal_handler_policy::install})
            .listen(std::move(listener_value))
            .on_connection_failure([](const ruvia::connection_failure_record& record) noexcept {
                try {
                    std::cerr << "tunnel connection failed: " << ruvia::exception_text(record.exception()) << '\n';
                } catch (...) {
                    std::cerr << "tunnel connection failed\n";
                }
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}

// websocket: upgrade routes, subprotocol options, lifecycle timeouts,
// text/binary echo and the RFC close handshake.
// Run ruvia_example_websocket, then connect to ws://localhost:8084/ws/echo.
// /ws/chat requires one of chat_options()'s advertised subprotocols: an upgrade
// offering none (send Sec-WebSocket-Protocol: chat.v1) is refused with 400.
// /ws/ticker pushes ticks and then closes with 4001 while its reader waits.
// Read until close; join the close handshake before releasing the connection.

class websocket_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/ws")

    RUVIA_ROUTES_BEGIN(websocket_controller)
    const auto chat_options = ruvia::websocket_route_config{
        .subprotocols_ = {"chat.v1"},
        .require_subprotocol_ = true,
        .lifecycle_ =
            {
                .heartbeat_ =
                    {
                        .ping_interval_ = std::chrono::seconds(30),
                        .pong_timeout_ = std::chrono::seconds(10),
                    },
                .close_handshake_timeout_ = std::chrono::seconds(5),
            },
    };
    RUVIA_GET_WS("/echo", echo);
    RUVIA_GET_WS("/chat", chat, chat_options);
    RUVIA_GET_WS("/ticker", ticker);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> echo(ruvia::context& c) {
        auto& ws = c.websocket();
        while (auto message = co_await ws.read()) {
            // Zero-copy echo: the frame borrows the read buffer and keeps the
            // read lane until it is written, preserving the message opcode.
            co_await ws.send(std::move(*message));
        }
    }

    ruvia::task<void> chat(ruvia::context& c) {
        auto& ws = c.websocket();
        co_await ws.text("welcome");
        while (auto message = co_await ws.read()) {
            if (message->text()) {
                co_await ws.send(std::move(*message));
            }
        }
        co_await ws.close({.code_ = ruvia::websocket_close_status::normal_closure, .reason_ = "bye"});
    }

    // close() claims only the write lane, so the pending read below returns
    // nullopt once the peer answers the 4001 Close (or abort() ends it).
    ruvia::task<void> ticker(ruvia::context& c) {
        auto& ws = c.websocket();
        ruvia::task_scope readers(c.worker());
        readers.spawn(drain(ws));
        std::exception_ptr failure;
        try {
            for (int tick = 0; tick < 5; ++tick) {
                co_await ws.text("tick");
                (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(1));
            }
            co_await ws.close({.code_ = ruvia::websocket_close_code::from_value(4001), .reason_ = "subscription ended"});
        } catch (...) {
            failure = std::current_exception();
            ws.abort();
        }
        co_await readers.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    static ruvia::task<void> drain(ruvia::websocket& ws) {
        for (;;) {
            try {
                if (!co_await ws.read({.timeout_ = std::chrono::seconds(30)})) {
                    co_return;
                }
            } catch (const std::system_error& error) {
                // A read deadline ends only that read; the session stays open.
                if (error.code() != std::errc::timed_out) {
                    throw;
                }
            }
        }
    }
};

[[maybe_unused]] int run_websocket_main() {
    try {
        ruvia::app()
            .listen({.address_ = "0.0.0.0", .http_ = 8084})
            .server({
                .worker_count_ = 2,
                .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                .max_websocket_message_bytes_ = 16 * 1024 * 1024,
            })
            .run();
    } catch (...) {
        std::cerr << ruvia::exception_text(std::current_exception()) << '\n';
        return 1;
    }
    return 0;
}
