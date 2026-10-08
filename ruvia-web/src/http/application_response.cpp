#include "context/ContextServices.h"
#include "router/RouteTable.h"
#include "server/HttpBufferedResponse.h"

namespace ruvia::detail {

Task<std::optional<prepared_application_response>> prepare_application_response(
    const HttpRequest& request, HttpResponseCodingPolicy policy, HttpResponse& response,
    const HttpServerOptions& options, const RouteTable& routes, RequestMemory& memory,
    const ContextServices& services, application_response_control control) {
    buffered_response_recovery recovery(control.recovery_mode);
    for (;;) {
        if (control.terminal_stop != nullptr && control.terminal_stop->stopRequested()) {
            co_return std::nullopt;
        }
        response.materializeBody();
        if (options.cors.has_value()) {
            applyCorsHeaders(request, response, *options.cors);
        }
        auto compression = HttpResponseCompressionResult::makeNotApplicable();
        if (options.compression.has_value()) {
            if (const auto* selection = policy.selection()) {
                compression = co_await applyResponseCompressionAsync(*selection,
                    request.knownMethod(), response, *options.compression,
                    options.blockingPool, services.worker());
            }
        }
        if (control.terminal_stop != nullptr && control.terminal_stop->stopRequested()) {
            co_return std::nullopt;
        }
        const auto step = recovery.advance(policy, request, response, compression);
        if (step.action == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{
                planBufferedHttpResponseWrite(request.knownMethod(), response), recovery.recovered()};
        }
        if (step.action == buffered_response_recovery_action::handle_error) {
            response = co_await routes.handleError(request, memory, *step.error, services);
        }
    }
}

}  // namespace ruvia::detail
