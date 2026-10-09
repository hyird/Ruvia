#pragma once

#include <memory_resource>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http1_server_request_parser.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_response.h"

#include "context/context_services.h"
#include "router/route_table.h"
#include "server/http1_request_sequence.h"
#include "server/http_response_compression.h"
#include "server/http_server_options.h"
#include "server/inbound_buffer_resource.h"

// What every HTTP/1 route dispatch needs from the session that owns the
// request: the transport, the worker's memory and scanner entry, the parsed
// head, the route table and services to run the handler with, the options, the
// response being built, and the keep-alive sequence. Each dispatcher takes this
// and adds only what its own kind of route needs.
//
// Passed by value, as its members were as separate arguments: the references
// cost nothing to copy and context_services is copied per dispatch either way.

namespace ruvia::detail {

template <typename stream_type>
struct http1_route_dispatch final {
    stream_type& stream_;
    worker_memory& memory_;
    ruvia::connection_scanner::entry_type& scanner_entry_;
    const http1_server_request_parse_state& parsed_;
    http_response_coding_selection response_coding_;
    http_response_coding_availability response_coding_availability_;
    const route_table& routes_;
    request_memory& request_memory_;
    context_services base_route_services_;
    const http_server_options& options_;
    http_response& response_;
    http1_request_sequence& request_sequence_;
    std::pmr::memory_resource* inbound_buffer_pool_{};
};

}  // namespace ruvia::detail
