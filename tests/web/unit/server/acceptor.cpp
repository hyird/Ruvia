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

#include "ruvia/core/timer.h"
#include "ruvia/core/worker_runtime_context.h"

#include "router/route_table.h"
#include "server/http_server_options_validation.h"
#include "server/native_accepted_socket_ticket.h"
#include "server/web_worker_runtime.h"
#include "test_harness.h"

namespace {

struct target_state final {
    asio::io_context* context_{};
    std::atomic<bool> ready_{true};
    std::atomic<unsigned> availability_checks_{0};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool received_{false};
    int failure_{};
    std::size_t listener_index_{};
};

bool available(void* object) noexcept {
    auto& target = *static_cast<target_state*>(object);
    target.availability_checks_.fetch_add(1, std::memory_order_relaxed);
    return target.ready_.load(std::memory_order_relaxed);
}

void receive(void* object, ruvia::detail::native_accepted_socket_ticket&& ticket) noexcept {
    auto& target = *static_cast<target_state*>(object);
    const auto listener_index = ticket.listener_index();
    try {
        auto socket = std::make_shared<asio::ip::tcp::socket>(*target.context_);
        asio::error_code error;
        socket->assign(ticket.protocol(), ticket.native_handle(), error);
        if (error) {
            std::lock_guard lock(target.mutex_);
            target.failure_ = error.value();
            target.condition_.notify_one();
            return;
        }
        (void)ticket.release();
        auto byte = std::make_shared<std::array<char, 1>>();
        socket->async_read_some(asio::buffer(*byte),
            [&target, socket, byte, listener_index](const asio::error_code& read_error,
                std::size_t size) {
                if (read_error || size != 1) {
                    std::lock_guard lock(target.mutex_);
                    target.failure_ = read_error.value();
                    target.condition_.notify_one();
                    return;
                }
                socket->async_write_some(asio::buffer(*byte),
                    [&target, socket, byte, listener_index](const asio::error_code& write_error,
                        std::size_t written) {
                        if (write_error || written != 1) {
                            std::lock_guard lock(target.mutex_);
                            target.failure_ = write_error.value();
                            target.condition_.notify_one();
                            return;
                        }
                        std::lock_guard lock(target.mutex_);
                        target.listener_index_ = listener_index;
                        target.received_ = true;
                        target.condition_.notify_one();
                    });
            });
    } catch (...) {
    }
}

using listener = ruvia::detail::http_server_listener_definition;

struct assignment_state final {
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread::id callback_thread_{};
    std::thread::id worker_thread_{};
    std::size_t received_{};
};

struct worker_state_init final {
    std::atomic<unsigned>* destruction_count_;
};

struct tracked_worker_capability final {
    explicit tracked_worker_capability(worker_state_init init)
        : destruction_count_(init.destruction_count_) {}
    ~tracked_worker_capability() {
        destruction_count_->fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<unsigned>* destruction_count_;
};

struct admission_cancellation_probe final {
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread::id started_thread_{};
    std::thread::id callback_thread_{};
    bool started_{false};
    bool callback_ran_{false};
    bool operation_cancelled_{false};
};

class test_watchdog final {
public:
    explicit test_watchdog(std::chrono::seconds timeout)
        : thread_([this, timeout] {
              const auto deadline_value = std::chrono::steady_clock::now() + timeout;
              while (std::chrono::steady_clock::now() < deadline_value) {
                  if (completed_.load(std::memory_order_acquire)) {
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
              if (!completed_.load(std::memory_order_acquire)) {
                  std::terminate();
              }
          }) {}

    ~test_watchdog() {
        completed_.store(true, std::memory_order_release);
        thread_.join();
    }

private:
    std::atomic<bool> completed_{false};
    std::thread thread_;
};

ruvia::task<void> wait_for_admission_cancellation(
    ruvia::web_worker_context& context_value, admission_cancellation_probe& probe_value) {
    const auto stop_token_value = context_value.get_stop_token();
    auto registration = stop_token_value.register_callback([&probe_value] {
        std::lock_guard lock(probe_value.mutex_);
        probe_value.callback_ran_ = true;
        probe_value.callback_thread_ = std::this_thread::get_id();
        probe_value.condition_.notify_one();
    });
    {
        std::lock_guard lock(probe_value.mutex_);
        probe_value.started_ = true;
        probe_value.started_thread_ = std::this_thread::get_id();
        probe_value.condition_.notify_one();
    }

    const auto result_value = co_await ruvia::sleep_for(
        context_value.worker(), std::chrono::hours(1), stop_token_value);
    {
        std::lock_guard lock(probe_value.mutex_);
        probe_value.operation_cancelled_ = result_value == ruvia::timer_sleep_result::stop_requested;
        probe_value.condition_.notify_one();
    }
}

bool assignment_available(void*) noexcept {
    return true;
}

bool web_worker_available(void* object) noexcept {
    return static_cast<ruvia::detail::web_worker_runtime*>(object)->available_for_network_dispatch();
}

void web_worker_accept(void* object,
    ruvia::detail::native_accepted_socket_ticket&& ticket) noexcept {
    static_cast<ruvia::detail::web_worker_runtime*>(object)->accept_transferred_connection(
        std::move(ticket));
}

void record_assignment(void* object,
    ruvia::detail::native_accepted_socket_ticket&&) noexcept {
    auto& state_value = *static_cast<assignment_state*>(object);
    std::lock_guard lock(state_value.mutex_);
    state_value.callback_thread_ = std::this_thread::get_id();
    ++state_value.received_;
    state_value.condition_.notify_one();
}

}  // namespace

RUVIA_TEST(acceptor_target_requires_a_bound_submission_view) {
    asio::io_context worker;
    ruvia::worker_runtime_context worker_runtime(worker, 1);
    target_state target{.context_ = &worker};
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        {}, &target, available, receive}};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        ruvia::detail::acceptor network(listeners, targets);
    }));

    const auto submission = worker_runtime.submission();
    worker_runtime.close();
    const std::array closed_targets{ruvia::detail::acceptor::worker_target{
        submission, &target, available, receive}};
    ruvia::detail::acceptor network(listeners, closed_targets);
    RUVIA_CHECK(!submission.accepting());
}

RUVIA_TEST(acceptor_assigns_native_socket_and_performs_worker_io) {
    using namespace std::chrono_literals;
    asio::io_context worker;
    ruvia::worker_runtime_context worker_runtime(worker, 8);
    auto worker_guard = asio::make_work_guard(worker);
    std::thread worker_thread([&] { worker_runtime.run(); });
    target_state target{.context_ = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        worker_runtime.submission(), &target, available, receive}};
    const std::array listeners{
        listener({asio::ip::address_v4::loopback(), 0}),
        listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    RUVIA_CHECK(network.local_endpoint(0).port() != network.local_endpoint(1).port());

    for (std::size_t index = 0; index < listeners.size(); ++index) {
        asio::io_context client_context;
        asio::ip::tcp::socket client(client_context);
        client.connect(network.local_endpoint(index));
        const std::array<char, 1> sent{'x'};
        asio::write(client, asio::buffer(sent));
        std::array<char, 1> received_value{};
        client.non_blocking(true);
        asio::error_code read_error;
        const auto read_deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < read_deadline) {
            const auto count = client.read_some(asio::buffer(received_value), read_error);
            if (!read_error && count == 1) {
                break;
            }
            if (read_error != asio::error::would_block && read_error != asio::error::try_again) {
                break;
            }
            read_error.clear();
            std::this_thread::sleep_for(1ms);
        }
        RUVIA_CHECK(!read_error);
        RUVIA_CHECK_EQ(received_value[0], 'x');
        std::unique_lock lock(target.mutex_);
        RUVIA_CHECK(target.condition_.wait_for(lock, 2s, [&] { return target.received_ || target.failure_ != 0; }));
        RUVIA_CHECK_EQ(target.listener_index_, index);
        RUVIA_CHECK_EQ(target.failure_, 0);
        target.received_ = false;
        target.failure_ = 0;
    }

    network.stop();
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::runtime_lifecycle::state_type::stopped);
    worker_runtime.close();
    worker_guard.reset();
    worker_thread.join();
}

RUVIA_TEST(acceptor_stop_before_serving_wakes_startup_waiters) {
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.stop();
    RUVIA_CHECK(!network.wait_until_serving());
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::runtime_lifecycle::state_type::stopped);
    RUVIA_CHECK(!network.failure());
}

RUVIA_TEST(acceptor_stop_before_launch_completes_waiters_without_runtime_failure) {
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
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
    RUVIA_CHECK_EQ(network.state(), ruvia::runtime_lifecycle::state_type::stopped);
    RUVIA_CHECK(!network.failure());
}

