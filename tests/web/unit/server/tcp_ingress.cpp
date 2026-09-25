#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/detail/worker/WorkerRuntimeContext.h"
#include "ruvia/web/detail/server/TcpIngressRuntime.h"

#include "test_harness.h"

namespace {

struct TargetState final {
    asio::io_context* context{};
    std::atomic<bool> ready{true};
    std::atomic<unsigned> availabilityChecks{0};
    std::mutex mutex;
    std::condition_variable condition;
    bool received{false};
    int failure{};
    std::size_t listenerIndex{};
};

bool available(void* object) noexcept {
    auto& target = *static_cast<TargetState*>(object);
    target.availabilityChecks.fetch_add(1, std::memory_order_relaxed);
    return target.ready.load(std::memory_order_relaxed);
}

void receive(void* object, ruvia::detail::NativeAcceptedSocketTicket&& ticket) noexcept {
    auto& target = *static_cast<TargetState*>(object);
    const auto listenerIndex = ticket.listenerIndex();
    try {
        auto socket = std::make_shared<asio::ip::tcp::socket>(*target.context);
        asio::error_code error;
        socket->assign(ticket.protocol(), ticket.nativeHandle(), error);
        if (error) {
            std::lock_guard lock(target.mutex);
            target.failure = error.value();
            target.condition.notify_one();
            return;
        }
        (void)ticket.release();
        auto byte = std::make_shared<std::array<char, 1>>();
        socket->async_read_some(asio::buffer(*byte),
            [&target, socket, byte, listenerIndex](const asio::error_code& readError,
                std::size_t size) {
                if (readError || size != 1) {
                    std::lock_guard lock(target.mutex);
                    target.failure = readError.value();
                    target.condition.notify_one();
                    return;
                }
                socket->async_write_some(asio::buffer(*byte),
                    [&target, socket, byte, listenerIndex](const asio::error_code& writeError,
                        std::size_t written) {
                        if (writeError || written != 1) {
                            std::lock_guard lock(target.mutex);
                            target.failure = writeError.value();
                            target.condition.notify_one();
                            return;
                        }
                        std::lock_guard lock(target.mutex);
                        target.listenerIndex = listenerIndex;
                        target.received = true;
                        target.condition.notify_one();
                    });
            });
    } catch (...) {
    }
}

using Listener = ruvia::detail::HttpServerListenerDefinition;

struct AssignmentState final {
    std::mutex mutex;
    std::condition_variable condition;
    std::thread::id callbackThread{};
    std::thread::id workerThread{};
    std::size_t received{};
};

bool assignmentAvailable(void*) noexcept {
    return true;
}

void recordAssignment(void* object,
    ruvia::detail::NativeAcceptedSocketTicket&&) noexcept {
    auto& state = *static_cast<AssignmentState*>(object);
    std::lock_guard lock(state.mutex);
    state.callbackThread = std::this_thread::get_id();
    ++state.received;
    state.condition.notify_one();
}

}  // namespace

RUVIA_TEST(tcp_ingress_assigns_native_socket_and_performs_worker_io) {
    using namespace std::chrono_literals;
    asio::io_context worker;
    ruvia::detail::WorkerRuntimeContext workerRuntime(worker, 8);
    auto workerGuard = asio::make_work_guard(worker);
    std::thread workerThread([&] { workerRuntime.run(); });
    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::TcpIngressRuntime::Target{
        &workerRuntime.handle(), &target, available, receive}};
    const std::array listeners{
        Listener({asio::ip::address_v4::loopback(), 0}),
        Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::TcpIngressRuntime ingress(listeners, targets);
    ingress.prepare();
    ingress.launch();
    ingress.waitUntilReady();
    ingress.requestServe();
    RUVIA_CHECK(ingress.waitUntilServing());
    RUVIA_CHECK(ingress.localEndpoint(0).port() != ingress.localEndpoint(1).port());

    for (std::size_t index = 0; index < listeners.size(); ++index) {
        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(ingress.localEndpoint(index));
        const std::array<char, 1> sent{'x'};
        asio::write(client, asio::buffer(sent));
        std::array<char, 1> received{};
        client.non_blocking(true);
        asio::error_code readError;
        const auto readDeadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < readDeadline) {
            const auto count = client.read_some(asio::buffer(received), readError);
            if (!readError && count == 1) {
                break;
            }
            if (readError != asio::error::would_block && readError != asio::error::try_again) {
                break;
            }
            readError.clear();
            std::this_thread::sleep_for(1ms);
        }
        RUVIA_CHECK(!readError);
        RUVIA_CHECK_EQ(received[0], 'x');
        std::unique_lock lock(target.mutex);
        RUVIA_CHECK(target.condition.wait_for(lock, 2s, [&] { return target.received || target.failure != 0; }));
        RUVIA_CHECK_EQ(target.listenerIndex, index);
        RUVIA_CHECK_EQ(target.failure, 0);
        target.received = false;
        target.failure = 0;
    }

    ingress.stop();
    ingress.join();
    RUVIA_CHECK_EQ(ingress.state(), ruvia::RuntimeLifecycle::State::kStopped);
    workerRuntime.close();
    workerGuard.reset();
    workerThread.join();
}

