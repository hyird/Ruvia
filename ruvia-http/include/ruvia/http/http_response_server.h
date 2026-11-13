#pragma once

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http1_response_head_error.h"
#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_head_buffer.h"
namespace ruvia {

namespace detail {

[[nodiscard]] bool is_forbidden_response_trailer_name(std::string_view name) noexcept;
[[nodiscard]] bool is_forbidden_http_binary_response_field(std::string_view name) noexcept;

}  // namespace detail

class http_response_trailer_section;

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. The source span must outlive its synchronous consumption.
class http_response_trailer_section final {
public:
    [[nodiscard]] std::span<const http_header_view> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

private:
    friend http_response_trailer_section validate_http_response_trailers(
        std::span<const http_header_view>);
    explicit http_response_trailer_section(std::span<const http_header_view> fields_value) noexcept
        : fields_(fields_value) {}

    std::span<const http_header_view> fields_;
};

// Serialize a finalized HTTP/1 response head into caller-owned reusable scratch.
// The plan must describe the same response representation.
// Canonical Content-Length is measured and emitted from the same decimal token.
// An explicit Date is preserved without consulting the clock; otherwise a
// cached current Date is generated when a usable clock is available. A head
// that breaks a message rule or the head size bound throws
// http1_response_head_error. The plan owns framing: an application
// Transfer-Encoding is accepted only as exactly `chunked` (OWS and ASCII case
// aside) and is replaced by the plan's framing; any other value throws.
void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head,
    const http1_response_head_plan& plan);

// Append only field lines; chunk delimiters belong to the caller. Inputs may
// alias output. Allocation failure leaves the existing output bytes unchanged.
void append_http1_response_trailers(
    std::string& output, const http_response_trailer_section& trailers);

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers);

template <detail::http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section validate_http_response_trailers(headers_type&&) = delete;
http_response_trailer_section validate_http_response_trailers(
    std::initializer_list<http_header_view>) = delete;

}  // namespace ruvia
