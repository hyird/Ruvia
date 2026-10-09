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
    void commit_final() {
        if (scope_.has_pending_operations()) {
            throw std::logic_error("interim response output is still active");
        }
        final_committed_ = true;
    }
    [[nodiscard]] bool final_committed() const noexcept {
        return final_committed_;
    }
    [[nodiscard]] bool busy() const noexcept {
        return scope_.has_pending_operations();
    }

private:
    [[nodiscard]] task<void> write_owned(http_status_code status, std::pmr::vector<http_header> fields);
    std::pmr::memory_resource* resource_;
    void* target_;
    write_type write_;
    bool final_committed_{};
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail
