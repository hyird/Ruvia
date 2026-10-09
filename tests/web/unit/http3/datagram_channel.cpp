#include <array>
#include <chrono>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/buffer_pool.h"
#include "ruvia/core/worker_notification.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/core/worker_runtime_context.h"

#include "http3/http3_datagram_channel.h"
#include "http3/http3_datagram_endpoint.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {
using channel = ruvia::detail::http3_datagram_channel;
using endpoint = ruvia::detail::http3_worker_datagram_endpoint;
using namespace std::chrono_literals;

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
            completion.wait_for(5s) != std::future_status::ready) {
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

const channel::udp::endpoint local(asio::ip::address_v4::loopback(), 4433);
const channel::udp::endpoint peer(asio::ip::address_v4::loopback(), 5544);

channel::datagram receive_packet(ruvia::buffer_pool& pool, std::byte value) {
    auto lease_value = pool.try_acquire();
    if (!lease_value) {
        std::terminate();
    }
    lease_value->bytes()[0] = value;
    return {std::move(*lease_value), 1, local, peer};
}

void retire(channel& packets, packet_worker& worker_value, ruvia::testing::test_context& ruvia_ctx) {
    packets.acceptor_close();
    worker_value.invoke([&] { packets.worker_stop(); });
    packets.acceptor_poll();
    worker_value.invoke([&] { packets.worker_close(); });
    RUVIA_CHECK(packets.worker_closed());
    RUVIA_CHECK(packets.acceptor_finalize());
}
}  // namespace

