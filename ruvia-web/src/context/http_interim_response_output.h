#pragma once

#include <stdexcept>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_interim_response.h"

#include "integration/operation_interrupt.h"

namespace ruvia {
class context;
}

namespace ruvia::detail {

// One request's typed interim output lane. The protocol driver outlives this
// object; operations own their fields until completion.
class http_interim_response_output final {
public:
    using write_type = task<void> (*)(void*, const http_interim_response_head&);
    http_interim_response_output(void* target, write_type write) noexcept
        : target_(target),
          write_(write) {}

    // An interrupted inform() resets the request of `context_value`.
    [[nodiscard]] task<void> inform(
        const http_interim_response_head& response, operation_options operation, context& context_value);
    // Answers a 100-continue expectation before the body is read, on the same
    // lane as inform() so the two heads never interleave. A final response
    // that has started already answered the expectation, and RFC 9110 §15.2
    // forbids any 1xx after it, so then nothing is written.
    task<void> send_continue();
    void commit_final() {
        if (output_active_) {
            throw std::logic_error("interim response output is still active");
        }
        final_committed_ = true;
    }

private:
    [[nodiscard]] task<void> write_owned(
        http_status_code status, std::vector<http_header> fields, operation_options operation, context& context_value);
    void* target_;
    write_type write_;
    bool final_committed_{};
    bool output_active_{};
    operation_interrupt_source interrupts_;
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail
