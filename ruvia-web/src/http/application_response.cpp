#include "context/context_services.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"

namespace ruvia::detail {

task<std::optional<prepared_application_response>> prepare_application_response(
    const http_request& request, http_response_coding_policy policy, http_response& response,
    const http_server_options& options, const route_table& routes_value, request_memory& memory,
    const context_services& services, application_response_control control) {
    buffered_response_recovery recovery(control.recovery_mode_);
    for (;;) {
        if (control.terminal_stop_ != nullptr && control.terminal_stop_->stop_requested()) {
            co_return std::nullopt;
        }
        response.materialize_body();
        if (options.cors_.has_value()) {
            apply_cors_headers(request, response, *options.cors_);
        }
        auto compression = http_response_compression_result::make_not_applicable();
        if (options.compression_.has_value()) {
            if (const auto* selection = policy.selection()) {
                compression = co_await apply_response_compression_async(*selection,
                    request.known_method(), response, *options.compression_,
                    options.blocking_pool_, services.worker());
            }
        }
        if (control.terminal_stop_ != nullptr && control.terminal_stop_->stop_requested()) {
            co_return std::nullopt;
        }
        const auto step = recovery.advance(policy, request, response, compression);
        if (step.action_ == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), recovery.recovered()};
        }
        if (step.action_ == buffered_response_recovery_action::handle_error) {
            response = co_await routes_value.handle_error(request, memory, *step.error_, services);
        }
    }
}

}  // namespace ruvia::detail
