#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http1_server_semantics.h"

#include "router/route_table.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_response_stream_sink.h"
#include "server/http_server_body_route_completion.h"
#include "server/http_server_response_state.h"

namespace ruvia::detail {

// `request_bytes` is the connection's buffered input at dispatch, from the
// start of this request, the suffix included; `pipeline_stash` is
// request-scoped storage for a pipelined request the body reader over-read.
template <typename stream_type>
task<http1_session_request_completion> dispatch_http_response_stream_route(http1_route_dispatch<stream_type> d,
    http_response_head_buffer& response_head, const http1_server_request_head_ready& request_head,
    const resolved_route& resolved, std::string_view request_bytes, protocol_byte_limit body_limit_value,
    std::pmr::string& pipeline_stash) {
    const auto body_and_pipeline = http_body_and_pipeline(request_head, request_bytes);
    http_lazy_buffered_body_route_state<stream_type> body_state;
    std::exception_ptr body_setup_failure;
    try {
        prepare_http_lazy_buffered_body_route(body_state, d, body_limit_value, body_and_pipeline);
    } catch (...) {
        body_setup_failure = std::current_exception();
    }
    if (body_setup_failure != nullptr) {
        co_return co_await complete_failed_http_route(d.scanner_entry_, body_setup_failure, d.parsed_,
            d.routes_, d.request_memory_, d.base_route_services_, d.response_);
    }
    d.base_route_services_ = body_state.with_loader(d.base_route_services_);
    const auto close_policy = d.request_sequence_.next_response_close_policy();
    const auto stream_plan = http1_plan_response_stream(d.parsed_, close_policy);
    auto connection_plan = stream_plan.request_connection_plan();
    using response_sink_type = response_stream_sink<stream_type>;
    const auto& route = resolved.route();
    const auto& endpoint = *route.endpoint().response_stream();
    response_sink_type response_sink(d.stream_, d.memory_, *d.options_.codecs_, response_head, d.scanner_entry_,
        d.base_route_services_.worker(), d.options_.cors_ ? &*d.options_.cors_ : nullptr,
        endpoint.kind(), stream_plan, http1_plan_consumed_response_stream(d.parsed_, close_policy),
        body_state.body(), d.response_coding_, d.response_coding_availability_);

    // The handler stays in the session's idle deadman phase; the sink times
    // its own writes.
    auto result_value = co_await dispatch_response_stream_with(response_sink, d.routes_, d.parsed_.request_,
        resolved, d.request_memory_, d.base_route_services_,
        /*peer_aborted=*/[]() noexcept { return false; });

    if (result_value.peer_aborted_before_commit() != nullptr) {
        throw std::logic_error(
            "HTTP/1 d.response d.stream reported an impossible peer-abort predicate");
    }
    if (auto* recovered = result_value.recovered_failure()) {
        d.response_ = std::move(*recovered).take_response();
        d.scanner_entry_.touch();
        co_return close_with_framework_response(d.response_, stream_plan.request_connection_plan());
    }
    if (auto* route_response = result_value.route_response()) {
        d.response_ = std::move(*route_response).take_response();
        d.scanner_entry_.touch();
        co_return finalize_buffered_route_response(
            d.response_, connection_plan, d.request_sequence_, request_head.header_bytes());
    }

    // Only the committed alternatives remain, and both carry a status.
    const auto committed_status = result_value.committed_status();

    connection_plan = response_sink.connection_plan();
    if (const auto* failed = result_value.failed_after_commit()) {
        // The client only learns of this as a truncated response; this is
        // the one place the reason for the truncation still exists.
        d.options_.connection_failure_.invoke(
            d.base_route_services_.get_conn_info().remote().address(), failed->exception());
        co_return http1_session_request_completion::make_truncated_stream(connection_plan, *committed_status);
    }
    d.request_sequence_.complete_committed_response(connection_plan);
    if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
        co_return http1_session_request_completion::make_committed_stream(
            connection_plan, *committed_status, request_head.header_bytes());
    }
    // The stream committed a reusable plan, so the request body was read in
    // full; what followed it is the next request, in the read buffer or in
    // the reader's own over-read.
    const auto pipeline = body_state.take_pipeline(pipeline_stash);
    if (!pipeline.stashed_.empty()) {
        co_return http1_session_request_completion::make_committed_stream_pipeline_restore(
            connection_plan, *committed_status, pipeline.stashed_);
    }
    co_return http1_session_request_completion::make_committed_stream(
        connection_plan, *committed_status, request_bytes.size() - pipeline.read_buffer_suffix_bytes_);
}

}  // namespace ruvia::detail
