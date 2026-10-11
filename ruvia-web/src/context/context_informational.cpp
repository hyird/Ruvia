#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "context/http_interim_response_output.h"
#include "util/operation_lane_lease.h"

namespace ruvia {
scoped_operation<void> context::inform(const http_interim_response_head& response) {
    if (services().interim_output() == nullptr) {
        throw std::logic_error("this dispatch has no transport response output");
    }
    return services().interim_output()->inform(response);
}
}  // namespace ruvia

namespace ruvia::detail {
scoped_operation<void> http_interim_response_output::inform(const http_interim_response_head& response) {
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    std::pmr::vector<http_header> owned(resource_);
    owned.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        owned.push_back(http_header::copy_of(field.name(), field.value(), resource_));
    }
    return ::ruvia::make_scoped_operation(scope_, write_owned(response.status(), std::move(owned)));
}
task<void> http_interim_response_output::write_owned(http_status_code status, std::pmr::vector<http_header> fields_value) {
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    std::pmr::vector<http_header_view> views(resource_);
    views.reserve(fields_value.size());
    for (const auto& field : fields_value) {
        views.emplace_back(field.name(), field.value());
    }
    const http_interim_response_head head(status, std::span<const http_header_view>(views));
    co_await write_(target_, head);
}
}  // namespace ruvia::detail