RUVIA_TEST(tcp_ingress_without_targets_stops_cleanly) {
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    const std::array<ruvia::detail::TcpIngressRuntime::Target, 0> targets{};
    ruvia::detail::TcpIngressRuntime ingress(listeners, targets);
    ingress.prepare();
    ingress.launch();
    ingress.waitUntilReady();
    ingress.requestServe();
    RUVIA_CHECK(ingress.waitUntilServing());
    ingress.stop();
    ingress.join();
    RUVIA_CHECK_EQ(ingress.state(), ruvia::RuntimeLifecycle::State::kStopped);
}

RUVIA_TEST(tcp_ingress_rejected_worker_closes_native_ticket) {
    asio::io_context worker;
    ruvia::detail::WorkerRuntimeContext workerRuntime(worker, 1);
    workerRuntime.close();
    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::TcpIngressRuntime::Target{
        &workerRuntime.handle(), &target, available, receive}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::TcpIngressRuntime ingress(listeners, targets);
    ingress.prepare();
    ingress.launch();
    ingress.waitUntilReady();
    ingress.requestServe();
    RUVIA_CHECK(ingress.waitUntilServing());
    asio::io_context clientContext;
    asio::ip::tcp::socket client(clientContext);
    client.connect(ingress.localEndpoint(0));
    std::array<char, 1> data{};
    asio::error_code error;
    client.read_some(asio::buffer(data), error);
    RUVIA_CHECK(static_cast<bool>(error));
    ingress.stop();
    ingress.join();
}

RUVIA_TEST(tcp_ingress_worker_stop_retires_queued_ticket) {
    using namespace std::chrono_literals;
    asio::io_context worker;
    ruvia::detail::WorkerRuntimeContext workerRuntime(worker, 4);
    auto workerGuard = asio::make_work_guard(worker);
    std::thread workerThread([&] { workerRuntime.run(); });
    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool entered = false;
    bool release = false;
    auto blocker = workerRuntime.handle().post([&] {
        std::unique_lock lock(gateMutex);
        entered = true;
        gateCondition.notify_one();
        gateCondition.wait(lock, [&] { return release; });
    });
    RUVIA_CHECK(blocker.accepted());
    {
        std::unique_lock lock(gateMutex);
        RUVIA_CHECK(gateCondition.wait_for(lock, 2s, [&] { return entered; }));
    }

    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::TcpIngressRuntime::Target{
        &workerRuntime.handle(), &target, available, receive}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::TcpIngressRuntime ingress(listeners, targets);
    ingress.prepare();
    ingress.launch();
    ingress.waitUntilReady();
    ingress.requestServe();
    RUVIA_CHECK(ingress.waitUntilServing());
    asio::io_context clientContext;
    asio::ip::tcp::socket client(clientContext);
    client.connect(ingress.localEndpoint(0));
    const auto enqueueDeadline = std::chrono::steady_clock::now() + 2s;
    while (target.availabilityChecks.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < enqueueDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(target.availabilityChecks.load(std::memory_order_relaxed) != 0);
    target.ready.store(false, std::memory_order_relaxed);
    workerRuntime.close();
    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gateCondition.notify_one();
    client.non_blocking(true);
    std::array<char, 1> byte{};
    asio::error_code error;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)client.read_some(asio::buffer(byte), error);
        if (error && error != asio::error::would_block && error != asio::error::try_again) {
            break;
        }
        error.clear();
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(error == asio::error::eof || error == asio::error::connection_reset);
    ingress.stop();
    ingress.join();
    workerGuard.reset();
    workerThread.join();
}

