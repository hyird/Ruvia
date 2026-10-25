#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_response.h"

#include "body/http_lazy_buffered_body.h"
#include "body/http_request_body_facade.h"
#include "router/route_table.h"
#include "server/connection_scanner.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_server_response_state.h"

namespace ruvia::detail {

template <typename stream_type>
struct http_lazy_buffered_body_route_state final {
    std::optional<request_body_loader_binding<lazy_buffered_body<stream_type>>> body_;

    void emplace(stream_type& stream, std::pmr::polymorphic_allocator<char> worker_allocator,
        std::pmr::memory_resource* request_resource, std::string_view body_and_pipeline,
        http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
        connection_scanner::entry& scanner_entry, http_interim_response_output& interim_output) {
        body_.emplace(stream, worker_allocator, request_resource, body_and_pipeline, body_plan, body_limit_value,
            scanner_entry, interim_output);
    }

    [[nodiscard]] context_services with_loader(const context_services& base_services) noexcept {
        auto services = base_services;
        services.bind_lazy_request_body(body_->facade());
        services.bind_request_trailers(body_->loader().trailers());
        return services;
    }

    [[nodiscard]] const lazy_buffered_body<stream_type>& body() const noexcept {
        return body_->loader();
    }

    [[nodiscard]] lazy_buffered_body<stream_type>& body() noexcept {
        return body_->loader();
    }

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept {
        return body_->loader().consumption();
    }

    [[nodiscard]] body_pipeline_handover take_pipeline(
        std::pmr::string& stash, std::string_view additional_pipeline = {}) {
        return body_->loader().take_pipeline(stash, additional_pipeline);
    }
};

template <typename stream_type>
inline void prepare_http_lazy_buffered_body_route(http_lazy_buffered_body_route_state<stream_type>& state_value,
    http1_route_dispatch<stream_type> d, protocol_byte_limit body_limit_value, std::string_view body_and_pipeline) {
    auto* resource = &d.inbound_buffer_pool_;
    // Every HTTP/1 request's services carry its session's interim lane.
    state_value.emplace(d.stream_, std::pmr::polymorphic_allocator<char>(resource), resource,
        body_and_pipeline, d.parsed_.body_plan_, body_limit_value, d.scanner_entry_,
        *d.base_route_services_.interim_output());
}

template <typename stream_type>
[[nodiscard]] inline std::exception_ptr try_prepare_http_lazy_buffered_body_route(
    http_lazy_buffered_body_route_state<stream_type>& state_value, http1_route_dispatch<stream_type> d,
    protocol_byte_limit body_limit_value, std::string_view body_and_pipeline) {
    try {
        prepare_http_lazy_buffered_body_route(state_value, d, body_limit_value, body_and_pipeline);
        return nullptr;
    } catch (...) {
        return std::current_exception();
    }
}

// `request_bytes` is the connection's buffered input from the start of this
// request: its head, then whatever body and pipelined bytes followed it.
[[nodiscard]] inline std::string_view http_body_and_pipeline(
    const http1_server_request_head_ready& request_head, std::string_view request_bytes) noexcept {
    return request_bytes.substr(request_head.header_bytes());
}

inline task<http1_session_request_completion> complete_failed_http_route(
    connection_scanner::entry& scanner_entry, std::exception_ptr exception,
    const http1_server_request_parse_state& parsed_value, const route_table& routes_value,
    request_memory& request_memory_value, context_services exception_services, http_response& response) {
    response = co_await routes_value.handle_exception(
        parsed_value.request_, request_memory_value, exception, exception_services);
    scanner_entry.touch();
    co_return close_with_framework_response(response, parsed_value.connection_plan_);
}

// `request_bytes` is the connection's buffered input at dispatch, from the
// start of this request, the suffix included. A suffix still in that buffer
// becomes a consumed prefix; only bytes the body reader over-read go through
// `pipeline_stash`, request-scoped storage that outlives the returned
// completion. The read buffer is not touched here -- the response has not been
// written and the access log not recorded yet, and both still read views that
// borrow it.
template <typename take_pipeline_type>
[[nodiscard]] inline http1_session_request_completion complete_successful_http_body_route(
    connection_scanner::entry& scanner_entry, http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    http1_request_body_consumption body_consumption, std::size_t request_bytes,
    std::pmr::string& pipeline_stash, take_pipeline_type take_pipeline) {
    connection_plan = apply_request_body_consumption(connection_plan, body_consumption);
    connection_plan = request_sequence.complete_uncommitted_response(connection_plan);
    scanner_entry.touch();
    return commit_http1_route_response(response, connection_plan,
        [&](http1_request_connection_plan committed) {
            if (committed.disposition() == http1_close_policy::allow_reuse) {
                const auto pipeline = take_pipeline(pipeline_stash);
                if (!pipeline.stashed_.empty()) {
                    return http1_session_request_completion::make_buffered_pipeline_restore(
                        committed, pipeline.stashed_);
                }
                return http1_session_request_completion::make_buffered_unrestored(
                    committed, request_bytes - pipeline.read_buffer_suffix_bytes_);
            }
            return http1_session_request_completion::make_buffered_closing(committed);
        });
}

template <typename body_owner_type>
[[nodiscard]] inline http1_session_request_completion complete_successful_http_body_route(
    connection_scanner::entry& scanner_entry, http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    std::size_t request_bytes, std::pmr::string& pipeline_stash, body_owner_type& body_owner) {
    return complete_successful_http_body_route(scanner_entry, response, connection_plan, request_sequence,
        body_owner.consumption(), request_bytes, pipeline_stash,
        [&body_owner](std::pmr::string& stash) { return body_owner.take_pipeline(stash); });
}

}  // namespace ruvia::detail
