#pragma once

#include "ruvia/core/event_loop_attachment.h"

#include "context/context_services.h"
#include "test_io_context.h"

namespace ruvia::test {

// Pure context tests still need the same mandatory, address-stable worker
// borrow as production dispatch. The attached endpoint is intentionally not
// driven: tests that perform worker I/O own and run their local event_loop.
[[nodiscard]] inline const worker_handle& test_worker_handle() {
    static auto attachment = attach_event_loop(new_test_io_context());
    static const auto worker_value = attachment.loop().handle();
    return worker_value;
}

[[nodiscard]] inline const stop_token& test_stop_token() {
    static const stop_token token;
    return token;
}

[[nodiscard]] inline detail::context_services test_context_services() {
    return detail::context_services(test_worker_handle(), test_stop_token());
}

}  // namespace ruvia::test
