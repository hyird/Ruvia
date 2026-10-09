#pragma once

#include "ruvia/http/http1_response_head_plan.h"

#include "server/http_response_head_buffer.h"

namespace ruvia {

class http_response;

namespace detail {

void append_response_head(
    const http_response& response, response_head_buffer_type& head, const http1_response_head_plan& plan);

}  // namespace detail
}  // namespace ruvia
