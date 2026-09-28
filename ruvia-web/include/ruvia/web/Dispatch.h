#pragma once

#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia {

class Context;

// Inputs are copied before dispatch() returns its lazy scoped operation.
// Only origin-form targets and buffered request/response bodies are accepted.
struct DispatchOptions final {
    std::string_view method{};
    std::string_view target{};
    std::span<const HttpHeaderView> headers{};
    std::string_view body{};
    OperationOptions operation{};
};

// Owns its bytes in the calling worker's reclaimable pool, independently of
// the child request arena. It must not outlive that worker.
class DispatchResponse final {
public:
    [[nodiscard]] HttpStatusCode status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::string_view body() const& noexcept {
        return body_;
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    std::optional<std::string_view> header(std::string_view) const&& = delete;

private:
    friend class Context;
    explicit DispatchResponse(std::pmr::memory_resource* resource)
        : headers_(resource),
          body_(resource) {}
    HttpStatusCode status_{http_status::kOk};
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> headers_;
    std::pmr::string body_;
};

}  // namespace ruvia
