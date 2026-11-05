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
    // Header-list bounds are enforced by the HTTP/2 submit path.
    [[nodiscard]] bool push(http_push_request_view request) {
        return submit_(target_, request);
    }

private:
    void* target_;
    submit_type submit_;
};
}  // namespace ruvia::detail
