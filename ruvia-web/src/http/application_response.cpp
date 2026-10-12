#include "context/context_services.h"
#include "http/http_cors.h"
#include "router/route_table.h"
#include "server/http_buffered_response.h"
#include "server/http_request_admission.h"

namespace ruvia::detail {
namespace {

// One synchronous representation pass: own borrowed body bytes, apply CORS,
// and run the compression policy up to its offload boundary.
[[nodiscard]] http_buffered_compression_step prepare_representation(const http_request& request,
    const http_response_coding_policy& policy, http_response& response, const http_server_options& options) {
    response.materialize_body();
    if (options.cors_.has_value()) {
        apply_cors_headers(request, response, *options.cors_);
    }
    if (options.compression_.has_value()) {
        if (const auto* selection = policy.selection()) {
            return apply_response_compression(*selection, request.known_method(), response, *options.compression_,
                options.blocking_pool_ != nullptr);
        }
    }
    return {http_response_compression_result::make_not_applicable(), false};
}

}  // namespace

http_response_preparation http_response_tail::prepare(
    http_response& response, const http_server_options& options) const {
    const auto& request = *request_;
    const auto first_pass = prepare_representation(request, coding_policy_, response, options);
    if (!first_pass.offload_) {
        auto policy = coding_policy_;
        buffered_response_recovery recovery;
        if (recovery.advance(policy, request, response, first_pass.result_).action_ ==
            buffered_response_recovery_action::ready) {
            return http_response_preparation(prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), false});
        }
    }
    return http_response_preparation(first_pass);
}

task<prepared_application_response> http_response_tail::finish_prepare(const http_response_preparation& preparation,
    http_response& response, const http_server_options& options, const route_table& routes, request_memory& memory,
    const context_services& services) const {
    const auto& request = *request_;
    auto policy = coding_policy_;
    buffered_response_recovery recovery;
    // Resume after prepare()'s pass over this same response. Recovery is a pure
    // function of policy, request, response and compression outcome, so a fresh
    // recovery state re-derives exactly the step that pass stopped at.
    auto pass = preparation.first_pass_;
    for (;;) {
        auto compression = pass.result_;
        if (pass.offload_) {
            compression = co_await compress_response_offloaded(
                policy.selection()->coding(), response, *options.blocking_pool_, services.worker());
        }
        const auto step = recovery.advance(policy, request, response, compression);
        if (step.action_ == buffered_response_recovery_action::ready) {
            co_return prepared_application_response{
                plan_buffered_http_response_write(request.known_method(), response), recovery.recovered()};
        }
        if (step.action_ == buffered_response_recovery_action::handle_error) {
            response = co_await routes.handle_error(request, memory, *step.error_, services);
        }
        pass = prepare_representation(request, policy, response, options);
    }
}

}  // namespace ruvia::detail
