#pragma once

#include <memory_resource>
#include <stdexcept>
#include <vector>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_interim_response.h"

namespace ruvia::detail {

// One request's typed interim output lane. The protocol driver and its worker
// resource outlive this object; operations own their fields until completion.
class http_interim_response_output final {
public:
    using write_type = task<void> (*)(void*, const http_interim_response_head&);
    http_interim_response_output(std::pmr::memory_resource* resource, void* target, write_type write) noexcept
        : resource_(resource),
          target_(target),
          write_(write) {}

    [[nodiscard]] scoped_operation<void> inform(const http_interim_response_head& response);
    // Answers a 100-continue expectation before the body is read, on the same
    // lane as inform() so the two heads never interleave. A final response
    // that has started already answered the expectation, and RFC 9110 §15.2
    // forbids any 1xx after it, so then nothing is written.
    task<void> send_continue();
    [[nodiscard]] bool final_committed() const noexcept {
        return final_committed_;
    }
    void commit_final() {
        if (output_active_) {
            throw std::logic_error("interim response output is still active");
        }
        final_committed_ = true;
    }

private:
    [[nodiscard]] task<void> write_owned(http_status_code status, std::pmr::vector<http_header> fields);
    std::pmr::memory_resource* resource_;
    void* target_;
    write_type write_;
    bool final_committed_{};
    bool output_active_{};
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail
