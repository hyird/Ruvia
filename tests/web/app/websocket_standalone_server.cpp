#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/read_until.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_server.h"

#include "http_server_fixture.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"

#include "http_server_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_test_client.h"
#endif

namespace standalone_websocket_test {
using namespace std::chrono_literals;

struct target final {
    std::string origin_;
    std::string authority_;
    ruvia::http_client_versions versions_;
    ruvia::http_protocol_version version_;
    asio::ip::tcp::endpoint endpoint_;
};

// What the handler observed once its session ended.
struct session_record final {
    ruvia::websocket_termination termination_{};
    std::uint64_t received_{};
    std::uint64_t sent_{};
};

struct observation final {
    unsigned entered_{};
    unsigned retired_{};
    unsigned refused_{};
    unsigned exclusive_{};
    unsigned cancelled_{};
    std::string_view scenario_{"startup"};
    std::optional<ruvia::task<std::optional<ruvia::websocket_message>>> expired_read_;
    std::optional<ruvia::websocket> expired_session_;
    std::optional<ruvia::task<ruvia::websocket_accept_result>> expired_accept_;
    std::map<std::string, ruvia::websocket_rejection, std::less<>> rejections_;
    std::map<std::string, session_record, std::less<>> sessions_;

    void record(std::string_view path, const ruvia::websocket& websocket) {
        sessions_.insert_or_assign(std::string(path),
            session_record{websocket.termination(), websocket.bytes_received(), websocket.bytes_sent()});
    }
    [[nodiscard]] session_record session(std::string_view path) const {
        const auto found = sessions_.find(path);
        if (found == sessions_.end()) {
            throw std::runtime_error("WebSocket session outcome was not recorded");
        }
        return found->second;
    }
};

struct request_retirement final {
    observation& observed_;
    explicit request_retirement(observation& observed)
        : observed_(observed) {
        ++observed_.entered_;
    }
    ~request_retirement() {
        ++observed_.retired_;
    }
};

ruvia::task<void> expect_read_interrupted(
    ruvia::task<std::optional<ruvia::websocket_message>> read, std::errc expected, bool& interrupted) {
    try {
        (void)co_await std::move(read);
    } catch (const std::system_error& error) {
        interrupted = error.code() == expected;
    }
}

ruvia::task<void> read_to_end(ruvia::websocket& websocket, bool& ended) {
    ended = !(co_await websocket.read()).has_value();
}

ruvia::task<void> read_operation_to_end(ruvia::task<std::optional<ruvia::websocket_message>> operation, bool& ended) {
    ended = !(co_await std::move(operation)).has_value();
}

// Reads interrupted by their own deadline or stop request keep the session;
// close(4001) then completes the handshake under a pending read.
ruvia::task<void> close_under_pending_read(ruvia::websocket& websocket, const ruvia::worker_ref& worker) {
    const auto start = co_await websocket.read();
    EXPECT_TRUE(start && start->payload() == "start");
    bool timed_out = false;
    co_await expect_read_interrupted(websocket.read({.timeout_ = 20ms}), std::errc::timed_out, timed_out);
    bool cancelled = false;
    {
        ruvia::stop_source stop;
        ruvia::task_scope reads(worker);
        reads.spawn(expect_read_interrupted(
            websocket.read({.stop_token_ = stop.token()}), std::errc::operation_canceled, cancelled));
        stop.request_stop();
        co_await reads.join();
    }
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::none);
    co_await websocket.text("interrupted");
    const auto resumed = co_await websocket.read();
    EXPECT_TRUE(resumed && resumed->payload() == "resume");
    bool ended = false;
    ruvia::task_scope reads(worker);
    reads.spawn(read_to_end(websocket, ended));
    std::exception_ptr failure;
    try {
        co_await websocket.close({.code_ = ruvia::websocket_close_code::from_value(4001), .reason_ = "token expired"});
    } catch (...) {
        failure = std::current_exception();
        websocket.abort();
    }
    co_await reads.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(ended);
}

constexpr std::size_t fragment_count = 4;

[[nodiscard]] std::string fragment_payload(std::size_t index) {
    return std::string(3000, static_cast<char>('a' + index));
}

// Sends each message in fragment_count frames with a Ping between the first
// two, uncompressed and then compressed.
ruvia::task<void> send_fragmented(ruvia::websocket& websocket) {
    for (const bool compress : {false, true}) {
        auto message = websocket.begin_text({.compress_ = compress});
        bool data_lane_held = false;
        try {
            co_await websocket.text("overlap");
        } catch (const std::logic_error&) {
            data_lane_held = true;
        }
        EXPECT_TRUE(data_lane_held);
        for (std::size_t index = 0; index + 1 < fragment_count; ++index) {
            co_await message.write(fragment_payload(index));
            if (index == 0) {
                co_await websocket.ping("between");
            }
        }
        co_await message.finish(fragment_payload(fragment_count - 1));
    }
}

// The identical business coroutine runs at both real ends, irrespective of role.
ruvia::task<void> exchange_fragmented(ruvia::websocket& websocket) {
    co_await send_fragmented(websocket);
    std::string expected;
    for (std::size_t i = 0; i < fragment_count; ++i) {
        expected += fragment_payload(i);
    }
    for (unsigned i = 0; i != 2; ++i) {
        const auto message = co_await websocket.read();
        EXPECT_TRUE(message && message->payload() == expected);
    }
    co_await websocket.close();
    EXPECT_EQ(websocket.peer_close_code(), 1000);
}

// Reassembles one message from fragment frames, counting frames and the Ping.
ruvia::task<std::string> receive_fragmented(auto& peer, bool compressed, std::size_t& data_frames, std::size_t& pings) {
    std::string payload;
    for (;;) {
        auto frame = co_await peer.read_frame();
        if (!frame) {
            throw std::runtime_error("missing message fragment");
        }
        if (frame->opcode_ == 9U) {
            EXPECT_TRUE(frame->final_);
            ++pings;
            continue;
        }
        EXPECT_EQ(frame->opcode_, data_frames == 0 ? 1U : 0U);
        EXPECT_EQ(frame->compressed_, compressed && data_frames == 0);
        ++data_frames;
        payload += frame->payload_;
        if (frame->final_) {
            break;
        }
    }
    if (!compressed) {
        co_return payload;
    }
    ruvia::websocket_deflate_codecs codecs;
    std::string inflated;
    EXPECT_TRUE(codecs.inflate_message(payload, inflated, ruvia::protocol_byte_limit::unlimited()) ==
                ruvia::websocket_inflate_result::ok);
    co_return std::string(inflated);
}

ruvia::task<void> serve(ruvia::request_stream& request, observation& observed) {
    request_retirement retirement(observed);
    const auto path = request.request().path();
    if (path == "/forbidden") {
        request.draft().status(ruvia::http_status::forbidden);
        (void)co_await request.response().commit();
        co_await request.response().end();
        try {
            (void)co_await request.accept_websocket();
        } catch (const std::logic_error&) {
            ++observed.exclusive_;
        }
        co_return;
    }
    if (path == "/cold") {
        observed.expired_accept_.emplace(request.accept_websocket());
        request.draft().status(ruvia::http_status::forbidden);
        (void)co_await request.response().commit();
        co_await request.response().end();
        co_return;
    }
    ruvia::websocket_route_config config{
        .subprotocols_ = {"standalone.v1"},
        .require_subprotocol_ = true,
        .lifecycle_ = {.close_handshake_timeout_ = 40ms},
        .deflate_ = {.context_takeover_ = true}};
    if (path == "/heartbeat") {
        config.lifecycle_.heartbeat_ = {.ping_interval_ = 20ms, .pong_timeout_ = 40ms};
    }
    if (path == "/limit") {
        config.max_message_bytes_ = ruvia::protocol_byte_limit::limited(256);
    }
    if (path == "/graceful") {
        config.lifecycle_.close_handshake_timeout_ = 5s;
    }
    {
        auto discarded = request.accept_websocket(config);
    }
    const auto accepted = co_await request.accept_websocket(std::move(config));
    auto* websocket = accepted.session();
    if (websocket == nullptr) {
        ++observed.refused_;
        observed.rejections_.insert_or_assign(std::string(path), *accepted.rejection());
        co_return;
    }
    EXPECT_EQ(websocket->subprotocol(), "standalone.v1");
    try {
        (void)co_await request.accept_tunnel();
    } catch (const std::logic_error&) {
        ++observed.exclusive_;
    }
    try {
        (void)co_await request.accept_websocket();
    } catch (const std::logic_error&) {
        ++observed.exclusive_;
    }
    try {
        (void)co_await request.response().commit();
    } catch (const std::logic_error&) {
        ++observed.exclusive_;
    }
    if (path == "/limit") {
        const auto first = co_await websocket->read();
        EXPECT_TRUE(first.has_value());
        if (first) {
            EXPECT_EQ(first->payload().size(), 256U);
        }
        // 本地接收上限不能限制本会话的发送方向。
        co_await websocket->text(std::string(1024, 'r'), {.compress_ = false});
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/close") {
        co_return;
    }
    if (path == "/moved") {
        observed.expired_session_.emplace(std::move(*websocket));
        co_return;
    }
    if (path == "/shared") {
        co_await exchange_fragmented(*websocket);
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/fragments") {
        co_await send_fragmented(*websocket);
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/assemble") {
        while (const auto message = co_await websocket->read()) {
            co_await websocket->text(std::to_string(message->payload().size()) + (message->binary() ? ":binary" : ":text"));
        }
        co_return;
    }
    if (path == "/graceful") {
        co_await close_under_pending_read(*websocket, request.worker());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/abort") {
        // Abort after the peer has its handshake response.
        EXPECT_TRUE((co_await websocket->read()).has_value());
        websocket->abort();
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/eof" || path == "/invalid-frame" || path == "/invalid-utf8") {
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/count") {
        // Echoes one message after a Ping, then reads until the peer's Close.
        auto message = co_await websocket->read();
        if (!message) {
            throw std::runtime_error("missing counted message");
        }
        const std::string payload(message->payload());
        co_await websocket->ping("abc");
        co_await websocket->text(payload);
        EXPECT_FALSE((co_await websocket->read()).has_value());
        observed.record(path, *websocket);
        co_return;
    }
    if (path == "/fail") {
        throw std::runtime_error("private application failure");
    }
    if (path == "/heartbeat" || path == "/cancel" || path == "/host") {
        try {
            EXPECT_FALSE((co_await websocket->read()).has_value());
        } catch (const std::system_error&) {
        }
        if (path == "/heartbeat") {
            observed.record(path, *websocket);
        }
        if (request.stop_token().stop_requested()) {
            ++observed.cancelled_;
        }
        co_return;
    }
    auto message = co_await websocket->read();
    if (!message) {
        throw std::runtime_error("missing application command");
    }
    if (path == "/wire") {
        EXPECT_EQ(message->payload(), "prebuffered");
        co_await websocket->ping("probe");
        co_await websocket->text(std::string(2048, 'x'));
        auto final = co_await websocket->read();
        EXPECT_FALSE(final.has_value());
        observed.expired_read_.emplace(websocket->read());
        co_return;
    }
    EXPECT_EQ(message->opcode(), ruvia::websocket_opcode::text);
    EXPECT_EQ(message->payload(), std::string(4096, 'z'));
    co_await websocket->text("accepted:4096", {.compress_ = false});
    co_await websocket->binary(std::as_bytes(std::span(std::string_view("binary-response"))));
    co_await websocket->text(std::string(4096, 'x'));
    co_await websocket->ping("server-control");
    message = co_await websocket->read();
    if (!message) {
        throw std::runtime_error("missing binary command");
    }
    EXPECT_EQ(message->opcode(), ruvia::websocket_opcode::binary);
    EXPECT_EQ(message->payload(), std::string_view("\0\1\2\3", 4));
    co_await websocket->text("binary:4");
}

