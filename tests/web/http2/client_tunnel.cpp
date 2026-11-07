#include <gtest/gtest.h>

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
#include "output_drain.h"
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
    const ruvia::http_tunnel_route_config capsule_route{.datagrams_ = true};
    RUVIA_CONNECT_PROTOCOL("test-capsules", "/capsules", capsules, capsule_route);
    RUVIA_CONNECT_PROTOCOL("test-no-datagrams", "/no-datagrams", no_datagrams);
    RUVIA_GET("/sibling", sibling);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& c);
    ruvia::task<void> capsules(ruvia::context& c);
    ruvia::task<void> no_datagrams(ruvia::context& c);
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
TEST(Ruvia, http2_client_tunnel_owns_cold_input_half_closes_and_preserves_siblings) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    std::optional<ruvia::http_client_tunnel> retained_tunnel;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        auto& observation_value = *observation;
        const std::string client_url = "http://127.0.0.1:" + std::to_string(server.endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
        const auto client_origin = client.client();
        try {
            for (unsigned round = 0; round != 4; ++round) {
                observation_value.finish_first_ = round >= 2;
                std::string authority = round == 1 ? "proxy.test" : "client-target.test:443";
                auto cold = client_origin.connect(authority).protocol(round == 1 ? "test-protocol" : "").target(round == 1 ? "/tunnel" : "").open();
                authority.assign("mutated");
                auto result_value = co_await std::move(cold);
                EXPECT_TRUE(result_value.response() == nullptr && result_value.tunnel() != nullptr);
                if (!result_value.tunnel()) {
                    throw std::runtime_error("CONNECT was rejected");
                }
                auto tunnel = std::move(*result_value.tunnel());
                EXPECT_TRUE(tunnel.status() == ruvia::http_status::ok);
                EXPECT_TRUE(!tunnel.header("content-length"));
                std::string echoed;
                const auto receive = [&]() -> ruvia::task<void> {
                    while (auto bytes = co_await tunnel.read()) {
                        echoed.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                    }
                };
                if (observation_value.finish_first_) {
                    co_await receive();
                    EXPECT_TRUE(echoed == "greeting");
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
                    EXPECT_TRUE(half_closed_write_rejected);
                    co_await readers.join();
                    auto sibling = co_await client_origin.get("/sibling").send();
                    std::string sibling_body;
                    while (auto bytes = co_await sibling.stream().text()) {
                        sibling_body.append(*bytes);
                    }
                    EXPECT_TRUE(sibling_body == "sibling");
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
                    EXPECT_TRUE(echoed == payload_value);
                }
                auto sibling = co_await client_origin.get("/sibling").send();
                std::string sibling_body;
                while (auto bytes = co_await sibling.stream().text()) {
                    sibling_body.append(*bytes);
                }
                EXPECT_TRUE(sibling_body == "sibling");
                EXPECT_TRUE(observation_value.received() == payload_value && observation_value.eof_);
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
            EXPECT_TRUE(late_write_rejected);
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

// The route protocol owns Capsule Protocol negotiation (RFC 9297 section 3.4):
// a route that did not declare datagrams cannot open a capsule facade, and the
// raw tunnel stays usable.
ruvia::task<void> client_tunnel_routes::no_datagrams(ruvia::context& c) {
    auto& tunnel = c.tunnel();
    unsigned rejected = 0;
    try {
        static_cast<void>(tunnel.datagrams());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    try {
        static_cast<void>(tunnel.udp());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    try {
        static_cast<void>(tunnel.capsules());
    } catch (const std::logic_error&) {
        ++rejected;
    }
    co_await tunnel.write(rejected == 3 ? std::string_view("rejected") : std::string_view("accepted"));
    co_await tunnel.finish();
}
}  // namespace
TEST(Ruvia, http2_client_capsule_stream_retains_results_and_cold_operations_after_shutdown) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::exception_ptr failure;
    ruvia::test::http_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_capsule> retained;
        std::unique_ptr<ruvia::task<void>> cold;
        {
            const std::string client_url = "http://127.0.0.1:" + std::to_string(server.endpoint().port());
            ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .protocol_ = ruvia::http_client_protocol::http2_only});
            const auto client_origin = client.client();
            try {
                auto result_value = co_await client_origin.connect("proxy.test").protocol("test-capsules").target("/capsules").open({.max_chunk_bytes_ = 1024});
                if (!result_value.tunnel()) {
                    throw std::runtime_error("capsule CONNECT rejected");
                }
                // RFC 9297 section 3.4: the datagram route's 2xx advertises it.
                EXPECT_EQ(result_value.tunnel()->header("Capsule-Protocol"), std::string_view("?1"));
                {
                    auto stream = std::move(*result_value.tunnel()).capsules();
                    std::string bytes_value(4099, 'c');
                    auto write = stream.write(0x123456789ULL, bytes_value);
                    bytes_value.assign("mutated");
                    co_await std::move(write);
                    retained = co_await stream.read();
                    EXPECT_TRUE(retained && retained->type() == 0x123456789ULL && retained->text() == std::string(4099, 'c') && retained->bytes().size() == 4099);
                    co_await stream.write(0, "");
                    auto empty = co_await stream.read();
                    EXPECT_TRUE(empty && empty->type() == 0 && empty->text().empty() && empty->bytes().empty());
                    co_await stream.finish();
                    EXPECT_TRUE(!(co_await stream.read()));
                    // Discarding a cold read expires its scope without running it.
                    auto pending = stream.read();
                }
                {
                    // Without declared datagrams: no Capsule-Protocol, and the
                    // handler's datagrams()/udp()/capsules() throw std::logic_error.
                    auto plain_result = co_await client_origin.connect("proxy.test").protocol("test-no-datagrams").target("/no-datagrams").open();
                    if (!plain_result.tunnel()) {
                        throw std::runtime_error("protocol CONNECT without datagrams rejected");
                    }
                    auto plain = std::move(*plain_result.tunnel());
                    EXPECT_TRUE(!plain.header("Capsule-Protocol"));
                    std::string answer;
                    while (auto text_chunk = co_await plain.text()) {
                        answer.append(*text_chunk);
                    }
                    EXPECT_EQ(answer, std::string("rejected"));
                    co_await plain.finish();
                }
                auto second = co_await client_origin.connect("proxy.test").protocol("test-capsules").target("/capsules").open();
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
        EXPECT_TRUE(retained && retained->payload() == std::string(4099, 'c'));
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
TEST(Ruvia, http2_client_udp_tunnel_skips_dropped_capsules_and_resets_malformed_input_with_protocol_error) {
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
                ruvia::testing::drain_output(connection, output);
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
                EXPECT_TRUE(fed != ruvia::http2_feed_result::protocol_failure);
                while (auto event = connection.next_event()) {
                    if (auto* head = event->request_head()) {
                        ruvia::http_response accepted(&resource);
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        EXPECT_TRUE(connection.submit_connect_response_head(head->stream_id(), accepted) ==
                                    ruvia::http2_submit_status::accepted);
                        std::string capsules;
                        capsules.append("\x17\x40\xc8", 3).append(200, 'u');
                        capsules.append("\x00\x40\xc9\x03", 4).append(200, 'c');
                        capsules.append(
                            "\x00\x06\x00"
                            "after",
                            8);
                        capsules.append("\x00\x00", 2);
                        EXPECT_TRUE(connection.submit_data(head->stream_id(), capsules, ruvia::http2_end_stream::keep_open, 65535).status_ ==
                                    ruvia::http2_data_submit_status::complete);
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
        const std::string client_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_count_ = 1, .request_timeout_ = std::chrono::seconds(5), .protocol_ = ruvia::http_client_protocol::http2_only});
        try {
            auto opened = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
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
    EXPECT_EQ(first, std::string("after"));
    EXPECT_TRUE(malformed_rejected);
    EXPECT_TRUE(reset_error == ruvia::http2_error_code::protocol_error);
}

// A truncated capsule remains an application error after both HTTP/2 stream
// directions close; a closed stream need not emit another reset frame.
TEST(Ruvia, http2_client_half_closed_tunnel_rejects_truncated_capsule_on_end_stream) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
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
                ruvia::testing::drain_output(connection, output);
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
                            end->stream_id(), truncated, ruvia::http2_end_stream::end_stream, 65535);
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

    auto run = [&]() -> ruvia::task<void> {
        const std::string client_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_count_ = 1,
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .protocol_ = ruvia::http_client_protocol::http2_only});
        auto opened = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened.tunnel() != nullptr);
        auto udp = std::move(*opened.tunnel()).udp({.max_capsule_length_ = 100});
        co_await udp.finish();
        try {
            (void)co_await udp.read();
        } catch (const std::runtime_error&) {
            client_threw = true;
        }
        server_done = true;
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    EXPECT_TRUE(client_threw);
}

// A5H2-04: A malformed capsule detected on a completed tunnel from an earlier
// HTTP/2 connection generation must not submit RST_STREAM(PROTOCOL_ERROR) on
// the same stream ID of a newly reconnected HTTP/2 session.
TEST(Ruvia, http2_client_stale_tunnel_malformed_abort_does_not_reset_reconnected_session_stream) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::uint32_t second_conn_rst_stream_id = 0;
    std::uint32_t second_conn_rst_code = 0;
    bool server_done = false;

    const auto serve = [&]() -> asio::awaitable<void> {
        {
            auto socket1 = co_await peer.async_accept(asio::use_awaitable);
            std::pmr::unsynchronized_pool_resource resource;
            auto conn1 = ruvia::http2_connection::server({.resource_ = &resource});
            std::optional<ruvia::http2_request_head_event> req1;
            std::array<char, 16384> input{};
            std::pmr::string output(&resource);
            bool sent_truncated = false;
            while (!sent_truncated) {
                if (conn1.wants_write()) {
                    ruvia::testing::drain_output(conn1, output);
                    co_await asio::async_write(socket1, asio::buffer(output), asio::use_awaitable);
                }
                const auto count = co_await socket1.async_read_some(asio::buffer(input), asio::use_awaitable);
                (void)conn1.feed(std::string_view(input.data(), count));
                while (auto ev = conn1.next_event()) {
                    if (auto* head = ev->request_head()) {
                        ruvia::http_response accepted(&resource);
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)conn1.submit_connect_response_head(head->stream_id(), accepted);
                        req1.emplace(std::move(*head));
                    } else if (auto* end = ev->tunnel_end()) {
                        (void)conn1.submit_data(
                            end->stream_id(), std::string_view("\x00\x0a"
                                                               "ab",
                                                  4),
                            ruvia::http2_end_stream::end_stream, 65535);
                        sent_truncated = true;
                    }
                }
            }
            if (conn1.wants_write()) {
                ruvia::testing::drain_output(conn1, output);
                co_await asio::async_write(socket1, asio::buffer(output), asio::use_awaitable);
            }
            req1.reset();
            std::error_code ec;
            socket1.close(ec);
        }

        {
            auto socket2 = co_await peer.async_accept(asio::use_awaitable);
            std::pmr::unsynchronized_pool_resource resource;
            auto conn2 = ruvia::http2_connection::server({.resource_ = &resource});
            std::optional<ruvia::http2_request_head_event> req2;
            std::array<char, 16384> input{};
            std::pmr::string output(&resource);
            while (!server_done) {
                if (conn2.wants_write()) {
                    ruvia::testing::drain_output(conn2, output);
                    co_await asio::async_write(socket2, asio::buffer(output), asio::use_awaitable);
                }
                std::size_t count = 0;
                try {
                    count = co_await socket2.async_read_some(asio::buffer(input), asio::use_awaitable);
                } catch (const std::system_error&) {
                    break;
                }
                (void)conn2.feed(std::string_view(input.data(), count));
                while (auto ev = conn2.next_event()) {
                    if (auto* head = ev->request_head()) {
                        ruvia::http_response accepted(&resource);
                        accepted.status(ruvia::http_status::ok);
                        accepted.header("capsule-protocol", "?1");
                        (void)conn2.submit_connect_response_head(head->stream_id(), accepted);
                        req2.emplace(std::move(*head));
                    } else if (auto* closed = ev->stream_closed()) {
                        second_conn_rst_stream_id = closed->stream_id();
                        second_conn_rst_code = static_cast<std::uint32_t>(closed->error());
                    }
                }
            }
            req2.reset();
        }
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool udp1_threw = false;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        const std::string client_url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url,
                                                         .connection_count_ = 1,
                                                         .request_timeout_ = std::chrono::seconds(5),
                                                         .protocol_ = ruvia::http_client_protocol::http2_only});
        auto opened1 = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened1.tunnel() != nullptr);
        auto udp1 = std::move(*opened1.tunnel()).udp({.max_capsule_length_ = 100});
        co_await udp1.finish();
        (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(30));

        auto opened2 = co_await client.client().udp("/.well-known/masque/udp/target.test/53/").open();
        EXPECT_TRUE(opened2.tunnel() != nullptr);
        auto udp2 = std::move(*opened2.tunnel()).udp({.max_capsule_length_ = 100});
        (void)udp2;

        try {
            (void)co_await udp1.read();
        } catch (const std::runtime_error&) {
            udp1_threw = true;
        }
        for (unsigned attempt = 0; second_conn_rst_stream_id == 0 && attempt != 30; ++attempt) {
            (void)co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(10));
        }
        server_done = true;
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    root.get();
    EXPECT_TRUE(udp1_threw);
    EXPECT_EQ(second_conn_rst_stream_id, std::uint32_t{0});
    EXPECT_EQ(second_conn_rst_code, std::uint32_t{0});
}
