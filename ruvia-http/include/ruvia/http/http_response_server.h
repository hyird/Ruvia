#pragma once

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.
#include <memory_resource>
#include <span>
#include <string>
#include <utility>

#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/server/http_response_write_plan.h"
#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_head_buffer.h"
#include "ruvia/http/http_response_trailer_section.h"

namespace ruvia {

// Serialize a finalized HTTP/1 response head into caller-owned reusable scratch.
// The plan must describe the same response representation.
void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head,
    const http1_response_head_plan& plan);

void append_http1_response_trailers(
    std::pmr::string& output, const http_response_trailer_section& trailers);

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] inline http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers) {
    auto result_value = detail::validated_response_trailer_section(trailers);
    return *result_value.section();
}

}  // namespace ruvia
