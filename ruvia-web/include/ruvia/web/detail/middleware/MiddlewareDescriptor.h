#pragma once

#include <string_view>

// Internal startup-time middleware descriptor.

#include "ruvia/core/Task.h"
#include "ruvia/web/Next.h"

namespace ruvia {

class Context;

namespace detail {

class ControllerMiddlewareDescriptor;

template <typename MiddlewareT, typename... Args>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor(Args&&... args);

class ControllerMiddlewareDescriptor final {
public:
    using Invoke = Task<void> (*)(void*, Context&, Next&);
    // `args` points at the registration-owned argument tuple captured by
    // use<T>(args...) -- empty when the middleware is default constructed. It
    // lives on the process registration resource, so it outlives every instance
    // the router materializes from this descriptor.
    using Create = void* (*)(const void* args);
    using Destroy = void (*)(void*) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return invoke_ != nullptr;
    }

    [[nodiscard]] Invoke invoke() const noexcept {
        return invoke_;
    }

    [[nodiscard]] Create create() const noexcept {
        return create_;
    }

    [[nodiscard]] const void* args() const noexcept {
        return args_;
    }

    [[nodiscard]] Destroy destroy() const noexcept {
        return destroy_;
    }

    // Two registrations of the same middleware type differ when they carry
    // different arguments, so identity spans the argument pointer as well.
    [[nodiscard]] friend bool operator==(const ControllerMiddlewareDescriptor& left, const ControllerMiddlewareDescriptor& right) noexcept {
        return left.invoke_ == right.invoke_ && left.create_ == right.create_ && left.destroy_ == right.destroy_ && left.args_ == right.args_ && left.prefix_ == right.prefix_;
    }

    [[nodiscard]] const void* validatedModelTypeKey() const noexcept {
        return validatedModelTypeKey_;
    }

    // Empty means app-wide. Otherwise the middleware runs only on routes whose
    // path is under this prefix, decided once when the route table is built --
    // there is no per-request pattern matching. The text is registration-owned
    // and outlives the table.
    [[nodiscard]] std::string_view prefix() const noexcept {
        return prefix_;
    }

    [[nodiscard]] ControllerMiddlewareDescriptor scopedTo(std::string_view prefix) const noexcept {
        auto scoped = *this;
        scoped.prefix_ = prefix;
        return scoped;
    }

    [[nodiscard]] bool usesRouteRateLimit() const noexcept {
        return usesRouteRateLimit_;
    }

private:
    template <typename MiddlewareT, typename... Args>
    friend ControllerMiddlewareDescriptor makeMiddlewareDescriptor(Args&&... args);

    constexpr ControllerMiddlewareDescriptor() noexcept = default;
    constexpr ControllerMiddlewareDescriptor(Invoke invoke, Create create, Destroy destroy, const void* args, const void* validatedModelTypeKey, bool usesRouteRateLimit) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy),
          args_(args),
          validatedModelTypeKey_(validatedModelTypeKey),
          usesRouteRateLimit_(usesRouteRateLimit) {}

    Invoke invoke_{nullptr};
    Create create_{nullptr};
    Destroy destroy_{nullptr};
    const void* args_{nullptr};
    const void* validatedModelTypeKey_{nullptr};
    std::string_view prefix_{};
    bool usesRouteRateLimit_{false};
};

}  // namespace detail

}  // namespace ruvia
