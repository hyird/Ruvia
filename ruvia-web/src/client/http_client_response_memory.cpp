#include "client/http_client_response_memory.h"

#include <exception>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>

#include "ruvia/core/memory/pmr_object.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "client/http_client_result_budget.h"

namespace ruvia::detail {

void* http_client_response_memory_domain::receive_resource::do_allocate(
    std::size_t bytes_value, std::size_t alignment) {
    if (!budget_.reserve_in_flight(bytes_value)) {
        throw http_client_error(http_client_error::code_type::result_budget_exceeded,
            "HTTP client in-flight response memory budget is exhausted");
    }
    try {
        return upstream_.allocate(bytes_value, alignment);
    } catch (...) {
        budget_.release_in_flight(bytes_value);
        throw;
    }
}

void http_client_response_memory_domain::receive_resource::do_deallocate(
    void* allocation, std::size_t bytes_value, std::size_t alignment) {
    upstream_.deallocate(allocation, bytes_value, alignment);
    budget_.release_in_flight(bytes_value);
}

http_client_response_memory_domain::http_client_response_memory_domain(const worker_handle& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain,
    std::pmr::memory_resource* worker_resource, std::pmr::memory_resource& storage)
    : storage_(storage),
      worker_(worker_value),
      result_budget_domain_(result_budget_domain),
      receive_resource_(response_upstream(owned_memory_, worker_resource), *result_budget_domain_) {
    if (!worker_.valid()) {
        throw std::invalid_argument("HTTP client response memory requires a worker");
    }
}

http_client_response_memory_domain::~http_client_response_memory_domain() {
    if (phase_ != phase_type::prepared) {
        require_current();
    }
    if (state_head_ != nullptr || references_ != 0) {
        std::terminate();
    }
}

std::pmr::memory_resource& http_client_response_memory_domain::response_upstream(
    std::optional<worker_memory>& owned_memory, std::pmr::memory_resource* worker_resource) {
    if (worker_resource != nullptr) {
        return *worker_resource;
    }
    return *owned_memory.emplace().resource();
}

http_client_response_memory_domain::owner http_client_response_memory_domain::create(
    const worker_handle& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain,
    std::pmr::memory_resource& worker_resource) {
    return allocate(worker_value, result_budget_domain, &worker_resource, worker_resource);
}

http_client_response_memory_domain::owner http_client_response_memory_domain::create_response_owned(
    const worker_handle& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    // The object cannot live in the memory it owns; new_delete is the only
    // other worker-free choice (the process resource is lock-shared).
    return allocate(worker_value, result_budget_domain, nullptr, *std::pmr::new_delete_resource());
}

http_client_response_memory_domain::owner http_client_response_memory_domain::allocate(
    const worker_handle& worker_value,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain,
    std::pmr::memory_resource* worker_resource, std::pmr::memory_resource& storage) {
    auto* const object = storage.allocate(sizeof(http_client_response_memory_domain),
        alignof(http_client_response_memory_domain));
    try {
        return owner(::new (object) http_client_response_memory_domain(
                         worker_value, result_budget_domain, worker_resource, storage),
            deleter_type{});
    } catch (...) {
        storage.deallocate(object, sizeof(http_client_response_memory_domain),
            alignof(http_client_response_memory_domain));
        throw;
    }
}

void http_client_response_memory_domain::destroy() noexcept {
    auto& storage = storage_;
    this->~http_client_response_memory_domain();
    storage.deallocate(this, sizeof(http_client_response_memory_domain),
        alignof(http_client_response_memory_domain));
}

void http_client_response_memory_domain::deleter_type::operator()(
    http_client_response_memory_domain* domain) const noexcept {
    if (domain != nullptr) {
        domain->release_root();
    }
}

void http_client_response_memory_domain::require_current() const noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
}

