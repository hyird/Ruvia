#pragma once

#include <memory_resource>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_push.h"

namespace ruvia::detail {
// Borrows the request's connection. Inputs are owned in the worker pool before
// returning a lazy operation; a cold operation never submits a promise.
class http_push_output final {
public:
    using submit_type = task<bool> (*)(void*, http_push_request_view);
    http_push_output(std::pmr::memory_resource* resource, void* target, submit_type submit) noexcept
        : resource_(resource),
          target_(target),
          submit_(submit) {}
    [[nodiscard]] scoped_operation<bool> push(http_push_request_view request);

private:
    task<bool> push_owned(http_push_request request);
    std::pmr::memory_resource* resource_;
    void* target_;
    submit_type submit_;
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail
