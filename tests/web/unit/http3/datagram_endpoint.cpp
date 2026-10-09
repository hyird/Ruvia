#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <memory_resource>
#include <optional>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_runtime_context.h"

#include "http3/http3_datagram_endpoint.h"
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
    [[nodiscard]] ruvia::worker_runtime_context& runtime() noexcept {
        return owner_.context();
    }
    template <typename function>
    auto invoke(function operation) {
        using result = decltype(operation());
        auto task_value = std::make_shared<std::packaged_task<result()>>(std::move(operation));
        auto completion = task_value->get_future();
        if (!runtime().submission().post([task_value] { (*task_value)(); }).accepted() ||
            completion.wait_for(5s) != std::future_status::ready) {
            std::terminate();
        }
        return completion.get();
    }

private:
    ruvia::worker_runtime owner_{{.queue_capacity_ = 8}};
};

struct endpoint_fixture final {
    asio::io_context io_;

    ruvia::worker_runtime_context& runtime_;
    ruvia::worker_notification notification_{runtime_};
    ruvia::buffer_pool pool_{5, channel::packet_capacity};
    channel packets_{pool_, notification_, nullptr, 2, 2};
    native_endpoint native_{io_, udp::endpoint(asio::ip::address_v4::loopback(), 0),
        {nullptr, [](void*, native_endpoint::notification_kind) noexcept {}}, pool_};
    std::optional<worker_endpoint> worker_;

    explicit endpoint_fixture(ruvia::worker_runtime_context& context_value)
        : runtime_(context_value) {
        packets_.stage_worker(runtime_);
        packets_.worker_start();
        native_.prepare();
        worker_.emplace(packets_, udp::endpoint(asio::ip::address_v4::loopback(), native_.bound_port()),
            worker_endpoint::notification{nullptr, [](void*, worker_endpoint::notification_kind) noexcept {}});
        worker_->prepare();
        (void)worker_->start();
        (void)native_.start();
    }
    ~endpoint_fixture() {
        worker_->request_stop();
        packets_.acceptor_close();
        native_.request_stop();
        while (packets_.acceptor_take_output()) {
        }
        if (!until([&] {
                packets_.acceptor_poll();
                worker_->poll_channel();
                return native_.endpoint_retired() && worker_->status() != worker_endpoint::stop_status::pending;
            })) {
            std::terminate();
        }
        worker_.reset();
        packets_.worker_close();
        if (!packets_.acceptor_finalize()) {
            std::terminate();
        }
        notification_.close();
    }
    template <class predicate>
    bool until(predicate ready) {
        const auto deadline_value = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline_value) {
            if (ready()) {
                return true;
            }
            if (io_.stopped()) {
                io_.restart();
            }
            io_.run_for(1ms);
        }
        return ready();
    }
    udp::endpoint local() const {
        return {asio::ip::address_v4::loopback(), native_.bound_port()};
    }
};
}  // namespace

RUVIA_TEST(http3_native_receive_transfers_lease_without_copying) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture_value(runner.runtime());
        udp::socket peer(fixture_value.io_, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        const std::array payload_value{std::byte{0x41}, std::byte{0x42}};
        peer.send_to(asio::buffer(payload_value), fixture_value.local());
        std::optional<channel::datagram> received;
        RUVIA_CHECK(fixture_value.until([&] { received = fixture_value.native_.take_receive(); return received.has_value(); }));
        if (!received) {
            return;
        }
        const auto* bytes_value = received->view().bytes_.data();
        RUVIA_CHECK(fixture_value.packets_.acceptor_push(std::move(*received)));
        const auto borrowed = fixture_value.worker_->receive_slot();
        RUVIA_CHECK(borrowed.has_value());
        RUVIA_CHECK(borrowed->bytes_.data() == bytes_value);
        RUVIA_CHECK(borrowed->peer_ == peer.local_endpoint());
        RUVIA_CHECK(borrowed->local_destination_ == fixture_value.local());
        RUVIA_CHECK(borrowed->bytes_[1] == payload_value[1]);
        (void)fixture_value.worker_->consume_receive();
        const auto outstanding = fixture_value.pool_.outstanding();
        fixture_value.packets_.acceptor_poll();
        RUVIA_CHECK_EQ(fixture_value.pool_.outstanding(), outstanding - 1);
        fixture_value.native_.poll_receive();
    });
}

RUVIA_TEST(http3_worker_output_native_send_keeps_storage_until_completion) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture_value(runner.runtime());
        udp::socket peer(fixture_value.io_, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        peer.non_blocking(true);
        auto output = fixture_value.worker_->packet_buffer();
        RUVIA_CHECK(!output.empty());
        output[0] = std::byte{0x53};
        const auto* bytes_value = output.data();
        RUVIA_CHECK(fixture_value.worker_->send_datagram(output.first(1), fixture_value.local(), peer.local_endpoint()) == worker_endpoint::pump_result::pending);
        auto packet = fixture_value.packets_.acceptor_take_output();
        RUVIA_CHECK(packet.has_value());
        RUVIA_CHECK(packet->view().bytes_.data() == bytes_value);
        RUVIA_CHECK(fixture_value.native_.send_owned_datagram(std::move(*packet)) == native_endpoint::pump_result::pending);
        RUVIA_CHECK(fixture_value.native_.outbound_pending());
        RUVIA_CHECK_EQ(fixture_value.worker_->outbound_count(), std::size_t{1});
        RUVIA_CHECK(fixture_value.until([&] { return !fixture_value.native_.outbound_pending(); }));
        std::array<std::byte, 8> input{};
        udp::endpoint sender;
        asio::error_code error;
        const auto count = peer.receive_from(asio::buffer(input), sender, 0, error);
        RUVIA_CHECK(!error);
        RUVIA_CHECK_EQ(count, std::size_t{1});
        RUVIA_CHECK(input[0] == std::byte{0x53});
        RUVIA_CHECK(sender == fixture_value.local());
        RUVIA_CHECK_EQ(fixture_value.worker_->outbound_count(), std::size_t{0});
    });
}

RUVIA_TEST(http3_worker_prepared_output_cancel_preserves_capacity) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture_value(runner.runtime());
        const auto outstanding = fixture_value.pool_.outstanding();
        auto prepared = fixture_value.worker_->packet_buffer();
        RUVIA_CHECK(!prepared.empty());
        fixture_value.worker_->cancel_packet();
        RUVIA_CHECK_EQ(fixture_value.pool_.outstanding(), outstanding);
        RUVIA_CHECK_EQ(fixture_value.worker_->outbound_count(), std::size_t{0});
        RUVIA_CHECK(fixture_value.worker_->outbound_capacity());
        RUVIA_CHECK(fixture_value.worker_->packet_buffer().data() == prepared.data());
        fixture_value.worker_->cancel_packet();
    });
}

RUVIA_TEST(http3_native_cancellation_retains_receive_and_send_leases) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture_value(runner.runtime());
        udp::socket peer(fixture_value.io_, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        auto output = fixture_value.worker_->packet_buffer();
        output[0] = std::byte{0x54};
        (void)fixture_value.worker_->send_datagram(output.first(1), fixture_value.local(), peer.local_endpoint());
        auto packet = fixture_value.packets_.acceptor_take_output();
        (void)fixture_value.native_.send_owned_datagram(std::move(*packet));
        const auto outstanding = fixture_value.pool_.outstanding();
        fixture_value.native_.request_stop();
        RUVIA_CHECK_EQ(fixture_value.pool_.outstanding(), outstanding);
        RUVIA_CHECK(fixture_value.native_.status() == native_endpoint::stop_status::pending);
        RUVIA_CHECK(fixture_value.until([&] { return fixture_value.native_.endpoint_retired(); }));
        RUVIA_CHECK_EQ(fixture_value.pool_.outstanding(), outstanding - 2);
        RUVIA_CHECK(fixture_value.native_.status() == native_endpoint::stop_status::done);
    });
}

RUVIA_TEST(http3_worker_invalid_addresses_stop_without_native_submission) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture_value(runner.runtime());
        auto output = fixture_value.worker_->packet_buffer();
        output[0] = std::byte{0x55};
        RUVIA_CHECK(fixture_value.worker_->send_datagram(output.first(1), fixture_value.local(),
                        udp::endpoint(asio::ip::address_v4::any(), 1234)) == worker_endpoint::pump_result::error);
        RUVIA_CHECK(fixture_value.worker_->error() == std::make_error_code(std::errc::bad_message));
        RUVIA_CHECK(!fixture_value.native_.outbound_pending());
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
        RUVIA_CHECK(ruvia::testing::throws_on([&] { endpoint.prepare(); }));
        RUVIA_CHECK(endpoint.status() == native_endpoint::stop_status::done);
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
    });
}

RUVIA_TEST(http3_native_backpressure_and_stop_leave_caller_lease_untouched) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture_value(runner.runtime());
        udp::socket peer(fixture_value.io_, udp::endpoint(asio::ip::address_v4::loopback(), 0));
        for (unsigned value = 1; value != 3; ++value) {
            auto output = fixture_value.worker_->packet_buffer();
            output[0] = std::byte(value);
            (void)fixture_value.worker_->send_datagram(output.first(1), fixture_value.local(), peer.local_endpoint());
        }
        auto first = fixture_value.packets_.acceptor_take_output();
        auto second = fixture_value.packets_.acceptor_take_output();
        RUVIA_CHECK(first && second);
        if (!first || !second) {
            return;
        }
        (void)fixture_value.native_.send_owned_datagram(std::move(*first));
        const auto* retained = second->storage_.bytes().data();
        RUVIA_CHECK(fixture_value.native_.send_owned_datagram(std::move(*second)) == native_endpoint::pump_result::pending);
        RUVIA_CHECK(second->storage_.bytes().data() == retained);
        fixture_value.native_.request_stop();
        RUVIA_CHECK(fixture_value.native_.send_owned_datagram(std::move(*second)) == native_endpoint::pump_result::stopped);
        RUVIA_CHECK(second->storage_.bytes().data() == retained);
        second.reset();
    });
}

RUVIA_TEST(http3_worker_oversized_datagram_stops_before_native_submission) {
    endpoint_worker runner;
    runner.invoke([&] {
        endpoint_fixture fixture_value(runner.runtime());
        auto output = fixture_value.worker_->packet_buffer();
        RUVIA_CHECK(fixture_value.worker_->send_datagram(output.first(65508), fixture_value.local(),
                        udp::endpoint(asio::ip::address_v4::loopback(), 1234)) == worker_endpoint::pump_result::error);
        RUVIA_CHECK(fixture_value.worker_->error() == std::make_error_code(std::errc::message_size));
        RUVIA_CHECK(!fixture_value.native_.outbound_pending());
    });
}
