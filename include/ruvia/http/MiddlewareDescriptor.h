#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/Next.h"

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

private:
    template <typename MiddlewareT>
    friend ControllerMiddlewareDescriptor makeMiddlewareDescriptor();

    constexpr ControllerMiddlewareDescriptor() noexcept = default;
    constexpr ControllerMiddlewareDescriptor(Invoke invoke, Create create, Destroy destroy) noexcept
        : invoke_(invoke),
          create_(create),
          destroy_(destroy) {}

    Invoke invoke_{nullptr};
    Create create_{nullptr};
    Destroy destroy_{nullptr};
};

}  // namespace detail

}  // namespace ruvia