std::string masked_frame(unsigned opcode, std::string_view payload, bool final = true, bool compressed = false) {
    if (payload.size() > 65535) {
        throw std::invalid_argument("test frame payload is too large");
    }
    const std::array<unsigned char, 4> mask{0x37, 0xfa, 0x21, 0x3d};
    std::string bytes;
    bytes.push_back(static_cast<char>((final ? 0x80U : 0U) | (compressed ? 0x40U : 0U) | opcode));
    if (payload.size() <= 125) {
        bytes.push_back(static_cast<char>(0x80U | payload.size()));
    } else {
        bytes.push_back(static_cast<char>(0xfe));
        bytes.push_back(static_cast<char>(payload.size() >> 8U));
        bytes.push_back(static_cast<char>(payload.size() & 0xffU));
    }
    for (const auto byte : mask) {
        bytes.push_back(static_cast<char>(byte));
    }
    for (std::size_t index = 0; index != payload.size(); ++index) {
        bytes.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]));
    }
    return bytes;
}

// The byte peer deliberately has no WebSocket runtime: assertions observe the
// server's actual frame bits, automatic Pong, and deadline transport retirement.
class wire_peer final {
public:
    explicit wire_peer(asio::io_context& io)
        : socket_(io) {}
    ~wire_peer() {
        abort();
    }

    ruvia::task<void> open(ruvia::http_client& client, const target& destination, std::string_view path,
        bool prebuffer = false, bool invalid_version = false) {
        if (destination.version_ != ruvia::http_protocol_version::http11) {
            if (invalid_version) {
                throw std::logic_error("malformed extended CONNECT needs the raw wire fixture");
            }
            result_.emplace(co_await client.client().connect(destination.authority_).protocol("websocket").target(path).header("Sec-WebSocket-Version", "13").header("Sec-WebSocket-Protocol", "standalone.v1").header("Sec-WebSocket-Extensions", "permessage-deflate").open());
            auto* tunnel = result_->tunnel();
            if (tunnel == nullptr) {
                throw std::runtime_error("extended CONNECT was refused");
            }
            EXPECT_EQ(tunnel->status(), ruvia::http_status::ok);
            EXPECT_EQ(tunnel->protocol_version(), destination.version_);
            EXPECT_EQ(tunnel->header("Sec-WebSocket-Protocol"), "standalone.v1");
            EXPECT_TRUE(tunnel->header("Sec-WebSocket-Extensions").has_value());
            if (prebuffer) {
                co_await write(masked_frame(1, "prebuffered"));
            }
            co_return;
        }
        const auto connected = co_await ruvia::async_asio([&](auto completion) {
            socket_.async_connect(destination.endpoint_, std::move(completion));
        });
        if (connected.error_code()) {
            throw std::system_error(connected.error_code());
        }
        std::string head = "GET " + std::string(path) + " HTTP/1.1\r\nHost: " + destination.authority_ +
                           "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: " +
                           (invalid_version ? "12" : "13") +
                           "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: standalone.v1\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n";
        if (prebuffer) {
            head += masked_frame(1, "prebuffered");
        }
        co_await write(head);
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
            asio::async_read_until(socket_, asio::dynamic_buffer(input_), "\r\n\r\n", std::move(completion));
        });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        const auto response = input_.substr(0, read.result());
        if (invalid_version) {
            EXPECT_TRUE(response.starts_with("HTTP/1.1 400 "));
            EXPECT_NE(response.find("Sec-WebSocket-Version: 13"), std::string::npos);
            co_return;
        }
        EXPECT_TRUE(response.starts_with("HTTP/1.1 101 "));
        EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
        EXPECT_NE(response.find("standalone.v1"), std::string::npos);
        EXPECT_NE(response.find("permessage-deflate"), std::string::npos);
        input_.erase(0, read.result());
    }

    ruvia::task<void> write(std::string_view bytes) {
        if (result_) {
            co_await result_->tunnel()->write(bytes);
        } else {
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
                asio::async_write(socket_, asio::buffer(bytes), std::move(completion));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
        }
    }

    struct frame final {
        unsigned opcode_;
        bool compressed_;
        std::string payload_;
        bool final_;
    };

    ruvia::task<std::optional<frame>> read_frame() {
        if (!(co_await fill(2))) {
            co_return std::nullopt;
        }
        const auto first = static_cast<unsigned char>(input_[0]);
        const auto second = static_cast<unsigned char>(input_[1]);
        EXPECT_EQ(second & 0x80U, 0U);
        std::size_t size = second & 0x7fU;
        std::size_t offset = 2;
        if (size == 126) {
            if (!(co_await fill(4))) {
                throw std::runtime_error("truncated frame length");
            }
            size = (static_cast<unsigned char>(input_[2]) << 8U) | static_cast<unsigned char>(input_[3]);
            offset = 4;
        } else if (size == 127) {
            throw std::runtime_error("unexpected huge test frame");
        }
        if (!(co_await fill(offset + size))) {
            throw std::runtime_error("truncated frame payload");
        }
        frame result{first & 0x0fU, (first & 0x40U) != 0, input_.substr(offset, size), (first & 0x80U) != 0};
        input_.erase(0, offset + size);
        co_return result;
    }

    // Ends the peer's sending direction without a Close frame.
    ruvia::task<void> finish() {
        if (result_) {
            co_await result_->tunnel()->finish();
        } else {
            std::error_code ignored;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        }
    }

    void abort() {
        if (result_ && result_->tunnel()) {
            result_->tunnel()->abort();
        }
        ruvia::close_socket(socket_);
    }

