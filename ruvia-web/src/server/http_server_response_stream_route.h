#pragma once

#include <cstddef>
#include <stdexcept>
#include <utility>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_response.h"

#include "router/route_table.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_buffered_response.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_response_stream_sink.h"
#include "server/http_server_body_route_completion.h"
#include "server/http_server_response_state.h"

namespace ruvia::detail {

template <typename stream_type>
task<http1_session_request_completion> dispatch_http_response_stream_route(http1_route_dispatch<stream_type> d,
    response_head_buffer_type& response_head, const http1_server_request_head_ready& request_head,
    const resolved_route& resolved, std::string_view body_and_pipeline, protocol_byte_limit body_limit_value) {
    http_lazy_buffered_body_route_state<stream_type> body_state;
    std::exception_ptr body_setup_failure;
    try {
        prepare_http_lazy_buffered_body_route(body_state, d, body_limit_value, body_and_pipeline);
    } catch (...) {
        body_setup_failure = std::current_exception();
    }
    if (body_setup_failure != nullptr) {
        co_return co_await complete_failed_http_body_route(d.scanner_entry_, body_setup_failure, d.parsed_,
            d.routes_, d.request_memory_, d.base_route_services_, d.response_);
    }
    d.base_route_services_ = body_state.with_loader(d.base_route_services_);
    const auto stream_plan =
        http1_plan_response_stream(d.parsed_, d.request_sequence_.next_response_close_policy());
    auto connection_plan = stream_plan.request_connection_plan();
    using response_sink_type = response_stream_sink<stream_type, ruvia::connection_scanner::entry_type>;
    const auto& route = resolved.route();
    const auto& endpoint = *route.endpoint().response_stream();
    response_sink_type response_sink(d.stream_, d.memory_, response_head, d.scanner_entry_,
        d.base_route_services_.worker(), endpoint.kind(), stream_plan, d.response_coding_,
        d.response_coding_availability_);

    d.scanner_entry_.set_phase(ruvia::connection_scanner::phase_type::writing);
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
        connection_plan = require_http1_final_response_commit(
            d.response_, stream_plan.request_connection_plan().require_close());
        co_return http1_session_request_completion::make_buffered_closing(connection_plan);
    }
    if (auto* route_response = result_value.route_response()) {
        d.response_ = std::move(*route_response).take_response();
        d.scanner_entry_.touch();
        connection_plan =
            finalize_buffered_route_response(d.response_, connection_plan, d.request_sequence_);
        co_return http1_session_request_completion::make_buffered_unrestored(
            connection_plan, request_head.header_bytes());
    }

    const auto committed_status = result_value.committed_status();
    if (!committed_status.has_value()) {
        throw std::logic_error("d.response d.stream dispatch returned no H1 terminal alternative");
    }

    connection_plan = response_sink.connection_plan();
    if (result_value.completed() != nullptr) {
        d.request_sequence_.complete_committed_response(connection_plan);
    } else {
        if (const auto* failed = result_value.failed_after_commit()) {
            // The client only learns of this as a truncated response; this is
            // the one place the reason for the truncation still exists.
            d.options_.connection_failure_.invoke(
                d.base_route_services_.get_conn_info().remote().address(), failed->exception());
        }
        connection_plan = connection_plan.require_close();
    }
    co_return http1_session_request_completion::make_committed_stream(
        connection_plan, *committed_status, request_head.header_bytes());
}

}  // namespace ruvia::detail
