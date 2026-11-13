#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/web/server_config.h"

#include "integration/runtime_control_completion.h"
#include "server/http_server_listener.h"

namespace ruvia::detail {

// The existing listener transaction stays alive across the public prepare/commit
// boundary, retaining its serialization lease and rollback state.
class prepared_listener_state final {
public:
    explicit prepared_listener_state(worker_ref caller)
        : caller_(std::move(caller)),
          ready_(caller_),
          finished_(caller_),
          ready_receipt_(ready_.make_receipt()),
          finished_receipt_(finished_.make_receipt()) {}
    ~prepared_listener_state() {
        if (decision_ == false) {
            (void)finished_.failure();
        }
    }

    void decide(bool activate) noexcept {
        std::coroutine_handle<> continuation;
        {
            std::lock_guard lock(mutex_);
            if (decision_) {
                return;
            }
            decision_ = activate;
            continuation = std::exchange(continuation_, {});
        }
        if (continuation) {
            caller_.defer_resume(continuation);
        }
    }
    struct decision_wait final {
        prepared_listener_state& state_;
        bool await_ready() const noexcept {
            return false;
        }
        bool await_suspend(std::coroutine_handle<> continuation) noexcept {
            std::lock_guard lock(state_.mutex_);
            if (state_.decision_) {
                return false;
            }
            state_.continuation_ = continuation;
            return true;
        }
        void await_resume() const noexcept {}
        void operator&() const = delete;
    };
    task<void> checkpoint(stop_token stopping) {
        auto registration = stopping.register_callback([this] { decide(false); });
        if (stopping.stop_requested()) {
            throw std::logic_error("prepared listener host is stopping");
        }
        ready_receipt_->complete();
        ready_receipt_.reset();
        co_await decision_wait{*this};
        std::lock_guard lock(mutex_);
        if (!*decision_) {
            throw std::logic_error("prepared listener was cancelled or its host stopped");
        }
    }
    void complete(std::exception_ptr failure) noexcept {
        if (ready_receipt_) {
            ready_receipt_->complete(failure);
            ready_receipt_.reset();
        }
        finished_receipt_->complete(std::move(failure));
        finished_receipt_.reset();
    }
    worker_ref caller_;
    runtime_control_completion ready_;
    runtime_control_completion finished_;

private:
    std::optional<runtime_control_completion::receipt> ready_receipt_;
    std::optional<runtime_control_completion::receipt> finished_receipt_;
    std::mutex mutex_;
    std::optional<bool> decision_;
    std::coroutine_handle<> continuation_;
};

struct prepared_listener_access final {
    static task<void> add(task<prepared_listener> preparation) {
        auto prepared = co_await std::move(preparation);
        co_await std::move(prepared).activate();
    }
    static std::shared_ptr<prepared_listener_state> make(worker_ref caller) {
        return std::make_shared<prepared_listener_state>(std::move(caller));
    }
    static task<prepared_listener> launch(std::shared_ptr<prepared_listener_state> state, task<void> operation) {
        if (!state->caller_.is_current() || !state->caller_.accepting()) {
            throw std::logic_error("listener preparation must run on its live caller worker");
        }
        async_start(std::move(operation), asio::bind_executor(state->caller_.executor(), [state](std::exception_ptr failure) { state->complete(std::move(failure)); }));
        co_await state->ready_.wait();
        if (auto failure = state->ready_.failure()) {
            co_await state->finished_.wait();
            (void)state->finished_.failure();
            std::rethrow_exception(failure);
        }
        co_return prepared_listener(std::move(state));
    }
    static task<void> finish(prepared_listener owner, bool activate) {
        const auto state = owner.state_;
        if (!state || !state->caller_.is_current()) {
            throw std::logic_error("prepared listener control must run on its preparation worker");
        }
        state->decide(activate);
        co_await state->finished_.wait();
        const auto failure = state->finished_.failure();
        owner.state_.reset();
        if (activate && failure) {
            std::rethrow_exception(failure);
        }
    }
};

}  // namespace ruvia::detail

namespace ruvia::detail {

// 在构造冷任务前接管整批 socket，并拥有全部借用地址及配置副本。
// load() 将全部 socket 一次性交给唯一 loader；任何失败只关闭每个句柄一次。
class owned_listen_configs final {
public:
    explicit owned_listen_configs(listen_config config);
    explicit owned_listen_configs(std::span<const listen_config> configs);
    owned_listen_configs(owned_listen_configs&& other) noexcept
        : configs_(std::move(other.configs_)),
          addresses_(std::move(other.addresses_)),
          inherited_owned_(std::exchange(other.inherited_owned_, false)) {}
    owned_listen_configs(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(owned_listen_configs&&) = delete;
    ~owned_listen_configs();

    [[nodiscard]] std::vector<http_server_listener_definition> load();
    [[nodiscard]] bool needs_tls_preparation() const noexcept;
    std::vector<listen_config> configs_{};

private:
    void own_addresses();
    std::vector<std::string> addresses_{};
    bool inherited_owned_{true};
};

}  // namespace ruvia::detail
