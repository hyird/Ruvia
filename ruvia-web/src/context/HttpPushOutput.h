#pragma once

#include <memory_resource>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/http/HttpPush.h"

namespace ruvia::detail {
// Borrows the request's connection. Inputs are owned in the worker pool before
// returning a lazy operation; a cold operation never submits a promise.
class HttpPushOutput final {
public:
    using Submit = Task<bool> (*)(void*, HttpPushRequestView);
    HttpPushOutput(std::pmr::memory_resource* resource, void* target, Submit submit) noexcept
        : resource_(resource),
          target_(target),
          submit_(submit) {}
    [[nodiscard]] ScopedOperation<bool> push(HttpPushRequestView request);

private:
    Task<bool> pushOwned(HttpPushRequest request);
    std::pmr::memory_resource* resource_;
    void* target_;
    Submit submit_;
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail
