#pragma once

#include <memory_resource>
#include <stdexcept>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/http/HttpInterimResponse.h"

namespace ruvia::detail {

// One request's typed interim output lane. The protocol driver and its worker
// resource outlive this object; operations own their fields until completion.
class HttpInterimResponseOutput final {
public:
    using Write = Task<void> (*)(void*, const HttpInterimResponseHead&);
    HttpInterimResponseOutput(std::pmr::memory_resource* resource, void* target, Write write) noexcept
        : resource_(resource),
          target_(target),
          write_(write) {}

    [[nodiscard]] ScopedOperation<void> inform(const HttpInterimResponseHead& response);
    void commitFinal() {
        if (scope_.has_pending_operations()) {
            throw std::logic_error("interim response output is still active");
        }
        finalCommitted_ = true;
    }
    [[nodiscard]] bool finalCommitted() const noexcept {
        return finalCommitted_;
    }
    [[nodiscard]] bool busy() const noexcept {
        return scope_.has_pending_operations();
    }

private:
    [[nodiscard]] Task<void> writeOwned(HttpStatusCode status, std::pmr::vector<HttpHeader> fields);
    std::pmr::memory_resource* resource_;
    void* target_;
    Write write_;
    bool finalCommitted_{};
    ::ruvia::operation_scope scope_;
};
}  // namespace ruvia::detail
