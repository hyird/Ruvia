#include <string_view>

#include "context/context_services.h"
#include "http/http_cors.h"
#include "router/route_table.h"
#include "server/http_alternative_service.h"
#include "server/http_buffered_response.h"
#include "server/http_request_admission.h"

namespace ruvia::detail {
namespace {

// One synchronous representation pass: apply CORS and the listener's Alt-Svc,
// and run the compression policy up to its offload boundary, which only a
// worker with a blocking pool can cross.
[[nodiscard]] http_buffered_compression_step prepare_representation(const http_request& request,
    const http_response_coding_policy& policy, std::string_view alternative_service, http_response& response,
    const http_server_options& options, const worker_handle& worker) {
    if (options.cors_.has_value()) {
        apply_cors_headers(request, response, *options.cors_);
    }
    apply_alternative_service(response, alternative_service);
    if (options.compression_.has_value()) {
        if (const auto* selection = policy.selection()) {
            return apply_response_compression(*selection, request.known_method(), response, *options.compression_,
                *options.codecs_, worker.has_blocking_pool());
        }
    }
    return {http_response_compression_status::not_applicable, false};
}

}  // namespace

http_response_preparation http_response_tail::prepare(
    http_response& response, const http_server_options& options, const worker_handle& worker) const {
    const auto& request = *request_;
    const auto first_pass =
        prepare_representation(request, coding_policy_, alternative_service_, response, options, worker);
    if (!first_pass.offload_) {
        auto policy = coding_policy_;
        buffered_response_recovery recovery;
        const auto step = recovery.advance(policy, request, response, first_pass.status_);
        if (step.action_ == buffered_response_recovery_action::ready) {
            return http_response_preparation(prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), false});
        }
        return http_response_preparation(first_pass, policy, recovery, step);
    }
    return http_response_preparation(first_pass, coding_policy_);
}

task<prepared_application_response> http_response_tail::finish_prepare(const http_response_preparation& preparation,
    http_response& response, const http_server_options& options, const route_table& routes, request_memory& memory,
    const context_services& services) const {
    const auto& request = *request_;
    auto policy = preparation.policy_;
    auto recovery = preparation.recovery_;
    auto pass = preparation.first_pass_;
    auto pending_step = preparation.pending_step_;
    for (;;) {
        buffered_response_recovery_step step;
        if (pending_step.has_value()) {
            step = *pending_step;
            pending_step.reset();
        } else {
            auto compression = pass.status_;
            if (pass.offload_) {
                compression = co_await compress_response_offloaded(
                    *policy.selection(), response, services.worker(), services.get_stop_token());
            }
            step = recovery.advance(policy, request, response, compression);
        }
        if (step.action_ == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), recovery.recovered()};
        }
        if (step.action_ == buffered_response_recovery_action::handle_error) {
            response = co_await routes.handle_error(request, memory, *step.error_, services);
        }
        pass = prepare_representation(request, policy, alternative_service_, response, options, services.worker());
    }
}

}  // namespace ruvia::detail
