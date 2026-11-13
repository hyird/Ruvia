#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/web/http_client.h"
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"

#include "http_server_fixture.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "ruvia/core/stop_token.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"

namespace http_client_request_controls_test {

class peer final {
public:
    peer(asio::io_context& io, const ruvia::worker_ref& worker, bool http2, bool silent)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          gate_(io),
          done_(worker),
          http2_(http2),
          silent_(silent) {
        gate_.expires_at(std::chrono::steady_clock::time_point::max());
        encoded_ = *ruvia::encode_http_content(ruvia::http_content_coding::gzip, "streamed representation",
            {.max_encoded_bytes_ = 1024});
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port());
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
    void release() {
        gate_.cancel();
    }
    void stop() {
        stopping_ = true;
        std::error_code ignored;
        gate_.cancel();
        socket_.close(ignored);
        acceptor_.close(ignored);
    }
    ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string encoded_{};
    std::string received_{};
    bool final_sent_{};
    bool tail_sent_{};

private:
    asio::awaitable<void> wait_gate() {
        std::error_code ignored;
        co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
    asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        if (http2_) {
#ifdef RUVIA_ENABLE_HTTP2
            co_await run_http2();
#else
            throw std::logic_error("HTTP/2 fixture unavailable");
#endif
        } else {
            co_await asio::async_read_until(socket_, asio::dynamic_buffer(received_), "\r\n\r\n", asio::use_awaitable);
            if (!silent_) {
                const std::string response =
                    "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
                    "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " +
                    std::to_string(encoded_.size()) + "\r\n\r\n";
                co_await asio::async_write(socket_, asio::buffer(response), asio::use_awaitable);
                co_await asio::async_write(socket_, asio::buffer(encoded_.data(), 1), asio::use_awaitable);
                final_sent_ = true;
                co_await wait_gate();
                if (stopping_) {
                    co_return;
                }
                co_await asio::async_write(socket_, asio::buffer(encoded_.data() + 1, encoded_.size() - 1), asio::use_awaitable);
                tail_sent_ = true;
            }
        }
        std::array<char, 4096> input;
        std::error_code error;
        while (!error) {
            co_await socket_.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        }
    }
#ifdef RUVIA_ENABLE_HTTP2
    asio::awaitable<void> run_http2() {
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        EXPECT_EQ(std::string_view(preface.data(), preface.size()), ruvia::http2_client_preface);
        std::string wire;
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::uint32_t stream = 0;
        while (stream == 0) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 request frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                stream = header->stream_id_;
                ruvia::hpack_decoder decoder;
                EXPECT_FALSE(decoder.decode(payload, [this](std::string_view name, std::string_view value) {
                                        received_.append(name).append(": ").append(value).append("\n");
                                        return true;
                                    })
                        .has_value());
            }
        }
        if (silent_) {
            co_return;
        }
        wire.clear();
        std::string fields;
        ruvia::hpack_encoder::encode_header(fields, ":status", "103");
        ruvia::hpack_encoder::encode_header(fields, "link", "</style.css>; rel=preload");
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, stream, fields);
        fields.clear();
        ruvia::hpack_encoder::encode_header(fields, ":status", "200");
        ruvia::hpack_encoder::encode_header(fields, "content-encoding", "gzip");
        ruvia::hpack_encoder::encode_header(fields, "content-length", std::to_string(encoded_.size()));
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, stream, fields);
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 0, stream, std::string_view(encoded_).substr(0, 1));
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        final_sent_ = true;
        co_await wait_gate();
        if (stopping_) {
            co_return;
        }
        wire.clear();
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 1, stream, std::string_view(encoded_).substr(1));
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        tail_sent_ = true;
    }
#endif
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer gate_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool http2_;
    bool silent_;
    bool stopping_{};
    bool finished_{};
};

