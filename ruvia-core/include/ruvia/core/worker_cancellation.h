#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

// Owns only a detachable owner pointer and a stable worker handle; queued work
// retains the target so that releasing it never depends on the owner's lifetime.
template <typename owner_type>
class worker_cancellation_target final
    : public std::enable_shared_from_this<worker_cancellation_target<owner_type>> {
public:
    worker_cancellation_target(owner_type& owner_value, const worker_handle& worker_value) noexcept
        : owner_(&owner_value),
          worker_(worker_value) {}

    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }
    [[nodiscard]] std::uint64_t next_operation_id() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (++next_operation_id_ == 0) {
            ++next_operation_id_;
        }
        return next_operation_id_;
    }
    [[nodiscard]] std::shared_ptr<worker_cancellation_target> retain() noexcept {
        auto target = this->weak_from_this().lock();
        if (!target) {
            std::terminate();
        }
        return target;
    }
    void dispatch(std::uint64_t operation_id) noexcept {
        if (auto* owner = owner_.load(std::memory_order_acquire)) {
            owner->cancel_operation_by_id(operation_id);
        }
    }
    void detach(owner_type& owner_value) noexcept {
        auto* previous = owner_.exchange(nullptr, std::memory_order_acq_rel);
        if (previous && previous != &owner_value) {
            std::terminate();
        }
    }

private:
    std::atomic<owner_type*> owner_;
    worker_handle worker_;
    std::uint64_t next_operation_id_{0};
};

template <typename owner_type>
[[nodiscard]] inline std::shared_ptr<worker_cancellation_target<owner_type>>
make_worker_cancellation_target(owner_type& owner_value, const worker_handle& worker_value) {
    using target_type = worker_cancellation_target<owner_type>;
    return std::allocate_shared<target_type>(
        std::pmr::polymorphic_allocator<target_type>(detail::process_resource()), owner_value, worker_value);
}

template <typename target_type>
class worker_cancellation_dispatch final {
public:
    worker_cancellation_dispatch(std::shared_ptr<target_type> target, std::uint64_t operation_id) noexcept
        : target_(std::move(target)),
          operation_id_(operation_id) {}
    void operator()() noexcept {
        target_->dispatch(operation_id_);
    }

private:
    std::shared_ptr<target_type> target_;
    std::uint64_t operation_id_;
};

template <typename target_type>
class worker_cancellation_post final {
public:
    worker_cancellation_post(const std::shared_ptr<target_type>& target, std::uint64_t operation_id) noexcept
        : target_(target.get()),
          operation_id_(operation_id) {
        if (!target_) {
            std::terminate();
        }
    }
    worker_cancellation_post(target_type* target, std::uint64_t operation_id) noexcept
        : target_(target),
          operation_id_(operation_id) {
        if (!target_) {
            std::terminate();
        }
    }
    void operator()() noexcept {
        if (target_->worker().is_current()) {
            target_->dispatch(operation_id_);
            return;
        }
        auto retained = target_->retain();
        // Cancellation belongs to an already admitted operation. Use the
        // dispatcher completion lane so a full user queue cannot reject it.
        // Detached endpoints have already retired their owners and operations.
        (void)detail::worker_handle_access::defer_if_attached(retained->worker(),
            worker_cancellation_dispatch<target_type>(retained, operation_id_));
    }

private:
    target_type* target_;
    std::uint64_t operation_id_;
};

template <typename target_type>
inline constexpr bool worker_cancellation_post_is_inline =
    sizeof(worker_cancellation_post<target_type>) <= 3 * sizeof(void*) &&
    sizeof(worker_cancellation_dispatch<target_type>) <= 3 * sizeof(void*) &&
    alignof(worker_cancellation_post<target_type>) <= alignof(std::max_align_t) &&
    alignof(worker_cancellation_dispatch<target_type>) <= alignof(std::max_align_t) &&
    std::is_nothrow_move_constructible_v<worker_cancellation_post<target_type>> &&
    std::is_nothrow_move_constructible_v<worker_cancellation_dispatch<target_type>>;

// Worker-affine registration. Bind owner state before arm(), which may deliver
// an already-requested stop immediately. reset() joins the callback before
// clearing its identity; a late posted cancellation cannot address a reused slot.
template <typename target_type>
class worker_cancellation_registration final {
public:
    worker_cancellation_registration(const std::shared_ptr<target_type>& target, std::uint64_t& identity)
        : target_(target.get()),
          identity_(&identity),
          id_(require_target(target).next_operation_id()) {
        identity = id_;
    }
    [[nodiscard]] static worker_cancellation_registration observe(
        const std::shared_ptr<target_type>& target, std::uint64_t id) {
        return worker_cancellation_registration(&require_target(target), id);
    }
    worker_cancellation_registration(const worker_cancellation_registration&) = delete;
    worker_cancellation_registration& operator=(const worker_cancellation_registration&) = delete;
    worker_cancellation_registration(worker_cancellation_registration&&) = delete;
    worker_cancellation_registration& operator=(worker_cancellation_registration&&) = delete;
    ~worker_cancellation_registration() {
        reset();
    }
    [[nodiscard]] std::uint64_t id() const noexcept {
        return id_;
    }
    void arm(const stop_token& token) {
        if (!target_ || armed_) {
            std::terminate();
        }
        armed_ = true;
        if (token.stoppable()) {
            // stop_token delivers an already-requested stop inline and invokes
            // a linked callback at most once. A second stop_requested dispatch
            // would duplicate cancellation and race a foreign queued delivery.
            token.register_callback(registration_, worker_cancellation_post<target_type>(target_, id_));
        }
    }
    void reset() noexcept {
        registration_.reset();
        if (identity_ && *identity_ == id_) {
            *identity_ = 0;
        }
        identity_ = nullptr;
        target_ = nullptr;
    }

private:
    static target_type& require_target(const std::shared_ptr<target_type>& target) {
        if (!target || !target->worker().is_current()) {
            std::terminate();
        }
        return *target;
    }
    worker_cancellation_registration(target_type* target, std::uint64_t id) noexcept
        : target_(target),
          id_(id) {}
    target_type* target_;
    std::uint64_t* identity_{};
    std::uint64_t id_;
    stop_registration registration_;
    bool armed_{};
};

}  // namespace ruvia
