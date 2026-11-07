#pragma once

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/stop_token.h"
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
    const auto body_setup_failure =
        try_prepare_http_lazy_buffered_body_route(body_state, d, body_limit_value, body_and_pipeline);
    if (body_setup_failure != nullptr) {
        co_return co_await complete_failed_http_route(d.scanner_entry_, body_setup_failure, d.parsed_,
            d.routes_, d.request_memory_, d.base_route_services_, d.response_);
    }
    const auto close_policy = d.request_sequence_.next_response_close_policy();
    // A body that arrived in full with the head does not cost the stream its
    // reuse even if the handler never reads it: completion consumes it
    // without I/O.
    const auto unread_body_consumption =
        buffered_request_body_bytes(d.parsed_.body_plan_, body_and_pipeline, body_limit_value)
            ? http1_request_body_consumption::complete
            : http1_request_body_consumption::incomplete;
    const auto stream_plan = http1_plan_response_stream(d.parsed_, close_policy, unread_body_consumption);
    using response_sink_type = response_stream_sink<stream_type>;
    const auto& route = resolved.route();
    const auto& endpoint = *route.endpoint().response_stream();
    body_reader stream_body_facade = make_body_reader_facade(body_state.body_->loader().reader());
    if (endpoint.request_body_mode() == request_body_mode::stream) {
        d.base_route_services_.bind_streaming_request_body(stream_body_facade);
        d.base_route_services_.bind_request_trailers(body_state.body().trailers());
    } else {
        d.base_route_services_ = body_state.with_loader(d.base_route_services_);
    }
    // The client closing the connection while the handler waits, or a failed
    // write, stops the request: context::stop_token() combines it with the
    // worker's stop, so every framework wait of the handler observes it.
    stop_source peer_stop;
    const stop_group request_stop(d.base_route_services_.get_stop_token(), peer_stop.token());
    const auto request_stop_token = request_stop.token();
    d.base_route_services_.bind_stop_token(request_stop_token);
    response_sink_type response_sink(d.stream_, d.memory_, *d.options_.codecs_, response_head, d.scanner_entry_,
        d.base_route_services_.worker(), d.parsed_.request_, d.options_.cors_ ? &*d.options_.cors_ : nullptr,
        stream_plan,
        http1_plan_response_stream(d.parsed_, close_policy, http1_request_body_consumption::complete),
        body_state.body(), d.response_coding_, d.options_.compression_ ? &*d.options_.compression_ : nullptr,
        d.inbound_buffer_pool_, peer_stop, d.request_pipelined_);

    // The handler stays in the session's idle deadman phase; the sink times
    // its own writes.
    std::optional<response_stream_dispatch_result> dispatched;
    std::exception_ptr escaped_failure;
    try {
        // This request arrived before the peer ended its request direction:
        // that end is no abandonment of it, and the transport has no input left.
        if (d.request_sequence_.request_input_ended()) {
            response_sink.retire_peer_monitor();
        }
        response_sink.arm_peer_monitor();
        dispatched.emplace(co_await dispatch_response_stream_with(response_sink, d.routes_, d.parsed_.request_,
            resolved, d.request_memory_, d.base_route_services_,
            [&response_sink] { return response_sink.peer_aborted(); }));
    } catch (...) {
        escaped_failure = std::current_exception();
    }
    // Even error recovery can throw. Drain the reader before the body, sink,
    // stream, or their borrowed request storage may be destroyed or reused.
    response_sink.retire_peer_monitor();
    if (response_sink.peer_eof_monitoring()) {
        try {
            co_await response_sink.finish_peer_monitor();
        } catch (...) {
            const auto monitor_failure = std::current_exception();
            const auto* reported_failure = dispatched ? dispatched->failed_after_commit() : nullptr;
            if (escaped_failure != nullptr || reported_failure != nullptr) {
                const auto prior_failure = escaped_failure != nullptr
                                               ? escaped_failure
                                               : reported_failure->exception();
                if (monitor_failure != prior_failure) {
                    d.options_.connection_failure_.invoke(
                        d.base_route_services_.get_conn_info().remote().address(), monitor_failure);
                }
            } else {
                escaped_failure = monitor_failure;
            }
        }
    }
    if (response_sink.request_input_ended()) {
        d.request_sequence_.mark_request_input_ended();
    }
    if (escaped_failure != nullptr) {
        if (!response_sink.committed()) {
            std::rethrow_exception(escaped_failure);
        }
        dispatched.emplace(response_stream_dispatch_result::make_failed_after_commit(
            committed_response_stream_status(response_sink), escaped_failure));
    }
    auto& result_value = *dispatched;

    if (const auto* failed = result_value.failed_after_commit()) {
        d.options_.connection_failure_.invoke(
            d.base_route_services_.get_conn_info().remote().address(), failed->exception());
        co_return http1_session_request_completion::make_truncated_stream(
            response_sink.connection_plan(), failed->status());
    }
    // The cancelled read can deliver a peer end after dispatch already
    // returned. Classify only after joining, when its final result is known.
    if (response_sink.peer_aborted()) {
        if (!response_sink.committed()) {
            throw inbound_transport_error(asio::error::eof, "HTTP response stream peer disconnected before commit");
        }
        co_return http1_session_request_completion::make_truncated_stream(
            response_sink.connection_plan(), committed_response_stream_status(response_sink));
    }

    if (auto* recovered = result_value.recovered_failure()) {
        d.response_ = std::move(*recovered).take_response();
        d.scanner_entry_.touch();
        co_return close_with_framework_response(d.response_, stream_plan.request_connection_plan());
    }
    auto& body_reader_value = body_state.body().reader();
    body_reader_value.consume_buffered_body();
    if (auto* route_response = result_value.route_response()) {
        d.response_ = std::move(*route_response).take_response();
        d.scanner_entry_.touch();
        co_return complete_successful_http_body_route(d.scanner_entry_, d.response_, d.parsed_.connection_plan_,
            d.request_sequence_, body_reader_value.consumption(), request_bytes.size(), pipeline_stash,
            [&body_reader_value, &response_sink](std::pmr::string& stash) {
                return body_reader_value.take_pipeline(stash, response_sink.monitored_pipeline());
            });
    }

    // Only the completed committed alternative remains.
    const auto committed_status = result_value.committed_status();

    auto connection_plan = response_sink.connection_plan();
    d.request_sequence_.complete_committed_response(connection_plan);
    // A reusable head promised a body read in full or buffered in full; a
    // read the handler failed after commit breaks that promise, so the
    // connection closes after the stream instead.
    if (body_reader_value.consumption() != http1_request_body_consumption::complete) {
        connection_plan = connection_plan.require_close();
    }
    if (connection_plan.disposition() != http1_close_policy::allow_reuse) {
        co_return http1_session_request_completion::make_committed_stream(
            connection_plan, *committed_status, request_head.header_bytes());
    }
    // What followed the body is the next request, in the read buffer or in
    // the reader's own over-read.
    const auto pipeline = body_reader_value.take_pipeline(pipeline_stash, response_sink.monitored_pipeline());
    if (!pipeline.stashed_.empty()) {
        co_return http1_session_request_completion::make_committed_stream_pipeline_restore(
            connection_plan, *committed_status, pipeline.stashed_);
    }
    co_return http1_session_request_completion::make_committed_stream(
        connection_plan, *committed_status, request_bytes.size() - pipeline.read_buffer_suffix_bytes_);
}

}  // namespace ruvia::detail
