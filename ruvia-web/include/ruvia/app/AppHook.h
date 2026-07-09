#pragma once

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "detail/HttpPmrObject.h"
#include "ruvia/memory/ProcessResource.h"

namespace ruvia {

class AppHook final {
public:
    AppHook() noexcept = default;
    AppHook(std::nullptr_t) noexcept {}

    template <
        typename Callable,
        typename Stored = std::decay_t<Callable>,
        std::enable_if_t<
            !std::is_same_v<Stored, AppHook> &&
            std::is_copy_constructible_v<Stored> &&
            std::is_invocable_r_v<void, Stored&>,
            int> = 0>
    AppHook(Callable&& callable)
        : resource_(detail::processResource()) {
        emplace<Stored>(std::forward<Callable>(callable));
    }

    AppHook(const AppHook& other)
        : resource_(other.resource_) {
        copyFrom(other);
    }

    AppHook& operator=(const AppHook& other) {
        if (this == &other) {
            return *this;
        }
        reset();
        resource_ = other.resource_;
        copyFrom(other);
        return *this;
    }

    AppHook(AppHook&& other) noexcept {
        moveFrom(other);
    }

    AppHook& operator=(AppHook&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        moveFrom(other);
        return *this;
    }

    ~AppHook() {
        reset();
    }

    void operator()() const {
        if (invoke_ == nullptr) {
            throw std::logic_error("app hook is empty");
        }
        invoke_(target_);
    }

private:
    using Invoke = void (*)(void*);
    using Destroy = void (*)(void*, std::pmr::memory_resource*) noexcept;
    using Clone = void* (*)(const void*, std::pmr::memory_resource*);

    template <typename Stored, typename Callable>
    void emplace(Callable&& callable) {
        target_ = detail::constructHttpPmrObject<Stored>(resource_, std::forward<Callable>(callable));
        invoke_ = [](void* target) {
            (*static_cast<Stored*>(target))();
        };
        destroy_ = [](void* target, std::pmr::memory_resource* resource) noexcept {
            detail::destroyHttpPmrObject(static_cast<Stored*>(target), resource);
        };
        clone_ = [](const void* target, std::pmr::memory_resource* resource) -> void* {
            return detail::constructHttpPmrObject<Stored>(
                resource,
                *static_cast<const Stored*>(target));
        };
    }

    void copyFrom(const AppHook& other) {
        void* cloned = nullptr;
        if (other.target_ != nullptr && other.clone_ != nullptr) {
            cloned = other.clone_(other.target_, resource_);
        }
        target_ = cloned;
        invoke_ = other.invoke_;
        destroy_ = other.destroy_;
        clone_ = other.clone_;
    }

    void moveFrom(AppHook& other) noexcept {
        target_ = std::exchange(other.target_, nullptr);
        invoke_ = std::exchange(other.invoke_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        clone_ = std::exchange(other.clone_, nullptr);
        resource_ = std::exchange(other.resource_, nullptr);
    }

    void reset() noexcept {
        if (target_ != nullptr && destroy_ != nullptr) {
            destroy_(target_, resource_);
        }
        target_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
        clone_ = nullptr;
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
    Destroy destroy_{nullptr};
    Clone clone_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

}  // namespace ruvia
