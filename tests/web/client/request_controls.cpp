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

#include "event_loop.h"
#include "http_client.h"
#include "http_client_request_view.h"
#include "http_content_codec.h"
#include "http_content_coding.h"
#include "task_scope.h"
#ifdef RUVIA_ENABLE_HTTP2
#include "http2_connection.h"
#include "http2_hpack.h"
#include "http_server_fixture.h"
#endif

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "stop_token.h"
#include "worker_signal.h"
#include "worker_task_fixture.h"

namespace http_client_request_controls_test {

class peer final {
public:
    peer(asio::io_context& io, const ruvia::worker_ref& worker, bool http2, bool silent) :
        acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
        socket_(io),
        gate_(io),
        done_(worker),
        http2_(http2),
        silent_(silent) {
        gate_.expires_at(std::chrono::steady_clock::time_point::max());
        encoded_ = *ruvia::encode_http_content(ruvia::http_content_coding::gzip, "streamed representation", {.max_encoded_bytes_ = 1024});
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
                EXPECT_FALSE(decoder
                        .decode(payload, [this](std::string_view name, std::string_view value) {
                    received_.append(name).append(": ").append(value).append("\n");
                    return true;
                }).has_value());
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
        ruvia::http_client client(attachment.loop(), {.base_url_ = url, .decode_content_ = !configured_raw, .read_timeout_ = std::chrono::milliseconds(5), .request_timeout_ = std::chrono::seconds(3), .versions_ = http2 ? ruvia::http_client_versions{.http1_ = false} : ruvia::http_client_versions{.http2_ = false}});
        try {
            bool informed = false;
            auto request = client.client().get("/raw");
            request.header("Content-Type", "unparsed forwarded value").configure({.decode_content_ = configured_raw ? std::optional<bool>{} : std::optional<bool>{false}, .field_source_ = ruvia::http_request_field_source::forwarded, .timeouts_ = {.read_ = ruvia::http_client_timeout_override::disabled()}, .informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> fields, ruvia::stop_token stop) -> ruvia::task<void> {
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
        ruvia::http_client client(attachment.loop(), {.base_url_ = url, .read_timeout_ = std::chrono::seconds(2), .request_timeout_ = total_only ? std::chrono::milliseconds(30) : std::chrono::milliseconds(2000), .versions_ = http2 ? ruvia::http_client_versions{.http1_ = false} : ruvia::http_client_versions{.http2_ = false}});
        std::optional<ruvia::http_client_error> observed_error;
        try {
            auto request = client.client().get("/silent");
            request.configure({.timeouts_ = {.read_ = total_only ? ruvia::http_client_timeout_override::disabled() : ruvia::http_client_timeout_override::after(std::chrono::milliseconds(20))}});
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
        ruvia::http_client client(attachment.loop(), {.base_url_ = url, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http1_ = false}});
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
        ruvia::http_client client(attachment.loop(), {.base_url_ = logical_url, .dial_host_ = "::1", .versions_ = ruvia::http_client_versions{.http2_ = false}});
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

        const ruvia::http_client_error error(std::make_error_code(std::errc::timed_out), "observed timeout", std::nullopt, &source);
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
        EXPECT_THROW((void)ruvia::http_client_endpoint_address("127.0.0.1", invalid), std::invalid_argument);
        ruvia::http_client_endpoint_address endpoint("127.0.0.1", 443);
        EXPECT_THROW(endpoint.port_ = invalid, std::invalid_argument);
        ruvia::http_client_config config{.base_url_ = "http://127.0.0.1:8001"};
        EXPECT_THROW(config.dial_port_ = invalid, std::invalid_argument);
        EXPECT_THROW(((void)ruvia::http_client_config{.base_url_ = "http://127.0.0.1:8001", .dial_port_ = invalid}), std::invalid_argument);
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
        constexpr std::string_view redirect_reply = "HTTP/1.1 303 See Other\r\nLocation: /landed\r\nContent-Length: 0\r\n\r\n";
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(socket, asio::buffer(redirect_reply), std::move(handler));
        });
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read_until(socket, asio::dynamic_buffer(second_hop_request), "\r\n\r\n", std::move(handler));
        });
        constexpr std::string_view final_reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
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
        builder.header("Content-Type", "text/plain").header("Digest", "SHA-256=abc=").header("Repr-Digest", "sha-256=:abc=:").header("X-Trace", "keep-me").body("body");
        auto pending = builder.send();
        EXPECT_THROW(builder.redirects(1), std::logic_error);
        auto response = co_await std::move(pending);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "ok");