RUVIA_TEST(acceptor_concurrent_launch_and_stop_share_one_lifecycle) {
    for (int attempt_value = 0; attempt_value < 32; ++attempt_value) {
        const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
        const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
        ruvia::detail::acceptor network(listeners, targets);
        network.prepare();
        std::barrier start(3);
        std::exception_ptr launch_failure;
        std::thread launcher([&] {
            start.arrive_and_wait();
            try {
                network.launch();
            } catch (const std::logic_error&) {
                // stop() won; a stopped runtime cannot subsequently launch.
            } catch (...) {
                launch_failure = std::current_exception();
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
        RUVIA_CHECK(!launch_failure);
        RUVIA_CHECK(!network.failure());
        RUVIA_CHECK_EQ(network.state(), ruvia::runtime_lifecycle::state_type::stopped);
    }
}

RUVIA_TEST(acceptor_without_targets_stops_cleanly) {
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    network.stop();
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::runtime_lifecycle::state_type::stopped);
}

RUVIA_TEST(acceptor_rejected_worker_closes_native_ticket) {
    asio::io_context worker;
    ruvia::worker_runtime_context worker_runtime(worker, 1);
    worker_runtime.close();
    target_state target{.context_ = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        worker_runtime.submission(), &target, available, receive}};
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    asio::io_context client_context;
    asio::ip::tcp::socket client(client_context);
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
    ruvia::worker_runtime_context worker_runtime(worker, 1);
    auto worker_guard = asio::make_work_guard(worker);
    std::thread worker_thread([&] { worker_runtime.run(); });

    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool entered = false;
    bool release = false;
    RUVIA_CHECK(worker_runtime.handle().post([&] {
                                           std::unique_lock lock(gate_mutex);
                                           entered = true;
                                           gate_condition.notify_one();
                                           gate_condition.wait(lock, [&] { return release; });
                                       })
            .accepted());
    {
        std::unique_lock lock(gate_mutex);
        RUVIA_CHECK(gate_condition.wait_for(lock, 2s, [&] { return entered; }));
    }
    RUVIA_CHECK(worker_runtime.handle().post([] {}).accepted());

    target_state target{.context_ = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        worker_runtime.submission(), &target, available, receive}};
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());

    asio::io_context client_context;
    asio::ip::tcp::socket client(client_context);
    client.connect(network.local_endpoint(0));
    client.non_blocking(true);
    std::array<char, 1> byte{};
    asio::error_code error;
    const auto deadline_value = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline_value) {
        (void)client.read_some(asio::buffer(byte), error);
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            break;
        }
        error.clear();
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(target.availability_checks_.load(std::memory_order_relaxed) != 0);
    RUVIA_CHECK(error == asio::error::eof || error == asio::error::connection_reset);

    network.stop();
    network.join();
    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_condition.notify_one();
    worker_runtime.close();
    worker_guard.reset();
    worker_thread.join();
}

