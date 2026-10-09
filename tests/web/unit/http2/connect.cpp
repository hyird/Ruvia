#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"

#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "memory_resource_fixture.h"
#include "router/router_impl.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct connect_observation {
    std::string received_;
    bool saw_eof_{};
    bool stable_{true};
    bool return_early_{};
};
ruvia::task<void> echo_tunnel(void* raw, ruvia::context& context_value) {
    auto& observation_value = *static_cast<connect_observation*>(raw);
    if (observation_value.return_early_) {
        co_return;
    }
    std::optional<std::pmr::string> retained;
    auto& tunnel = context_value.tunnel();
    {
        auto cold = tunnel.read();
    }
    while (auto chunk = co_await tunnel.read()) {
        observation_value.received_.append(*chunk);
        if (!retained) {
            retained.emplace(*chunk, context_value.pool());
        }
        auto output = tunnel.write(std::string_view(*chunk));
        chunk->assign("changed-input");
        co_await std::move(output);
        observation_value.stable_ = observation_value.stable_ && retained->find_first_not_of('t') == std::string_view::npos;
    }
    observation_value.saw_eof_ = true;
    co_await tunnel.finish();
}
ruvia::task<ruvia::http_response> after_tunnel(void*, ruvia::context& context_value) {
    co_return context_value.text("sibling");
}
ruvia::task<void> serve_connect(asio::ip::tcp::acceptor& acceptor, const ruvia::worker_handle& worker_value,
    const ruvia::detail::route_table& routes_value, ruvia::worker_memory& memory) {
    auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto completion) { acceptor.async_accept(std::move(completion)); });
    if (accepted.error_code()) {
        throw std::system_error(accepted.error_code());
    }
    auto socket = std::move(accepted.result());
    ruvia::test::http2_sans_io_session_fixture fixture;
    ruvia::connection_scanner scanner(worker_value, {.scan_interval_ = std::chrono::milliseconds(5)});
    ruvia::connection_scanner::guard_type guard_value(&scanner, fixture.scanner_entry_, socket);
    scanner.start();
    co_await ruvia::detail::run_http2_sans_io_session(socket, routes_value, memory, fixture.context(fixture.services(worker_value)));
    scanner.stop();
}
ruvia::task<void> flush_peer(asio::ip::tcp::socket& socket, ruvia::http2_connection& connection, std::pmr::memory_resource* resource) {
    std::pmr::string bytes(resource);
    while (connection.wants_write()) {
        bytes.clear();
        (void)connection.take_output_batch(16384, bytes);
        const auto completion = co_await ruvia::async_asio<std::size_t>([&](auto handler) { asio::async_write(socket, asio::buffer(bytes), std::move(handler)); });
        if (completion.error_code()) {
            throw std::system_error(completion.error_code());
        }
    }
}
}  // namespace
RUVIA_TEST(http2_connect_routes_echo_large_duplex_streams_and_leave_sibling_requests_available) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::counting_memory_resource allocations;
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().handle();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        ruvia::worker_memory memory(allocations);
        ruvia::detail::router router;
        auto& routes_value = ruvia::detail::router_impl::from(router);
        connect_observation observation;
        routes_value.register_tunnel_route({}, std::pmr::string("target.test:443"), {&observation, &echo_tunnel}, {}, {},
            {.peer_transport_fin_timeout_ = std::chrono::milliseconds(40)});
        routes_value.register_tunnel_route("test-tunnel", std::pmr::string("/udp/:host/:port"), {&observation, &echo_tunnel}, {}, {});
        routes_value.register_route(ruvia::http_known_method::get, std::pmr::string("/after"), {nullptr, &after_tunnel}, ruvia::detail::request_body_mode::buffered, {}, {});
        routes_value.finalize();
        ruvia::task_scope tasks(worker_value);
        tasks.spawn(serve_connect(acceptor, worker_value, routes_value.route_table(), memory));
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(acceptor.local_endpoint(), std::move(handler)); });
            if (connected.error_code()) {
                throw std::system_error(connected.error_code());
            }
            auto peer = ruvia::http2_connection::client({.resource_ = memory.resource()});
            std::array<char, 16384> input{};
            std::string received_wire;
            auto pump = [&]() -> ruvia::task<void> {
                const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) { socket.async_read_some(asio::buffer(input), std::move(handler)); });
                if (read.error_code()) {
                    throw std::system_error(read.error_code());
                }
                received_wire.append(input.data(), read.result());
                if (peer.feed(std::string_view(input.data(), read.result())) == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("CONNECT peer protocol failure");
                }
            };
            co_await flush_peer(socket, peer, memory.resource());
            while (!peer.received_peer_settings()) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (peer.connection_error()) {
                        throw std::runtime_error("CONNECT SETTINGS failure");
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            for (unsigned round = 0; round != 3; ++round) {
                observation.return_early_ = round == 2;
                const auto request = round != 1 ? peer.submit_request_head(ruvia::http2_connect_request_head_view{.authority_ = "TARGET.TEST:443"})
                                                : peer.submit_request_head(ruvia::http2_extended_connect_request_head_view{.protocol_ = "test-tunnel", .scheme_ = "https", .authority_ = "proxy.test", .target_ = "/udp/target.test/443"});
                if (!request.submitted()) {
                    throw std::runtime_error("CONNECT request submission rejected");
                }
                const auto id = request.submitted()->stream_id();
                co_await flush_peer(socket, peer, memory.resource());
                bool established = false;
                while (!established) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (const auto* head = event->response_head()) {
                            RUVIA_CHECK(head->stream_id() == id);
                            RUVIA_CHECK(head->head().status() == ruvia::http_status::ok);
                            RUVIA_CHECK(std::ranges::none_of(head->head().headers(), [](const auto& header_value) { return header_value.name() == "content-length"; }));
                            established = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT handshake reset");
                        }
                    }
                    co_await flush_peer(socket, peer, memory.resource());
                }
                const std::string payload_value(100003, 't');
                if (round == 2) {
                    bool reset = false;
                    while (!reset) {
                        co_await pump();
                        while (auto event = peer.next_event()) {
                            if (event->stream_closed()) {
                                reset = true;
                            }
                            RUVIA_CHECK(!peer.connection_error());
                        }
                        co_await flush_peer(socket, peer, memory.resource());
                    }
                    bool sent_fin = false;
                    for (std::size_t offset = 0; offset + ruvia::http2_frame_header_bytes <= received_wire.size();) {
                        const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(received_wire.data() + offset, received_wire.size() - offset));
                        if (!header_value || header_value->length_ > received_wire.size() - offset - ruvia::http2_frame_header_bytes) {
                            break;
                        }
                        sent_fin = sent_fin || (header_value->stream_id_ == id && header_value->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && (header_value->flags_ & 1U) != 0);
                        offset += ruvia::http2_frame_header_bytes + header_value->length_;
                    }
                    RUVIA_CHECK(sent_fin);
                    continue;
                }
                const auto submitted = peer.submit_data(id, payload_value, ruvia::http2_end_stream::end_stream);
                RUVIA_CHECK(submitted == ruvia::http2_data_submit_status::accepted || submitted == ruvia::http2_data_submit_status::queued);
                co_await flush_peer(socket, peer, memory.resource());
                std::string echoed;
                bool ended = false;
                while (!ended) {
                    co_await pump();
                    while (auto event = peer.next_event()) {
                        if (auto* data = event->tunnel_data()) {
                            RUVIA_CHECK(data->stream_id() == id);
                            echoed.append(data->bytes());
                            (void)peer.acknowledge(data->take_credit());
                        }
                        if (const auto* end = event->tunnel_end()) {
                            RUVIA_CHECK(end->stream_id() == id);
                            ended = true;
                        }
                        if (event->stream_closed() || peer.connection_error()) {
                            throw std::runtime_error("CONNECT stream reset");
                        }
                    }
                    co_await flush_peer(socket, peer, memory.resource());
                }
                RUVIA_CHECK(echoed == payload_value);
                RUVIA_CHECK(observation.saw_eof_ && observation.stable_);
                RUVIA_CHECK(observation.received_ == payload_value);
                observation.received_.clear();
                observation.saw_eof_ = false;
            }
            auto sibling = peer.submit_request_head(ruvia::http2_regular_request_head_view{.authority_ = "proxy.test", .target_ = "/after"});
            if (!sibling.submitted()) {
                throw std::runtime_error("sibling request failed");
            }
            co_await flush_peer(socket, peer, memory.resource());
            bool ended = false;
            std::string body;
            while (!ended) {
                co_await pump();
                while (auto event = peer.next_event()) {
                    if (auto* bytes = event->message_body_chunk()) {
                        body.append(bytes->bytes());
                    }
                    if (event->message_end()) {
                        ended = true;
                    }
                }
                co_await flush_peer(socket, peer, memory.resource());
            }
            RUVIA_CHECK(body == "sibling");
        } catch (...) {
            failure = std::current_exception();
        }
        ruvia::close_socket(socket);
        acceptor.close();
        (void)watchdog_value.cancel();
        co_await tasks.join();
        RUVIA_CHECK(!expired);
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    io.run();
    root.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(allocations.live_allocations(), std::size_t{0});
}
