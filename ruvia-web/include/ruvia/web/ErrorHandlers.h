#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/Error.h"

namespace ruvia {

class Context;

// Answers a request that failed. Accepts a plain function -- onError(&handler)
// -- and equally a lambda or any other callable, including one that captures
// the logger, config, or metrics sink the handler needs. A self-contained
// callable is owned by the handler value; references captured by that callable
// must still outlive the registered handler.
class HttpErrorHandler final {
public:
    constexpr HttpErrorHandler() noexcept = default;

    constexpr HttpErrorHandler(std::nullptr_t) noexcept {}

    template <typename Callable, typename Stored = std::decay_t<Callable>>
        requires(!std::is_same_v<Stored, HttpErrorHandler> && std::is_invocable_r_v<Task<HttpResponse>, Stored&, Context&, HttpErrorInfo> && std::is_copy_constructible_v<Stored>)
    HttpErrorHandler(Callable&& callable)
        : target_(detail::constructPmrObject<Stored>(detail::processResource(), std::forward<Callable>(callable))),
          invoke_([](void* target, Context& context, HttpErrorInfo error) -> Task<HttpResponse> { return (*static_cast<Stored*>(target))(context, std::move(error)); }),
          destroy_([](void* target, std::pmr::memory_resource* resource) noexcept { detail::destroyPmrObject(static_cast<Stored*>(target), resource); }),
          clone_([](const void* target, std::pmr::memory_resource* resource) -> void* { return detail::constructPmrObject<Stored>(resource, *static_cast<const Stored*>(target)); }),
          resource_(detail::processResource()) {}

    HttpErrorHandler(const HttpErrorHandler& other) {
        copyFrom(other);
    }

    HttpErrorHandler& operator=(const HttpErrorHandler& other) {
        if (this != &other) {
            HttpErrorHandler copy(other);
            swap(copy);
        }
        return *this;
    }

    HttpErrorHandler(HttpErrorHandler&& other) noexcept {
        moveFrom(other);
    }

    HttpErrorHandler& operator=(HttpErrorHandler&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }

    ~HttpErrorHandler() {
        reset();
    }

    // App owns the registered callable. Routers and request services retain
    // only this allocation-free view and never participate in its lifetime.
    [[nodiscard]] HttpErrorHandler borrow() const noexcept {
        return HttpErrorHandler(target_, invoke_);
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context, HttpErrorInfo error) const {
        return invoke_(target_, context, std::move(error));
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] friend constexpr bool operator==(const HttpErrorHandler& left, const HttpErrorHandler& right) noexcept {
        return left.target_ == right.target_ && left.invoke_ == right.invoke_;
    }

private:
    using Invoke = Task<HttpResponse> (*)(void*, Context&, HttpErrorInfo);
    using Destroy = void (*)(void*, std::pmr::memory_resource*) noexcept;
    using Clone = void* (*)(const void*, std::pmr::memory_resource*);

    constexpr HttpErrorHandler(void* target, Invoke invoke) noexcept
        : target_(target), invoke_(invoke) {}

    void copyFrom(const HttpErrorHandler& other) {
        target_ = other.clone_ == nullptr ? other.target_ : other.clone_(other.target_, other.resource_);
        invoke_ = other.invoke_;
        destroy_ = other.destroy_;
        clone_ = other.clone_;
        resource_ = other.resource_;
    }

    void moveFrom(HttpErrorHandler& other) noexcept {
        target_ = std::exchange(other.target_, nullptr);
        invoke_ = std::exchange(other.invoke_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        clone_ = std::exchange(other.clone_, nullptr);
        resource_ = std::exchange(other.resource_, nullptr);
    }

    void swap(HttpErrorHandler& other) noexcept {
        std::swap(target_, other.target_);
        std::swap(invoke_, other.invoke_);
        std::swap(destroy_, other.destroy_);
        std::swap(clone_, other.clone_);
        std::swap(resource_, other.resource_);
    }

    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(target_, resource_);
        }
        target_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
        clone_ = nullptr;
        resource_ = nullptr;
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
    Destroy destroy_{nullptr};
    Clone clone_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

// Answers a request that matched no route, under the same registration and
// lifetime rules as HttpErrorHandler.
class HttpNotFoundHandler final {
public:
    constexpr HttpNotFoundHandler() noexcept = default;

