#pragma once

// Internal startup-time middleware descriptor.

#include "ruvia/core/Task.h"
#include "ruvia/web/Next.h"

namespace ruvia {

class Context;

namespace detail {

class ControllerMiddlewareDescriptor;

template <typename MiddlewareT>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor();

class ControllerMiddlewareDescriptor final {
public:
    using Invoke = Task<void> (*)(void*, Context&, Next&);
    using Create = void* (*)();
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

    [[nodiscard]] Destroy destroy() const noexcept {
        return destroy_;
    }

    [[nodiscard]] const void* validatedModelTypeKey() const noexcept {
        return validatedModelTypeKey_;
    }

private:
    template <typename MiddlewareT>
    friend ControllerMiddlewareDescriptor makeMiddlewareDescriptor();

    constexpr ControllerMiddlewareDescriptor() noexcept = default;
    constexpr ControllerMiddlewareDescriptor(
        Invoke invoke,
        Create create,
        Destroy destroy,
        const void* validatedModelTypeKey) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy),
          validatedModelTypeKey_(validatedModelTypeKey) {}

    Invoke invoke_{nullptr};
    Create create_{nullptr};
    Destroy destroy_{nullptr};
    const void* validatedModelTypeKey_{nullptr};
};

}  // namespace detail

}  // namespace ruvia