void raw_stream(bool http2, bool configured_raw) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    peer upstream(io, worker, http2, false);
    const auto url = upstream.url();
    upstream.start();
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = url,
                                                         .decode_content_ = !configured_raw,
                                                         .read_timeout_ = std::chrono::milliseconds(5),
                                                         .request_timeout_ = std::chrono::seconds(3),
                                                         .versions_ = http2 ? ruvia::http_client_versions{.http1_ = false} : ruvia::http_client_versions{.http2_ = false}});
        try {
            bool informed = false;
            auto request = client.client().get("/raw");
            request.header("Content-Type", "unparsed forwarded value")
                .configure({.decode_content_ = configured_raw ? std::optional<bool>{} : std::optional<bool>{false},
                    .field_source_ = ruvia::http_request_field_source::forwarded,
                    .timeouts_ = {.read_ = ruvia::http_client_timeout_override::disabled()},
                    .informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> fields, ruvia::stop_token stop) -> ruvia::task<void> {
                        EXPECT_EQ(status.value(), 103U);
                        EXPECT_EQ(fields.front().value(), "</style.css>; rel=preload");
                        EXPECT_EQ(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20), stop), ruvia::timer_sleep_result::elapsed);
                        informed = true;
                    }});
            auto response = co_await request.send();
            EXPECT_TRUE(informed);
            EXPECT_EQ(response.framing(), http2 ? ruvia::http_client_response_framing::stream : ruvia::http_client_response_framing::fixed_length);
            EXPECT_EQ(response.content_length(), upstream.encoded_.size());
            EXPECT_EQ(response.header("content-encoding"), "gzip");
            EXPECT_TRUE(response.header("content-length").has_value());
            const auto first = co_await response.read();
            EXPECT_TRUE(first.has_value());
            EXPECT_FALSE(upstream.tail_sent_);
            std::string bytes;
            if (first) {
                bytes.append(reinterpret_cast<const char*>(first->data()), first->size());
            }
            EXPECT_EQ(bytes, std::string_view(upstream.encoded_).substr(0, 1));
            (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20));
            upstream.release();
            auto rest = co_await response.bytes();
            bytes.append(rest.text());
            EXPECT_EQ(bytes, upstream.encoded_);
            EXPECT_NE(upstream.received_.find("unparsed forwarded value"), std::string::npos);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        upstream.stop();
        co_await upstream.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void read_timeout(bool http2, bool total_only) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    peer upstream(io, worker, http2, true);
    const auto url = upstream.url();
    upstream.start();
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = url,
                                                         .read_timeout_ = std::chrono::seconds(2),
                                                         .request_timeout_ = total_only ? std::chrono::milliseconds(30) : std::chrono::milliseconds(2000),
                                                         .versions_ = http2 ? ruvia::http_client_versions{.http1_ = false} : ruvia::http_client_versions{.http2_ = false}});
        std::optional<ruvia::http_client_error> observed_error;
        try {
            auto request = client.client().get("/silent");
            request.configure({.timeouts_ = {.read_ = total_only ? ruvia::http_client_timeout_override::disabled()
                                                                 : ruvia::http_client_timeout_override::after(std::chrono::milliseconds(20))}});
            bool timed_out = false;
            try {
                (void)co_await request.send();
            } catch (const ruvia::http_client_error& error) {
                timed_out = error.code() == std::make_error_code(std::errc::timed_out);
                observed_error.emplace(error);
            }
            EXPECT_TRUE(timed_out);
            EXPECT_TRUE(observed_error.has_value());
            if (observed_error) {
                EXPECT_TRUE(observed_error->request_written());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        EXPECT_TRUE(observed_error.has_value());
        if (observed_error) {
            const auto* connection = observed_error->connection_info();
            EXPECT_NE(connection, nullptr);
            if (connection != nullptr) {
                EXPECT_NE(connection->id_, 0u);
                EXPECT_EQ(connection->peer_ip_.to_string(), "127.0.0.1");
                EXPECT_NE(connection->peer_port_, 0u);
                EXPECT_FALSE(connection->reused_);
            }
        }
        upstream.stop();
        co_await upstream.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http_client_request_controls_test

TEST(Ruvia, http_client_http1_raw_stream_forwarded_fields_and_request_timeouts) {
    for (const bool configured_raw : {false, true}) {
        http_client_request_controls_test::raw_stream(false, configured_raw);
    }
    for (const bool total_only : {false, true}) {
        http_client_request_controls_test::read_timeout(false, total_only);
    }
}
#ifdef RUVIA_ENABLE_HTTP2
TEST(Ruvia, http_client_http2_raw_stream_forwarded_fields_and_request_timeouts) {
    for (const bool configured_raw : {false, true}) {
        http_client_request_controls_test::raw_stream(true, configured_raw);
    }
    for (const bool total_only : {false, true}) {
        http_client_request_controls_test::read_timeout(true, total_only);
    }
}

TEST(Ruvia, http_client_http2_informational_backpressure_does_not_block_other_streams) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket socket(io);
    ruvia::worker_signal peer_done(worker);
    bool finished = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        co_await acceptor.async_accept(socket, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket, asio::buffer(preface), asio::use_awaitable);
        std::string wire;
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        unsigned requests = 0;
        while (requests != 2) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid multiplexed frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
            }
            if (header->type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                continue;
            }
            wire.clear();
            std::string fields;
            if (++requests == 1) {
                ruvia::hpack_encoder::encode_header(fields, ":status", "103");
                ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, header->stream_id_, fields);
                fields.clear();
            }
            ruvia::hpack_encoder::encode_header(fields, ":status", "200");
            ruvia::hpack_encoder::encode_header(fields, "content-length", "0");
            ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 5, header->stream_id_, fields);
            co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        }
        std::array<char, 4096> bytes;
        std::error_code error;
        while (!error) {
            co_await socket.async_read_some(asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        finished = true;
        peer_done.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = url,
                                                         .request_timeout_ = std::chrono::seconds(3),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        ruvia::task_scope operations(worker);
        ruvia::worker_signal entered(worker);
        ruvia::stop_source release;
        bool callback_entered = false;
        bool first_returned = false;
        const auto first = [&]() -> ruvia::task<void> {
            try {
                auto request = client.client().get("/first");
                request.configure({.informational_ = [&](ruvia::http_status_code, std::span<const ruvia::http_header>, ruvia::stop_token stop) -> ruvia::task<void> {
                    callback_entered = true;
                    entered.notify();
                    ruvia::stop_group cancellation(stop, release.token());
                    (void)co_await ruvia::sleep_for(worker, std::chrono::hours(1), cancellation.token());
                }});
                auto response = co_await request.send();
                EXPECT_TRUE(response.ok());
                first_returned = true;
            } catch (...) {
                failure = std::current_exception();
                entered.notify();
            }
        };
        operations.spawn(first());
        try {
            while (!callback_entered && !failure) {
                co_await entered.wait();
            }
            if (!failure) {
                auto second = co_await client.client().get("/second").send();
                EXPECT_TRUE(second.ok());
                EXPECT_FALSE(first_returned);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        release.request_stop();
        co_await operations.join();
        co_await client.shutdown();
        std::error_code ignored;
        socket.close(ignored);
        acceptor.close(ignored);
        while (!finished) {
            co_await peer_done.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
}
#endif

TEST(Ruvia, http_client_ipv6_dial_target_preserves_logical_authority) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::acceptor acceptor(io);
    std::error_code ipv6_error;
    acceptor.open(asio::ip::tcp::v6(), ipv6_error);
    if (!ipv6_error) {
        acceptor.bind({asio::ip::address_v6::loopback(), 0}, ipv6_error);
    }
    if (ipv6_error) {
        GTEST_SKIP() << "IPv6 loopback unavailable: " << ipv6_error.message();
    }
    acceptor.listen();
    const auto port = acceptor.local_endpoint().port();
    const auto authority = std::string("[2001:db8::1]:") + std::to_string(port);
    const auto logical_url = "http://" + authority;
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    ruvia::task_scope peers(worker);
    std::exception_ptr failure;
    std::string received;
    const auto peer = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        const auto accepted = co_await ruvia::async_asio<void>([&](auto completion) {
            acceptor.async_accept(socket, std::move(completion));
        });
        if (accepted.error_code()) {
            throw std::system_error(accepted.error_code());
        }
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
            asio::async_read_until(socket, asio::dynamic_buffer(received), "\r\n\r\n", std::move(completion));
        });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        const auto written = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
            asio::async_write(socket, asio::buffer(reply), std::move(completion));
        });
        if (written.error_code()) {
            throw std::system_error(written.error_code());
        }
    };
    const auto run = [&]() -> ruvia::task<void> {
        peers.spawn(peer());
        ruvia::http_client client(attachment.loop(), {.base_url_ = logical_url,
                                                         .dial_host_ = "::1",
                                                         .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            auto response = co_await client.client().get("/").send();
            EXPECT_EQ(response.status().value(), 200);
            EXPECT_NE(received.find(authority), std::string::npos);
            EXPECT_EQ(response.connection_info().peer_ip_.to_string(), "::1");
            EXPECT_EQ(response.connection_info().peer_port_, port);
            EXPECT_EQ(response.content_length(), 0u);
            const auto addresses = client.client().dns_snapshot();
            EXPECT_EQ(addresses.size(), 1u);
            if (!addresses.empty()) {
                EXPECT_EQ(addresses.front().ip_.to_string(), "::1");
                EXPECT_EQ(addresses.front().port_.value(), port);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        co_await peers.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_metadata_values_preserve_strong_ip_identity) {
    const auto parsed_address = ruvia::ip_address::parse("2001:db8:1234:5678:90ab:cdef:1234:5678");
    ASSERT_TRUE(parsed_address.has_value());
    std::optional<ruvia::http_client_error> saved_error;
    {
        ruvia::http_client_connection_info source;
        source.id_ = 17;
        source.peer_ip_ = *parsed_address;
        source.peer_port_ = 443;
        auto copied = source;
        ruvia::http_client_connection_info assigned;
        assigned = source;
        assigned = std::move(copied);
        EXPECT_EQ(assigned.peer_ip_, *parsed_address);

        ruvia::http_client_endpoint_address endpoint(*parsed_address, 443);
        auto copied_endpoint = endpoint;
        ruvia::http_client_endpoint_address assigned_endpoint;
        assigned_endpoint = endpoint;
        assigned_endpoint = std::move(copied_endpoint);
        EXPECT_EQ(assigned_endpoint.ip_, *parsed_address);

        const ruvia::http_client_error error(
            std::make_error_code(std::errc::timed_out), "observed timeout", std::nullopt, &source);
        saved_error.emplace(error);
    }
    const auto* connection = saved_error->connection_info();
    EXPECT_NE(connection, nullptr);
    if (connection != nullptr) {
        EXPECT_EQ(connection->id_, 17u);
        EXPECT_EQ(connection->peer_ip_, *parsed_address);
    }
}

TEST(Ruvia, http_client_dial_and_dns_ports_reject_invalid_original_integer_domain) {
    for (const int invalid : {-1, 65536}) {
        EXPECT_THROW(
            (void)ruvia::http_client_endpoint_address("127.0.0.1", invalid),
            std::invalid_argument);
        ruvia::http_client_endpoint_address endpoint("127.0.0.1", 443);
        EXPECT_THROW(endpoint.port_ = invalid, std::invalid_argument);
        ruvia::http_client_config config{.base_url_ = "http://127.0.0.1:8001"};
        EXPECT_THROW(config.dial_port_ = invalid, std::invalid_argument);
        EXPECT_THROW(
            ((void)ruvia::http_client_config{.base_url_ = "http://127.0.0.1:8001", .dial_port_ = invalid}),
            std::invalid_argument);
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto reject_zero_dial = [&]() {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001", .dial_port_ = 0});
    };
    EXPECT_THROW(reject_zero_dial(), std::invalid_argument);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        const std::array addresses{ruvia::http_client_endpoint_address("127.0.0.1", 0)};
        EXPECT_THROW(client.client().update_dns(addresses), std::invalid_argument);
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, http_client_builders_enforce_consumed_state_websocket_options_and_digest_redirect_strip) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());

    std::string second_hop_request;
    const auto serve_redirect = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        const auto accepted = co_await ruvia::async_asio([&](auto handler) {
            acceptor.async_accept(socket, std::move(handler));
        });
        if (accepted.error_code()) {
            co_return;
        }
        std::string first_hop;
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read_until(socket, asio::dynamic_buffer(first_hop), "\r\n\r\n", std::move(handler));
        });
        // Drain the 4-byte body ("body") if not yet in first_hop.
        const auto header_end = first_hop.find("\r\n\r\n");
        const std::size_t body_have = header_end == std::string::npos ? 0 : first_hop.size() - (header_end + 4);
        if (body_have < 4) {
            std::array<char, 4> rest{};
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read(socket, asio::buffer(rest.data(), 4 - body_have), std::move(handler));
            });
        }
        constexpr std::string_view redirect_reply =
            "HTTP/1.1 303 See Other\r\nLocation: /landed\r\nContent-Length: 0\r\n\r\n";
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(socket, asio::buffer(redirect_reply), std::move(handler));
        });
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read_until(socket, asio::dynamic_buffer(second_hop_request), "\r\n\r\n", std::move(handler));
        });
        constexpr std::string_view final_reply =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(socket, asio::buffer(final_reply), std::move(handler));
        });
    };

    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::task_scope peers(worker);
        peers.spawn(serve_redirect());

        ruvia::http_client client(attachment.loop(), {.base_url_ = url});
        auto builder = client.client().post("/submit");
        builder.header("Content-Type", "text/plain")
            .header("Digest", "SHA-256=abc=")
            .header("Repr-Digest", "sha-256=:abc=:")
            .header("X-Trace", "keep-me")
            .body("body");
        auto pending = builder.send();
        EXPECT_THROW(builder.redirects(1), std::logic_error);
        auto response = co_await std::move(pending);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "ok");

        EXPECT_THROW(
            client.client().websocket("/ws").configure({.dial_port_ = 0}),
            std::invalid_argument);
        EXPECT_THROW(
            client.client().websocket("/ws").configure({.authority_ = "user@example.test"}),
            std::invalid_argument);

        co_await client.shutdown();
        co_await peers.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();

    EXPECT_TRUE(second_hop_request.starts_with("GET /landed HTTP/1.1\r\n"));
    EXPECT_NE(second_hop_request.find("X-Trace: keep-me\r\n"), std::string::npos);
    EXPECT_EQ(second_hop_request.find("Digest:"), std::string::npos);
    EXPECT_EQ(second_hop_request.find("Repr-Digest:"), std::string::npos);
    EXPECT_EQ(second_hop_request.find("Content-Type:"), std::string::npos);
}

