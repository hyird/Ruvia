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

#include "body/http_request_body_facade.h"
#include "body/http_stream_body_reader.h"
#include "router/route_table.h"
#include "server/http1_route_dispatch.h"
#include "server/http_server_body_route_completion.h"
#include "server/http_server_options.h"
#include "server/request_body_limit.h"

namespace ruvia::detail {

template <typename stream_type>
task<http1_session_request_completion> dispatch_http_stream_body_route(http1_route_dispatch<stream_type> d,
    const http1_server_request_head_ready& request_head, const route_resolution& route_resolution_value,
    const std::pmr::string& read_buffer, std::size_t used_bytes, std::pmr::string& pipeline_stash) {
    const auto body_and_pipeline = http_body_and_pipeline(request_head, read_buffer, used_bytes);

    std::exception_ptr exception;
    std::optional<body_reader_binding<stream_body_reader<stream_type>>> body_reader;
    try {
        const auto* resolved = route_resolution_value.resolved();
        const auto route_limit =
            resolved != nullptr ? resolved->route().max_request_body_bytes() : std::size_t{0};
        body_reader.emplace(d.stream_, d.memory_.template allocator<char>(), body_and_pipeline,
            d.parsed_.body_plan_,
            request_body_byte_limit(request_body_mode::stream, d.options_.max_stream_body_bytes_,
                d.options_.max_buffered_body_bytes_, route_limit),
            d.scanner_entry_);
        d.response_ = co_await d.routes_.dispatch(d.parsed_.request_, route_resolution_value, d.request_memory_,
            d.base_route_services_.with_streaming_request_body(body_reader->facade()).with_request_trailers(body_reader->reader().trailers()));
    } catch (...) {
        exception = std::current_exception();
    }

    if (exception != nullptr) {
        auto exception_services = d.base_route_services_;
        if (body_reader) {
            exception_services = exception_services.with_streaming_request_body(body_reader->facade()).with_request_trailers(body_reader->reader().trailers());
        }
        co_return co_await complete_failed_http_body_route(d.scanner_entry_, exception, d.parsed_,
            d.routes_, d.request_memory_, exception_services, d.response_);
    }

    co_return complete_successful_http_body_route(d.scanner_entry_, d.response_, d.parsed_.connection_plan_,
        d.request_sequence_, body_reader->reader().consumption(), pipeline_stash,
        [&body_reader](std::pmr::string& stash) { body_reader->reader().take_pipeline(stash); });
}

}  // namespace ruvia::detail
