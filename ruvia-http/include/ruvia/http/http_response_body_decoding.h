#pragma once

#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_field_section.h"

namespace ruvia {

// Visit normalized response trailer fields from an HTTP/1 trailer block: the
// bytes between the terminal zero-size chunk and the empty line that ends the
// trailer section (a final CRLF after the last field is accepted). Values are
// exposed with optional whitespace stripped and borrow `block`. Returns false
// for a malformed, forbidden or oversized section, or when the visitor returns
// false.
template <typename visitor_type>
[[nodiscard]] inline bool visit_http_response_trailers(std::string_view block, visitor_type&& visitor) {
    auto budget = http_trailer_section_budget();
    return detail::visit_http1_trailer_block(block, detail::is_forbidden_response_trailer_name, budget, 0,
               std::forward<visitor_type>(visitor)) == detail::http_trailer_block_status::ok;
}

}  // namespace ruvia
