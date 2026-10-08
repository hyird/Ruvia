#include "server/acceptor.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerRuntimeContext.h"

#include "router/RouteTable.h"
#include "server/HttpServerOptionsValidation.h"
#include "server/NativeAcceptedSocketTicket.h"
#include "server/WebWorkerRuntime.h"
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

struct WorkerStateInit final {
    std::atomic<unsigned>* destructionCount;
};

struct TrackedWorkerCapability final {
    explicit TrackedWorkerCapability(WorkerStateInit init)
        : destructionCount(init.destructionCount) {}
    ~TrackedWorkerCapability() {
        destructionCount->fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<unsigned>* destructionCount;
};

struct AdmissionCancellationProbe final {
    std::mutex mutex;
    std::condition_variable condition;
    std::thread::id startedThread{};
    std::thread::id callbackThread{};
    bool started{false};
    bool callbackRan{false};
    bool operationCancelled{false};
};

class TestWatchdog final {
public:
    explicit TestWatchdog(std::chrono::seconds timeout)
        : thread_([this, timeout] {
              const auto deadline = std::chrono::steady_clock::now() + timeout;
              while (std::chrono::steady_clock::now() < deadline) {
                  if (completed_.load(std::memory_order_acquire)) {
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
              if (!completed_.load(std::memory_order_acquire)) {
                  std::terminate();
              }
          }) {}

    ~TestWatchdog() {
        completed_.store(true, std::memory_order_release);
        thread_.join();
    }

private:
    std::atomic<bool> completed_{false};
    std::thread thread_;
};

ruvia::Task<void> waitForAdmissionCancellation(
    ruvia::WebWorkerContext& context, AdmissionCancellationProbe& probe) {
    const auto stopToken = context.stopToken();
    auto registration = stopToken.registerCallback([&probe] {
        std::lock_guard lock(probe.mutex);
        probe.callbackRan = true;
        probe.callbackThread = std::this_thread::get_id();
        probe.condition.notify_one();
    });
    {
        std::lock_guard lock(probe.mutex);
        probe.started = true;
        probe.startedThread = std::this_thread::get_id();
        probe.condition.notify_one();
    }

    const auto result = co_await ruvia::sleepFor(
        context.worker(), std::chrono::hours(1), stopToken);
    {
        std::lock_guard lock(probe.mutex);
        probe.operationCancelled = result == ruvia::TimerSleepResult::kStopRequested;
        probe.condition.notify_one();
    }
}

bool assignmentAvailable(void*) noexcept {
    return true;
}

bool webWorkerAvailable(void* object) noexcept {
    return static_cast<ruvia::detail::WebWorkerRuntime*>(object)->availableForNetworkDispatch();
}

void webWorkerAccept(void* object,
    ruvia::detail::NativeAcceptedSocketTicket&& ticket) noexcept {
    static_cast<ruvia::detail::WebWorkerRuntime*>(object)->acceptTransferredConnection(
        std::move(ticket));
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

RUVIA_TEST(acceptor_target_requires_a_bound_submission_view) {
    asio::io_context worker;
    ruvia::WorkerRuntimeContext workerRuntime(worker, 1);
    TargetState target{.context = &worker};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        {}, &target, available, receive}};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        ruvia::detail::acceptor network(listeners, targets);
    }));

    const auto submission = workerRuntime.submission();
    workerRuntime.close();
    const std::array closedTargets{ruvia::detail::acceptor::worker_target{
        submission, &target, available, receive}};
    ruvia::detail::acceptor network(listeners, closedTargets);
    RUVIA_CHECK(!submission.accepting());
}

RUVIA_TEST(acceptor_assigns_native_socket_and_performs_worker_io) {
    using namespace std::chrono_literals;
    asio::io_context worker;
    ruvia::WorkerRuntimeContext workerRuntime(worker, 8);
    auto workerGuard = asio::make_work_guard(worker);
    std::thread workerThread([&] { workerRuntime.run(); });
    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        workerRuntime.submission(), &target, available, receive}};
    const std::array listeners{
        Listener({asio::ip::address_v4::loopback(), 0}),
        Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    RUVIA_CHECK(network.local_endpoint(0).port() != network.local_endpoint(1).port());

    for (std::size_t index = 0; index < listeners.size(); ++index) {
        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(network.local_endpoint(index));
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

    network.stop();
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::RuntimeLifecycle::State::kStopped);
    workerRuntime.close();
    workerGuard.reset();
    workerThread.join();
}

RUVIA_TEST(acceptor_stop_before_serving_wakes_startup_waiters) {
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.stop();
    RUVIA_CHECK(!network.wait_until_serving());
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::RuntimeLifecycle::State::kStopped);
    RUVIA_CHECK(!network.failure());
}

RUVIA_TEST(acceptor_stop_before_launch_completes_waiters_without_runtime_failure) {
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.stop();
    bool launch_rejected = false;
    try {
        network.launch();
    } catch (const std::logic_error&) {
        launch_rejected = true;
    }
    RUVIA_CHECK(launch_rejected);
    network.wait_until_ready();
    RUVIA_CHECK(!network.wait_until_serving());
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::RuntimeLifecycle::State::kStopped);
    RUVIA_CHECK(!network.failure());
}

RUVIA_TEST(acceptor_concurrent_launch_and_stop_share_one_lifecycle) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
        const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
        ruvia::detail::acceptor network(listeners, targets);
        network.prepare();
        std::barrier start(3);
        std::exception_ptr launchFailure;
        std::thread launcher([&] {
            start.arrive_and_wait();
            try {
                network.launch();
            } catch (const std::logic_error&) {
                // stop() won; a stopped runtime cannot subsequently launch.
            } catch (...) {
                launchFailure = std::current_exception();
            }
        });
        std::thread stopper([&] {
            start.arrive_and_wait();
            network.stop();
        });
        start.arrive_and_wait();
        launcher.join();
        stopper.join();
        network.wait_until_ready();
        network.join();
        RUVIA_CHECK(!launchFailure);
        RUVIA_CHECK(!network.failure());
        RUVIA_CHECK_EQ(network.state(), ruvia::RuntimeLifecycle::State::kStopped);
    }
}

RUVIA_TEST(acceptor_without_targets_stops_cleanly) {
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    network.stop();
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::RuntimeLifecycle::State::kStopped);
}

RUVIA_TEST(acceptor_rejected_worker_closes_native_ticket) {
    asio::io_context worker;
    ruvia::WorkerRuntimeContext workerRuntime(worker, 1);
    workerRuntime.close();
    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        workerRuntime.submission(), &target, available, receive}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    asio::io_context clientContext;
    asio::ip::tcp::socket client(clientContext);
    client.connect(network.local_endpoint(0));
    std::array<char, 1> data{};
    asio::error_code error;
    client.read_some(asio::buffer(data), error);
    RUVIA_CHECK(static_cast<bool>(error));
    network.stop();
    network.join();
}

RUVIA_TEST(acceptor_queue_full_drops_accepted_ticket) {
    using namespace std::chrono_literals;
    asio::io_context worker;
    ruvia::WorkerRuntimeContext workerRuntime(worker, 1);
    auto workerGuard = asio::make_work_guard(worker);
    std::thread workerThread([&] { workerRuntime.run(); });

    std::mutex gateMutex;
    std::condition_variable gateCondition;
    bool entered = false;
    bool release = false;
    RUVIA_CHECK(workerRuntime.handle().post([&] {
                                          std::unique_lock lock(gateMutex);
                                          entered = true;
                                          gateCondition.notify_one();
                                          gateCondition.wait(lock, [&] { return release; });
                                      })
            .accepted());
    {
        std::unique_lock lock(gateMutex);
        RUVIA_CHECK(gateCondition.wait_for(lock, 2s, [&] { return entered; }));
    }
    RUVIA_CHECK(workerRuntime.handle().post([] {}).accepted());

    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        workerRuntime.submission(), &target, available, receive}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());

    asio::io_context clientContext;
    asio::ip::tcp::socket client(clientContext);
    client.connect(network.local_endpoint(0));
    client.non_blocking(true);
    std::array<char, 1> byte{};
    asio::error_code error;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)client.read_some(asio::buffer(byte), error);
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            break;
        }
        error.clear();
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(target.availabilityChecks.load(std::memory_order_relaxed) != 0);
    RUVIA_CHECK(error == asio::error::eof || error == asio::error::connection_reset);

    network.stop();
    network.join();
    {
        std::lock_guard lock(gateMutex);
        release = true;
    }
    gateCondition.notify_one();
    workerRuntime.close();
    workerGuard.reset();
    workerThread.join();
}

RUVIA_TEST(acceptor_worker_stop_retires_queued_ticket) {
    using namespace std::chrono_literals;
    asio::io_context worker;
    ruvia::WorkerRuntimeContext workerRuntime(worker, 4);
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
    const std::array targets{ruvia::detail::acceptor::worker_target{
        workerRuntime.submission(), &target, available, receive}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    asio::io_context clientContext;
    asio::ip::tcp::socket client(clientContext);
    client.connect(network.local_endpoint(0));
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
    network.stop();
    network.join();
    workerGuard.reset();
    workerThread.join();
}

RUVIA_TEST(acceptor_round_robins_across_available_workers) {
    using namespace std::chrono_literals;
    struct Worker final {
        asio::io_context context;
        ruvia::WorkerRuntimeContext runtime{context, 8};
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
        ruvia::detail::acceptor::worker_target{
            first.runtime.submission(), &firstState, assignmentAvailable, recordAssignment},
        ruvia::detail::acceptor::worker_target{
            second.runtime.submission(), &secondState, assignmentAvailable, recordAssignment}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());

    for (std::size_t expected = 0; expected < targets.size(); ++expected) {
        asio::io_context clientContext;
        asio::ip::tcp::socket client(clientContext);
        client.connect(network.local_endpoint(0));
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
    network.stop();
    network.join();
}

RUVIA_TEST(acceptor_prepare_failure_closes_prior_listener) {
    asio::io_context context;
#ifdef _WIN32
    asio::ip::tcp::acceptor occupied(context);
    occupied.open(asio::ip::tcp::v4());
    BOOL exclusive = TRUE;
    RUVIA_CHECK(::setsockopt(occupied.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == 0);
    occupied.bind({asio::ip::address_v4::loopback(), 0});
    occupied.listen();
#else
    asio::ip::tcp::acceptor occupied(context,
        {asio::ip::address_v4::loopback(), 0});
#endif
    const std::array listeners{
        Listener({asio::ip::address_v4::loopback(), 0}), Listener(occupied.local_endpoint())};
    const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
    ruvia::detail::acceptor network(listeners, targets);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { network.prepare(); }));

    asio::ip::tcp::acceptor rebound(context);
    asio::error_code error;
    rebound.open(asio::ip::tcp::v4(), error);
    if (!error) {
        rebound.set_option(asio::socket_base::reuse_address(true), error);
    }
    if (!error) {
        rebound.bind(network.local_endpoint(0), error);
    }
    if (!error) {
        rebound.listen(asio::socket_base::max_listen_connections, error);
    }
    RUVIA_CHECK(!error);
}

RUVIA_TEST(app_worker_remains_alive_until_network_quiesces_and_finalizes) {
    using namespace std::chrono_literals;
    const TestWatchdog watchdog(15s);

    const Listener listener({asio::ip::address_v4::loopback(), 0});
    auto configuration = ruvia::detail::validateHttpServerConfiguration(
        std::span<const Listener>(&listener, 1), ruvia::detail::HttpServerOptions{});
    ruvia::detail::RouteTable routes(std::pmr::get_default_resource());
    std::atomic<unsigned> capabilityDestructions{0};
    const std::array workerStateDefinitions{
        ruvia::detail::WorkerStateDefinition::make<TrackedWorkerCapability>(
            [&] { return WorkerStateInit{&capabilityDestructions}; })};
    const ruvia::detail::WorkerCapabilityDefinitions capabilities{
        .workerStates = workerStateDefinitions};
    ruvia::detail::WebWorkerRuntime worker(configuration, routes, capabilities);
    worker.prepare();
    worker.launch();
    worker.waitUntilReady();
    worker.requestServe();
    RUVIA_CHECK(worker.waitUntilServing());

    AdmissionCancellationProbe cancellation;
    const auto operation = worker.webWorker().post([&](ruvia::WebWorkerContext& context) {
        return waitForAdmissionCancellation(context, cancellation);
    });
    RUVIA_CHECK(operation.accepted());
    {
        std::unique_lock lock(cancellation.mutex);
        const bool started = cancellation.condition.wait_for(
            lock, 2s, [&] { return cancellation.started; });
        RUVIA_CHECK(started);
        if (!started) {
            std::terminate();
        }
    }

    const std::array targets{ruvia::detail::acceptor::worker_target{
        worker.networkSubmission(), &worker, webWorkerAvailable, webWorkerAccept}};
    ruvia::detail::acceptor network(configuration.listeners(), targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());

    asio::io_context clientContext;
    asio::ip::tcp::socket client(clientContext);
    client.connect(network.local_endpoint(0));
    network.stop();
    worker.stopAdmission();
    {
        std::unique_lock lock(cancellation.mutex);
        const bool cancelled = cancellation.condition.wait_for(lock, 2s, [&] {
            return cancellation.callbackRan && cancellation.operationCancelled;
        });
        RUVIA_CHECK(cancelled);
        if (!cancelled) {
            std::terminate();
        }
        RUVIA_CHECK_EQ(cancellation.callbackThread, cancellation.startedThread);
        RUVIA_CHECK(cancellation.callbackThread != std::this_thread::get_id());
    }
    RUVIA_CHECK_EQ(capabilityDestructions.load(std::memory_order_relaxed), 0U);
    network.join();
    RUVIA_CHECK_EQ(capabilityDestructions.load(std::memory_order_relaxed), 0U);

    // A ticket posted before admission closed can still run after the public
    // dispatcher is closed. Its worker-side check must reject it as well.
    asio::ip::tcp::acceptor lateListener(clientContext,
        {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket lateClient(clientContext);
    lateClient.connect(lateListener.local_endpoint());
    asio::ip::tcp::socket lateServer(clientContext);
    lateListener.accept(lateServer);
    asio::error_code releaseError;
    const auto native = lateServer.release(releaseError);
    if (releaseError) {
        throw std::runtime_error("failed to create late network ticket");
    }
    ruvia::detail::NativeAcceptedSocketTicket lateTicket(
        asio::ip::tcp::v4(), 0, native);

    std::mutex probeMutex;
    std::condition_variable probeCondition;
    bool workerResponded = false;
    bool lateTicketAdmitted = false;
    std::thread::id workerThread;
    asio::post(worker.workerExecutor(), [&, ticket = std::move(lateTicket)]() mutable {
        const auto before = worker.stats().activeConnections;
        worker.acceptTransferredConnection(std::move(ticket));
        const auto after = worker.stats().activeConnections;
        std::lock_guard lock(probeMutex);
        lateTicketAdmitted = after != before;
        workerResponded = true;
        workerThread = std::this_thread::get_id();
        probeCondition.notify_one();
    });
    {
        std::unique_lock lock(probeMutex);
        const bool completed = probeCondition.wait_for(lock, 2s, [&] { return workerResponded; });
        RUVIA_CHECK(completed);
        if (!completed) {
            std::terminate();
        }
        RUVIA_CHECK(!lateTicketAdmitted);
    }
    RUVIA_CHECK(workerThread != std::this_thread::get_id());
    RUVIA_CHECK_EQ(capabilityDestructions.load(std::memory_order_relaxed), 0U);

    worker.finalizeAfterNetworkQuiesced();
    worker.join();
    RUVIA_CHECK(!worker.worker().accepting());
    RUVIA_CHECK_EQ(capabilityDestructions.load(std::memory_order_relaxed), 1U);
}

RUVIA_TEST(acceptor_stop_closes_listener_with_accept_pending) {
    asio::io_context worker;
    ruvia::WorkerRuntimeContext workerRuntime(worker, 4);
    auto workerGuard = asio::make_work_guard(worker);
    std::thread workerThread([&] { workerRuntime.run(); });
    TargetState target{.context = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        workerRuntime.submission(), &target, available, receive}};
    const std::array listeners{Listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    network.stop();
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::RuntimeLifecycle::State::kStopped);
    workerRuntime.close();
    workerGuard.reset();
    workerThread.join();
}
