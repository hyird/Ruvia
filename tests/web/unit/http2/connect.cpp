#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/socket.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "http2_server_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
struct connect_observation {
    std::mutex mutex_;
    std::string received_;
    std::atomic<bool> saw_eof_{};
    std::atomic<bool> stable_{true};
    std::atomic<bool> return_early_{};
};
const auto observation = std::make_shared<connect_observation>();
[[maybe_unused]] const bool observation_registered = [] {
    ruvia::app().use_worker_state<std::shared_ptr<connect_observation>>([] { return observation; });
    return true;
}();

class connect_routes final : public ruvia::controller<connect_routes> {
    RUVIA_ROUTES_BEGIN
    const ruvia::http_tunnel_route_config options{
        .peer_transport_fin_timeout_ = std::chrono::milliseconds(40)};
    RUVIA_CONNECT_OPTIONS("target.test:443", echo, options);
    RUVIA_CONNECT_PROTOCOL("test-tunnel", "/udp/:host/:port", echo);
    RUVIA_GET("/after", after);
    RUVIA_ROUTES_END

    ruvia::task<void> echo(ruvia::context& context_value);
    ruvia::task<ruvia::http_response> after(ruvia::context& context_value) {
        co_return context_value.text("sibling");
    }
};
ruvia::task<void> connect_routes::echo(ruvia::context& context_value) {
    auto& observation_value = *context_value.worker_state<std::shared_ptr<connect_observation>>();
    if (observation_value.return_early_) {
        co_return;
    }
    std::optional<std::pmr::string> retained;
    auto& tunnel = context_value.tunnel();
    {
        auto cold = tunnel.read();
    }
    while (auto chunk = co_await tunnel.read()) {
        {
            std::lock_guard lock(observation_value.mutex_);
            observation_value.received_.append(*chunk);
        }
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
    ruvia::test::http2_server_fixture server(io);
    auto run = [&]() -> ruvia::task<void> {
        auto& observation_value = *observation;
        asio::ip::tcp::socket socket(io);
        asio::steady_timer watchdog_value(io, std::chrono::seconds(5));
        bool expired = false;
        watchdog_value.async_wait([&](std::error_code error) { if (!error) { expired = true; ruvia::close_socket(socket); } });
        ruvia::worker_memory memory(allocations);
        try {
            const auto connected = co_await ruvia::async_asio([&](auto handler) { socket.async_connect(server.endpoint(), std::move(handler)); });
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
                observation_value.return_early_ = round == 2;
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
                RUVIA_CHECK(observation_value.saw_eof_ && observation_value.stable_);
                {
                    std::lock_guard lock(observation_value.mutex_);
                    RUVIA_CHECK(observation_value.received_ == payload_value);
                    observation_value.received_.clear();
                }
                observation_value.saw_eof_ = false;
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
        (void)watchdog_value.cancel();
        server.finish();
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