RUVIA_TEST(acceptor_worker_stop_retires_queued_ticket) {
    using namespace std::chrono_literals;
    asio::io_context worker;
    ruvia::worker_runtime_context worker_runtime(worker, 4);
    auto worker_guard = asio::make_work_guard(worker);
    std::thread worker_thread([&] { worker_runtime.run(); });
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool entered = false;
    bool release = false;
    auto blocker = worker_runtime.handle().post([&] {
        std::unique_lock lock(gate_mutex);
        entered = true;
        gate_condition.notify_one();
        gate_condition.wait(lock, [&] { return release; });
    });
    RUVIA_CHECK(blocker.accepted());
    {
        std::unique_lock lock(gate_mutex);
        RUVIA_CHECK(gate_condition.wait_for(lock, 2s, [&] { return entered; }));
    }

    target_state target{.context_ = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        worker_runtime.submission(), &target, available, receive}};
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    asio::io_context client_context;
    asio::ip::tcp::socket client(client_context);
    client.connect(network.local_endpoint(0));
    const auto enqueue_deadline = std::chrono::steady_clock::now() + 2s;
    while (target.availability_checks_.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < enqueue_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    RUVIA_CHECK(target.availability_checks_.load(std::memory_order_relaxed) != 0);
    target.ready_.store(false, std::memory_order_relaxed);
    worker_runtime.close();
    {
        std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_condition.notify_one();
    client.non_blocking(true);
    std::array<char, 1> byte{};
    asio::error_code error;
    const auto deadline_value = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline_value) {
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
    worker_guard.reset();
    worker_thread.join();
}

RUVIA_TEST(acceptor_round_robins_across_available_workers) {
    using namespace std::chrono_literals;
    struct worker final {
        asio::io_context context_;
        ruvia::worker_runtime_context runtime_{context_, 8};
        asio::executor_work_guard<asio::io_context::executor_type> guard_{
            asio::make_work_guard(context_)};
        std::thread thread_{[this] {
            try {
                runtime_.run();
            } catch (...) {
            }
        }};
        ~worker() {
            runtime_.close();
            guard_.reset();
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    } first, second;

    assignment_state first_state, second_state;
    RUVIA_CHECK(first.runtime_.handle().post([&] {
                                           std::lock_guard lock(first_state.mutex_);
                                           first_state.worker_thread_ = std::this_thread::get_id();
                                           first_state.condition_.notify_one();
                                       })
            .accepted());
    RUVIA_CHECK(second.runtime_.handle().post([&] {
                                            std::lock_guard lock(second_state.mutex_);
                                            second_state.worker_thread_ = std::this_thread::get_id();
                                            second_state.condition_.notify_one();
                                        })
            .accepted());
    {
        std::unique_lock lock(first_state.mutex_);
        RUVIA_CHECK(first_state.condition_.wait_for(lock, 2s,
            [&] { return first_state.worker_thread_ != std::thread::id{}; }));
    }
    {
        std::unique_lock lock(second_state.mutex_);
        RUVIA_CHECK(second_state.condition_.wait_for(lock, 2s,
            [&] { return second_state.worker_thread_ != std::thread::id{}; }));
    }
    const std::array targets{
        ruvia::detail::acceptor::worker_target{
            first.runtime_.submission(), &first_state, assignment_available, record_assignment},
        ruvia::detail::acceptor::worker_target{
            second.runtime_.submission(), &second_state, assignment_available, record_assignment}};
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());

    for (std::size_t expected = 0; expected < targets.size(); ++expected) {
        asio::io_context client_context;
        asio::ip::tcp::socket client(client_context);
        client.connect(network.local_endpoint(0));
        auto& state_value = expected == 0 ? first_state : second_state;
        std::unique_lock lock(state_value.mutex_);
        RUVIA_CHECK(state_value.condition_.wait_for(lock, 2s, [&] { return state_value.received_ == 1; }));
    }
    {
        std::scoped_lock lock(first_state.mutex_, second_state.mutex_);
        RUVIA_CHECK_EQ(first_state.received_, 1U);
        RUVIA_CHECK_EQ(second_state.received_, 1U);
        RUVIA_CHECK_EQ(first_state.callback_thread_, first_state.worker_thread_);
        RUVIA_CHECK_EQ(second_state.callback_thread_, second_state.worker_thread_);
        RUVIA_CHECK(first_state.worker_thread_ != second_state.worker_thread_);
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
        listener({asio::ip::address_v4::loopback(), 0}), listener(occupied.local_endpoint())};
    const std::array<ruvia::detail::acceptor::worker_target, 0> targets{};
    ruvia::detail::acceptor network(listeners, targets);
    RUVIA_CHECK(ruvia::testing::throws_on([&] { network.prepare(); }));

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
    const test_watchdog watchdog_value(15s);

    const listener listener_value({asio::ip::address_v4::loopback(), 0});
    auto configuration = ruvia::detail::validate_http_server_configuration(
        std::span<const listener>(&listener_value, 1), ruvia::detail::http_server_options{});
    ruvia::detail::route_table routes(std::pmr::get_default_resource());
    std::atomic<unsigned> capability_destructions{0};
    const std::array worker_state_definitions{
        ruvia::detail::worker_state_definition::make<tracked_worker_capability>(
            [&] { return worker_state_init{&capability_destructions}; })};
    const ruvia::detail::worker_capability_definitions capabilities{
        .worker_states_ = worker_state_definitions};
    ruvia::detail::web_worker_runtime worker(configuration, routes, capabilities);
    worker.prepare();
    worker.launch();
    worker.wait_until_ready();
    worker.request_serve();
    RUVIA_CHECK(worker.wait_until_serving());

    admission_cancellation_probe cancellation;
    const auto operation = worker.web_worker().post([&](ruvia::web_worker_context& context_value) {
        return wait_for_admission_cancellation(context_value, cancellation);
    });
    RUVIA_CHECK(operation.accepted());
    {
        std::unique_lock lock(cancellation.mutex_);
        const bool started = cancellation.condition_.wait_for(
            lock, 2s, [&] { return cancellation.started_; });
        RUVIA_CHECK(started);
        if (!started) {
            std::terminate();
        }
    }

    const std::array targets{ruvia::detail::acceptor::worker_target{
        worker.network_submission(), &worker, web_worker_available, web_worker_accept}};
    ruvia::detail::acceptor network(configuration.listeners(), targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());

    asio::io_context client_context;
    asio::ip::tcp::socket client(client_context);
    client.connect(network.local_endpoint(0));
    network.stop();
    worker.stop_admission();
    {
        std::unique_lock lock(cancellation.mutex_);
        const bool cancelled = cancellation.condition_.wait_for(lock, 2s, [&] {
            return cancellation.callback_ran_ && cancellation.operation_cancelled_;
        });
        RUVIA_CHECK(cancelled);
        if (!cancelled) {
            std::terminate();
        }
        RUVIA_CHECK_EQ(cancellation.callback_thread_, cancellation.started_thread_);
        RUVIA_CHECK(cancellation.callback_thread_ != std::this_thread::get_id());
    }
    RUVIA_CHECK_EQ(capability_destructions.load(std::memory_order_relaxed), 0U);
    network.join();
    RUVIA_CHECK_EQ(capability_destructions.load(std::memory_order_relaxed), 0U);

    // A ticket posted before admission closed can still run after the public
    // dispatcher is closed. Its worker-side check must reject it as well.
    asio::ip::tcp::acceptor late_listener(client_context,
        {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::socket late_client(client_context);
    late_client.connect(late_listener.local_endpoint());
    asio::ip::tcp::socket late_server(client_context);
    late_listener.accept(late_server);
    asio::error_code release_error;
    const auto native = late_server.release(release_error);
    if (release_error) {
        throw std::runtime_error("failed to create late network ticket");
    }
    ruvia::detail::native_accepted_socket_ticket late_ticket(
        asio::ip::tcp::v4(), 0, native);

    std::mutex probe_mutex;
    std::condition_variable probe_condition;
    bool worker_responded = false;
    bool late_ticket_admitted = false;
    std::thread::id worker_thread;
    asio::post(worker.worker_executor(), [&, ticket = std::move(late_ticket)]() mutable {
        const auto before = worker.stats().active_connections_;
        worker.accept_transferred_connection(std::move(ticket));
        const auto after = worker.stats().active_connections_;
        std::lock_guard lock(probe_mutex);
        late_ticket_admitted = after != before;
        worker_responded = true;
        worker_thread = std::this_thread::get_id();
        probe_condition.notify_one();
    });
    {
        std::unique_lock lock(probe_mutex);
        const bool completed = probe_condition.wait_for(lock, 2s, [&] { return worker_responded; });
        RUVIA_CHECK(completed);
        if (!completed) {
            std::terminate();
        }
        RUVIA_CHECK(!late_ticket_admitted);
    }
    RUVIA_CHECK(worker_thread != std::this_thread::get_id());
    RUVIA_CHECK_EQ(capability_destructions.load(std::memory_order_relaxed), 0U);

    worker.finalize_after_network_quiesced();
    worker.join();
    RUVIA_CHECK(!worker.worker().accepting());
    RUVIA_CHECK_EQ(capability_destructions.load(std::memory_order_relaxed), 1U);
}

RUVIA_TEST(acceptor_stop_closes_listener_with_accept_pending) {
    asio::io_context worker;
    ruvia::worker_runtime_context worker_runtime(worker, 4);
    auto worker_guard = asio::make_work_guard(worker);
    std::thread worker_thread([&] { worker_runtime.run(); });
    target_state target{.context_ = &worker};
    const std::array targets{ruvia::detail::acceptor::worker_target{
        worker_runtime.submission(), &target, available, receive}};
    const std::array listeners{listener({asio::ip::address_v4::loopback(), 0})};
    ruvia::detail::acceptor network(listeners, targets);
    network.prepare();
    network.launch();
    network.wait_until_ready();
    network.request_serve();
    RUVIA_CHECK(network.wait_until_serving());
    network.stop();
    network.join();
    RUVIA_CHECK_EQ(network.state(), ruvia::runtime_lifecycle::state_type::stopped);
    worker_runtime.close();
    worker_guard.reset();
    worker_thread.join();
}
