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

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/web/http_client.h"
#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"

#include "http2_wire_fixture.h"
#endif

#include "test_io_context.h"

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
