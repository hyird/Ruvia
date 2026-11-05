#pragma once

#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/url_encoding.h"

namespace ruvia::detail {

// Visits each name/value pair of a URL-encoded form body with the name decoded
// and the value still encoded. Form decoding keeps malformed '%' sequences
// literally (WHATWG), so the body needs no separate percent-encoding pass and
// never fails.
template <typename visitor_type>
    requires std::invocable<visitor_type&, std::string_view, std::string_view>
void visit_form_body_fields(std::string_view body, std::pmr::memory_resource* resource, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    std::pmr::string decoded_name(resource);
    (void)ruvia::visit_url_encoded_pairs(body, [&](std::string_view name, std::string_view value) {
        visitor_ref(*ruvia::decode_url_component(name, decoded_name, url_decode_mode::form), value);
    });
}

}  // namespace ruvia::detail