RUVIA_TEST(tcp_ingress_round_robins_across_available_workers) {
    using namespace std::chrono_literals;
    struct Worker final {
        asio::io_context context;
        ruvia::detail::WorkerRuntimeContext runtime{context, 8};
        asio::executor_work_guard<asio::io_context::executor_type> guard{
            asio::make_work_guard(context)};
        std::thread thread{[this] {
            try {
                runtime.run();
            } catch (...) {
            }
        }};
        ~Worker() {
            runtime.close();
            guard.reset();
            if (thread.joinable()) {
                thread.join();
            }
        }
    } first, second;

    AssignmentState firstState, secondState;
    RUVIA_CHECK(first.runtime.handle().post([&] {
                                          std::lock_guard lock(firstState.mutex);
                                          firstState.workerThread = std::this_thread::get_id();
                                          firstState.condition.notify_one();
                                      })
            .accepted());
    RUVIA_CHECK(second.runtime.handle().post([&] {
                                           std::lock_guard lock(secondState.mutex);
                                           secondState.workerThread = std::this_thread::get_id();
                                           secondState.condition.notify_one();
                                       })
            .accepted());
    {
        std::unique_lock lock(firstState.mutex);
        RUVIA_CHECK(firstState.condition.wait_for(lock, 2s,
            [&] { return firstState.workerThread != std::thread::id{}; }));
    }
    {
        std::unique_lock lock(secondState.mutex);
        RUVIA_CHECK(secondState.condition.wait_for(lock, 2s,
            [&] { return secondState.workerThread != std::thread::id{}; }));
    }
    const std::array targets{
        ruvia::detail::TcpIngressRuntime::Target{
            &first.runtime.handle(), &firstState, assignmentAvailable, recordAssignment},
        ruvia::detail::TcpIngressRuntime::Target{
            &second.runtime.handle(), &secondState, assignmentAvailable, recordAssignment}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::TcpIngressRuntime ingress(listeners, targets);
    ingress.prepare();
    ingress.launch();
    ingress.waitUntilReady();
    ingress.requestServe();
    RUVIA_CHECK(ingress.waitUntilServing());

    for (std::size_t expected = 0; expected < targets.size(); ++expected) {
        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(ingress.localEndpoint(0));
        auto& state = expected == 0 ? firstState : secondState;
        std::unique_lock lock(state.mutex);
        RUVIA_CHECK(state.condition.wait_for(lock, 2s, [&] { return state.received == 1; }));
    }
    {
        std::scoped_lock lock(firstState.mutex, secondState.mutex);
        RUVIA_CHECK_EQ(firstState.received, 1U);
        RUVIA_CHECK_EQ(secondState.received, 1U);
        RUVIA_CHECK_EQ(firstState.callbackThread, firstState.workerThread);
        RUVIA_CHECK_EQ(secondState.callbackThread, secondState.workerThread);
        RUVIA_CHECK(firstState.workerThread != secondState.workerThread);
    }
    ingress.stop();
    ingress.join();
}

RUVIA_TEST(tcp_ingress_prepare_failure_closes_prior_listener) {
    asio::io_context context;
    asio::ip::tcp::acceptor occupied(context,
        {asio::ip::address_v4::loopback(), 0});
    const std::array listeners{
        Listener({asio::ip::address_v4::loopback(), 0}), Listener(occupied.local_endpoint())};
    const std::array<ruvia::detail::TcpIngressRuntime::Target, 0> targets{};
    ruvia::detail::TcpIngressRuntime ingress(listeners, targets);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { ingress.prepare(); }));

    asio::ip::tcp::acceptor rebound(context);
    asio::error_code error;
    rebound.open(asio::ip::tcp::v4(), error);
    if (!error) {
        rebound.set_option(asio::socket_base::reuse_address(true), error);
    }
    if (!error) {
        rebound.bind(ingress.localEndpoint(0), error);
    }
    if (!error) {
        rebound.listen(asio::socket_base::max_listen_connections, error);
    }
    RUVIA_CHECK(!error);
}

RUVIA_TEST(tcp_ingress_stop_closes_listener_with_accept_pending) {
    asio::io_context worker;
    ruvia::detail::WorkerRuntimeContext workerRuntime(worker, 4);
    auto workerGuard = asio::make_work_guard(worker);
    std::thread workerThread([&] { workerRuntime.run(); });
    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::TcpIngressRuntime::Target{
        &workerRuntime.handle(), &target, available, receive}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::TcpIngressRuntime ingress(listeners, targets);
    ingress.prepare();
    ingress.launch();
    ingress.waitUntilReady();
    ingress.requestServe();
    RUVIA_CHECK(ingress.waitUntilServing());
    ingress.stop();
    ingress.join();
    RUVIA_CHECK_EQ(ingress.state(), ruvia::RuntimeLifecycle::State::kStopped);
    workerRuntime.close();
    workerGuard.reset();
    workerThread.join();
}