void http_client_response_memory_domain::retain() noexcept {
    require_current();
    if (references_ == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references_;
}

void http_client_response_memory_domain::release() noexcept {
    require_current();
    if (references_ == 0) {
        std::terminate();
    }
    if (--references_ == 0) {
        destroy();
    }
}

void http_client_response_memory_domain::release_root() noexcept {
    // Startup rollback has never published worker-affine response storage.
    // No worker scheduling is necessary (or possible after failed preparation).
    if (phase_ == phase_type::prepared) {
        if (references_ != 1 || state_head_ != nullptr) {
            std::terminate();
        }
        references_ = 0;
        destroy();
        return;
    }
    // Active storage is retired explicitly by the pool on its owning worker;
    // do not conceal a missing join with unstructured deferred destruction.
    release();
}

http_client_response_state* http_client_response_memory_domain::create_state(http_client_pool& pool) {
    require_current();
    if (phase_ == phase_type::retired) {
        throw std::logic_error("HTTP client response transport is retired");
    }
    phase_ = phase_type::active;
    auto* state_value = construct_pmr_object<http_client_response_state>(
        resource(), *this);
    state_value->pool_ = &pool;
    attach_state(*state_value);
    return state_value;
}

void http_client_response_memory_domain::attach_state(http_client_response_state& state_value) noexcept {
    require_current();
    state_value.next_memory_state_ = state_head_;
    if (state_head_ != nullptr) {
        state_head_->previous_memory_state_ = &state_value;
    }
    state_head_ = &state_value;
    retain();
}

void http_client_response_memory_domain::destroy_state(http_client_response_state* state_value) noexcept {
    require_current();
    if (state_value == nullptr || &state_value->memory_domain_ != this) {
        std::terminate();
    }
    if (state_value->pool_ != nullptr) {
        unlink_state(*state_value);
    } else if (state_value->previous_memory_state_ != nullptr || state_value->next_memory_state_ != nullptr) {
        std::terminate();
    }
    destroy_pmr_object(state_value, resource());
    release();
}

void http_client_response_memory_domain::unlink_state(http_client_response_state& state_value) noexcept {
    if (state_value.previous_memory_state_ != nullptr) {
        state_value.previous_memory_state_->next_memory_state_ = state_value.next_memory_state_;
    } else if (state_head_ == &state_value) {
        state_head_ = state_value.next_memory_state_;
    } else {
        std::terminate();
    }
    if (state_value.next_memory_state_ != nullptr) {
        state_value.next_memory_state_->previous_memory_state_ = state_value.previous_memory_state_;
    }
    state_value.previous_memory_state_ = nullptr;
    state_value.next_memory_state_ = nullptr;
}

void http_client_response_memory_domain::detach_transport_bindings(http_client_pool& pool) noexcept {
    require_current();
    phase_ = phase_type::retired;
    while (state_head_ != nullptr) {
        auto* state_value = state_head_;
        if (state_value->pool_ != &pool) {
            std::terminate();
        }
        // Revoke registration before notifying consumers. A continuation may
        // release this state or a sibling; never cache a next pointer across it.
        unlink_state(*state_value);
        state_value->detach_transport_bindings();
    }
}

http_client_response_state::http_client_response_state(http_client_response_memory_domain& memory_domain)
    : head_signal_(memory_domain.worker()),
      data_signal_(memory_domain.worker()),
      space_signal_(memory_domain.worker()),
      resource_(memory_domain.resource()),
      informational_(resource_),
      headers_(resource_),
      trailers_(resource_),
      buffered_(resource_),
      pending_(resource_),
      memory_domain_(memory_domain) {}

void http_client_response_state::require_owner_worker(const char* message) const {
    if (!memory_domain_.worker().is_current()) {
        throw std::logic_error(message);
    }
}

void http_client_response_state::detach_transport_bindings() noexcept {
    if (tunnel_) {
        tunnel_->output_.stop();
    }
    if (upload_) {
        upload_->output_.stop();
    }
    cancellation_id_ = 0;
    pool_ = nullptr;
    if (!complete_) {
        pool_closed_ = true;
        complete_ = true;
    }
    head_signal_.notify();
    data_signal_.notify();
    space_signal_.notify();
}

}  // namespace ruvia::detail