RUVIA_TEST(http3_datagram_channel_full_input_recovers_without_overwriting_borrowed_packet) {
    asio::io_context io;
    ruvia::worker_runtime_context acceptor(io, 8);
    ruvia::worker_notification notification(acceptor);
    ruvia::test::counting_memory_resource memory;
    {
        ruvia::buffer_pool pool(8, channel::packet_capacity, &memory);
        packet_worker worker;
        channel packets(pool, notification, &memory, 2, 1);
        packets.stage_worker(worker.runtime_);
        worker.invoke([&] { packets.worker_start(); });
        auto first = receive_packet(pool, std::byte{1});
        auto* original = first.storage_.bytes().data();
        auto second = receive_packet(pool, std::byte{3});
        auto dropped = receive_packet(pool, std::byte{4});
        RUVIA_CHECK(packets.acceptor_push(std::move(first)));
        RUVIA_CHECK(!first.storage_);
        RUVIA_CHECK(packets.acceptor_push(std::move(second)));
        RUVIA_CHECK(!packets.acceptor_push(std::move(dropped)));
        RUVIA_CHECK(dropped.storage_);
        RUVIA_CHECK(worker.invoke([&] {
            const auto input = packets.worker_input();
            return input && input->bytes_.data() == original && input->bytes_[0] == std::byte{1} &&
                   input->local_destination_ == local && input->peer_ == peer;
        }));
        RUVIA_CHECK(!packets.acceptor_push(std::move(dropped)));
        const auto available = pool.available();
        worker.invoke([&] { packets.worker_consume_input(); });
        // Worker completion returns a credit, never touches Acceptor free indices.
        RUVIA_CHECK_EQ(pool.available(), available);
        // Dequeue frees a descriptor, not the channel's outstanding RX budget.
        RUVIA_CHECK(!packets.acceptor_push(std::move(dropped)));
        RUVIA_CHECK(dropped.storage_);
        packets.acceptor_poll();
        RUVIA_CHECK_EQ(pool.available(), available + 1);
        RUVIA_CHECK(packets.acceptor_push(std::move(dropped)));
        RUVIA_CHECK(worker.invoke([&] {
            const auto input = packets.worker_input();
            const bool ordered = input && input->bytes_[0] == std::byte{3};
            packets.worker_consume_input();
            const auto next_value = packets.worker_input();
            const bool replacement = next_value && next_value->bytes_[0] == std::byte{4};
            packets.worker_consume_input();
            return ordered && replacement && !packets.worker_input();
        }));
        retire(packets, worker, ruvia_ctx);
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(http3_datagram_channel_reservation_cancellation_and_output_window_recover) {
    asio::io_context io;
    ruvia::worker_runtime_context acceptor(io, 8);
    ruvia::worker_notification notification(acceptor);
    ruvia::test::counting_memory_resource memory;
    {
        ruvia::buffer_pool pool(5, channel::packet_capacity, &memory);
        packet_worker worker;
        channel packets(pool, notification, &memory, 3, 1);
        packets.stage_worker(worker.runtime_);
        worker.invoke([&] { packets.worker_start(); });
        const auto allocations = memory.allocation_count();
        auto* prepared = worker.invoke([&] {
            auto bytes_value = packets.worker_output_buffer();
            const bool stable = bytes_value.size() == channel::packet_capacity &&
                                packets.worker_output_buffer().data() == bytes_value.data();
            packets.worker_cancel_output();
            return stable ? bytes_value.data() : nullptr;
        });
        RUVIA_CHECK(prepared);
        RUVIA_CHECK(!packets.acceptor_take_output());
        RUVIA_CHECK(worker.invoke([&] {
            // Idle writes cancel locally: the same issued lease and descriptor
            // admission are reusable without any Acceptor credit round trip.
            for (unsigned count = 0; count != 8; ++count) {
                if (!packets.worker_outbound_capacity()) {
                    return false;
                }
                auto bytes_value = packets.worker_output_buffer();
                if (bytes_value.data() != prepared) {
                    return false;
                }
                packets.worker_cancel_output();
                if (!packets.worker_outbound_quiescent() || packets.worker_outbound_count() != 0) {
                    return false;
                }
            }
            return packets.worker_outbound_capacity();
        }));
        RUVIA_CHECK_EQ(pool.available(), std::size_t{4});
        RUVIA_CHECK(worker.invoke([&] {
            auto bytes_value = packets.worker_output_buffer();
            if (bytes_value.data() != prepared) {
                return false;
            }
            bytes_value[0] = std::byte{1};
            bytes_value[1] = std::byte{2};
            return packets.worker_send(bytes_value.first(2), local, peer) &&
                   packets.worker_outbound_count() == 1 && packets.worker_output_buffer().empty();
        }));
        auto loan = packets.acceptor_take_output();
        RUVIA_CHECK(loan && loan->view().bytes_.data() == prepared && loan->size_ == 2);
        RUVIA_CHECK(worker.invoke([&] { return !packets.worker_outbound_capacity(); }));
        RUVIA_CHECK(loan && loan->view().bytes_[1] == std::byte{2});
        // Completion releases the linear lease independently of any QUIC ACK.
        loan.reset();
        packets.acceptor_poll();
        RUVIA_CHECK(worker.invoke([&] {
            return packets.worker_outbound_count() == 0 && packets.worker_outbound_capacity();
        }));
        packets.acceptor_close(std::make_error_code(std::errc::operation_canceled));
        worker.invoke([&] { packets.worker_stop(); });
        packets.acceptor_poll();
        worker.invoke([&] { packets.worker_close(); });
        RUVIA_CHECK(packets.acceptor_finalize());
        RUVIA_CHECK(packets.error() == std::errc::operation_canceled);
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
        RUVIA_CHECK_EQ(memory.allocation_count(), allocations);
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
}

RUVIA_TEST(http3_worker_endpoint_waits_for_out_of_order_udp_lease_completion_before_detach) {
    asio::io_context io;
    ruvia::worker_runtime_context acceptor(io, 8);
    ruvia::worker_notification notification(acceptor);
    ruvia::test::counting_memory_resource memory;
    {
        ruvia::buffer_pool pool(7, channel::packet_capacity, &memory);
        packet_worker worker;
        auto packets = std::make_unique<channel>(pool, notification, &memory, 2, 3);
        packets->stage_worker(worker.runtime_);
        std::unique_ptr<endpoint> consumer;
        std::size_t completed{};
        worker.invoke([&] {
            packets->worker_start();
            consumer = std::make_unique<endpoint>(*packets, local,
                endpoint::notification{&completed, [](void* context_value, endpoint::notification_kind kind) noexcept {
                                           if (kind == endpoint::notification_kind::output_drained) {
                                               ++*static_cast<std::size_t*>(context_value);
                                           }
                                       }});
            consumer->prepare();
            static_cast<void>(consumer->start());
            for (unsigned value = 1; value != 4; ++value) {
                auto bytes_value = consumer->packet_buffer();
                bytes_value[0] = std::byte(value);
                if (consumer->send_datagram(bytes_value.first(1), local, peer) != endpoint::pump_result::pending) {
                    std::terminate();
                }
            }
        });
        auto first = packets->acceptor_take_output();
        auto second = packets->acceptor_take_output();
        auto third = packets->acceptor_take_output();
        RUVIA_CHECK(first && second && third);
        auto* reusable = third->storage_.bytes().data();
        RUVIA_CHECK(worker.invoke([&] {
            return consumer->outbound_count() == 3 && !consumer->outbound_capacity() &&
                   consumer->packet_buffer().empty();
        }));
        third.reset();
        packets->acceptor_poll();
        RUVIA_CHECK(first->view().bytes_[0] == std::byte{1});
        RUVIA_CHECK(second->view().bytes_[0] == std::byte{2});
        RUVIA_CHECK(worker.invoke([&] {
            consumer->poll_channel();
            auto bytes_value = consumer->packet_buffer();
            const bool returned = completed == 1 && consumer->outbound_count() == 2 &&
                                  bytes_value.data() == reusable;
            consumer->cancel_packet();
            return returned;
        }));
        packets->acceptor_close(std::make_error_code(std::errc::operation_canceled));
        RUVIA_CHECK(worker.invoke([&] {
            consumer->poll_channel();
            consumer->request_stop();
            return !consumer->endpoint_retired() && consumer->status() == endpoint::stop_status::pending;
        }));
        RUVIA_CHECK(!packets->worker_closed());
        second.reset();
        RUVIA_CHECK(first->view().bytes_[0] == std::byte{1});
        first.reset();
        packets->acceptor_poll();
        RUVIA_CHECK(worker.invoke([&] {
            consumer->poll_channel();
            const bool drained = consumer->endpoint_retired() && consumer->outbound_quiescent();
            consumer->retire_channel();
            packets->worker_close();
            return drained;
        }));
        RUVIA_CHECK(packets->acceptor_finalize());
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
        packets.reset();
        RUVIA_CHECK(worker.invoke([&] {
            consumer->poll_channel();
            consumer->cancel_packet();
            consumer->retire_channel();
            const std::array bytes_value{std::byte{1}};
            const bool retired = consumer->endpoint_retired() && consumer->outbound_quiescent() &&
                                 !consumer->outbound_pending() && consumer->outbound_count() == 0 &&
                                 consumer->status() == endpoint::stop_status::error &&
                                 consumer->send_datagram(bytes_value, local, peer) == endpoint::pump_result::stopped;
            consumer.reset();
            return retired;
        }));
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(http3_datagram_channel_full_lanes_recover_through_wrapping_independent_credit_batches) {
    asio::io_context io;
    ruvia::worker_runtime_context acceptor(io, 8);
    ruvia::worker_notification notification(acceptor);
    ruvia::buffer_pool pool(7, channel::packet_capacity);
    packet_worker worker;
    channel packets(pool, notification, nullptr, 3, 2);
    packets.stage_worker(worker.runtime_);
    worker.invoke([&] { packets.worker_start(); });
    for (unsigned round = 0; round != 5; ++round) {
        for (unsigned value = 1; value != 4; ++value) {
            auto packet = receive_packet(pool, std::byte(value));
            RUVIA_CHECK(packets.acceptor_push(std::move(packet)));
        }
        worker.invoke([&] {
            // Fill real TX descriptors while RX credits return independently;
            // UDP completion below retains and returns actual submitted leases.
            for (unsigned count = 0; count != 2; ++count) {
                auto bytes_value = packets.worker_output_buffer();
                if (bytes_value.empty()) {
                    std::terminate();
                }
                bytes_value[0] = std::byte(count + 1);
                if (!packets.worker_send(bytes_value.first(1), local, peer)) {
                    std::terminate();
                }
            }
            for (unsigned value = 1; value != 4; ++value) {
                auto packet = packets.worker_input();
                RUVIA_CHECK(packet && packet->bytes_[0] == std::byte(value));
                packets.worker_consume_input();
            }
        });
        RUVIA_CHECK_EQ(pool.available(), std::size_t{2});
        auto unrouted = receive_packet(pool, std::byte{9});
        const auto* retained = unrouted.storage_.bytes().data();
        RUVIA_CHECK(!packets.acceptor_push(std::move(unrouted)));
        RUVIA_CHECK(unrouted.storage_.bytes().data() == retained);
        unrouted.storage_.reset();
        auto first = packets.acceptor_take_output();
        auto second = packets.acceptor_take_output();
        RUVIA_CHECK(first && second);
        RUVIA_CHECK(worker.invoke([&] { return !packets.worker_outbound_capacity(); }));
        packets.acceptor_poll();
        RUVIA_CHECK_EQ(pool.available(), std::size_t{5});
        RUVIA_CHECK(worker.invoke([&] { return !packets.worker_outbound_capacity(); }));
        second.reset();
        RUVIA_CHECK(first && first->view().bytes_[0] == std::byte{1});
        first.reset();
        packets.acceptor_poll();
        RUVIA_CHECK(worker.invoke([&] {
            return packets.worker_outbound_count() == 0 && packets.worker_outbound_capacity();
        }));
    }
    retire(packets, worker, ruvia_ctx);
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
}

RUVIA_TEST(http3_datagram_channel_cold_abandonment_reclaims_only_on_acceptor_owner) {
    asio::io_context io;
    ruvia::worker_runtime_context acceptor(io, 8);
    ruvia::worker_notification notification(acceptor);
    asio::io_context worker_io;
    ruvia::worker_runtime_context worker_value(worker_io, 8);
    ruvia::test::counting_memory_resource memory;
    {
        ruvia::buffer_pool pool(4, channel::packet_capacity, &memory);
        channel packets(pool, notification, &memory, 1, 2);
        packets.stage_worker(worker_value);
        auto input = receive_packet(pool, std::byte{1});
        RUVIA_CHECK(packets.acceptor_push(std::move(input)));
        const auto outstanding = pool.outstanding();
        std::thread coordinator([&] { packets.abandon_worker(); });
        coordinator.join();
        RUVIA_CHECK_EQ(pool.outstanding(), outstanding);
        RUVIA_CHECK(!packets.worker_closed());
        RUVIA_CHECK(packets.acceptor_finalize());
        RUVIA_CHECK(packets.worker_closed());
        RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
        auto late = receive_packet(pool, std::byte{2});
        RUVIA_CHECK(!packets.acceptor_push(std::move(late)));
        late.storage_.reset();
    }
    RUVIA_CHECK_EQ(memory.live_allocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocation_count(), memory.deallocation_count());
}

RUVIA_TEST(http3_datagram_channel_rejects_unrepresentable_or_unfunded_budget) {
    asio::io_context io;
    ruvia::worker_runtime_context acceptor(io, 8);
    ruvia::worker_notification notification(acceptor);
    ruvia::buffer_pool pool(4, channel::packet_capacity);
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        channel packets(pool, notification, nullptr, std::numeric_limits<std::size_t>::max(), 1);
    }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        channel packets(pool, notification, nullptr, 3, 2);
    }));
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
}

RUVIA_TEST(http3_datagram_channel_full_credit_budget_retires_prepared_and_available_leases) {
    asio::io_context io;
    ruvia::worker_runtime_context acceptor(io, 8);
    ruvia::worker_notification notification(acceptor);
    ruvia::buffer_pool pool(6, channel::packet_capacity);
    packet_worker worker;
    channel packets(pool, notification, nullptr, 3, 2);
    packets.stage_worker(worker.runtime_);
    worker.invoke([&] { packets.worker_start(); });
    for (unsigned value = 0; value != 3; ++value) {
        auto packet = receive_packet(pool, std::byte(value));
        RUVIA_CHECK(packets.acceptor_push(std::move(packet)));
    }
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{5});
    RUVIA_CHECK(worker.invoke([&] { return !packets.worker_output_buffer().empty(); }));
    packets.acceptor_close();
    worker.invoke([&] {
        // All three RX credits remain pending when the final two TX returns
        // fill the independent credit lane. Prepared TX still counts in O.
        packets.worker_stop();
    });
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{5});
    RUVIA_CHECK(!packets.acceptor_finalize());
    RUVIA_CHECK_EQ(pool.outstanding(), std::size_t{0});
    worker.invoke([&] { packets.worker_close(); });
    RUVIA_CHECK(packets.acceptor_finalize());
}