TEST(Ruvia, http_client_plain_connect_omits_origin_cookies_even_for_matching_authority) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto authority = "127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
    const auto url = "http://" + authority;

    std::string get_request_head;
    std::string connect_request_head;
    const auto serve_origin_and_proxy = [&]() -> ruvia::task<void> {
        // Connection 1: two ordinary origin GET requests to seed and verify cookie jar.
        {
            asio::ip::tcp::socket socket(io);
            const auto accepted = co_await ruvia::async_asio([&](auto handler) {
                acceptor.async_accept(socket, std::move(handler));
            });
            if (accepted.error_code()) {
                co_return;
            }
            std::string first_get;
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read_until(socket, asio::dynamic_buffer(first_get), "\r\n\r\n", std::move(handler));
            });
            constexpr std::string_view seed_reply =
                "HTTP/1.1 200 OK\r\nSet-Cookie: jar_secret=stored; Path=/\r\nContent-Length: 2\r\n\r\nok";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(seed_reply), std::move(handler));
            });

            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read_until(socket, asio::dynamic_buffer(get_request_head), "\r\n\r\n", std::move(handler));
            });
            constexpr std::string_view verify_reply =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(verify_reply), std::move(handler));
            });
        }
        // Connection 2: plain CONNECT tunnel whose tunnel_authority matches the client's authority.
        {
            asio::ip::tcp::socket socket(io);
            const auto accepted = co_await ruvia::async_asio([&](auto handler) {
                acceptor.async_accept(socket, std::move(handler));
            });
            if (accepted.error_code()) {
                co_return;
            }
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read_until(socket, asio::dynamic_buffer(connect_request_head), "\r\n\r\n", std::move(handler));
            });
            constexpr std::string_view connect_reply =
                "HTTP/1.1 200 Connection Established\r\n\r\n";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(connect_reply), std::move(handler));
            });
        }
    };

    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::task_scope peers(worker);
        peers.spawn(serve_origin_and_proxy());

        ruvia::http_client client(attachment.loop(), {
                                                         .base_url_ = url,
                                                         .versions_ = ruvia::http_client_versions{.http2_ = false},
                                                         .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send,
                                                         .cookies_ = {{"configured_secret", "1"}},
                                                     });
        auto seeded = co_await client.client().get("/seed").send();
        const auto seeded_body = co_await seeded.text();
        EXPECT_EQ(seeded_body.text(), "ok");

        auto verified = co_await client.client().get("/verify").send();
        const auto verified_body = co_await verified.text();
        EXPECT_EQ(verified_body.text(), "ok");

        auto opened = co_await client.client().connect(authority).open();
        EXPECT_NE(opened.tunnel(), nullptr);
        if (opened.tunnel() != nullptr) {
            co_await opened.tunnel()->finish();
        }

        co_await client.shutdown();
        co_await peers.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();

    EXPECT_NE(get_request_head.find("cookie: configured_secret=1; jar_secret=stored\r\n"), std::string::npos);
    EXPECT_TRUE(connect_request_head.starts_with("CONNECT " + authority + " HTTP/1.1\r\n"));
    EXPECT_EQ(connect_request_head.find("Cookie:"), std::string::npos);
    EXPECT_EQ(connect_request_head.find("cookie:"), std::string::npos);
    EXPECT_EQ(connect_request_head.find("configured_secret"), std::string::npos);
    EXPECT_EQ(connect_request_head.find("jar_secret"), std::string::npos);
}

namespace client_upgrade_tests {

struct peer_observation final {
    std::string request_;
    std::string payload_;
};

// A real TCP peer writes one coalesced response, half-closes its sending side,
// then observes the client's bytes through EOF. No production protocol parser
// participates on the peer side.
template <typename client_fn_type>
peer_observation exchange(std::string_view wire, client_fn_type&& exercise) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto& worker = attachment.loop().worker();
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_finished(worker);
    peer_observation observed;
    std::exception_ptr peer_failure;
    bool peer_done = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        const auto head_bytes = co_await asio::async_read_until(
            socket, asio::dynamic_buffer(observed.request_), "\r\n\r\n", asio::use_awaitable);
        observed.payload_ = observed.request_.substr(head_bytes);
        observed.request_.resize(head_bytes);
        co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        std::array<char, 4096> bytes;
        for (;;) {
            std::error_code error;
            const auto count = co_await socket.async_read_some(
                asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
            observed.payload_.append(bytes.data(), count);
            if (error == asio::error::eof || error == asio::error::connection_reset) {
                co_return;
            }
            if (error) {
                throw std::system_error(error);
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done = true;
        peer_finished.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = url,
                .request_timeout_ = std::chrono::seconds(3),
                .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            co_await exercise(client, worker);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        while (!peer_done) {
            co_await peer_finished.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    return observed;
}

}  // namespace client_upgrade_tests

TEST(Ruvia, http1_client_generic_upgrade_preserves_early_binary_bytes_and_caller_fields) {
    for (const std::string_view protocol : {"chat/7", "websocket"}) {
        SCOPED_TRACE(protocol);
        std::string wire =
            "HTTP/1.1 103 Early Hints\r\n\r\n"
            "HTTP/1.1 101 Switching Protocols\r\nConnection: keep-alive, Upgrade\r\nUpgrade: ";
        wire.append(protocol);
        wire.append("\r\n\r\n");
        constexpr std::string_view early("early\0bytes", 11);
        constexpr std::string_view outgoing("client\0bytes", 12);
        wire.append(early);
        const auto observed = client_upgrade_tests::exchange(wire,
            [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                unsigned informational_count = 0;
                auto request = client.client().get("/generic");
                request.header("Connection", "Upgrade")
                    .header("Upgrade", protocol)
                    .header("Sec-WebSocket-Protocol", "caller-owned")
                    .header("Sec-WebSocket-Key", "caller-owned-key");
                request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>,
                                                         ruvia::stop_token) -> ruvia::task<void> {
                    EXPECT_EQ(status.value(), 103);
                    ++informational_count;
                    co_return;
                }});
                auto opened = co_await request.upgrade();
                EXPECT_EQ(opened.response(), nullptr);
                if (opened.tunnel() == nullptr) {
                    throw std::runtime_error("generic Upgrade did not open a tunnel");
                }
                auto& tunnel = *opened.tunnel();
                EXPECT_EQ(tunnel.status(), ruvia::http_status::switching_protocols);
                EXPECT_EQ(tunnel.header("upgrade"), std::optional(protocol));
                EXPECT_EQ(informational_count, 1u);
                EXPECT_EQ(tunnel.informational_responses().size(), std::size_t{1});
                co_await tunnel.write(outgoing);
                co_await tunnel.finish();
                std::string received;
                while (auto bytes = co_await tunnel.text()) {
                    received.append(*bytes);
                }
                EXPECT_EQ(received, early);
            });
        EXPECT_TRUE(observed.request_.starts_with("GET /generic HTTP/1.1\r\n"));
        EXPECT_NE(observed.request_.find(": caller-owned\r\n"), std::string::npos);
        EXPECT_NE(observed.request_.find(": caller-owned-key\r\n"), std::string::npos);
        EXPECT_EQ(observed.payload_, outgoing);
    }
}

TEST(Ruvia, http1_client_generic_upgrade_rejection_is_an_ordinary_response) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 426 Upgrade Required\r\nContent-Length: 6\r\nX-Rejected: yes\r\n\r\ndenied",
        [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            auto request = client.client().get("/rejected");
            request.header("Connection", "Upgrade").header("Upgrade", "chat/7");
            auto result = co_await request.upgrade();
            EXPECT_EQ(result.tunnel(), nullptr);
            if (result.response() == nullptr) {
                throw std::runtime_error("rejected Upgrade lost its HTTP response");
            }
            auto& response = *result.response();
            EXPECT_EQ(response.status().value(), 426);
            EXPECT_EQ(response.header("x-rejected"), std::optional<std::string_view>("yes"));
            EXPECT_EQ(response.framing(), ruvia::http_client_response_framing::fixed_length);
            EXPECT_EQ(response.content_length(), std::optional<std::uint64_t>(6));
            const auto body = co_await response.stream().read_all();
            EXPECT_EQ(body.text(), "denied");
        });
}

TEST(Ruvia, http1_client_generic_upgrade_rejects_invalid_switch_agreement) {
    for (const std::string_view fields : {
             "Upgrade: chat/7\r\n",
             "Connection: Upgrade\r\n",
             "Connection: Upgrade\r\nUpgrade: other/1\r\n",
             "Connection: close, Upgrade\r\nUpgrade: chat/7\r\n"}) {
        SCOPED_TRACE(fields);
        std::string wire = "HTTP/1.1 101 Switching Protocols\r\n";
        wire.append(fields);
        wire.append("\r\nnot-a-tunnel");
        client_upgrade_tests::exchange(wire,
            [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                auto request = client.client().get("/invalid-switch");
                request.header("Connection", "Upgrade").header("Upgrade", "chat/7");
                bool rejected = false;
                try {
                    auto result = co_await request.upgrade();
                    if (result.tunnel() != nullptr) {
                        result.tunnel()->abort();
                    }
                } catch (const ruvia::http_client_error& error) {
                    rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_error);
                }
                EXPECT_TRUE(rejected);
            });
    }
}

