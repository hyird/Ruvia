#pragma once

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_head_buffer.h"
namespace ruvia {

class http_response_trailer_section;
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers);

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
// cached current Date is generated when a usable clock is available.
void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head,
    const http1_response_head_plan& plan);

// Append only field lines; chunk delimiters belong to the caller. Inputs may
// alias output. Allocation failure leaves the existing output bytes unchanged.
void append_http1_response_trailers(
    std::pmr::string& output, const http_response_trailer_section& trailers);

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers);

template <detail::http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section validate_http_response_trailers(headers_type&&) = delete;

// Visit normalized response trailer fields from a received HTTP/1 trailer
// block: the bytes between the terminal zero-size chunk and the empty line that
// ends the trailer section (a final CRLF after the last field is accepted),
// already free of obs-fold as http1_client_response_body_decoder publishes it
// (RFC 9112 section 5.2); a fold left in `block` is malformed.
// Values are exposed with optional whitespace stripped and borrow `block`.
// Fields not permitted in response trailers (RFC 9110 section 6.5.1) are
// discarded without reaching the visitor (RFC 9112 section 7.1.2). Returns false
// for a malformed or oversized section, or when the visitor returns false.
template <typename visitor_type>
[[nodiscard]] inline bool visit_http_response_trailers(std::string_view block, visitor_type&& visitor) {
    auto budget = http_trailer_section_budget();
    return detail::visit_http1_trailer_block(block, detail::is_forbidden_response_trailer_name, budget,
               std::forward<visitor_type>(visitor)) == detail::http_trailer_block_status::ok;
}
}  // namespace ruvia