private:
    ruvia::task<bool> fill(std::size_t bytes) {
        while (input_.size() < bytes) {
            if (result_) {
                auto chunk = co_await result_->tunnel()->text();
                if (!chunk) {
                    co_return false;
                }
                input_.append(*chunk);
            } else {
                std::array<char, 4096> storage;
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
                    socket_.async_read_some(asio::buffer(storage), std::move(completion));
                });
                if (read.result() != 0) {
                    input_.append(storage.data(), read.result());
                }
                if (read.error_code() == asio::error::eof) {
                    co_return input_.size() >= bytes;
                }
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
            }
        }
        co_return true;
    }

    asio::ip::tcp::socket socket_;
    std::optional<ruvia::http_client_tunnel_result> result_;
    std::string input_;
};

ruvia::task<void> await_retirement(ruvia::http_server& server, const observation& observed) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (observed.entered_ != observed.retired_) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("WebSocket request did not retire");
        }
        (void)co_await ruvia::sleep_for(server.worker(), 1ms);
    }
}

// Typed CONNECT senders enforce RFC 6455's client-side version requirement.
// Malformed bootstrap coverage must bypass those senders, not weaken them.
void reject_invalid_extended_connect(const target& destination, const std::filesystem::path& ca_file) {
#if defined(RUVIA_ENABLE_HTTP2)
    if (destination.version_ == ruvia::http_protocol_version::http2) {
        asio::io_context io;
        asio::ssl::context tls(asio::ssl::context::tls_client);
        tls.load_verify_file(ca_file.string());
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
        stream.set_verify_mode(asio::ssl::verify_peer);
        stream.set_verify_callback(asio::ssl::host_name_verification("127.0.0.1"));
        constexpr std::array<unsigned char, 3> alpn{2, 'h', '2'};
        if (SSL_set_alpn_protos(stream.native_handle(), alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
            throw std::runtime_error("cannot configure malformed HTTP/2 peer ALPN");
        }
        stream.next_layer().connect(destination.endpoint_);
        stream.handshake(asio::ssl::stream_base::client);
        std::string wire(ruvia::http2_client_preface);
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        asio::write(stream, asio::buffer(wire));
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        for (;;) {
            const auto frame = ruvia::test::read_frame(stream, deadline);
            if (frame.header_.type_ == static_cast<unsigned>(ruvia::http2_frame_type::settings) &&
                (frame.header_.flags_ & 1U) == 0) {
                break;
            }
        }
        wire.clear();
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 1, 0, {});
        std::string block;
        for (const auto& field : std::array<std::pair<std::string_view, std::string_view>, 7>{{{":method", "CONNECT"}, {":protocol", "websocket"}, {":scheme", "https"},
                 {":authority", destination.authority_}, {":path", "/invalid-version"},
                 {"sec-websocket-version", "12"}, {"sec-websocket-protocol", "standalone.v1"}}}) {
            ruvia::hpack_encoder::encode_header(block, field.first, field.second);
        }
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, 1, block);
        asio::write(stream, asio::buffer(wire));
        block.clear();
        ruvia::hpack_decoder decoder;
        bool ended = false;
        for (;;) {
            const auto frame = ruvia::test::read_frame(stream, deadline);
            if (frame.header_.stream_id_ != 1) {
                continue;
            }
            const auto type = frame.header_.type_;
            if (type != static_cast<unsigned>(ruvia::http2_frame_type::headers) &&
                type != static_cast<unsigned>(ruvia::http2_frame_type::continuation)) {
                throw std::runtime_error("malformed HTTP/2 bootstrap did not receive response HEADERS");
            }
            if (type == static_cast<unsigned>(ruvia::http2_frame_type::headers)) {
                ended = (frame.header_.flags_ & 1U) != 0;
            }
            block.append(frame.payload_);
            if ((frame.header_.flags_ & 4U) == 0) {
                continue;
            }
            bool status = false;
            bool version = false;
            const auto error = decoder.decode(block, [&](std::string_view name, std::string_view value) {
                if (name == ":status") {
                    EXPECT_EQ(value, "400");
                    status = true;
                } else if (name == "sec-websocket-version") {
                    EXPECT_EQ(value, "13");
                    version = true;
                }
                return true;
            });
            EXPECT_FALSE(error.has_value());
            EXPECT_TRUE(status);
            EXPECT_TRUE(version);
            while (!ended) {
                const auto body = ruvia::test::read_frame(stream, deadline);
                if (body.header_.stream_id_ != 1) {
                    continue;
                }
                if (body.header_.type_ != static_cast<unsigned>(ruvia::http2_frame_type::data)) {
                    throw std::runtime_error("malformed HTTP/2 bootstrap response did not complete normally");
                }
                ended = (body.header_.flags_ & 1U) != 0;
            }
            return;
        }
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    if (destination.version_ == ruvia::http_protocol_version::http3) {
        const auto port = static_cast<std::uint16_t>(std::stoul(destination.authority_.substr(destination.authority_.rfind(':') + 1)));
        // The public client validates Sec-WebSocket-Version: 13 before sending.
        ruvia::test::http3_raw_request_client peer({asio::ip::address_v4::loopback(), port}, ca_file, "127.0.0.1", 3s);
        const auto response = peer.request({{":method", "CONNECT"}, {":protocol", "websocket"}, {":scheme", "https"},
                                               {":authority", destination.authority_}, {":path", "/invalid-version"},
                                               {"sec-websocket-version", "12"}, {"sec-websocket-protocol", "standalone.v1"}},
            false);
        EXPECT_EQ(response.status_, 400);
        const auto version = response.header("sec-websocket-version");
        ASSERT_TRUE(version.has_value());
        EXPECT_EQ(*version, "13");
        return;
    }
#endif
    throw std::logic_error("malformed extended CONNECT needs HTTP/2 or HTTP/3");
}

ruvia::task<void> exercise(ruvia::http_client& client, const target& destination,
    ruvia::http_server& server, observation& observed, asio::io_context& io, const std::filesystem::path& ca_file) {
    for (unsigned mode = 0; mode != 3; ++mode) {
        observed.scenario_ = "/limit";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/limit");
        ruvia::websocket_deflate_codecs codecs;
        const auto message_frame = [&](std::string_view payload, bool compressed) {
            if (!compressed) {
                return masked_frame(1, payload);
            }
            std::string encoded;
            if (!codecs.deflate_message(payload, encoded, 6, 15)) {
                throw std::runtime_error("test message compression failed");
            }
            EXPECT_LT(encoded.size(), 256U);
            return masked_frame(1, encoded, true, true);
        };
        const auto first = message_frame(std::string(256, 'a'), mode == 1);
        co_await peer.write(first);
        const auto response = co_await peer.read_frame();
        if (!response) {
            throw std::runtime_error("missing response above the receive-only limit");
        }
        EXPECT_EQ(response->opcode_, 1U);
        EXPECT_FALSE(response->compressed_);
        EXPECT_EQ(response->payload_, std::string(1024, 'r'));
        std::string oversized;
        if (mode == 0) {
            // 只发送声明257字节的帧头；不得等待或预分配完整正文才拒绝。
            oversized.assign("\x81\xfe\x01\x01\0\0\0\0", 8);
        } else if (mode == 1) {
            // 压缩消息线面小于256字节，解压输出仍必须在本地限额处拒绝。
            oversized = message_frame(std::string(8192, 'a'), true);
        } else {
            // 每片都合法，跨分片累计消息超过256字节时拒绝。
            const std::string fragment(100, 'a');
            oversized = masked_frame(1, fragment, false);
            oversized += masked_frame(0, fragment, false);
            oversized += masked_frame(0, fragment);
        }
        co_await peer.write(oversized);
        // 协议失败允许服务器发Close后直接终结传输（RFC6455 §7.1.7）；
        // 观察真实1009帧，不启动客户端自动Close回复写入已终结的隧道。
        const auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing receive-limit Close");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_FALSE(close->compressed_);
        EXPECT_EQ(close->payload_, std::string("\x03\xf1", 2));
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/limit").termination_ == ruvia::websocket_termination::message_too_large);
    }
    {
        observed.scenario_ = "/shared";
        auto websocket = co_await client.client().websocket("/shared").subprotocol("standalone.v1").deflate({.compression_ = {.enabled_ = true}}).connect();
        co_await exchange_fragmented(websocket);
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/moved";
        auto websocket = co_await client.client().websocket("/moved").subprotocol("standalone.v1").connect();
        EXPECT_FALSE((co_await websocket.read()).has_value());
        co_await await_retirement(server, observed);
        bool expired = false;
        try {
            (void)co_await observed.expired_session_->read();
        } catch (const std::logic_error&) {
            expired = true;
        }
        EXPECT_TRUE(expired);
        observed.expired_session_.reset();
    }
    {
        observed.scenario_ = "/exchange";
        auto websocket = co_await client.client().websocket("/exchange").subprotocol("standalone.v1").deflate({.compression_ = {.enabled_ = true}}).connect();
        EXPECT_EQ(websocket.subprotocol(), "standalone.v1");
        co_await websocket.text(std::string(4096, 'z'));
        auto message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing command response");
        }
        EXPECT_EQ(message->payload(), "accepted:4096");
        message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing binary response");
        }
        EXPECT_EQ(message->opcode(), ruvia::websocket_opcode::binary);
        EXPECT_EQ(message->payload(), "binary-response");
        message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing compressed response");
        }
        EXPECT_EQ(message->payload(), std::string(4096, 'x'));
        co_await websocket.ping("client-control");
        const std::array<std::byte, 4> command{std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}};
        co_await websocket.binary(command, {.compress_ = false});
        message = co_await websocket.read();
        if (!message) {
            throw std::runtime_error("missing binary command response");
        }
        EXPECT_EQ(message->payload(), "binary:4");
        co_await websocket.close();
        EXPECT_EQ(websocket.peer_close_code(), 1000);
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/fragments";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/fragments");
        std::string expected;
        for (std::size_t index = 0; index != fragment_count; ++index) {
            expected += fragment_payload(index);
        }
        for (const bool compressed : {false, true}) {
            std::size_t data_frames = 0;
            std::size_t pings = 0;
            const auto payload = co_await receive_fragmented(peer, compressed, data_frames, pings);
            EXPECT_EQ(payload, expected);
            EXPECT_EQ(data_frames, fragment_count);
            EXPECT_EQ(pings, 1U);
        }
        co_await peer.write(masked_frame(8, std::string("\x03\xe8", 2)));
        const auto close = co_await peer.read_frame();
        EXPECT_TRUE(close && close->opcode_ == 8U);
        co_await await_retirement(server, observed);
        const auto counted = observed.session("/fragments");
        // Two messages of fragment_count frames, the Pings and the Close echo.
        EXPECT_GT(counted.sent_, 2 * fragment_payload(0).size());
    }
    {
        observed.scenario_ = "/assemble";
        auto websocket = co_await client.client().websocket("/assemble").subprotocol("standalone.v1").deflate({.compression_ = {.enabled_ = true}}).connect();
        for (const bool compress : {false, true}) {
            const auto sent_before = websocket.bytes_sent();
            auto message = websocket.begin_binary({.compress_ = compress});
            auto moved = std::move(websocket);
            bool data_lane_held = false;
            try {
                co_await moved.binary(std::as_bytes(std::span(std::string_view("overlap"))));
            } catch (const std::logic_error&) {
                data_lane_held = true;
            }
            EXPECT_TRUE(data_lane_held);
            for (std::size_t index = 0; index + 1 < fragment_count; ++index) {
                co_await message.write(fragment_payload(index));
                if (index == 0) {
                    co_await moved.ping("between");
                }
                EXPECT_GT(moved.bytes_sent(), sent_before);
            }
            co_await message.finish(fragment_payload(fragment_count - 1));
            const auto reply = co_await moved.read();
            EXPECT_TRUE(reply && reply->payload() == std::to_string(fragment_count * fragment_payload(0).size()) + ":binary");
            websocket = std::move(moved);
        }
        {
            // An unfinished message cannot be completed later, so dropping it ends the session.
            auto abandoned = websocket.begin_text();
            co_await abandoned.write("partial");
        }
        EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::aborted);
        co_await await_retirement(server, observed);
    }
    {
        observed.scenario_ = "/graceful";
        auto original = co_await client.client().websocket("/graceful").subprotocol("standalone.v1").connect();
        auto cold_read = original.read({.timeout_ = 20ms});
        auto websocket = std::move(original);
        // The server stays silent until "start", so these client reads can
        // only end by their own deadline or stop request.
        bool timed_out = false;
        try {
            (void)co_await std::move(cold_read);
        } catch (const std::system_error& error) {
            timed_out = error.code() == std::errc::timed_out;
        }
        bool cancelled = false;
        {
            ruvia::stop_source stop;
            ruvia::task_scope reads(websocket.worker());
            reads.spawn(expect_read_interrupted(websocket.read({.stop_token_ = stop.token()}), std::errc::operation_canceled, cancelled));
            auto moved = std::move(websocket);
            stop.request_stop();
            co_await reads.join();
            websocket = std::move(moved);
        }
        EXPECT_TRUE(timed_out);
        EXPECT_TRUE(cancelled);
        EXPECT_TRUE(websocket.connected());
        co_await websocket.text("start");
        const auto interrupted = co_await websocket.read();
        EXPECT_TRUE(interrupted && interrupted->payload() == "interrupted");
        co_await websocket.text("resume");
        EXPECT_FALSE((co_await websocket.read()).has_value());
        EXPECT_EQ(websocket.peer_close_code(), 4001);
        EXPECT_EQ(websocket.peer_close_reason(), "token expired");
        EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::peer_close);
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/graceful").termination_ == ruvia::websocket_termination::local_close);
    }
    // Masked text (2 + 4 + 5), automatic Pong to Ping "abc" (2 + 4 + 3) and a
    // Close carrying 1000 (2 + 4 + 2) up; Ping (2 + 3), the echo (2 + 5) and
    // the Close echo (2 + 2) down. Deflated payloads count as compressed.
    for (const bool deflate : {false, true}) {
        observed.scenario_ = "/count";
        const std::string payload = deflate ? std::string(4096, 'c') : std::string("hello");
        auto request = client.client().websocket("/count");
        request.subprotocol("standalone.v1");
        if (deflate) {
            request.deflate({.compression_ = {.enabled_ = true}});
        }
        auto websocket = co_await request.connect();
        co_await websocket.text(payload);
        const auto echo = co_await websocket.read();
        EXPECT_TRUE(echo && echo->payload() == payload);
        co_await websocket.close();
        EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::local_close);
        co_await await_retirement(server, observed);
        const auto counted = observed.session("/count");
        EXPECT_TRUE(counted.termination_ == ruvia::websocket_termination::peer_close);
        EXPECT_EQ(websocket.bytes_sent(), counted.received_);
        EXPECT_EQ(websocket.bytes_received(), counted.sent_);
        if (deflate) {
            EXPECT_LT(websocket.bytes_sent(), payload.size());
            EXPECT_LT(websocket.bytes_received(), payload.size());
        } else {
            EXPECT_EQ(websocket.bytes_sent(), 11U + 9U + 8U);
            EXPECT_EQ(websocket.bytes_received(), 5U + 7U + 4U);
        }
    }
    for (const auto path : {"/forbidden", "/missing-subprotocol", "/cold"}) {
        observed.scenario_ = path;
        bool rejected = false;
        try {
            auto websocket = co_await client.client().websocket(path).connect();
        } catch (const ruvia::http_client_error& error) {
            rejected = true;
            EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::handshake_rejected));
            EXPECT_EQ(error.status(), path == std::string_view("/missing-subprotocol") ? ruvia::http_status::bad_request : ruvia::http_status::forbidden);
        }
        EXPECT_TRUE(rejected);
    }
    EXPECT_TRUE(observed.expired_accept_.has_value());
    observed.scenario_ = "expired accept";
    if (observed.expired_accept_) {
        bool expired = false;
        try {
            (void)co_await std::move(*observed.expired_accept_);
        } catch (const std::logic_error&) {
            expired = true;
        }
        observed.expired_accept_.reset();
        EXPECT_TRUE(expired);
    }
    observed.scenario_ = "/invalid-version";
    if (destination.version_ == ruvia::http_protocol_version::http11) {
        wire_peer invalid(io);
        co_await invalid.open(client, destination, "/invalid-version", false, true);
    } else {
        auto rejection = std::async(std::launch::async, [&] {
            reject_invalid_extended_connect(destination, ca_file);
        });
        while (rejection.wait_for(0s) != std::future_status::ready) {
            (void)co_await ruvia::sleep_for(server.worker(), 1ms);
        }
        rejection.get();
    }
    co_await await_retirement(server, observed);
    {
        // The callback reads the refusal the server sends: RFC 6455 section
        // 4.4 answers an unsupported version with 400.
        const auto found = observed.rejections_.find("/invalid-version");
        EXPECT_TRUE(found != observed.rejections_.end());
        if (found != observed.rejections_.end()) {
            EXPECT_EQ(found->second.status_, ruvia::http_status::bad_request);
            EXPECT_EQ(found->second.code_, "unsupported_websocket_version");
        }
        const auto subprotocol = observed.rejections_.find("/missing-subprotocol");
        EXPECT_TRUE(subprotocol != observed.rejections_.end());
        if (subprotocol != observed.rejections_.end()) {
            EXPECT_EQ(subprotocol->second.status_, ruvia::http_status::bad_request);
            EXPECT_EQ(subprotocol->second.code_, "unsupported_websocket_subprotocol");
        }
        observed.rejections_.clear();
    }
    {
        observed.scenario_ = "/wire";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/wire", true);
        auto ping = co_await peer.read_frame();
        if (!ping) {
            throw std::runtime_error("missing server Ping");
        }
        EXPECT_EQ(ping->opcode_, 9U);
        EXPECT_EQ(ping->payload_, "probe");
        co_await peer.write(masked_frame(10, ping->payload_));
        auto compressed = co_await peer.read_frame();
        if (!compressed) {
            throw std::runtime_error("missing deflated frame");
        }
        EXPECT_EQ(compressed->opcode_, 1U);
        EXPECT_TRUE(compressed->compressed_);
        EXPECT_LT(compressed->payload_.size(), 2048U);
        co_await peer.write(masked_frame(9, "wire-control"));
        auto pong = co_await peer.read_frame();
        if (!pong) {
            throw std::runtime_error("missing automatic Pong");
        }
        EXPECT_EQ(pong->opcode_, 10U);
        EXPECT_FALSE(pong->compressed_);
        EXPECT_EQ(pong->payload_, "wire-control");
        co_await peer.write(masked_frame(8, std::string_view("\x03\xe8", 2)));
        auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing peer Close acknowledgement");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_EQ(close->payload_, std::string_view("\x03\xe8", 2));
    }
    co_await await_retirement(server, observed);
    EXPECT_TRUE(observed.expired_read_.has_value());
    observed.scenario_ = "expired read";
    if (observed.expired_read_) {
        bool expired = false;
        try {
            (void)co_await std::move(*observed.expired_read_);
        } catch (const std::logic_error&) {
            expired = true;
        }
        observed.expired_read_.reset();
        EXPECT_TRUE(expired);
    }
    {
        observed.scenario_ = "/fail";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/fail");
        const auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing failed-handler Close");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_EQ(close->payload_, std::string_view("\x03\xf3"
                                                    "internal server error",
                                       23));
        co_await peer.write(masked_frame(8, std::string_view("\x03\xf3", 2)));
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/cancel";
        std::optional<ruvia::websocket> websocket;
        websocket.emplace(co_await client.client().websocket("/cancel").subprotocol("standalone.v1").connect());
        auto cold = websocket->read();
        bool ended = false;
        ruvia::task_scope reads(client.worker());
        reads.spawn(read_operation_to_end(websocket->read(), ended));
        websocket.reset();
        co_await reads.join();
        EXPECT_TRUE(ended);
        bool expired = false;
        try {
            (void)co_await std::move(cold);
        } catch (const std::logic_error&) {
            expired = true;
        }
        EXPECT_TRUE(expired);
        co_await await_retirement(server, observed);
    }
    for (const auto path : {"/heartbeat", "/close"}) {
        observed.scenario_ = path;
        wire_peer peer(io);
        co_await peer.open(client, destination, path);
        const auto start = std::chrono::steady_clock::now();
        auto frame = co_await peer.read_frame();
        if (!frame) {
            throw std::runtime_error("missing liveness control frame");
        }
        EXPECT_EQ(frame->opcode_, path == std::string_view("/heartbeat") ? 9U : 8U);
        bool retired = false;
        try {
            retired = !(co_await peer.read_frame()).has_value();
        } catch (const std::system_error&) {
            retired = true;
        }
        EXPECT_TRUE(retired);
        EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
        co_await await_retirement(server, observed);
        if (path == std::string_view("/heartbeat")) {
            EXPECT_TRUE(observed.session(path).termination_ == ruvia::websocket_termination::timeout);
        }
    }
    {
        observed.scenario_ = "/cancel";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/cancel");
        peer.abort();
    }
    co_await await_retirement(server, observed);
    {
        observed.scenario_ = "/abort";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/abort");
        co_await peer.write(masked_frame(1, "abort"));
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/abort").termination_ == ruvia::websocket_termination::aborted);
    }
    {
        observed.scenario_ = "/eof";
        wire_peer peer(io);
        co_await peer.open(client, destination, "/eof");
        co_await peer.finish();
        co_await await_retirement(server, observed);
        EXPECT_TRUE(observed.session("/eof").termination_ == ruvia::websocket_termination::eof);
    }
    // An unmasked client frame fails as 1002, a non-UTF-8 text message as 1007.
    for (const auto path : {"/invalid-frame", "/invalid-utf8"}) {
        observed.scenario_ = path;
        const bool unmasked = path == std::string_view("/invalid-frame");
        wire_peer peer(io);
        co_await peer.open(client, destination, path);
        co_await peer.write(unmasked ? std::string("\x81\x01x", 3) : masked_frame(1, "\xff"));
        const auto close = co_await peer.read_frame();
        if (!close) {
            throw std::runtime_error("missing protocol failure Close");
        }
        EXPECT_EQ(close->opcode_, 8U);
        EXPECT_EQ(close->payload_, unmasked ? std::string("\x03\xea", 2) : std::string("\x03\xef", 2));
        co_await await_retirement(server, observed);
        const auto failed = observed.session(path);
        EXPECT_TRUE(failed.termination_ == (unmasked ? ruvia::websocket_termination::protocol_error : ruvia::websocket_termination::invalid_payload_data));
        EXPECT_EQ(failed.received_, unmasked ? 0U : 7U);
        EXPECT_EQ(failed.sent_, 4U);
    }
}
}  // namespace standalone_websocket_test

