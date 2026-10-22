#pragma once

#include "ruvia/http/http_push.h"

namespace ruvia::detail {
// Borrows the request's connection.
class http_push_output final {
public:
    using submit_type = bool (*)(void*, http_push_request_view);
    http_push_output(void* target, submit_type submit) noexcept
        : target_(target),
          submit_(submit) {}
    [[nodiscard]] bool push(http_push_request_view request);

private:
    void* target_;
    submit_type submit_;
};
}  // namespace ruvia::detail
