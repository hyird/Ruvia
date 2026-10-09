#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_response.h"

#include "body/http_lazy_buffered_body.h"
#include "body/http_request_body_facade.h"
#include "http/request_body_loader.h"
#include "router/route_table.h"
#include "server/http1_route_dispatch.h"
#include "server/http1_session_request_completion.h"
#include "server/http_server_options.h"
#include "server/http_server_response_state.h"

namespace ruvia::detail {

template <typename stream_type>
struct http_lazy_buffered_body_route_state final {
    std::optional<request_body_loader_binding<lazy_buffered_body<stream_type>>> body_;

    void emplace(stream_type& stream, std::pmr::polymorphic_allocator<char> worker_allocator,
        std::pmr::memory_resource* request_resource, std::string_view body_and_pipeline,
        http1_request_body_plan body_plan, protocol_byte_limit body_limit_value,
        ruvia::connection_scanner::entry_type& scanner_entry) {
        body_.emplace(stream, worker_allocator, request_resource, body_and_pipeline, body_plan, body_limit_value,
            scanner_entry);
    }

    [[nodiscard]] context_services with_loader(context_services services) noexcept {
        return services.with_lazy_request_body(body_->facade()).with_request_trailers(body_->loader().trailers());
    }

    [[nodiscard]] http1_request_body_consumption consumption() const noexcept {
        return body_->loader().consumption();
    }

    void take_pipeline(std::pmr::string& stash) {
        body_->loader().take_pipeline(stash);
    }
};

template <typename stream_type>
inline void prepare_http_lazy_buffered_body_route(http_lazy_buffered_body_route_state<stream_type>& state_value,
    http1_route_dispatch<stream_type> d, protocol_byte_limit body_limit_value, std::string_view body_and_pipeline) {
    auto* resource = d.inbound_buffer_pool_ != nullptr ? d.inbound_buffer_pool_ : d.memory_.resource();
    state_value.emplace(d.stream_, std::pmr::polymorphic_allocator<char>(resource), resource,
        body_and_pipeline, d.parsed_.body_plan_, body_limit_value, d.scanner_entry_);
}

[[nodiscard]] inline std::string_view http_body_and_pipeline(
    const http1_server_request_head_ready& request_head, const std::pmr::string& read_buffer,
    std::size_t used_bytes) noexcept {
    const auto header_bytes = request_head.header_bytes();
    return std::string_view(read_buffer.data() + header_bytes, used_bytes - header_bytes);
}

inline task<http1_session_request_completion> complete_failed_http_body_route(
    ruvia::connection_scanner::entry_type& scanner_entry, std::exception_ptr exception,
    const http1_server_request_parse_state& parsed_value, const route_table& routes_value,
    request_memory& request_memory_value, context_services exception_services, http_response& response) {
    response = co_await routes_value.handle_exception(
        parsed_value.request_, request_memory_value, exception, exception_services);
    materialize_response_body(response);
    scanner_entry.touch();
    const auto connection_plan =
        require_http1_final_response_commit(response, parsed_value.connection_plan_.require_close());
    co_return http1_session_request_completion::make_buffered_closing(connection_plan);
}

// `pipeline_stash` must be request-scoped storage that outlives the returned
// completion: it carries the next pipelined request until the session installs
// it. The read buffer is not touched here -- the response has not been written
// and the access log not recorded yet, and both still read views that borrow it.
template <typename take_pipeline_type>
[[nodiscard]] inline http1_session_request_completion complete_successful_http_body_route(
    ruvia::connection_scanner::entry_type& scanner_entry, http_response& response,
    http1_request_connection_plan connection_plan, http1_request_sequence& request_sequence,
    http1_request_body_consumption body_consumption, std::pmr::string& pipeline_stash,
    take_pipeline_type take_pipeline) {
    connection_plan =
        finalize_body_route_response(response, connection_plan, request_sequence, body_consumption);
    if (connection_plan.disposition() == http1_close_policy::allow_reuse) {
        take_pipeline(pipeline_stash);
        scanner_entry.touch();
        return http1_session_request_completion::make_buffered_pipeline_restore(
            connection_plan, std::string_view(pipeline_stash));
    }
    scanner_entry.touch();
    return http1_session_request_completion::make_buffered_closing(connection_plan);
}

}  // namespace ruvia::detail