namespace standalone_websocket_test {
void exercise_protocol(ruvia::http_client_versions protocol) {
    asio::io_context io;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &preparation});
    ruvia::test::tcp_port_reservation plain(io), secure(io);
    ruvia::test::udp_port_reservation udp(io);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    observation observed;
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_server server(attachment.loop(), {.request_stream_ = [&](ruvia::request_stream& request) {
            return serve(request, observed);
        }},
            {.connection_scan_interval_ = 5ms});
        server.listen({.address_ = "127.0.0.1", .http_ = plain.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::request_stream});
        ruvia::listen_config tls{.address_ = "127.0.0.1", .https_ = secure.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .dispatch_ = ruvia::listener_dispatch::request_stream};
#if defined(RUVIA_ENABLE_HTTP3)
        tls.http3_ = udp.endpoint().port();
#endif
        server.listen(std::move(tls));
        std::exception_ptr failure;
        try {
            co_await server.start();
            std::vector<target> destinations{{"http://127.0.0.1:" + std::to_string(plain.endpoint().port()),
                "127.0.0.1:" + std::to_string(plain.endpoint().port()), ruvia::http_client_versions{.http2_ = false},
                ruvia::http_protocol_version::http11, plain.endpoint()}};
#if defined(RUVIA_ENABLE_HTTP2)
            destinations.push_back({"https://127.0.0.1:" + std::to_string(secure.endpoint().port()),
                "127.0.0.1:" + std::to_string(secure.endpoint().port()), ruvia::http_client_versions{.http1_ = false},
                ruvia::http_protocol_version::http2, secure.endpoint()});
#endif
#if defined(RUVIA_ENABLE_HTTP3)
            destinations.push_back({"https://127.0.0.1:" + std::to_string(udp.endpoint().port()),
                "127.0.0.1:" + std::to_string(udp.endpoint().port()), ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true},
                ruvia::http_protocol_version::http3, {}});
