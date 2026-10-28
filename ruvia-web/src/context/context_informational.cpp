#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "context/http_interim_response_output.h"
#include "util/operation_lane_lease.h"

namespace ruvia {
task<void> context::inform(const http_interim_response_head& response) {
    if (services().interim_output() == nullptr) {
        throw std::logic_error("this dispatch has no transport response output");
    }
    return services().interim_output()->inform(response);
}
}  // namespace ruvia

namespace ruvia::detail {
task<void> http_interim_response_output::inform(const http_interim_response_head& response) {
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    std::pmr::vector<http_header> owned(resource_);
    owned.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        owned.emplace_back(field.name(), field.value(), resource_);
    }
    return ::ruvia::make_scoped_operation(scope_, write_owned(response.status(), std::move(owned)));
}
task<void> http_interim_response_output::write_owned(http_status_code status, std::pmr::vector<http_header> fields_value) {
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    if (final_committed_) {
        throw std::logic_error("the final response has already started");
    }
    static constexpr std::size_t inline_capacity = 16;
    std::array<http_header_view, inline_capacity> inline_views{};
    std::pmr::vector<http_header_view> heap_views(resource_);
    std::span<const http_header_view> active_views;
    if (fields_value.size() <= inline_capacity) {
        for (std::size_t i = 0; i < fields_value.size(); ++i) {
            inline_views[i] = http_header_view(fields_value[i].name(), fields_value[i].value());
        }
        active_views = std::span<const http_header_view>(inline_views.data(), fields_value.size());
    } else {
        heap_views.reserve(fields_value.size());
        for (const auto& field : fields_value) {
            heap_views.emplace_back(field.name(), field.value());
        }
        active_views = heap_views;
    }
    const http_interim_response_head head(status, active_views);
    co_await write_(target_, head);
}
task<void> http_interim_response_output::send_continue() {
    if (final_committed_) {
        co_return;
    }
    auto lease = claim_operation_lane(output_active_, "interim response output is already active");
    co_await write_(target_, http_interim_response_head(http_status::continue_value));
}
}  // namespace ruvia::detail
