#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/Next.h"

namespace ruvia {

class Context;
class HttpResponse;

namespace detail {

struct ControllerMiddlewareDescriptor final {
    using Invoke = Task<HttpResponse> (*)(void*, Context&, const Next&);
    using Create = void* (*)();
    using Destroy = void (*)(void*) noexcept;

    Invoke invoke{nullptr};
    Create create{nullptr};
    Destroy destroy{nullptr};

    [[nodiscard]] explicit operator bool() const noexcept {
        return invoke != nullptr && create != nullptr && destroy != nullptr;
    }
};

}  // namespace detail

}  // namespace ruvia
