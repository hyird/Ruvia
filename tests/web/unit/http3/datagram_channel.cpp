#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/WorkerNotification.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/web/detail/http3/Http3DatagramEndpoint.h"
#include "ruvia/web/detail/http3/http3_datagram_channel.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {
using channel = ruvia::detail::http3_datagram_channel;
using endpoint = ruvia::detail::Http3DatagramEndpoint;
using namespace std::chrono_literals;

class packet_worker final {
    ruvia::worker_runtime owner_{{.mailbox_capacity = 8}};

public:
    packet_worker()
        : runtime(owner_.context()) {
        owner_.start();
    }

    template <typename Function>
    auto invoke(Function function) {
        using result = decltype(function());
        auto task = std::make_shared<std::packaged_task<result()>>(std::move(function));
        auto completion = task->get_future();
        if (!runtime.submission().post([task] { (*task)(); }).accepted() ||
            completion.wait_for(5s) != std::future_status::ready) {
            std::terminate();
        }
        return completion.get();
    }
    ~packet_worker() {
        owner_.request_stop();
        owner_.join();
    }

    ruvia::WorkerRuntimeContext& runtime;
};
}  // namespace

RUVIA_TEST(http3_datagram_channel_full_input_recovers_without_overwriting_borrowed_packet) {
    asio::io_context acceptor_io;
    ruvia::WorkerRuntimeContext acceptor(acceptor_io, 8);
    ruvia::WorkerNotification notification(acceptor);
    ruvia::test::CountingMemoryResource memory;
    {
        packet_worker worker;
        channel packets(notification, &memory, 2);
        packets.stage_worker(worker.runtime);
        worker.invoke([&] { packets.worker_start(); });
        const channel::udp::endpoint local(asio::ip::address_v4::loopback(), 4433);
        const channel::udp::endpoint peer(asio::ip::address_v4::loopback(), 5544);
        std::array first{std::byte{1}, std::byte{2}};
        const std::array second{std::byte{3}};
        const std::array dropped{std::byte{4}};
        RUVIA_CHECK(packets.acceptor_push(first, local, peer));
        first.fill(std::byte{9});
        RUVIA_CHECK(packets.acceptor_push(second, local, peer));
        RUVIA_CHECK(!packets.acceptor_push(dropped, local, peer));
        const auto held = worker.invoke([&] {
            const auto input = packets.worker_input();
            return input && input->bytes.size() == 2 && input->bytes[0] == std::byte{1} &&
                   input->bytes[1] == std::byte{2} && input->local_destination == local && input->peer == peer;
        });
        RUVIA_CHECK(held);
        RUVIA_CHECK(!packets.acceptor_push(dropped, local, peer));
        worker.invoke([&] { packets.worker_consume_input(); });
        RUVIA_CHECK(packets.acceptor_push(dropped, local, peer));
        const auto recovered = worker.invoke([&] {
            const auto input = packets.worker_input();
            const bool ordered = input && input->bytes.size() == 1 && input->bytes[0] == std::byte{3};
            packets.worker_consume_input();
            const auto next = packets.worker_input();
            const bool replacement = next && next->bytes.size() == 1 && next->bytes[0] == std::byte{4};
            packets.worker_consume_input();
            return ordered && replacement && !packets.worker_input();
        });
        RUVIA_CHECK(recovered);
        packets.acceptor_close();
        RUVIA_CHECK(!packets.acceptor_push(second, local, peer));
        worker.invoke([&] { packets.worker_close(); });
        RUVIA_CHECK(packets.worker_closed());
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(http3_forwarded_endpoint_output_loan_survives_detach_and_worker_close_ack) {
    asio::io_context acceptor_io;
    ruvia::WorkerRuntimeContext acceptor(acceptor_io, 8);
    ruvia::WorkerNotification notification(acceptor);
    ruvia::test::CountingMemoryResource memory;
    {
        packet_worker worker;
        channel packets(notification, &memory, 1);
        packets.stage_worker(worker.runtime);
        const channel::udp::endpoint local(asio::ip::address_v4::loopback(), 4433);
        const channel::udp::endpoint peer(asio::ip::address_v4::loopback(), 5544);
        std::unique_ptr<endpoint> consumer;
        std::size_t completed{};
        worker.invoke([&] {
            packets.worker_start();
            consumer = std::make_unique<endpoint>(packets, local,
                endpoint::notification{&completed, [](void* raw, endpoint::notification_kind kind) noexcept {
                                           if (kind == endpoint::notification_kind::output_drained) {
                                               ++*static_cast<std::size_t*>(raw);
                                           }
                                       }});
            consumer->prepare();
            static_cast<void>(consumer->start());
        });
        const auto sent = worker.invoke([&] {
            std::array bytes{std::byte{1}, std::byte{2}, std::byte{3}};
            const bool accepted = consumer->send_datagram(bytes, local, peer) == endpoint::pump_result::pending;
            bytes.fill(std::byte{9});
            return accepted;
        });
        RUVIA_CHECK(sent);
        const auto loan = packets.acceptor_output();
        RUVIA_CHECK(loan && loan->bytes.size() == 3 && loan->bytes[0] == std::byte{1});
        RUVIA_CHECK(loan && loan->peer == peer && loan->local_destination == local);
        const auto blocked = worker.invoke([&] {
            const std::array bytes{std::byte{8}};
            return consumer->send_in_flight() &&
                   consumer->send_datagram(bytes, local, peer) == endpoint::pump_result::pending &&
                   !packets.worker_send(bytes, local, peer);
        });
        RUVIA_CHECK(blocked);
        RUVIA_CHECK(loan && loan->bytes[2] == std::byte{3});
        packets.acceptor_consume_output();
        const auto recovered = worker.invoke([&] {
            consumer->poll_forwarded();
            const std::array bytes{std::byte{4}, std::byte{5}};
            return completed == 1 && !consumer->send_in_flight() &&
                   consumer->send_datagram(bytes, local, peer) == endpoint::pump_result::pending;
        });
        RUVIA_CHECK(recovered);
        const auto late_loan = packets.acceptor_output();
        RUVIA_CHECK(late_loan && late_loan->bytes.size() == 2);
        packets.acceptor_close(std::make_error_code(std::errc::operation_canceled));
        const auto detached = worker.invoke([&] {
            consumer->poll_forwarded();
            consumer->request_stop();
            const bool stopped = consumer->socket_done();
            consumer.reset();
            packets.worker_close();
            return stopped;
        });
        RUVIA_CHECK(detached);
        RUVIA_CHECK(packets.worker_closed());
        RUVIA_CHECK(late_loan && late_loan->bytes[0] == std::byte{4} && late_loan->bytes[1] == std::byte{5});
        RUVIA_CHECK(packets.acceptor_output().has_value());
        packets.acceptor_consume_output();
        RUVIA_CHECK(!packets.acceptor_output());
        RUVIA_CHECK(packets.error() == std::errc::operation_canceled);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(http3_datagram_channel_cold_worker_ack_rejects_late_packets_and_retires_pmr_storage) {
    asio::io_context acceptor_io;
    ruvia::WorkerRuntimeContext acceptor(acceptor_io, 8);
    ruvia::WorkerNotification notification(acceptor);
    asio::io_context worker_io;
    ruvia::WorkerRuntimeContext worker(worker_io, 8);
    ruvia::test::CountingMemoryResource memory;
    {
        channel packets(notification, &memory, 1);
        packets.stage_worker(worker);
        const channel::udp::endpoint local(asio::ip::address_v4::loopback(), 4433);
        const channel::udp::endpoint peer(asio::ip::address_v4::loopback(), 5544);
        const std::array input{std::byte{1}};
        RUVIA_CHECK(packets.acceptor_push(input, local, peer));
        packets.acceptor_close();
        packets.abandon_worker();
        RUVIA_CHECK(packets.worker_closed());
        RUVIA_CHECK(!packets.acceptor_push(input, local, peer));
        RUVIA_CHECK(!packets.acceptor_output());
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}
