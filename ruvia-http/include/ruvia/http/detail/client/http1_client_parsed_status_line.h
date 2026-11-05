#pragma once

#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

// A validated HTTP/1 status line, kept by http1_client_response_parser from
// the arrival of its line feed until the head it starts completes.
struct http1_client_parsed_status_line final {
    http_received_status received_status_;
    http_protocol_version protocol_version_;
};

}  // namespace ruvia::detail
