#pragma once

#include <memory>
#include <utility>

namespace ruvia::detail {

class worker_dispatcher;
// A shutdown hook is an intrusive node in its worker's registry, held as a
// data member of the state it notifies. Linking and unlinking are O(1) under
// the registry mutex and never allocate or scan. The node keeps the registry
// alive, so it can unlink from its destructor after the owner has already
// released its own worker handle; the owner may die on any thread, so the
// registry stays mutex-protected. Owners declare the hook as their first data
// member so it unlinks only after every other member has been destroyed.
class worker_shutdown_hook final {
public:
    // Per-owner-type phase table; a null entry skips that phase.
    struct operations_type final {
        using phase_type = void (*)(void* owner) noexcept;

        phase_type stopping_;
        phase_type stopping_complete_;
    };

    worker_shutdown_hook() noexcept = default;
    worker_shutdown_hook(const worker_shutdown_hook&) = delete;
    worker_shutdown_hook& operator=(const worker_shutdown_hook&) = delete;
    ~worker_shutdown_hook();

private:
    friend class worker_dispatcher;

    const operations_type* operations_{nullptr};
    std::shared_ptr<worker_dispatcher> registry_;
    // Aliases the owner object; locking it pins the owner, and therefore this
    // hook, for the duration of one phase callback.
    std::weak_ptr<void> owner_;
    worker_shutdown_hook* previous_{nullptr};
    worker_shutdown_hook* next_{nullptr};
    bool linked_{false};
};

// Phase table of one owner type: `void worker_stopping() noexcept` runs in the
// first notification pass and `void worker_stopping_complete() noexcept` in the
// second, after every hook has seen the first.
template <typename owner_type>
[[nodiscard]] consteval worker_shutdown_hook::operations_type make_worker_shutdown_operations() noexcept {
    constexpr bool has_stopping = requires(owner_type& owner) { owner.worker_stopping(); };
    constexpr bool has_stopping_complete = requires(owner_type& owner) { owner.worker_stopping_complete(); };
    static_assert(has_stopping || has_stopping_complete, "a worker shutdown hook owner must handle a stop phase");
    worker_shutdown_hook::operations_type operations{nullptr, nullptr};
    if constexpr (has_stopping) {
        static_assert(noexcept(std::declval<owner_type&>().worker_stopping()));
        operations.stopping_ = [](void* owner) noexcept { static_cast<owner_type*>(owner)->worker_stopping(); };
    }
    if constexpr (has_stopping_complete) {
        static_assert(noexcept(std::declval<owner_type&>().worker_stopping_complete()));
        operations.stopping_complete_ = [](void* owner) noexcept {
            static_cast<owner_type*>(owner)->worker_stopping_complete();
        };
    }
    return operations;
}

template <typename owner_type>
inline constexpr worker_shutdown_hook::operations_type worker_shutdown_operations =
    make_worker_shutdown_operations<owner_type>();

}  // namespace ruvia::detail
