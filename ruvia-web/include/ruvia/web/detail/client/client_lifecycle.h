#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/web/detail/client/ClientCloseState.h"

namespace ruvia::detail {

enum class client_phase : unsigned char { fresh,
    connecting,
    connected,
    closing,
    closed };

// Composed by an address-stable protocol owner, after its backend and memory.
// Owns the sole standalone-client startup/cancel/retirement chain. Backend
// connect/close/join and the not-ready error are explicit compile-time policies.
// No type erasure, extra sharing, or locking is added to operation admission.
template <typename owner_type>
class client_lifecycle final {
public:
    client_lifecycle(owner_type& owner, const EventLoop& loop, const WorkerHandle& worker,
        client_phase initial = client_phase::fresh)
        : owner_(owner),
          loop_(loop),
          worker_(worker),
          close_(loop, worker),
          phase_(initial) {}

    ~client_lifecycle() {
        if (phase_.load(std::memory_order_acquire) != client_phase::closed ||
            !close_.complete() || operations_.has_pending_operations()) {
            std::terminate();
        }
    }

    client_lifecycle(const client_lifecycle&) = delete;
    client_lifecycle& operator=(const client_lifecycle&) = delete;

    [[nodiscard]] static EventLoop require_loop(EventLoop loop) {
        if (!loop.valid()) {
            throw std::invalid_argument(message(" requires a valid event loop"));
        }
        return loop;
    }

    void bind_stop() {
        try {
            std::weak_ptr<owner_type> weak = owner_.shared_from_this();
            stop_registration_ = loop_.onStop([weak = std::move(weak)]() -> Task<void> {
                if (const auto state = weak.lock()) {
                    co_await shutdown_owned(state, ClientCloseState::ObservationMode::kRetirement);
                }
            });
        } catch (...) {
            owner_.backend().closeNow();
            phase_.store(client_phase::closed, std::memory_order_release);
            close_.completeBeforePublication();
            throw;
        }
    }

    [[nodiscard]] Task<void> connect() {
        return connect_owned(owner_.shared_from_this());
    }

    void require_ready() const {
        require_current(" must be used on its bound event loop");
        if (phase_.load(std::memory_order_acquire) != client_phase::connected) {
            owner_type::throw_not_ready();
        }
    }

    [[nodiscard]] ::ruvia::operation_scope& operation_scope() noexcept {
        return operations_;
    }

    [[nodiscard]] OperationOptions options(OperationOptions options) const {
        return mergeOperationOptions(
            OperationOptions{.timeout = std::nullopt, .stopToken = stop_.token()}, std::move(options));
    }

    void request_close() noexcept {
        stop_.requestStop();
        if (!begin_close()) {
            return;
        }
        if (worker_.isCurrent()) {
            start_close();
            return;
        }
        try {
            if (!loop_.defer_cleanup([state = owner_.shared_from_this()] { state->lifecycle_.start_close(); }) &&
                phase_.load(std::memory_order_acquire) != client_phase::closed) {
                std::terminate();
            }
        } catch (...) {
            if (phase_.load(std::memory_order_acquire) != client_phase::closed) {
                std::terminate();
            }
        }
    }

    [[nodiscard]] Task<void> shutdown() {
        return shutdown_owned(owner_.shared_from_this(), ClientCloseState::ObservationMode::kCaller);
    }

private:
    [[nodiscard]] static std::string message(std::string_view suffix) {
        std::string result(owner_type::client_name);
        result.append(" client");
        result.append(suffix);
        return result;
    }

    void require_current(std::string_view suffix) const {
        if (!worker_.isCurrent()) {
            throw std::logic_error(message(suffix));
        }
    }

    [[nodiscard]] static Task<void> connect_owned(std::shared_ptr<owner_type> state) {
        co_await state->lifecycle_.connect_on_worker();
    }

    Task<void> connect_on_worker() {
        require_current(" must connect on its bound event loop");
        auto expected = client_phase::fresh;
        if (!phase_.compare_exchange_strong(expected, client_phase::connecting,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (expected == client_phase::closing || expected == client_phase::closed) {
                throw std::runtime_error(message(" closed before connecting"));
            }
            throw std::logic_error(message(" can only connect once"));
        }
        // Only the startup lease holder may tear down a failed connection.
        try {
            connecting_ = true;
            if (stop_.stopRequested() || !worker_.accepting()) {
                throw std::runtime_error(message(" closed before connecting"));
            }
            co_await owner_.backend().connect();
            expected = client_phase::connecting;
            if (!phase_.compare_exchange_strong(expected, client_phase::connected,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                throw std::runtime_error(message(" stopped while connecting"));
            }
            connecting_ = false;
            close_.notifyProgress();
        } catch (...) {
            owner_.backend().closeNow();
            stop_.requestStop();
            phase_.store(client_phase::closed, std::memory_order_release);
            connecting_ = false;
            if (close_.taskStarted()) {
                close_.notifyProgress();
            } else if (!close_.complete()) {
                close_.completeNow();
            }
            throw;
        }
    }

    [[nodiscard]] bool begin_close() noexcept {
        auto phase = phase_.load(std::memory_order_acquire);
        while (phase != client_phase::closed && phase != client_phase::closing) {
            if (phase_.compare_exchange_weak(phase, client_phase::closing,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static Task<void> shutdown_owned(std::shared_ptr<owner_type> state,
        ClientCloseState::ObservationMode mode) {
        auto& lifecycle = state->lifecycle_;
        return lifecycle.close_.shutdown_owned(std::move(state), [&lifecycle] { lifecycle.start_close(); }, mode);
    }

    void start_close() noexcept {
        if (!worker_.isCurrent()) {
            std::terminate();
        }
        (void)begin_close();
        stop_.requestStop();
        owner_.backend().closeNow();
        close_.start_cleanup(owner_.shared_from_this(), [this] { return close_on_worker(); }, [this](std::exception_ptr failure) { finish_close(std::move(failure)); });
    }

    Task<void> close_on_worker() {
        while (connecting_) {
            co_await close_.wait();
        }
        std::exception_ptr failure;
        if constexpr (requires { owner_.backend().join(); }) {
            try {
                co_await owner_.backend().join();
            } catch (...) {
                failure = std::current_exception();
            }
        }
        try {
            co_await operations_.close_and_join();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

    void finish_close(std::exception_ptr failure) {
        if (connecting_ || operations_.has_pending_operations()) {
            std::terminate();
        }
        phase_.store(client_phase::closed, std::memory_order_release);
        close_.finish(std::move(failure));
    }

    owner_type& owner_;
    const EventLoop& loop_;
    const WorkerHandle& worker_;
    StopSource stop_;
    EventLoopStopRegistration stop_registration_;
    ClientCloseState close_;
    std::atomic<client_phase> phase_;
    bool connecting_{};
    // Last: invalidate cold operations before backend and allocator retirement.
    ::ruvia::operation_scope operations_;
};

}  // namespace ruvia::detail
