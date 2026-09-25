#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/RuntimeLifecycle.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/NativeAcceptedSocketTicket.h"

namespace ruvia::detail {

using TcpIngressSocket = asio::ip::tcp::socket;

// Owns one acceptor per listener on a shared ingress event loop. Target objects
// and their callback state are borrowed and must outlive this runtime and its
// joined thread. Only endpoints are retained; worker session configuration stays
// owned by the worker.
class TcpIngressRuntime final {
public:
    struct Target final {
        const WorkerHandle* worker{};
        void* object{};
        // Must report false while the worker is unready, stopping, or at capacity.
        bool (*available)(void*) noexcept {};
        // Called on the target worker after a successful post; consumes a detached ticket.
        void (*accept)(void*, NativeAcceptedSocketTicket&&) noexcept {};
    };

    using FailureCallback = void (*)(void*) noexcept;

    TcpIngressRuntime(std::span<const HttpServerListenerDefinition> listeners,
        std::span<const Target> targets, void* failureTarget = nullptr,
        FailureCallback failureCallback = nullptr);
    ~TcpIngressRuntime();

    TcpIngressRuntime(const TcpIngressRuntime&) = delete;
    TcpIngressRuntime& operator=(const TcpIngressRuntime&) = delete;

    // Binds/listens synchronously; may be called only once before launch().
    void prepare();
    void launch();
    void waitUntilReady();
    void requestServe();
    [[nodiscard]] bool waitUntilServing();
    void stop() noexcept;
    void join();

    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint(std::size_t index) const;
    // Non-null after a fatal ingress failure; callers should propagate this to
    // the application lifecycle instead of treating the runtime as serving.
    [[nodiscard]] std::exception_ptr failure() const noexcept;
    void rethrowFailure() const;
    [[nodiscard]] ruvia::RuntimeLifecycle::State state() const noexcept {
        return lifecycle_.state();
    }

private:
    struct Listener final {
        Listener(asio::io_context& context, asio::ip::tcp::endpoint configuredEndpoint)
            : acceptor(context),
              endpoint(std::move(configuredEndpoint)),
              retry(context) {}
        asio::ip::tcp::acceptor acceptor;
        asio::ip::tcp::endpoint endpoint;
        asio::steady_timer retry;
    };
    using ListenerPtr = std::unique_ptr<Listener, PmrObjectDeleter<Listener>>;

    void run() noexcept;
    void beginAccept(std::size_t listenerIndex) noexcept;
    void accepted(std::size_t listenerIndex, const asio::error_code& error,
        TcpIngressSocket socket) noexcept;
    void scheduleRetry(std::size_t listenerIndex) noexcept;
    void fail(std::exception_ptr failure) noexcept;
    void closeAcceptors() noexcept;

    asio::io_context ioContext_;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard_;
    std::pmr::vector<ListenerPtr> listeners_;
    std::pmr::vector<Target> targets_;
    void* failureTarget_{};
    FailureCallback failureCallback_{};
    ruvia::RuntimeLifecycle lifecycle_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread thread_;
    std::thread::id ownerThreadId_{};
    std::exception_ptr failure_;
    bool prepared_{false};
    bool ready_{false};
    bool serveRequested_{false};
    bool serving_{false};
    bool launched_{false};
    bool joining_{false};
    bool joined_{false};
    std::size_t nextTarget_{0};
};

}  // namespace ruvia::detail
