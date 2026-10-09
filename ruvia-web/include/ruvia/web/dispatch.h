#pragma once

#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/operation_options.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

class context;

// Inputs are copied before dispatch() returns its lazy scoped operation.
// Only origin-form targets and buffered request/response bodies are accepted.
struct dispatch_options final {
    std::string_view method_{};
    std::string_view target_{};
    std::span<const http_header_view> headers_{};
    std::string_view body_{};
    operation_options operation_{};
};

// Owns its bytes in the calling worker's reclaimable pool, independently of
// the child request arena. It must not outlive that worker.
class dispatch_response final {
public:
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::string_view body() const& noexcept {
        return body_;
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    std::optional<std::string_view> header(std::string_view) const&& = delete;

private:
    friend class context;
    explicit dispatch_response(std::pmr::memory_resource* resource)
        : headers_(resource),
          body_(resource) {}
    http_status_code status_{http_status::ok};
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> headers_;
    std::pmr::string body_;
};

}  // namespace ruvia
