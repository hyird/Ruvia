#include <barrier>
#include <stdexcept>
#include <thread>

#include "ruvia/web/detail/server/UdpIngressRuntime.h"

#include "test_harness.h"

namespace {
void recordUdpIngressFailure(void* target) noexcept {
    ++*static_cast<int*>(target);
}
}  // namespace

RUVIA_TEST(udp_ingress_runtime_ready_serve_and_stop_lifecycle) {
    using State = ruvia::RuntimeLifecycle::State;
    int failureNotifications = 0;
    ruvia::detail::UdpIngressRuntime runtime(&failureNotifications, &recordUdpIngressFailure);

    RUVIA_CHECK_EQ(runtime.state(), State::kReady);
    runtime.launch();
    runtime.waitUntilReady();
    RUVIA_CHECK_EQ(runtime.state(), State::kRunning);
    runtime.requestServe();
    RUVIA_CHECK(runtime.waitUntilServing());
    RUVIA_CHECK_EQ(runtime.state(), State::kRunning);

    runtime.stop();
    runtime.join();
    RUVIA_CHECK_EQ(runtime.state(), State::kStopped);
    RUVIA_CHECK_EQ(failureNotifications, 0);
}

RUVIA_TEST(udp_ingress_runtime_can_stop_before_serve) {
    using State = ruvia::RuntimeLifecycle::State;
    ruvia::detail::UdpIngressRuntime runtime;

    runtime.launch();
    runtime.waitUntilReady();
    runtime.stop();
    RUVIA_CHECK(!runtime.waitUntilServing());
    runtime.join();
    RUVIA_CHECK_EQ(runtime.state(), State::kStopped);
}

RUVIA_TEST(udp_ingress_runtime_concurrent_launch_and_stop_are_synchronized) {
    using State = ruvia::RuntimeLifecycle::State;
    for (int attempt = 0; attempt < 64; ++attempt) {
        ruvia::detail::UdpIngressRuntime runtime;
        std::barrier start(3);
        bool launchRejected = false;
        std::thread launcher([&] {
            start.arrive_and_wait();
            try {
                runtime.launch();
            } catch (const std::logic_error&) {
                launchRejected = true;
            }
        });
        std::thread stopper([&] {
            start.arrive_and_wait();
            runtime.stop();
        });
        start.arrive_and_wait();
        launcher.join();
        stopper.join();

        runtime.waitUntilReady();
        runtime.join();
        RUVIA_CHECK(launchRejected || runtime.state() == State::kStopped);
        RUVIA_CHECK_EQ(runtime.state(), State::kStopped);
    }
}

RUVIA_TEST(udp_ingress_runtime_rejects_a_second_launch) {
    ruvia::detail::UdpIngressRuntime runtime;
    runtime.launch();
    runtime.waitUntilReady();

    bool rejected = false;
    try {
        runtime.launch();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    runtime.stop();
    runtime.join();
}