TEST(Ruvia, http1_client_informational_callbacks_backpressure_coalesced_heads) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 103 Early Hints\r\nX-Order: first\r\n\r\n"
        "HTTP/1.1 102 Processing\r\nX-Order: second\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
        [](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            ruvia::worker_signal entered(worker);
            ruvia::worker_signal release(worker);
            std::vector<unsigned> statuses;
            bool returned = false;
            std::exception_ptr failure;
            auto request = client.client().get("/hints");
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> headers,
                                                     ruvia::stop_token stop) -> ruvia::task<void> {
                EXPECT_FALSE(stop.stop_requested());
                statuses.push_back(status.value());
                EXPECT_EQ(headers.size(), std::size_t{1});
                const auto expected = statuses.size() == 1 ? "first" : "second";
                entered.notify();
                co_await release.wait();
                if (!headers.empty()) {
                    EXPECT_EQ(headers.front().value(), expected);
                }
            }});
            const auto send = [&]() -> ruvia::task<void> {
                try {
                    auto response = co_await request.send();
                    returned = true;
                    EXPECT_EQ(response.informational_responses().size(), std::size_t{2});
                    const auto body = co_await response.stream().read_all();
                    EXPECT_EQ(body.text(), "ok");
                } catch (...) {
                    failure = std::current_exception();
                }
                entered.notify();
            };
            ruvia::task_scope requests(worker);
            requests.spawn(send());
            for (std::size_t count = 1; count <= 2; ++count) {
                co_await entered.wait();
                if (failure || returned) {
                    break;
                }
                EXPECT_EQ(statuses.size(), count);
                EXPECT_FALSE(returned);
                release.notify();
            }
            co_await requests.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            EXPECT_EQ(statuses, (std::vector<unsigned>{103, 102}));
            EXPECT_TRUE(returned);
        });
}

TEST(Ruvia, http1_client_informational_callback_failure_terminates_exchange) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            auto request = client.client().get("/callback-failure");
            request.configure({.informational_ = [](ruvia::http_status_code, std::span<const ruvia::http_header>,
                                                     ruvia::stop_token) -> ruvia::task<void> {
                throw std::runtime_error("informational callback failed");
                co_return;
            }});
            bool failed = false;
            try {
                auto response = co_await request.send();
                static_cast<void>(response);
            } catch (const std::runtime_error& error) {
                failed = std::string_view(error.what()) == "informational callback failed";
            }
            EXPECT_TRUE(failed);
        });
}

TEST(Ruvia, http1_client_response_framing_comes_from_parser_precedence) {
    struct framing_case final {
        std::string_view method_;
        std::string_view wire_;
        ruvia::http_client_response_framing framing_;
        std::optional<std::uint64_t> length_;
        std::string_view body_;
    };
    using framing = ruvia::http_client_response_framing;
    const framing_case cases[] = {
        {"GET", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", framing::fixed_length, 2, "ok"},
        {"GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", framing::chunked, std::nullopt, "ok"},
        {"GET", "HTTP/1.1 200 OK\r\n\r\nok", framing::close_delimited, std::nullopt, "ok"},
        {"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n", framing::none, 42, ""},
        {"GET", "HTTP/1.1 204 No Content\r\n\r\n", framing::none, std::nullopt, ""},
        {"GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", framing::chunked, std::nullopt, ""},
        {"GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\nok", framing::fixed_length, 2, "ok"},
    };
    for (const auto& scenario : cases) {
        SCOPED_TRACE(scenario.wire_);
        client_upgrade_tests::exchange(scenario.wire_,
            [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                auto response = co_await client.client().request(scenario.method_, "/framing").send();
                EXPECT_EQ(response.framing(), scenario.framing_);
                EXPECT_EQ(response.content_length(), scenario.length_);
                const auto body = co_await response.stream().read_all();
                EXPECT_EQ(body.text(), scenario.body_);
                EXPECT_EQ(response.framing(), scenario.framing_);
                EXPECT_EQ(response.content_length(), scenario.length_);
            });
    }
}

TEST(Ruvia, http1_client_informational_callback_observes_deadline_and_explicit_stop) {
    for (const bool deadline : {false, true}) {
        SCOPED_TRACE(deadline);
        client_upgrade_tests::exchange(
            "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
            [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
                ruvia::stop_source stop;
                ruvia::worker_signal entered(worker);
                const auto handle = client.client().with_options(
                    {.timeout_ = deadline ? std::optional<ruvia::timeout_duration>(std::chrono::milliseconds(500)) : std::nullopt,
                        .stop_token_ = stop.token()});
                auto request = handle.get("/cancel-hints");
                bool callback_stopped = false;
                bool callback_entered = false;
                bool completed = false;
                std::exception_ptr failure;
                std::optional<std::error_code> code;
                request.configure({.informational_ = [&](ruvia::http_status_code, std::span<const ruvia::http_header>,
                                                         ruvia::stop_token token) -> ruvia::task<void> {
                    callback_entered = true;
                    entered.notify();
                    const auto result = co_await ruvia::sleep_for(worker, std::chrono::seconds(2), token);
                    callback_stopped = result == ruvia::timer_sleep_result::stop_requested && token.stop_requested();
                }});
                const auto send = [&]() -> ruvia::task<void> {
                    try {
                        auto response = co_await request.send();
                        static_cast<void>(response);
                    } catch (const ruvia::http_client_error& error) {
                        code = error.code();
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    completed = true;
                    entered.notify();
                };
                ruvia::task_scope requests(worker);
                requests.spawn(send());
                while (!callback_entered && !completed) {
                    co_await entered.wait();
                }
                if (!deadline) {
                    stop.request_stop();
                }
                co_await requests.join();
                if (failure) {
                    std::rethrow_exception(failure);
                }
                EXPECT_TRUE(callback_entered);
                EXPECT_TRUE(callback_stopped);
                EXPECT_EQ(code, deadline ? std::make_error_code(std::errc::timed_out)
                                         : std::make_error_code(std::errc::operation_canceled));
            });
    }
}

TEST(Ruvia, http1_client_buffered_upload_receives_hints_before_blocked_write_completes) {
    enum class outcome { complete,
        cancel,
        early_final };
    constexpr std::size_t content_bytes = 16 * 1024 * 1024;
    for (const auto expected : {outcome::complete, outcome::cancel, outcome::early_final}) {
        SCOPED_TRACE(static_cast<int>(expected));
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto& worker = attachment.loop().worker();
        asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::socket socket(io);
        asio::steady_timer drain_gate(io);
        drain_gate.expires_after(std::chrono::seconds(10));
        ruvia::worker_signal peer_finished(worker);
        ruvia::worker_signal callback_entered(worker);
        ruvia::worker_signal callback_release(worker);
        std::size_t received_body = 0;
        bool peer_done = false;
        bool stopping = false;
        std::exception_ptr peer_failure;
        const auto serve = [&]() -> asio::awaitable<void> {
            co_await peer.async_accept(socket, asio::use_awaitable);
            socket.set_option(asio::socket_base::receive_buffer_size(4096));
            std::string request;
            const auto head_bytes = co_await asio::async_read_until(
                socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            EXPECT_TRUE(request.starts_with("POST /buffered HTTP/1.1\r\n"));
            received_body = request.size() - head_bytes;
            constexpr std::string_view hints = "HTTP/1.1 103 Early Hints\r\nX-Phase: blocked-write\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(hints), asio::use_awaitable);
            std::error_code gate_error;
            co_await drain_gate.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
            if (stopping) {
                co_return;
            }
            EXPECT_EQ(gate_error, asio::error::operation_aborted);
            if (expected == outcome::early_final) {
                constexpr std::string_view rejected = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\n\r\n";
                co_await asio::async_write(socket, asio::buffer(rejected), asio::use_awaitable);
                co_return;
            }
            socket.set_option(asio::socket_base::receive_buffer_size(1024 * 1024));
            std::array<char, 65536> bytes;
            while (received_body < content_bytes) {
                const auto count = co_await socket.async_read_some(
                    asio::buffer(bytes.data(), std::min(bytes.size(), content_bytes - received_body)), asio::use_awaitable);
                EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + count, [](char value) { return value == 'b'; }));
                received_body += count;
            }
            constexpr std::string_view accepted = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(socket, asio::buffer(accepted), asio::use_awaitable);
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr error) {
            if (!stopping) {
                peer_failure = error;
            }
            peer_done = true;
            peer_finished.notify();
        });
        std::exception_ptr failure;
        bool saw_callback = false;
        bool completed = false;
        std::optional<std::error_code> error_code;
        const auto run = [&]() -> ruvia::task<void> {
            const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
            ruvia::http_client client(attachment.loop(),
                {.base_url_ = url, .request_timeout_ = std::chrono::seconds(5), .versions_ = ruvia::http_client_versions{.http2_ = false}});
            ruvia::stop_source stop;
            const auto handle = client.client().with_options({.stop_token_ = stop.token()});
            auto request = handle.post("/buffered");
            std::string content(content_bytes, 'b');
            request.body(content);
            std::string().swap(content);
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>,
                                                     ruvia::stop_token token) -> ruvia::task<void> {
                EXPECT_EQ(status.value(), 103);
                EXPECT_LT(received_body, content_bytes);
                EXPECT_LT(handle.stats().plaintext_bytes_sent_, content_bytes);
                saw_callback = true;
                callback_entered.notify();
                co_await callback_release.wait();
                if (expected == outcome::cancel) {
                    EXPECT_TRUE(token.stop_requested());
                } else {
                    drain_gate.cancel();
                }
            }});
            const auto send = [&]() -> ruvia::task<void> {
                try {
                    auto response = co_await request.send();
                    EXPECT_NE(expected, outcome::cancel);
                    EXPECT_EQ(response.status().value(), expected == outcome::early_final ? 413 : 200);
                    const auto body = co_await response.stream().read_all();
                    EXPECT_EQ(body.text(), expected == outcome::early_final ? "" : "ok");
                } catch (const ruvia::http_client_error& error) {
                    error_code = error.code();
                } catch (...) {
                    failure = std::current_exception();
                }
                completed = true;
                callback_entered.notify();
            };
            ruvia::task_scope requests(worker);
            requests.spawn(send());
            while (!saw_callback && !completed) {
                co_await callback_entered.wait();
            }
            if (expected == outcome::cancel) {
                stop.request_stop();
            }
            callback_release.notify();
            co_await requests.join();
            co_await client.shutdown();
            stopping = true;
            drain_gate.cancel();
            peer.close();
            std::error_code ignored;
            socket.close(ignored);
            while (!peer_done) {
                co_await peer_finished.wait();
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        std::move(root).get();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(saw_callback);
        if (expected == outcome::cancel) {
            EXPECT_EQ(error_code, std::make_error_code(std::errc::operation_canceled));
        } else {
            EXPECT_FALSE(error_code.has_value());
        }
        if (expected == outcome::complete) {
            EXPECT_EQ(received_body, content_bytes);
        } else {
            EXPECT_LT(received_body, content_bytes);
        }
    }
}

