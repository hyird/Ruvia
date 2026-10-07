#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/web/detail/http3/http3_datagram_endpoint.h"

#include "test_harness.h"

namespace {
using native_endpoint = ruvia::detail::http3_acceptor_datagram_endpoint;
using worker_endpoint = ruvia::detail::http3_worker_datagram_endpoint;
using channel = ruvia::detail::http3_datagram_channel;
using udp = asio::ip::udp;
using namespace std::chrono_literals;

class endpoint_worker final {
public:
    endpoint_worker() {
        owner_.start();
    }
    ~endpoint_worker() {
        owner_.request_stop();
        owner_.join();
    }
    [[nodiscard]] ruvia::WorkerRuntimeContext& runtime() noexcept {
        return owner_.context();
    }
    template <typename function>
    auto invoke(function operation) {
        using result = decltype(operation());
        auto task = std::make_shared<std::packaged_task<result()>>(std::move(operation));
        auto completion = task->get_future();
        if (!runtime().submission().post([task] { (*task)(); }).accepted() ||
            completion.wait_for(5s) != std::future_status::ready) {
            std::terminate();
        }
        return completion.get();
    }

private:
    ruvia::worker_runtime owner_{{.mailbox_capacity = 8}};
};

struct endpoint_fixture final {
    asio::io_context io;

    ruvia::WorkerRuntimeContext& runtime;
    ruvia::WorkerNotification notification{runtime};
    ruvia::buffer_pool pool{5, channel::packet_capacity};
    channel packets{pool, notification, nullptr, 2, 2};
    native_endpoint native{io, udp::endpoint(asio::ip::address_v4::loopback(), 0),
        {nullptr, [](void*, native_endpoint::notification_kind) noexcept {}}, pool};
    std::optional<worker_endpoint> worker;

    explicit endpoint_fixture(ruvia::WorkerRuntimeContext& context)
        : runtime(context) {
        packets.stage_worker(runtime);
        packets.worker_start();
        native.prepare();
        worker.emplace(packets, udp::endpoint(asio::ip::address_v4::loopback(), native.bound_port()),
            worker_endpoint::notification{nullptr, [](void*, worker_endpoint::notification_kind) noexcept {}});
        worker->prepare();
        (void)worker->start();
        (void)native.start();
    }
    ~endpoint_fixture() {
        worker->request_stop();
        packets.acceptor_close();
        native.request_stop();
        while (packets.acceptor_take_output()) {
        }
        if (!until([&] {
                packets.acceptor_poll();
                worker->poll_channel();
                return native.endpoint_retired() && worker->status() != worker_endpoint::stop_status::pending;
            })) {
            std::terminate();
        }
        worker.reset();
        packets.worker_close();
        if (!packets.acceptor_finalize()) {
            std::terminate();
        }
        notification.close();
    }
    template <class predicate>
    bool until(predicate ready) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (ready()) {
                return true;
            }
            if (io.stopped()) {
                io.restart();
            }
            io.run_for(1ms);
        }
        return ready();
    }
    udp::endpoint local() const {
        return {asio::ip::address_v4::loopback(), native.bound_port()};
    }
};
}  // namespace

RUVIA_TEST(http3_native_receive_transfers_lease_without_copying) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture(runner.runtime());
        udp::socket peer(fixture.io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        const std::array payload{std::byte{0x41}, std::byte{0x42}};
        peer.send_to(asio::buffer(payload), fixture.local());
        std::optional<channel::datagram> received;
        RUVIA_CHECK(fixture.until([&] { received = fixture.native.take_receive(); return received.has_value(); }));
        if (!received) {
            return;
        }
        const auto* bytes = received->view().bytes.data();
        RUVIA_CHECK(fixture.packets.acceptor_push(std::move(*received)));
        const auto borrowed = fixture.worker->receive_slot();
        RUVIA_CHECK(borrowed.has_value());
        RUVIA_CHECK(borrowed->bytes.data() == bytes);
        RUVIA_CHECK(borrowed->peer == peer.local_endpoint());
        RUVIA_CHECK(borrowed->local_destination == fixture.local());
        RUVIA_CHECK(borrowed->bytes[1] == payload[1]);
        (void)fixture.worker->consume_receive();
        const auto outstanding = fixture.pool.outstanding();
        fixture.packets.acceptor_poll();
        RUVIA_CHECK_EQ(fixture.pool.outstanding(), outstanding - 1);
        fixture.native.poll_receive();
    });
}

RUVIA_TEST(http3_worker_output_native_send_keeps_storage_until_completion) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture(runner.runtime());
        udp::socket peer(fixture.io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        auto output = fixture.worker->packet_buffer();
        RUVIA_CHECK(!output.empty());
        output[0] = std::byte{0x53};
        const auto* bytes = output.data();
        RUVIA_CHECK(fixture.worker->send_datagram(output.first(1), fixture.local(), peer.local_endpoint()) == worker_endpoint::pump_result::pending);
        auto packet = fixture.packets.acceptor_take_output();
        RUVIA_CHECK(packet.has_value());
        RUVIA_CHECK(packet->view().bytes.data() == bytes);
        RUVIA_CHECK(fixture.native.send_owned_datagram(std::move(*packet)) == native_endpoint::pump_result::pending);
        RUVIA_CHECK(fixture.native.outbound_pending());
        RUVIA_CHECK_EQ(fixture.worker->outbound_count(), std::size_t{1});
        RUVIA_CHECK(fixture.until([&] { return !fixture.native.outbound_pending(); }));
        std::array<std::byte, 8> input{};
        udp::endpoint sender;
        asio::error_code error;
        const auto count = peer.receive_from(asio::buffer(input), sender, 0, error);
        RUVIA_CHECK(!error);
        RUVIA_CHECK_EQ(count, std::size_t{1});
        RUVIA_CHECK(input[0] == std::byte{0x53});
        RUVIA_CHECK(sender == fixture.local());
        RUVIA_CHECK_EQ(fixture.worker->outbound_count(), std::size_t{0});
    });
}

RUVIA_TEST(http3_worker_prepared_output_cancel_preserves_capacity) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture(runner.runtime());
        const auto outstanding = fixture.pool.outstanding();
        auto prepared = fixture.worker->packet_buffer();
        RUVIA_CHECK(!prepared.empty());
        fixture.worker->cancel_packet();
        RUVIA_CHECK_EQ(fixture.pool.outstanding(), outstanding);
        RUVIA_CHECK_EQ(fixture.worker->outbound_count(), std::size_t{0});
        RUVIA_CHECK(fixture.worker->outbound_capacity());
        RUVIA_CHECK(fixture.worker->packet_buffer().data() == prepared.data());
        fixture.worker->cancel_packet();
    });
}

RUVIA_TEST(http3_native_cancellation_retains_receive_and_send_leases) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture(runner.runtime());
        udp::socket peer(fixture.io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        auto output = fixture.worker->packet_buffer();
        output[0] = std::byte{0x54};
        (void)fixture.worker->send_datagram(output.first(1), fixture.local(), peer.local_endpoint());
        auto packet = fixture.packets.acceptor_take_output();
        (void)fixture.native.send_owned_datagram(std::move(*packet));
        const auto outstanding = fixture.pool.outstanding();
        fixture.native.request_stop();
        RUVIA_CHECK_EQ(fixture.pool.outstanding(), outstanding);
        RUVIA_CHECK(fixture.native.status() == native_endpoint::stop_status::pending);
        RUVIA_CHECK(fixture.until([&] { return fixture.native.endpoint_retired(); }));
        RUVIA_CHECK_EQ(fixture.pool.outstanding(), outstanding - 2);
        RUVIA_CHECK(fixture.native.status() == native_endpoint::stop_status::done);
    });
}

RUVIA_TEST(http3_worker_invalid_addresses_stop_without_native_submission) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture(runner.runtime());
        auto output = fixture.worker->packet_buffer();
        output[0] = std::byte{0x55};
        RUVIA_CHECK(fixture.worker->send_datagram(output.first(1), fixture.local(),
                        udp::endpoint(asio::ip::address_v4::any(), 1234)) == worker_endpoint::pump_result::error);
        RUVIA_CHECK(fixture.worker->error() == std::make_error_code(std::errc::bad_message));
        RUVIA_CHECK(!fixture.native.outbound_pending());
    });
}

RUVIA_TEST(http3_native_bind_failure_releases_prepared_resources) {
    endpoint_worker runner;
    runner.invoke([&] {
        asio::io_context io;
        udp::socket occupied(io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        ruvia::buffer_pool pool(1, channel::packet_capacity);
        native_endpoint endpoint(io, occupied.local_endpoint(),
            {nullptr, [](void*, native_endpoint::notification_kind) noexcept {}}, pool);
        RUVIA_CHECK(ruvia::testing::throwsOn([&] { endpoint.prepare(); }));
        RUVIA_CHECK(endpoint.status() == native_endpoint::stop_status::done);
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
    });
}

RUVIA_TEST(http3_native_backpressure_and_stop_leave_caller_lease_untouched) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture(runner.runtime());
        udp::socket peer(fixture.io, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        for (unsigned value = 1; value != 3; ++value) {
            auto output = fixture.worker->packet_buffer();
            output[0] = std::byte(value);
            (void)fixture.worker->send_datagram(output.first(1), fixture.local(), peer.local_endpoint());
        }
        auto first = fixture.packets.acceptor_take_output();
        auto second = fixture.packets.acceptor_take_output();
        RUVIA_CHECK(first && second);
        if (!first || !second) {
            return;
        }
        (void)fixture.native.send_owned_datagram(std::move(*first));
        const auto* retained = second->storage.bytes().data();
        RUVIA_CHECK(fixture.native.send_owned_datagram(std::move(*second)) == native_endpoint::pump_result::pending);
        RUVIA_CHECK(second->storage.bytes().data() == retained);
        fixture.native.request_stop();
        RUVIA_CHECK(fixture.native.send_owned_datagram(std::move(*second)) == native_endpoint::pump_result::stopped);
        RUVIA_CHECK(second->storage.bytes().data() == retained);
        second.reset();
    });
}

RUVIA_TEST(http3_worker_oversized_datagram_stops_before_native_submission) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture(runner.runtime());
        auto output = fixture.worker->packet_buffer();
        RUVIA_CHECK(fixture.worker->send_datagram(output.first(65508), fixture.local(),
                        udp::endpoint(asio::ip::address_v4::loopback(), 1234)) == worker_endpoint::pump_result::error);
        RUVIA_CHECK(fixture.worker->error() == std::make_error_code(std::errc::message_size));
        RUVIA_CHECK(!fixture.native.outbound_pending());
    });
}
