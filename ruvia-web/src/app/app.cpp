#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <openssl/ssl.h>

#include "ruvia/core/worker_selection.h"

#include "app/app_run_coordinator.h"
#include "app/app_runtime_graph.h"
#include "app/app_state.h"

namespace ruvia::detail {

app_state::app_state()
    : runtime_(nullptr, pmr_object_deleter<app_runtime_graph>{process_resource()}) {
    // listen() keeps loaded TLS credentials here; initializing OpenSSL first registers its atexit cleanup before the app singleton completes, so the app is destroyed first.
    if (OPENSSL_init_ssl(0, nullptr) != 1) {
        throw std::runtime_error("failed to initialize OpenSSL");
    }
}

app_state::~app_state() = default;

}  // namespace ruvia::detail

namespace ruvia {

application& app() {
    static application instance;
    return instance;
}

application::application()
    : state_(construct_pmr_object<detail::app_state>(process_resource())) {}

application::~application() = default;

void application::state_deleter_type::operator()(detail::app_state* state_value) const noexcept {
    destroy_pmr_object(state_value, process_resource());
}

const env& application::env() const noexcept {
    return state_->env_;
}

http_server_stats application::http_stats() const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    http_server_stats total;
    if (!state_value.runtime_) {
        return total;
    }
    for (const auto& worker : state_value.runtime_->workers_) {
        const auto stats = worker.runtime_->stats();
        total.active_connections_ += stats.active_connections_;
        total.connections_refused_ += stats.connections_refused_;
        total.connection_failures_ += stats.connection_failures_;
        total.accept_failures_ += stats.accept_failures_;
        total.worker_failures_ += stats.worker_failures_;
    }
    // One application-wide refresher owns the count; workers only adopt.
    if (state_value.runtime_->document_root_) {
        total.document_root_refresh_failures_ = state_value.runtime_->document_root_->failures();
    }
    return total;
}

blocking_pool_stats application::get_blocking_pool_stats() const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    if (!state_value.runtime_ || !state_value.runtime_->blocking_pool_) {
        return {};
    }
    return state_value.runtime_->blocking_pool_->stats();
}

std::vector<web_worker_handle> application::workers() const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    std::vector<web_worker_handle> result;
    if (!state_value.runtime_ || !state_value.runtime_->workers_ready_) {
        return result;
    }
    result.reserve(state_value.runtime_->workers_.size());
    for (const auto& worker : state_value.runtime_->workers_) {
        result.push_back(worker.runtime_->web_worker());
    }
    return result;
}

web_worker_handle application::worker_for(std::uint64_t key) const {
    auto& state_value = *state_;
    std::lock_guard lock(state_value.mutex_);
    if (!state_value.runtime_ || !state_value.runtime_->workers_ready_ ||
        state_value.runtime_->workers_.empty()) {
        return {};
    }
    return state_value.runtime_->workers_[key % state_value.runtime_->workers_.size()].runtime_->web_worker();
}

web_worker_handle application::worker_for(std::string_view key) const {
    return worker_for(ruvia::worker_selection_hash(key));
}

void application::run() {
    detail::run_app(*this, *state_);
}

void application::stop() {
    auto& state_value = *state_;
    {
        std::lock_guard lock(state_value.mutex_);
        const auto request = state_value.lifecycle_.request_stop();
        if (request == detail::app_stop_request::ignored) {
            return;
        }
        // A serving app drains first: its run() owner wakes below and stops
        // the runtime once in-flight requests have finished.
        if (request == detail::app_stop_request::startup) {
            detail::request_runtime_stop(state_value);
        }
    }
    state_value.lifecycle_changed_.notify_all();
}

}  // namespace ruvia