TEST(Ruvia, http1_client_borrowed_small_chunk_pauses_read_inactivity_until_next_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto& worker = attachment.loop().worker();
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket socket(io);
    asio::steady_timer tail_gate(io);
    tail_gate.expires_after(std::chrono::seconds(5));
    ruvia::worker_signal peer_finished(worker);
    bool peer_done = false;
    bool stopping = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        co_await peer.async_accept(socket, asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view first = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\na";
        co_await asio::async_write(socket, asio::buffer(first), asio::use_awaitable);
        std::error_code gate_error;
        co_await tail_gate.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
        if (!stopping) {
            EXPECT_EQ(gate_error, asio::error::operation_aborted);
            co_await asio::async_write(socket, asio::buffer("b", 1), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr error) {
        if (!stopping) {
            peer_failure = error;
        }
        peer_done = true;
        peer_finished.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = url, .read_timeout_ = std::chrono::milliseconds(50), .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            auto response = co_await client.client().get("/slow-consumer").send();
            const auto first = co_await response.stream().text();
            EXPECT_EQ(first, std::optional<std::string_view>("a"));
            (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(200));
            EXPECT_EQ(first, std::optional<std::string_view>("a"));
            tail_gate.cancel();
            const auto tail = co_await response.stream().text();
            EXPECT_EQ(tail, std::optional<std::string_view>("b"));
            EXPECT_FALSE((co_await response.stream().text()).has_value());
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        stopping = true;
        tail_gate.cancel();
        peer.close();
        std::error_code ignored;
        socket.close(ignored);
        while (!peer_done) {
            co_await peer_finished.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
}

namespace websocket_client_test {

template <typename value_type>
ruvia::task<bool> rejects_running_operation(ruvia::task<value_type>&& operation) {
    try {
        static_cast<void>(co_await std::move(operation));
    } catch (const std::logic_error&) {
        co_return true;
    }
    co_return false;
}

ruvia::task<void> await_output(ruvia::task<void>&& operation) {
    co_await std::move(operation);
}

ruvia::task<void> receive_payload(ruvia::task<std::optional<ruvia::websocket_message>>&& operation,
    std::string& payload) {
    const auto message = co_await std::move(operation);
    if (!message) {
        throw std::runtime_error("WebSocket client message expected");
    }
    payload.assign(message->payload());
}

}  // namespace websocket_client_test

namespace {

[[nodiscard]] std::string make_server_handshake_response(std::string_view request) {
    ruvia::http1_server_request_parse_state parsed;
    parsed.parse_head(request, 0);
    if (!parsed.head_ready()) {
        throw std::runtime_error("invalid WebSocket handshake request");
    }
    auto handshake = ruvia::make_websocket_server_handshake(parsed.request(), {});
    std::string response{};
    handshake.for_each_response_part([&response](std::string_view part) {
        response.append(part);
    });
    return response;
}

void check_peer_exchange(bool reply_close,
    bool truncated_frame = false) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool saw_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        // The first frame can arrive in the same transport write as the upgrade.
        std::string response(make_server_handshake_response(request));
        if (truncated_frame) {
            response.append("\x81\x05hi", 4);
        } else {
            response.append("\x82\x05hello", 7);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        if (truncated_frame) {
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
            co_return;
        }
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        saw_close = header_value[0] == 0x88 && (header_value[1] & 0x80) != 0;
        std::array<unsigned char, 129> payload_value{};
        co_await asio::async_read(socket, asio::buffer(payload_value.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        if (reply_close) {
            const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
            co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool succeeded = false;
    bool returned_eof = false;
    ruvia::websocket_termination termination{};
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_sent = 0;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        std::optional<ruvia::websocket> websocket;
        try {
            websocket.emplace(co_await client.client().websocket("/").connect());
            const auto greeting = co_await websocket->read();
            if (truncated_frame) {
                returned_eof = !greeting.has_value();
            } else {
                EXPECT_TRUE(greeting.has_value());
                if (greeting) {
                    EXPECT_EQ(greeting->payload(), std::string_view("hello"));
                }
                if (reply_close) {
                    auto close_operation = websocket->close({});
                    ruvia::task_scope closing(client.worker());
                    closing.spawn(websocket_client_test::await_output(std::move(close_operation)));
                    const bool read_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket->read());
                    const bool write_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket->text("overlap"));
                    const bool close_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket->close({}));
                    co_await closing.join();
                    EXPECT_TRUE(read_rejected && write_rejected && close_rejected);
                } else {
                    co_await websocket->close({});
                }
                succeeded = true;
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (websocket) {
            termination = websocket->termination();
            bytes_received = websocket->bytes_received();
            bytes_sent = websocket->bytes_sent();
        }
        websocket.reset();
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (truncated_frame) {
        EXPECT_TRUE(returned_eof);
        EXPECT_TRUE(termination == ruvia::websocket_termination::eof);
        EXPECT_EQ(bytes_received, 0U);
    } else {
        EXPECT_TRUE(saw_close);
        EXPECT_TRUE(succeeded);
        // The peer answering Close completes the client's own handshake;
        // otherwise its EOF ends the session.
        EXPECT_TRUE(termination == (reply_close ? ruvia::websocket_termination::local_close : ruvia::websocket_termination::eof));
        // Received "\x82\x05hello" (7 bytes) and the 4-byte Close echo; sent a
        // masked Close carrying 1000 (2 + 4 + 2 bytes).
        EXPECT_EQ(bytes_received, reply_close ? 11U : 7U);
        EXPECT_EQ(bytes_sent, 8U);
    }
}

// RFC 6455 section 7.1.1: after the close handshake the server closes the
// TCP connection first; the client waits for that, bounded by
// close_handshake_timeout_, and only then closes without failing.
void check_server_closes_first(bool client_initiates, bool server_closes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool server_closed = false;
    bool client_eof = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response(make_server_handshake_response(request));
        const std::array<unsigned char, 7> close{0x88, 5, 3, 0xe8, 'b', 'y', 'e'};
        if (!client_initiates) {
            response.append(reinterpret_cast<const char*>(close.data()), close.size());
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        std::array<unsigned char, 129> close_payload{};
        co_await asio::async_read(
            socket, asio::buffer(close_payload.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        std::string after_close{};
        if (client_initiates) {
            after_close.append(reinterpret_cast<const char*>(close.data()), close.size());
        }
        // Data after Close is ignored by the client.
        after_close.append("\x81\x01x", 3);
        co_await asio::async_write(socket, asio::buffer(after_close), asio::use_awaitable);
        asio::steady_timer delay(io, std::chrono::milliseconds(50));
        co_await delay.async_wait(asio::use_awaitable);
        if (server_closes) {
            server_closed = true;
            // Fails if the client already closed; the order check below reports that.
            std::error_code ignored;
            socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        }
        std::array<char, 64> sink{};
        std::error_code error;
        while (!error) {
            (void)co_await socket.async_read_some(asio::buffer(sink), asio::redirect_error(asio::use_awaitable, error));
        }
        client_eof = true;
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool completed = false;
    bool completed_after_server_close = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.close_handshake_timeout_ = server_closes ? std::chrono::milliseconds(10000)
                                                                                                                          : std::chrono::milliseconds(100)})
                                 .connect();
            auto& handle = websocket;
            EXPECT_TRUE(handle.connected());
            EXPECT_TRUE(handle.subprotocol().empty());
            if (client_initiates) {
                co_await handle.close({});
            } else {
                const auto message = co_await handle.read();
                EXPECT_TRUE(!message.has_value());
            }
            EXPECT_TRUE(!handle.connected());
            EXPECT_EQ(handle.peer_close_code(), std::optional<std::uint16_t>(1000));
            EXPECT_EQ(handle.peer_close_reason(), std::string_view("bye"));
            EXPECT_EQ(websocket.peer_close_code(), std::optional<std::uint16_t>(1000));
            EXPECT_EQ(websocket.peer_close_reason(), std::string_view("bye"));
            completed = true;
            completed_after_server_close = server_closed;
            EXPECT_TRUE(websocket.termination() == (client_initiates ? ruvia::websocket_termination::local_close : ruvia::websocket_termination::peer_close));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        // Let the peer observe the client's end of the connection.
        for (int i = 0; i < 500 && failure == nullptr && !client_eof && peer_failure == nullptr; ++i) {
            (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(10));
        }
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(client_eof);
    EXPECT_EQ(completed_after_server_close, server_closes);
}

}  // namespace

TEST(Ruvia, websocket_client_negotiates_deflate_and_can_skip_individual_messages) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool offered = false;
    bool compressed = false;
    bool uncompressed = false;
    const std::string payload_value(4096, 'a');
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        offered = request.find("permessage-deflate") != std::string_view::npos;
        // The server handshake negotiates and serializes the extension once;
        // appending a second offer would make the response invalid.
        const auto response = make_server_handshake_response(request);
        const auto extension = response.find("permessage-deflate");
        EXPECT_TRUE(extension != std::string_view::npos);
        if (extension != std::string_view::npos) {
            EXPECT_TRUE(response.find("permessage-deflate", extension + 1) == std::string_view::npos);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        ruvia::http_io_buffer protocol_input;
        ruvia::websocket_connection protocol(protocol_input, {.compression_ = {.enabled_ = true}});
        std::array<char, 8192> bytes_value{};
        unsigned messages = 0;
        while (messages != 4) {
            const auto count = co_await socket.async_read_some(asio::buffer(bytes_value), asio::use_awaitable);
            if (messages == 0) {
                compressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) != 0;
            }
            if (messages == 1) {
                uncompressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) == 0;
            }
            protocol_input.append(std::string_view(bytes_value.data(), count));
            while (auto event = protocol.next_event()) {
                if (const auto* message = event->message()) {
                    EXPECT_EQ(message->payload(), payload_value);
                    ++messages;
                    EXPECT_TRUE(protocol.submit_frame(ruvia::websocket_opcode::text, message->payload()) == ruvia::websocket_frame_submit_status::accepted);
                    const auto output = protocol.output_plan();
                    co_await asio::async_write(socket, asio::buffer(output.bytes()), asio::use_awaitable);
                    (void)protocol.consume_output(output.bytes().size());
                }
            }
        }
        socket.close();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        {
            auto cold = client.client().websocket("/").deflate({.compression_ = {.enabled_ = true}}).connect();
        }
        auto websocket = co_await client.client().websocket("/").deflate({.compression_ = {.enabled_ = true}}).connect();
        auto& handle = websocket;
        {
            auto first_write = handle.text("discarded");
            auto second_write = handle.binary(std::as_bytes(std::span(std::string_view("discarded"))));
            auto byte_write = handle.binary(std::as_bytes(std::span("discarded", 9)));
            auto owned_text = handle.text(std::string("discarded"));
            auto owned_binary = [&] {
                const std::string source("discarded");
                return handle.binary(std::as_bytes(std::span(source)));
            }();
            auto first_read = handle.read();
            auto second_read = handle.read();
            auto first_close = handle.close();
            auto second_close = handle.close({});
            auto ping = handle.ping("discarded");
            auto owned_ping = handle.ping(std::string("discarded"));
            auto pong = handle.pong("discarded");
            auto default_pong = handle.pong();
            auto owned_pong = handle.pong(std::string("discarded"));
            auto send_msg = handle.send(
                ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, "discarded"));
        }

        std::string mutable_payload(payload_value);
        auto first = handle.text(mutable_payload);
        auto second = handle.text(std::string(payload_value), {.compress_ = false});
        mutable_payload.assign("changed before starting the write");
        auto first_read = handle.read();
        auto second_read = handle.read();
        std::string first_payload{};
        {
            ruvia::task_scope writing(client.worker());
            writing.spawn(websocket_client_test::await_output(std::move(first)));
            const bool write_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.text("overlap"));
            const bool close_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.close({}));
            // A failed close releases its read claim and leaves the running write claimed.
            const bool write_still_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.text("overlap"));
            ruvia::task_scope reading(client.worker());
            reading.spawn(websocket_client_test::receive_payload(std::move(first_read), first_payload));
            const bool read_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.read());
            // close() may overlap the running read, but not the running write.
            const bool close_during_write_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.close({}));
            co_await writing.join();
            co_await reading.join();
            EXPECT_TRUE(write_rejected && close_rejected && write_still_rejected && read_rejected &&
                        close_during_write_rejected);
        }
        EXPECT_EQ(first_payload, payload_value);
        co_await std::move(second);
        const auto second_message = co_await std::move(second_read);
        EXPECT_TRUE(second_message.has_value());
        if (second_message) {
            EXPECT_EQ(second_message->payload(), payload_value);
        }
        for (unsigned i = 2; i < 4; ++i) {
            const std::string source(payload_value);
            auto operation = [&] {
                if (i == 2) {
                    return handle.send(
                        ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, source));
                }
                return handle.binary(std::as_bytes(std::span(std::string_view(source))));
            }();
            co_await std::move(operation);
            const auto message = co_await handle.read();
            EXPECT_TRUE(message.has_value());
            if (message) {
                EXPECT_EQ(message->payload(), payload_value);
            }
            EXPECT_TRUE(websocket.connected());
            EXPECT_TRUE(handle.connected());
        }
        client.close();
        co_await client.shutdown();
        peer.close();
        attachment.stop();
    };
    const auto run_to_completion = [&]() -> ruvia::task<void> {
        try {
            co_await run();
        } catch (...) {
            // A rejected handshake must report its error, not leave the test's
            // event-loop work guard running until the whole suite times out.
            peer.close();
            attachment.stop();
            throw;
        }
    };
    auto root = attachment.loop().start(run_to_completion());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(offered);
    EXPECT_TRUE(compressed);
    EXPECT_TRUE(uncompressed);
}