        EXPECT_THROW(client.client().websocket("/ws").configure({.dial_port_ = 0}), std::invalid_argument);
        EXPECT_THROW(client.client().websocket("/ws").configure({.authority_ = "user@example.test"}), std::invalid_argument);

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
            constexpr std::string_view seed_reply = "HTTP/1.1 200 OK\r\nSet-Cookie: jar_secret=stored; Path=/\r\nContent-Length: 2\r\n\r\nok";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(seed_reply), std::move(handler));
            });

            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read_until(socket, asio::dynamic_buffer(get_request_head), "\r\n\r\n", std::move(handler));
            });
            constexpr std::string_view verify_reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
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
            constexpr std::string_view connect_reply = "HTTP/1.1 200 Connection Established\r\n\r\n";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(connect_reply), std::move(handler));
            });
        }
    };

    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::task_scope peers(worker);
        peers.spawn(serve_origin_and_proxy());

        ruvia::http_client client(attachment.loop(),
            {
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
        const auto head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(observed.request_), "\r\n\r\n", asio::use_awaitable);
        observed.payload_ = observed.request_.substr(head_bytes);
        observed.request_.resize(head_bytes);
        co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        std::array<char, 4096> bytes;
        for (;;) {
            std::error_code error;
            const auto count = co_await socket.async_read_some(asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
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
        ruvia::http_client client(attachment.loop(), {.base_url_ = url, .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}});
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
        const auto observed = client_upgrade_tests::exchange(wire, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            unsigned informational_count = 0;
            auto request = client.client().get("/generic");
            request.header("Connection", "Upgrade").header("Upgrade", protocol).header("Sec-WebSocket-Protocol", "caller-owned").header("Sec-WebSocket-Key", "caller-owned-key");
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>, ruvia::stop_token) -> ruvia::task<void> {
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
    client_upgrade_tests::exchange("HTTP/1.1 426 Upgrade Required\r\nContent-Length: 6\r\nX-Rejected: yes\r\n\r\ndenied", [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
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
    for (const std::string_view fields : {"Upgrade: chat/7\r\n", "Connection: Upgrade\r\n", "Connection: Upgrade\r\nUpgrade: other/1\r\n", "Connection: close, Upgrade\r\nUpgrade: chat/7\r\n"}) {
        SCOPED_TRACE(fields);
        std::string wire = "HTTP/1.1 101 Switching Protocols\r\n";
        wire.append(fields);
        wire.append("\r\nnot-a-tunnel");
        client_upgrade_tests::exchange(wire, [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
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
        request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> headers, ruvia::stop_token stop) -> ruvia::task<void> {
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
    client_upgrade_tests::exchange("HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        auto request = client.client().get("/callback-failure");
        request.configure({.informational_ = [](ruvia::http_status_code, std::span<const ruvia::http_header>, ruvia::stop_token) -> ruvia::task<void> {
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
        client_upgrade_tests::exchange(scenario.wire_, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
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
        client_upgrade_tests::exchange("HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            ruvia::stop_source stop;
            ruvia::worker_signal entered(worker);
            const auto handle = client.client().with_options({.timeout_ = deadline ? std::optional<ruvia::timeout_duration>(std::chrono::milliseconds(500)) : std::nullopt, .stop_token_ = stop.token()});
            auto request = handle.get("/cancel-hints");
            bool callback_stopped = false;
            bool callback_entered = false;
            bool completed = false;
            std::exception_ptr failure;
            std::optional<std::error_code> code;
            request.configure({.informational_ = [&](ruvia::http_status_code, std::span<const ruvia::http_header>, ruvia::stop_token token) -> ruvia::task<void> {
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
            EXPECT_EQ(code, deadline ? std::make_error_code(std::errc::timed_out) : std::make_error_code(std::errc::operation_canceled));
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
            const auto head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
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
                const auto count = co_await socket.async_read_some(asio::buffer(bytes.data(), std::min(bytes.size(), content_bytes - received_body)), asio::use_awaitable);
                EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + count, [](char value) {
                    return value == 'b';
                }));
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
            ruvia::http_client client(attachment.loop(), {.base_url_ = url, .request_timeout_ = std::chrono::seconds(5), .versions_ = ruvia::http_client_versions{.http2_ = false}});
            ruvia::stop_source stop;
            const auto handle = client.client().with_options({.stop_token_ = stop.token()});
            auto request = handle.post("/buffered");
            std::string content(content_bytes, 'b');
            request.body(content);
            std::string().swap(content);
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>, ruvia::stop_token token) -> ruvia::task<void> {
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
        ruvia::http_client client(attachment.loop(), {.base_url_ = url, .read_timeout_ = std::chrono::milliseconds(50), .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}});
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
