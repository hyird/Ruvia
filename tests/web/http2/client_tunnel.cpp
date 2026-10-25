#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_tunnel.h"

#include "http_server_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct tunnel_observation {
    std::mutex mutex_;
    std::string bytes_;
    std::atomic<bool> finish_first_{};
    std::atomic<bool> eof_{};
    [[nodiscard]] std::string received() {
        std::lock_guard lock(mutex_);
        return bytes_;
    }
};
const auto observation = std::make_shared<tunnel_observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<tunnel_observation>>([] { return observation; });
    return true;
}();

class client_tunnel_routes final {
public:
    RUVIA_ROUTES_BEGIN(client_tunnel_routes)
    RUVIA_CONNECT("client-target.test:443", echo);
    RUVIA_CONNECT_PROTOCOL("test-protocol", "/tunnel", echo);
    RUVIA_CONNECT_PROTOCOL("test-capsules", "/capsules", capsules);
    RUVIA_GET("/sibling", sibling);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& c);
    ruvia::task<void> capsules(ruvia::context& c);
    ruvia::task<ruvia::http_response> sibling(ruvia::context& c) {
        co_return c.text("sibling");
    }
};
ruvia::task<void> client_tunnel_routes::echo(ruvia::context& c) {
    auto& observation_value = *c.worker_state<std::shared_ptr<tunnel_observation>>();
    {
        std::lock_guard lock(observation_value.mutex_);
        observation_value.bytes_.clear();
    }
    observation_value.eof_ = false;
    auto& tunnel = c.tunnel();
    if (observation_value.finish_first_) {
        co_await tunnel.write("greeting");
        co_await tunnel.finish();
    }
    while (auto bytes = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.bytes_.append(*bytes);
        }
        if (!observation_value.finish_first_) {
            co_await tunnel.write(std::string_view(*bytes));
        }
    }
    observation_value.eof_ = true;
    co_await tunnel.finish();
}
}  // namespace
RUVIA_TEST(http2_client_tunnel_owns_cold_input_half_closes_and_preserves_siblings) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    std::optional<ruvia::http_client_tunnel> retained_tunnel;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        auto& observation_value = *observation;
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = server.endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
        const auto client_origin = client.handle();
        try {
            for (unsigned round = 0; round != 4; ++round) {
                observation_value.finish_first_ = round >= 2;
                std::string authority = round == 1 ? "proxy.test" : "client-target.test:443";
                auto cold = client_origin.open_tunnel({.authority_ = authority, .protocol_ = round == 1 ? "test-protocol" : "", .target_ = round == 1 ? "/tunnel" : ""});
                authority.assign("mutated");
                auto result_value = co_await std::move(cold);
                RUVIA_CHECK(result_value.response() == nullptr && result_value.tunnel() != nullptr);
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT was rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                RUVIA_CHECK(tunnel.status() == ruvia::http_status::ok);
                RUVIA_CHECK(!tunnel.header("content-length"));
                std::string echoed;
                const auto receive = [&]() -> ruvia::task<void> {
                    while (auto bytes = co_await tunnel.read()) {
                        echoed.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                };
                if (observation_value.finish_first_) {
                    co_await receive();
                    RUVIA_CHECK(echoed == "greeting");
                }
                ruvia::task_scope readers(worker_value);
                if (round == 3) {
                    tunnel.abort();
                    bool half_closed_write_rejected = false;
                    try {
                        auto late_write = tunnel.write("late");
                        static_cast<void>(late_write);
                    } catch (const ruvia::http_client_error& error) {
                        half_closed_write_rejected = error.code() == ruvia::http_client_error::code_type::cancelled;
                    }
                    RUVIA_CHECK(half_closed_write_rejected);
                    co_await readers.join();
                    auto sibling = co_await client_origin.send({.target_ = "/sibling"});
                    std::string sibling_body;
                    while (auto bytes = co_await sibling.body().text()) {
                        sibling_body.append(*bytes);
                    }
                    RUVIA_CHECK(sibling_body == "sibling");
                    retained_tunnel.emplace(std::move(tunnel));
                    continue;
                }
                if (!observation_value.finish_first_) {
                    readers.spawn(receive());
                }
                std::string payload_value(100003, 't');
                for (std::size_t offset = 0; offset != payload_value.size();) {
                    const auto count = std::min<std::size_t>(16384, payload_value.size() - offset);
                    auto output = tunnel.write(std::string_view(payload_value).substr(offset, count));
                    {
                        // Discarding a cold finish does not claim the write lane.
                        auto unused_finish = tunnel.finish();
                        static_cast<void>(unused_finish);
                    }
                    co_await std::move(output);
                    offset += count;
                }
                co_await tunnel.finish();
                co_await readers.join();
                if (!observation_value.finish_first_) {
                    RUVIA_CHECK(echoed == payload_value);
                }
                auto sibling = co_await client_origin.send({.target_ = "/sibling"});
                std::string sibling_body;
                while (auto bytes = co_await sibling.body().text()) {
                    sibling_body.append(*bytes);
                }
                RUVIA_CHECK(sibling_body == "sibling");
                RUVIA_CHECK(observation_value.received() == payload_value && observation_value.eof_);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (retained_tunnel) {
            retained_tunnel->abort();
            retained_tunnel->abort();
            bool late_write_rejected = false;
            try {
                auto late_write = retained_tunnel->write("late");
                static_cast<void>(late_write);
            } catch (const ruvia::http_client_error&) {
                late_write_rejected = true;
            }
            RUVIA_CHECK(late_write_rejected);
            retained_tunnel.reset();
        }
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

namespace {
ruvia::task<void> client_tunnel_routes::capsules(ruvia::context& c) {
    auto stream = c.tunnel().capsules();
    while (auto capsule = co_await stream.read()) {
        co_await stream.write(capsule->type(), capsule->payload());
    }
    co_await stream.finish();
}
}  // namespace
RUVIA_TEST(http2_client_capsule_stream_retains_results_and_cold_operations_after_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_capsule> retained;
        std::unique_ptr<ruvia::task<void>> cold;
        {
            ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = server.endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
            const auto client_origin = client.handle();
            try {
                auto result_value = co_await client_origin.open_tunnel({.authority_ = "proxy.test", .protocol_ = "test-capsules", .target_ = "/capsules"}, {.max_chunk_bytes_ = 1024});
                if (!result_value.tunnel()) {
                    throw std::runtime_error("capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*result_value.tunnel()).capsules();
                    std::string bytes_value(4099, 'c');
                    auto write = stream.write(0x123456789ULL, bytes_value);
                    bytes_value.assign("mutated");
                    co_await std::move(write);
                    retained = co_await stream.read();
                    RUVIA_CHECK(retained && retained->type() == 0x123456789ULL && retained->payload() == std::string(4099, 'c'));
                    co_await stream.write(0, "");
                    auto empty = co_await stream.read();
                    RUVIA_CHECK(empty && empty->type() == 0 && empty->payload().empty());
                    co_await stream.finish();
                    RUVIA_CHECK(!(co_await stream.read()));
                    // Discarding a cold read expires its scope without running it.
                    auto pending = stream.read();
                }
                auto second = co_await client_origin.open_tunnel({.authority_ = "proxy.test", .protocol_ = "test-capsules", .target_ = "/capsules"});
                if (!second.tunnel()) {
                    throw std::runtime_error("second capsule CONNECT rejected");
                }
                {
                    auto stream = std::move(*second.tunnel()).capsules();
                    cold.reset(new ruvia::task<void>(stream.write(12, std::string(4099, 'x'))));
                }
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        RUVIA_CHECK(retained && retained->payload() == std::string(4099, 'c'));
        cold.reset();
        retained.reset();
        server.finish();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// RFC 9297 section 3.2: an unknown capsule type and a DATAGRAM with an
// unknown Context ID (RFC 9298 section 4) are skipped even beyond the reader's
// max_capsule_length_. A malformed DATAGRAM then fails the tunnel as a
// malformed message (RFC 9297 section 3.3): RST_STREAM(PROTOCOL_ERROR) per
// RFC 9113 section 8.1.1, not CANCEL.
RUVIA_TEST(http2_client_udp_tunnel_skips_dropped_capsules_and_resets_malformed_input_with_protocol_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    std::optional<ruvia::http2_error_code> reset_error;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::pmr::monotonic_buffer_resource resource;
        auto connection = ruvia::http2_connection::server({.resource_ = &resource});
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::pmr::string output(&resource);
        while (!reset_error) {
            if (connection.wants_write()) {
                output.clear();
                connection.take_output(output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                break;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                RUVIA_CHECK(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        ruvia::http_response accepted(&resource);
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        RUVIA_CHECK(connection.submit_connect_response_head(head->stream_id(), accepted) ==
                                    ruvia::http2_submit_status::accepted);
                        std::string capsules;
                        capsules.append("\x17\x40\xc8", 3).append(200, 'u');
                        capsules.append("\x00\x40\xc9\x03", 4).append(200, 'c');
                        capsules.append(
                            "\x00\x06\x00"
                            "after",
                            8);
                        capsules.append("\x00\x00", 2);
                        RUVIA_CHECK(connection.submit_data(head->stream_id(), capsules, ruvia::http2_end_stream::keep_open) ==
                                    ruvia::http2_data_submit_status::accepted);
                        request.emplace(std::move(*head));
                    } else if (auto* closed = event->stream_closed()) {
                        if (closed->source() == ruvia::http2_stream_close_source::peer) {
                            reset_error = closed->error();
                        }
                    } else if (auto* data = event->tunnel_data()) {
                        (void)connection.acknowledge(data->take_credit());
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
        request.reset();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    std::string first;
    bool malformed_rejected = false;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = peer.local_endpoint().port(), .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .protocol_ = ruvia::http_client_protocol::http2_only});
        try {
            auto opened = co_await client.handle().open_udp_tunnel({.target_ = "/.well-known/masque/udp/target.test/53/"});
            if (!opened.tunnel()) {
                throw std::runtime_error("CONNECT-UDP rejected");
            }
            auto udp = std::move(*opened.tunnel()).udp({.max_capsule_length_ = 100});
            auto datagram = co_await udp.read();
            if (datagram) {
                first.assign(reinterpret_cast<const char*>(datagram->payload().data()), datagram->payload().size());
            }
            try {
                (void)co_await udp.read();
            } catch (const std::runtime_error&) {
                malformed_rejected = true;
            }
            // The reset is queued with the abort; let the session write it
            // before shutdown closes the connection.
            const auto& worker_value = attachment.loop().handle();
            for (unsigned attempt = 0; !reset_error && attempt != 200; ++attempt) {
                (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(10));
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    RUVIA_CHECK_EQ(first, std::string("after"));
    RUVIA_CHECK(malformed_rejected);
    RUVIA_CHECK(reset_error == ruvia::http2_error_code::protocol_error);
}

// WST-01: When the client half-closes its tunnel send direction first (finish())
// and the server responds with a truncated capsule on DATA(END_STREAM), the
// client still resets the stream with RST_STREAM(PROTOCOL_ERROR).
RUVIA_TEST(http2_client_half_closed_tunnel_resets_truncated_capsule_on_end_stream_with_protocol_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::string raw_from_client;
    bool server_done = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::pmr::monotonic_buffer_resource resource;
        auto connection = ruvia::http2_connection::server({.resource_ = &resource});
        std::optional<ruvia::http2_request_head_event> request;
        std::array<char, 16384> input{};
        std::pmr::string output(&resource);
        while (!server_done) {
            if (connection.wants_write()) {
                output.clear();
                connection.take_output(output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            std::size_t count = 0;
            try {
                count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            } catch (const std::system_error&) {
                break;
            }
            const auto bytes_value = std::string_view(input.data(), count);
            raw_from_client.append(bytes_value);
            for (;;) {
                const auto fed = connection.feed(bytes_value);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        ruvia::http_response accepted(&resource);
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)connection.submit_connect_response_head(head->stream_id(), accepted);
                        request.emplace(std::move(*head));
                    } else if (auto* end = event->tunnel_end()) {
                        std::string truncated(
                            "\x00\x0a"
                            "ab",
                            4);
                        (void)connection.submit_data(
                            end->stream_id(), truncated, ruvia::http2_end_stream::end_stream);
                    } else if (auto* data = event->tunnel_data()) {
                        (void)connection.acknowledge(data->take_credit());
                    }
                }
                if (fed != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
        request.reset();
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool client_threw = false;
    auto find_rst_code = [&]() -> std::optional<std::uint32_t> {
        for (std::size_t off = 24; off + ruvia::http2_frame_header_bytes <= raw_from_client.size();) {
            const auto hdr = ruvia::parse_http2_frame_header(
                std::span<const char>(raw_from_client.data() + off, ruvia::http2_frame_header_bytes));
            if (!hdr || off + ruvia::http2_frame_header_bytes + hdr->length_ > raw_from_client.size()) {
                break;
            }
            const auto* p = reinterpret_cast<const unsigned char*>(
                raw_from_client.data() + off + ruvia::http2_frame_header_bytes);
            if (hdr->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::rst_stream) &&
                hdr->length_ == 4) {
                return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                       (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
            }
            off += ruvia::http2_frame_header_bytes + hdr->length_;
        }
        return std::nullopt;
    };

    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.scheme_ = ruvia::http_scheme::http,
                                                         .host_ = "127.0.0.1",
                                                         .port_ = peer.local_endpoint().port(),
                                                         .connection_count_ = 1,
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .protocol_ = ruvia::http_client_protocol::http2_only});
        auto opened = co_await client.handle().open_udp_tunnel(
            {.target_ = "/.well-known/masque/udp/target.test/53/"});
        RUVIA_CHECK(opened.tunnel() != nullptr);
        auto udp = std::move(*opened.tunnel()).udp({.max_capsule_length_ = 100});
        co_await udp.finish();
        try {
            (void)co_await udp.read();
        } catch (const std::runtime_error&) {
            client_threw = true;
        }
        const auto& worker_value = attachment.loop().handle();
        for (unsigned attempt = 0; !find_rst_code() && attempt != 50; ++attempt) {
            (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(10));
        }
        server_done = true;
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    RUVIA_CHECK(client_threw);
    const auto rst_code = find_rst_code();
    RUVIA_CHECK(rst_code.has_value());
    if (rst_code) {
        RUVIA_CHECK_EQ(*rst_code, static_cast<std::uint32_t>(ruvia::http2_error_code::protocol_error));
    }
}