TEST(Ruvia, websocket_client_event_loop_stop_joins_pending_connect) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::steady_timer gate(io, std::chrono::steady_clock::time_point::max());
    std::promise<void> request_received;
    auto received_value = request_received.get_future();
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        request_received.set_value();
        std::error_code ignored;
        co_await gate.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool connect_failed = false;
    const auto connect_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error&) {
            connect_failed = true;
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(connect_client());
    std::thread driver([&] { attachment.run(); });
    if (received_value.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        asio::post(io, [&gate] { gate.cancel(); });
        driver.join();
        std::move(root).get();
        throw std::runtime_error("WebSocket peer did not receive the upgrade request");
    }
    received_value.get();
    attachment.stop();
    asio::post(io, [&gate] { gate.cancel(); });
    driver.join();
    std::move(root).get();
    EXPECT_TRUE(connect_failed);
}

TEST(Ruvia, websocket_client_handshake_sends_heads_beyond_the_retained_buffer) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    // Larger than the handshake head buffer first tried, within the limit.
    const std::string large_value(20 * 1024, 'v');
    std::exception_ptr peer_failure;
    bool received_large_header = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        ruvia::http1_server_request_parse_state parsed;
        parsed.parse_head(request);
        received_large_header = parsed.head_ready() && parsed.request().header("x-large") == large_value;
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) { peer_failure = failure; });

    bool connected = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const ruvia::http_header_view large_headers[]{{"X-Large", large_value}};
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").headers(large_headers).connect();
            connected = websocket.connected();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(received_large_header);
    EXPECT_TRUE(connected);
}

// One User-Agent rule for every client: the configured agent (default "Ruvia")
// unless the application headers carry their own field, which replaces it; an
// empty agent sends none.
TEST(Ruvia, websocket_client_handshake_sends_the_default_user_agent_unless_headers_supply_one) {
    struct case_type final {
        std::span<const ruvia::http_header_view> headers_;
        std::optional<std::string_view> user_agent_;
        std::string_view expected_;
    };
    const ruvia::http_header_view custom[]{{"User-Agent", "custom/1"}};
    const case_type cases[]{
        {.expected_ = "Ruvia"},
        {.headers_ = custom, .expected_ = "custom/1"},
        {.user_agent_ = "configured/2", .expected_ = "configured/2"},
        {.headers_ = custom, .user_agent_ = "configured/2", .expected_ = "custom/1"},
        {.user_agent_ = "", .expected_ = {}},
    };
    for (const auto& test_case : cases) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        std::string request{};
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await peer.async_accept(asio::use_awaitable);
            co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            const auto response = make_server_handshake_response(request);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            std::array<char, 1024> input{};
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
            }
        };
        asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) { peer_failure = failure; });

        const auto run_client = [&]() -> ruvia::task<void> {
            const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
            ruvia::http_client_config config{.base_url_ = base_url};
            if (test_case.user_agent_) {
                config.user_agent_ = *test_case.user_agent_;
            }
            ruvia::http_client client(attachment.loop(), config);
            std::exception_ptr failure;
            try {
                auto websocket = co_await client.client().websocket("/").headers(test_case.headers_).connect();
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            peer.close();
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        };
        auto root = attachment.loop().start(run_client());
        attachment.run();
        std::move(root).get();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        ruvia::http1_server_request_parse_state parsed;
        parsed.parse_head(request);
        EXPECT_TRUE(parsed.head_ready());
        std::size_t user_agents = 0;
        for (const auto& field : parsed.request().headers()) {
            if (ruvia::http_ascii_equals_ignore_case(field.name(), "user-agent")) {
                ++user_agents;
                EXPECT_EQ(field.value(), test_case.expected_);
            }
        }
        EXPECT_EQ(user_agents, test_case.expected_.empty() ? 0U : 1U);
    }
}