    constexpr HttpNotFoundHandler(std::nullptr_t) noexcept {}

    template <typename Callable, typename Stored = std::decay_t<Callable>>
        requires(!std::is_same_v<Stored, HttpNotFoundHandler> && std::is_invocable_r_v<Task<HttpResponse>, Stored&, Context&> && std::is_copy_constructible_v<Stored>)
    HttpNotFoundHandler(Callable&& callable)
        : target_(detail::constructPmrObject<Stored>(detail::processResource(), std::forward<Callable>(callable))),
          invoke_([](void* target, Context& context) -> Task<HttpResponse> { return (*static_cast<Stored*>(target))(context); }),
          destroy_([](void* target, std::pmr::memory_resource* resource) noexcept { detail::destroyPmrObject(static_cast<Stored*>(target), resource); }),
          clone_([](const void* target, std::pmr::memory_resource* resource) -> void* { return detail::constructPmrObject<Stored>(resource, *static_cast<const Stored*>(target)); }),
          resource_(detail::processResource()) {}

    HttpNotFoundHandler(const HttpNotFoundHandler& other) {
        copyFrom(other);
    }

    HttpNotFoundHandler& operator=(const HttpNotFoundHandler& other) {
        if (this != &other) {
            HttpNotFoundHandler copy(other);
            swap(copy);
        }
        return *this;
    }

    HttpNotFoundHandler(HttpNotFoundHandler&& other) noexcept {
        moveFrom(other);
    }

    HttpNotFoundHandler& operator=(HttpNotFoundHandler&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }

    ~HttpNotFoundHandler() {
        reset();
    }

    [[nodiscard]] HttpNotFoundHandler borrow() const noexcept {
        return HttpNotFoundHandler(target_, invoke_);
    }

    [[nodiscard]] Task<HttpResponse> operator()(Context& context) const {
        return invoke_(target_, context);
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] friend constexpr bool operator==(const HttpNotFoundHandler& left, const HttpNotFoundHandler& right) noexcept {
        return left.target_ == right.target_ && left.invoke_ == right.invoke_;
    }

private:
    using Invoke = Task<HttpResponse> (*)(void*, Context&);
    using Destroy = void (*)(void*, std::pmr::memory_resource*) noexcept;
    using Clone = void* (*)(const void*, std::pmr::memory_resource*);

    constexpr HttpNotFoundHandler(void* target, Invoke invoke) noexcept
        : target_(target), invoke_(invoke) {}

    void copyFrom(const HttpNotFoundHandler& other) {
        target_ = other.clone_ == nullptr ? other.target_ : other.clone_(other.target_, other.resource_);
        invoke_ = other.invoke_;
        destroy_ = other.destroy_;
        clone_ = other.clone_;
        resource_ = other.resource_;
    }

    void moveFrom(HttpNotFoundHandler& other) noexcept {
        target_ = std::exchange(other.target_, nullptr);
        invoke_ = std::exchange(other.invoke_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        clone_ = std::exchange(other.clone_, nullptr);
        resource_ = std::exchange(other.resource_, nullptr);
    }

    void swap(HttpNotFoundHandler& other) noexcept {
        std::swap(target_, other.target_);
        std::swap(invoke_, other.invoke_);
        std::swap(destroy_, other.destroy_);
        std::swap(clone_, other.clone_);
        std::swap(resource_, other.resource_);
    }

    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(target_, resource_);
        }
        target_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
        clone_ = nullptr;
        resource_ = nullptr;
    }

    void* target_{nullptr};
    Invoke invoke_{nullptr};
    Destroy destroy_{nullptr};
    Clone clone_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
};

static_assert(sizeof(HttpErrorHandler) == 5 * sizeof(void*));
static_assert(sizeof(HttpNotFoundHandler) == 5 * sizeof(void*));

}  // namespace ruvia
