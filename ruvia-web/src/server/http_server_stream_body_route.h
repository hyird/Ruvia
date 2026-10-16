#pragma once

#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/task.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/protocol_byte_limit.h"

#include "body/http_request_body_facade.h"
#include "body/http_stream_body_reader.h"
#include "router/route_table.h"
#include "server/http1_route_dispatch.h"
#include "server/http_server_body_route_completion.h"

namespace ruvia::detail {

template <typename stream_type>
task<http1_session_request_completion> dispatch_http_stream_body_route(http1_route_dispatch<stream_type> d,
    const http1_server_request_head_ready& request_head, const route_resolution& route_resolution_value,
    protocol_byte_limit body_limit, std::string_view request_bytes, std::pmr::string& pipeline_stash) {
    const auto body_and_pipeline = http_body_and_pipeline(request_head, request_bytes);

    std::exception_ptr exception;
    std::optional<body_reader_binding<stream_body_reader<stream_type>>> body_reader;
    try {
        // The reader's buffers are inbound memory: they count against the
        // connection's and the worker's inbound budgets.
        body_reader.emplace(d.stream_, std::pmr::polymorphic_allocator<char>(&d.inbound_buffer_pool_),
            body_and_pipeline, d.parsed_.body_plan_, body_limit, d.scanner_entry_,
            *d.base_route_services_.interim_output());
        auto services = d.base_route_services_;
        services.bind_streaming_request_body(body_reader->facade());
        services.bind_request_trailers(body_reader->reader().trailers());
        co_await d.routes_.dispatch_buffered_response(d.parsed_.request_, route_resolution_value,
            d.request_memory_, document_root_binding::none(), services,
            static_file_selection_mode::identity_only, d.response_);
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exception_services = d.base_route_services_;
        if (body_reader) {
            exception_services.bind_streaming_request_body(body_reader->facade());
            exception_services.bind_request_trailers(body_reader->reader().trailers());
        }
        co_return co_await complete_failed_http_route(d.scanner_entry_, exception, d.parsed_,
            d.routes_, d.request_memory_, exception_services, d.response_);
    }

    co_return complete_successful_http_body_route(d.scanner_entry_, d.response_, d.parsed_.connection_plan_,
        d.request_sequence_, body_reader->reader().consumption(), request_bytes.size(), pipeline_stash,
        [&body_reader](std::pmr::string& stash) { return body_reader->reader().take_pipeline(stash); });
}

}  // namespace ruvia::detail
