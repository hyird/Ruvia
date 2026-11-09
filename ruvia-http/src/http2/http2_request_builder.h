#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request.h"

#include "http2/http2_request_headers.h"
#include "http2/http2_stream_state.h"
#include "parser/http_parser_syntax.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"

namespace ruvia::detail {

// Materializes the validated request head of `stream`; its views borrow stream storage.
[[nodiscard]] inline http_request build_http2_request(
    http2_stream_state& stream, std::pmr::memory_resource* resource, std::string_view body) {
    const auto* pending = stream.tunnel().pending();
    const binary_request_materialization_spec spec{
        .version_ = http_protocol_version::http2,
        .target_form_ = ::ruvia::http_request_target_form::http2,
        .method_ = stream.request_method(),
        .scheme_ = stream.request_scheme(),
        .authority_ = stream.request_authority(),
        .host_ = http2_request_host_field(stream),
        .path_ = stream.request_path(),
        .protocol_ = stream.request_protocol(),
        .cookie_ = stream.request_cookie(),
        .body_ = body,
        .regular_header_count_ = stream.remote_header_count(),
        .has_authority_ = stream.has_authority(),
        .has_host_ = stream.has_host(),
        .has_path_ = stream.has_path(),
        .has_cookie_ = stream.has_cookie(),
        .standard_connect_ = pending != nullptr && pending->form() == http2_connect_form::standard,
        .failure_message_ = "validated HTTP/2 request cannot be materialized",
    };
    return materialize_binary_http_request(resource, spec, [&stream](std::size_t index) noexcept {
        const auto header_value = stream.remote_header_at(index);
        return std::pair{http_header_view{header_value.name_, header_value.value_}, header_value.kind_};
    });
}

}  // namespace ruvia::detail
