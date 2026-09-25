#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/RuntimeLifecycle.h"

namespace ruvia::detail {

// Lifecycle owner for future UDP ingress integration. It intentionally does not
// bind a socket, receive datagrams, or advertise an application protocol.
class UdpIngressRuntime final {
public:
    using FailureCallback = void (*)(void*) noexcept;

    explicit UdpIngressRuntime(void* failureTarget = nullptr,
        FailureCallback failureCallback = nullptr);
    ~UdpIngressRuntime();

    UdpIngressRuntime(const UdpIngressRuntime&) = delete;
    UdpIngressRuntime& operator=(const UdpIngressRuntime&) = delete;

    // Launches the owned event-loop thread. The owner controls when the runtime
    // becomes serving, allowing it to coordinate readiness across runtimes.
    void launch();
    void waitUntilReady();
    void requestServe();
    [[nodiscard]] bool waitUntilServing();
    void stop() noexcept;
    void join();

    [[nodiscard]] ruvia::RuntimeLifecycle::State state() const noexcept {
        return lifecycle_.state();
    }
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    void rethrowFailure() const;

private:
    void run() noexcept;

    asio::io_context ioContext_;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard_;
    ruvia::RuntimeLifecycle lifecycle_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    // Accessed only while mutex_ is held. The App joins before destruction;
    // destroying this object from the ingress thread violates that contract.
    std::thread thread_;
    bool ready_{false};
    bool serveRequested_{false};
    bool serving_{false};
    std::exception_ptr failure_;
    void* failureTarget_{};
    FailureCallback failureCallback_{};
};

}  // namespace ruvia::detail