TEST(Ruvia, websocket_client_event_loop_stop_joins_connected_heartbeat_and_pending_read) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::promise<void> ping_received;
    auto ping_ready = ping_received.get_future();
    std::exception_ptr peer_failure;
    bool observed_client_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        const auto payload_bytes = static_cast<std::size_t>(header_value[1] & 0x7f);
        if (header_value[0] != 0x89 || (header_value[1] & 0x80) == 0 || payload_bytes > 125) {
            throw std::runtime_error("expected a masked client heartbeat ping");
        }
        std::array<unsigned char, 4> mask{};
        co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
        std::array<unsigned char, 125> payload_value{};
        co_await asio::async_read(
            socket, asio::buffer(payload_value.data(), payload_bytes), asio::use_awaitable);
        ping_received.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input),
            asio::redirect_error(asio::use_awaitable, error))) {
        }
        observed_client_close = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    bool connected = false;
    bool read_cancelled = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        auto websocket = co_await client.client().websocket("/").lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20),
                                                                                .pong_timeout_ = std::chrono::seconds(5)}})
                             .connect();
        connected = websocket.connected();
        try {
            read_cancelled = !(co_await websocket.read()).has_value();
        } catch (const std::system_error&) {
            read_cancelled = true;
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    if (ping_ready.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        driver.join();
        std::move(root).get();
        if (peer_failure != nullptr) {
            std::rethrow_exception(peer_failure);
        }
        throw std::runtime_error("WebSocket client did not send its heartbeat ping");
    }
    ping_ready.get();
    attachment.stop();
    driver.join();
    std::move(root).get();
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(connected);
    EXPECT_TRUE(read_cancelled);
    EXPECT_TRUE(observed_client_close);
}

TEST(Ruvia, websocket_client_heartbeat_pings_after_a_blocked_message_write) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    constexpr std::size_t payload_size = std::size_t{16} * 1024 * 1024;
    std::promise<void> ping_received;
    auto ping_ready = ping_received.get_future();
    std::exception_ptr peer_failure;
    std::uint64_t message_bytes = 0;
    bool message_complete = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        // Stop reading for many ping intervals so the large client write stays
        // blocked while its heartbeat becomes due.
        asio::steady_timer pause(socket.get_executor(), std::chrono::milliseconds(200));
        co_await pause.async_wait(asio::use_awaitable);

        std::vector<char> discard(64 * 1024);
        for (;;) {
            std::array<unsigned char, 2> header_value{};
            co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
            if ((header_value[1] & 0x80) == 0) {
                throw std::runtime_error("expected masked client frames");
            }
            std::uint64_t payload_bytes = header_value[1] & 0x7f;
            if (payload_bytes >= 126) {
                std::array<unsigned char, 8> length_value{};
                const std::size_t length_size = payload_bytes == 126 ? 2 : 8;
                co_await asio::async_read(socket, asio::buffer(length_value.data(), length_size), asio::use_awaitable);
                payload_bytes = 0;
                for (std::size_t i = 0; i < length_size; ++i) {
                    payload_bytes = (payload_bytes << 8) | length_value[i];
                }
            }
            std::array<unsigned char, 4> mask{};
            co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
            for (auto remaining = payload_bytes; remaining != 0;) {
                const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, discard.size()));
                co_await asio::async_read(socket, asio::buffer(discard.data(), chunk), asio::use_awaitable);
                remaining -= chunk;
            }
            const auto opcode = header_value[0] & 0x0f;
            if (opcode == 0x9) {
                if (payload_bytes > 125) {
                    throw std::runtime_error("expected a client heartbeat ping");
                }
                break;
            }
            if (opcode != 0x2 && opcode != 0x0) {
                throw std::runtime_error("unexpected client frame before the heartbeat ping");
            }
            message_bytes += payload_bytes;
            message_complete = (header_value[0] & 0x80) != 0;
        }
        ping_received.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input),
            asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    bool wrote = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_size).lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20), .pong_timeout_ = std::chrono::seconds(5)}}).connect();
        const std::string payload(payload_size, 'x');
        co_await websocket.binary(std::as_bytes(std::span(std::string_view(payload))));
        wrote = true;
        try {
            (void)co_await websocket.read();
        } catch (const std::system_error&) {
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    if (ping_ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        attachment.stop();
        driver.join();
        std::move(root).get();
        if (peer_failure != nullptr) {
            std::rethrow_exception(peer_failure);
        }
        throw std::runtime_error("WebSocket client did not ping after its blocked write");
    }
    ping_ready.get();
    attachment.stop();
    driver.join();
    std::move(root).get();
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(wrote);
    EXPECT_TRUE(message_complete);
    EXPECT_EQ(message_bytes, std::uint64_t{payload_size});
}

// A peer that stops reading blocks the application write and defers the
// heartbeat Ping. The liveness budget (Ping interval plus Pong timeout) still
// bounds that stall, independently of any transport write timeout.
TEST(Ruvia, websocket_client_heartbeat_aborts_a_write_stalled_by_a_non_reading_peer) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    constexpr std::size_t payload_size = std::size_t{16} * 1024 * 1024;
    // Held open without reading, so the client's large write fills the
    // socket buffers and stalls until the client gives up.
    std::optional<asio::ip::tcp::socket> stalled;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        stalled.emplace(std::move(socket));
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    std::optional<std::error_code> write_error;
    std::promise<void> write_finished;
    auto write_done = write_finished.get_future();
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .write_timeout_ = std::nullopt,
                                                         .request_timeout_ = std::nullopt});
        auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_size).lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(50), .pong_timeout_ = std::chrono::milliseconds(100)}}).connect();
        const std::string payload(payload_size, 'x');
        try {
            co_await websocket.binary(std::as_bytes(std::span(std::string_view(payload))));
        } catch (const std::system_error& error) {
            write_error = error.code();
        }
        co_await client.shutdown();
        write_finished.set_value();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    // Well under the 30 s default write timeout the configuration disabled.
    const bool finished = write_done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    attachment.stop();
    driver.join();
    std::move(root).get();
    if (!finished) {
        throw std::runtime_error("WebSocket client write stalled by a non-reading peer never timed out");
    }
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(write_error.has_value());
    if (write_error) {
        EXPECT_TRUE(*write_error == std::errc::timed_out);
    }
}

TEST(Ruvia, websocket_client_close_requires_peer_close) {
    check_peer_exchange(false);
    check_peer_exchange(true);
}

TEST(Ruvia, websocket_client_read_rejects_eof_during_incomplete_frame) {
    check_peer_exchange(false, true);
}

TEST(Ruvia, websocket_client_lets_the_server_close_the_connection_first) {
    for (const bool client_initiates : {false, true}) {
        check_server_closes_first(client_initiates, true);
        check_server_closes_first(client_initiates, false);
    }
}

TEST(Ruvia, websocket_client_reads_frame_above_default_message_limit) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    std::string payload_value(ruvia::default_max_websocket_message_bytes + 1, '\0');
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>('a' + i % 26);
    }
    bool saw_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response(make_server_handshake_response(request));
        response.push_back('\x82');
        response.push_back('\x7f');
        const auto length = static_cast<std::uint64_t>(payload_value.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            response.push_back(static_cast<char>((length >> shift) & 0xffU));
        }
        response.append(payload_value);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        saw_close = header_value[0] == 0x88 && (header_value[1] & 0x80) != 0;
        std::array<unsigned char, 129> close_payload{};
        co_await asio::async_read(socket, asio::buffer(close_payload.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
        co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool matched = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        // A single frame larger than the protocol's default input bound must be
        // delivered intact when the configured message limit admits it.
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url, .read_timeout_ = std::chrono::seconds(10)});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_value.size()).connect();
            const auto message = co_await websocket.read();
            matched = message.has_value() && message->payload() == payload_value;
            co_await websocket.close({});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(matched);
    EXPECT_TRUE(saw_close);
}

TEST(Ruvia, websocket_client_rejects_untrusted_tls_peer) {
    asio::ssl::context tls(asio::ssl::context::tls_server);
    const auto require = [](bool success) {
        if (!success) {
            throw std::runtime_error("could not generate test certificate");
        }
    };
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
    require(generator != nullptr);
    require(EVP_PKEY_keygen_init(generator.get()) == 1);
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1);
    EVP_PKEY* raw_key = nullptr;
    require(EVP_PKEY_generate(generator.get(), &raw_key) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
    require(certificate != nullptr);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    require(name != nullptr);
    require(X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_subject_name(certificate.get(), name.get()) == 1);
    require(X509_set_issuer_name(certificate.get(), name.get()) == 1);
    require(ruvia::test::sign_tls_certificate(certificate.get(), key.get()) > 0);
    require(SSL_CTX_use_certificate(tls.native_handle(), certificate.get()) == 1);
    require(SSL_CTX_use_PrivateKey(tls.native_handle(), key.get()) == 1);

    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool handshake_rejected = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), tls);
        std::error_code error;
        co_await stream.async_handshake(asio::ssl::stream_base::server,
            asio::redirect_error(asio::use_awaitable, error));
        handshake_rejected = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool tls_failure = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("https://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error& error) {
            tls_failure = error.code() == ruvia::make_error_code(ruvia::http_client_errc::tls_failed);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(tls_failure);
    EXPECT_TRUE(handshake_rejected);
}

