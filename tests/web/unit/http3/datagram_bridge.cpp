#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <span>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>

#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/worker_notification.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_runtime_context.h"

#include "http3/http3_datagram_channel.h"
#include "http3/http3_datagram_endpoint.h"
#include "http3/http3_quic_client_tls_context.h"
#include "http3/http3_quic_client_transport.h"
#include "http3/http3_quic_packet_io.h"
#include "http3/http3_quic_socket_address.h"
#include "test_harness.h"

namespace {

class packet_worker final {
    ruvia::worker_runtime owner_{{.queue_capacity_ = 8}};

public:
    packet_worker()
        : runtime_(owner_.context()) {
        owner_.start();
    }
    template <typename function>
    auto invoke(function operation) {
        using result = decltype(operation());
        auto task_value = std::make_shared<std::packaged_task<result()>>(std::move(operation));
        auto completion = task_value->get_future();
        if (!runtime_.submission().post([task_value] { (*task_value)(); }).accepted() ||
            completion.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            std::terminate();
        }
        return completion.get();
    }
    ~packet_worker() {
        owner_.request_stop();
        owner_.join();
    }
    ruvia::worker_runtime_context& runtime_;
};

ruvia::detail::http3_quic_datagram_address address(std::uint16_t port) {
    using namespace ruvia::detail;
    return std::get<0>(to_http3_quic_datagram_address(
        asio::ip::udp::endpoint(asio::ip::address_v4({127, 0, 0, 1}), port)));
}

}  // namespace

RUVIA_TEST(http3_quic_packet_io_writes_directly_into_reserved_transport_lease) {
    using namespace ruvia::detail;
    http3_quic_client_tls_context tls(client_transport_config_view{});
    const auto local = address(40000);
    const auto peer = address(4433);
    ruvia::quic_connection_config config;
    config.local_address_ = to_quic_address(local);
    config.peer_address_ = to_quic_address(peer);
    http3_quic_client_transport transport(tls, config, "example.test",
        std::chrono::steady_clock::now());

    asio::io_context acceptor_io;
    ruvia::worker_runtime_context acceptor(acceptor_io, 8);
    packet_worker worker;
    ruvia::worker_notification notification(acceptor);
    ruvia::buffer_pool pool(4, http3_datagram_channel::packet_capacity);
    http3_datagram_channel channel(pool, notification, nullptr, 1, 1);
    channel.stage_worker(worker.runtime_);
    std::unique_ptr<http3_worker_datagram_endpoint> endpoint;
    std::span<std::byte> packet;
    const auto result_value = worker.invoke([&] {
        channel.worker_start();
        endpoint = std::make_unique<http3_worker_datagram_endpoint>(channel, std::get<0>(to_udp_endpoint(local)),
            http3_worker_datagram_endpoint::notification{
                nullptr, [](void*, http3_worker_datagram_endpoint::notification_kind) noexcept {}});
        endpoint->prepare();
        static_cast<void>(endpoint->start());
        packet = endpoint->packet_buffer();
        const auto written = http3_quic_packet_io::write(transport.connection(), packet.first(1500),
            std::chrono::steady_clock::now());
        RUVIA_CHECK(written.size_ != 0);
        RUVIA_CHECK(written.size_ <= 1500);
        const auto expected_peer = to_quic_address(peer);
        RUVIA_CHECK(written.peer_.bytes_ == expected_peer.bytes_);
        RUVIA_CHECK(written.peer_.port_ == expected_peer.port_);
        RUVIA_CHECK(written.peer_.family_ == expected_peer.family_);
        RUVIA_CHECK(endpoint->send_datagram(packet.first(written.size_),
                        std::get<0>(to_udp_endpoint(local)), std::get<0>(to_udp_endpoint(peer))) ==
                    http3_worker_datagram_endpoint::pump_result::pending);
        RUVIA_CHECK(!endpoint->outbound_capacity());
        RUVIA_CHECK(endpoint->packet_buffer().empty());
        return written;
    });
    auto submitted = channel.acceptor_take_output();
    RUVIA_CHECK(submitted && submitted->view().bytes_.data() == packet.data());
    RUVIA_CHECK(submitted && submitted->size_ == result_value.size_);
    channel.acceptor_close();
    worker.invoke([&] {
        endpoint->request_stop();
        RUVIA_CHECK(!endpoint->endpoint_retired());
    });
    // Native completion, not protocol ACK, retires this exact output lease.
    submitted.reset();
    channel.acceptor_poll();
    worker.invoke([&] {
        endpoint->poll_channel();
        RUVIA_CHECK(endpoint->endpoint_retired());
        RUVIA_CHECK(endpoint->outbound_quiescent());
        endpoint.reset();
        channel.worker_close();
    });
    RUVIA_CHECK(channel.acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
}