#endif
            for (const auto& destination : destinations) {
                if (destination.versions_ != protocol) continue;
                ruvia::http_client_config config{.base_url_ = destination.origin_, .versions_ = destination.versions_};
                config.tls_.ca_file_ = identity.ca_file_;
                ruvia::http_client client(attachment.loop(), config);
                std::exception_ptr exchange_failure;
                try {
                    co_await exercise(client, destination, server, observed, io, identity.ca_file_);
                    observed.scenario_ = "/host";
                    wire_peer pending(io);
                    co_await pending.open(client, destination, "/host");
                    // Stopping this host must wake the accepted WebSocket read,
                    // retire its borrowed facade, and join every scanner write.
                    server.close();
                    co_await server.shutdown();
                    EXPECT_EQ(observed.entered_, observed.retired_);
                    EXPECT_EQ(server.stats().active_connections_, 0U);
                } catch (...) {
                    exchange_failure = std::current_exception();
                    try {
                        std::rethrow_exception(exchange_failure);
                    } catch (const std::exception& error) {
                        std::string detail = std::string(observed.scenario_) + ": " + error.what();
                        if (const auto* client_error = dynamic_cast<const ruvia::http_client_error*>(&error)) {
                            detail += " [client code=" + std::to_string(client_error->code().value());
                            if (const auto status = client_error->status()) {
                                detail += ", status=" + std::to_string(status->value());
                            }
                            detail += "]";
                        } else if (const auto* system_error = dynamic_cast<const std::system_error*>(&error)) {
                            detail += std::string(" [") + system_error->code().category().name() +
                                      ":" + std::to_string(system_error->code().value()) + "]";
                        }
                        detail += " [entered=" + std::to_string(observed.entered_) +
                                  ", retired=" + std::to_string(observed.retired_) + "]";
                        exchange_failure = std::make_exception_ptr(std::runtime_error(std::move(detail)));
                    } catch (...) {
                        exchange_failure = std::current_exception();
                    }
                }
                co_await client.shutdown();
                if (exchange_failure) std::rethrow_exception(exchange_failure);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await server.shutdown();
        EXPECT_EQ(observed.entered_, observed.retired_);
        EXPECT_GT(observed.refused_, 0U);
        EXPECT_GE(observed.exclusive_, 4U);
        EXPECT_GT(observed.cancelled_, 0U);
        attachment.stop();
        if (failure) std::rethrow_exception(failure);
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}
}  // namespace standalone_websocket_test

TEST(Ruvia, independent_http1_websocket_handshake_frames_liveness_and_retirement) {
    standalone_websocket_test::exercise_protocol(ruvia::http_client_versions{.http2_ = false});
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, independent_http2_websocket_handshake_frames_liveness_and_retirement) {
    standalone_websocket_test::exercise_protocol(ruvia::http_client_versions{.http1_ = false});
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, independent_http3_websocket_handshake_frames_liveness_and_retirement) {
    standalone_websocket_test::exercise_protocol(ruvia::http_client_versions{.http1_ = false, .http2_ = false, .http3_ = true});
}
#endif