namespace {

struct client_frame final {
    unsigned char opcode_;
    std::string payload_{};
};

asio::awaitable<client_frame> read_client_frame(asio::ip::tcp::socket& socket) {
    std::array<unsigned char, 2> header_value{};
    co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
    if ((header_value[1] & 0x80) == 0) {
        throw std::runtime_error("expected masked client frames");
    }
    std::uint64_t payload_bytes = header_value[1] & 0x7f;
    if (payload_bytes >= 126) {
        std::array<unsigned char, 8> length_value{};
        const std::size_t length_size = payload_bytes == 126 ? 2 : 8;
        co_await asio::async_read(socket, asio::buffer(length_value.data(), length_size), asio::use_awaitable);
        payload_bytes = 0;
        for (std::size_t i = 0; i < length_size; ++i) {
            payload_bytes = (payload_bytes << 8) | length_value[i];
        }
    }
    std::array<unsigned char, 4> mask{};
    co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
    std::string payload_value(static_cast<std::size_t>(payload_bytes), '\0');
    co_await asio::async_read(socket, asio::buffer(payload_value), asio::use_awaitable);
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>(payload_value[i] ^ static_cast<char>(mask[i % mask.size()]));
    }
    co_return client_frame{static_cast<unsigned char>(header_value[0] & 0x0f), std::move(payload_value)};
}

// Answers the client's Close and ends the peer.
asio::awaitable<void> answer_client_close(asio::ip::tcp::socket& socket) {
    constexpr std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
    co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
    socket.shutdown(asio::ip::tcp::socket::shutdown_send);
}

void check_heartbeat_policy(bool enabled) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto worker = attachment.loop().worker();
    ruvia::worker_signal peer_done(worker);
    std::exception_ptr peer_failure;
    std::size_t pings = 0;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        try {
            for (;;) {
                const auto frame = co_await read_client_frame(socket);
                if (frame.opcode_ == 0x8) {
                    co_await answer_client_close(socket);
                    co_return;
                }
                if (frame.opcode_ == 0x9) {
                    ++pings;
                    continue;  // Deliberately never acknowledge heartbeat Pings.
                }
                if (frame.opcode_ == 0x1) {
                    constexpr std::string_view ready = "\x81\x05ready";
                    co_await asio::async_write(socket, asio::buffer(ready), asio::use_awaitable);
                }
            }
        } catch (const std::system_error& error) {
            if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset) {
                throw;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .read_timeout_ = std::chrono::seconds(5),
                                                         .request_timeout_ = std::nullopt});
        std::exception_ptr failure;
        try {
            ruvia::websocket_lifecycle_options lifecycle;
            lifecycle.heartbeat_.ping_interval_ =
                enabled ? std::optional(std::chrono::milliseconds(20)) : std::nullopt;
            lifecycle.heartbeat_.pong_timeout_ =
                enabled ? std::optional(std::chrono::milliseconds(50)) : std::nullopt;
            auto websocket = co_await client.client().websocket("/").lifecycle(lifecycle).connect();
            if (!enabled) {
                (void)co_await ruvia::sleep_for(websocket.worker(), std::chrono::milliseconds(200));
                EXPECT_TRUE(websocket.connected());
            }
            co_await websocket.text("check");
            const auto ready = co_await websocket.read();
            EXPECT_TRUE(ready && ready->payload() == "ready");
            if (enabled) {
                const auto started = std::chrono::steady_clock::now();
                EXPECT_FALSE((co_await websocket.read()).has_value());
                EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
                EXPECT_TRUE(!websocket.connected());
                EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::timeout);
            } else {
                co_await websocket.close();
                EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::local_close);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        // Opening can fail before the peer accepts. Cancel that pending accept
        // before joining its completion so the original exception is reported.
        peer.close();
        co_await peer_done.wait();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(enabled ? pings != 0 : pings == 0);
}

enum class client_failure_scenario : std::uint8_t {
    protocol_error,
    message_too_large,
    aborted,
};

// The peer sends the scenario's frame, then drains the client until it ends.
void check_client_failure_termination(client_failure_scenario scenario) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto worker = attachment.loop().worker();
    ruvia::worker_signal peer_done(worker);
    std::exception_ptr peer_failure;
    std::string close_payload{};
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response(make_server_handshake_response(request));
        if (scenario == client_failure_scenario::protocol_error) {
            // Reserved data opcode 0x3 (RFC 6455 section 5.2).
            response.append("\x83\x00", 2);
        } else if (scenario == client_failure_scenario::message_too_large) {
            response.append("\x81\x05hello", 7);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        try {
            for (;;) {
                auto frame = co_await read_client_frame(socket);
                if (frame.opcode_ == 0x8) {
                    close_payload = std::move(frame.payload_);
                }
            }
        } catch (const std::system_error& error) {
            if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset) {
                throw;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    ruvia::websocket_termination termination{};
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_sent = 0;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url, .read_timeout_ = std::chrono::seconds(5)});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").max_message_bytes(4).connect();
            EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::none);
            if (scenario == client_failure_scenario::aborted) {
                websocket.abort();
            } else {
                EXPECT_FALSE((co_await websocket.read()).has_value());
            }
            // A later abort keeps the first cause.
            websocket.abort();
            termination = websocket.termination();
            bytes_received = websocket.bytes_received();
            bytes_sent = websocket.bytes_sent();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        co_await peer_done.wait();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    // A rejected frame is never counted; the failure Close carries only its
    // status code (2 + 4 + 2 bytes).
    EXPECT_EQ(bytes_received, 0U);
    switch (scenario) {
        case client_failure_scenario::protocol_error:
            EXPECT_TRUE(termination == ruvia::websocket_termination::protocol_error);
            EXPECT_EQ(close_payload, std::string_view("\x03\xea", 2));
            EXPECT_EQ(bytes_sent, 8U);
            break;
        case client_failure_scenario::message_too_large:
            EXPECT_TRUE(termination == ruvia::websocket_termination::message_too_large);
            EXPECT_EQ(close_payload, std::string_view("\x03\xf1", 2));
            EXPECT_EQ(bytes_sent, 8U);
            break;
        case client_failure_scenario::aborted:
            EXPECT_TRUE(termination == ruvia::websocket_termination::aborted);
            EXPECT_TRUE(close_payload.empty());
            EXPECT_EQ(bytes_sent, 0U);
            break;
    }
}

}  // namespace

TEST(Ruvia, websocket_client_active_receive_times_out_an_unanswered_heartbeat) {
    check_heartbeat_policy(true);
}

TEST(Ruvia, websocket_client_termination_reports_protocol_failures_and_local_abort) {
    check_client_failure_termination(client_failure_scenario::protocol_error);
    check_client_failure_termination(client_failure_scenario::message_too_large);
    check_client_failure_termination(client_failure_scenario::aborted);
}

TEST(Ruvia, websocket_client_absent_ping_interval_disables_heartbeat) {
    check_heartbeat_policy(false);
}

TEST(Ruvia, websocket_client_keeps_a_peer_whose_pong_waits_for_the_next_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        bool answered = false;
        for (;;) {
            const auto frame = co_await read_client_frame(socket);
            if (frame.opcode_ == 0x8) {
                co_await answer_client_close(socket);
                co_return;
            }
            if (frame.opcode_ != 0x9) {
                continue;
            }
            std::string pong({static_cast<char>(0x8a), static_cast<char>(frame.payload_.size())});
            pong.append(frame.payload_);
            if (!answered) {
                pong.append("\x81\x05ready", 7);
                answered = true;
            }
            co_await asio::async_write(socket, asio::buffer(pong), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool connected_before_read = false;
    bool received = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20),
                                                                                    .pong_timeout_ = std::chrono::milliseconds(50)}})
                                 .connect();
            // The answered Ping's Pong stays unread far past its timeout.
            (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(300));
            connected_before_read = websocket.connected();
            const auto message = co_await websocket.read();
            received = message.has_value() && message->payload() == std::string_view("ready");
            co_await websocket.close({});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(connected_before_read);
    EXPECT_TRUE(received);
}

TEST(Ruvia, websocket_client_read_progresses_between_back_to_back_writes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        bool answered = false;
        for (;;) {
            const auto frame = co_await read_client_frame(socket);
            if (frame.opcode_ == 0x8) {
                co_await answer_client_close(socket);
                co_return;
            }
            if (!answered) {
                // Arrives while the client keeps writing.
                constexpr std::string_view hello = "\x81\x05hello";
                co_await asio::async_write(socket, asio::buffer(hello), asio::use_awaitable);
                answered = true;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    constexpr std::size_t write_count = 1000;
    std::size_t writes = 0;
    std::size_t writes_before_read = write_count;
    bool received = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
            ruvia::task_scope reader(client.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                const auto message = co_await websocket.read();
                writes_before_read = writes;
                received = message.has_value() && message->payload() == std::string_view("hello");
            };
            reader.spawn(receive());
            for (; writes != write_count; ++writes) {
                co_await websocket.binary(std::as_bytes(std::span(std::string_view("x"))));
            }
            co_await reader.join();
            co_await websocket.close({});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(received);
    EXPECT_TRUE(writes_before_read < write_count);
}

TEST(Ruvia, websocket_client_handshake_rejection_records_upstream_http_status) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view rejected =
            "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(rejected), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    std::optional<std::error_code> error_code;
    std::optional<ruvia::http_status_code> status_code;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error& error) {
            error_code = error.code();
            status_code = error.status();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(error_code == ruvia::make_error_code(ruvia::http_client_errc::handshake_rejected));
    EXPECT_TRUE(status_code == ruvia::http_status::unauthorized);
}
