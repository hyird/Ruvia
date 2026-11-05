#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http_ascii.h"

#include "http2/http2_field_name.h"

namespace ruvia::detail {

inline constexpr std::size_t http2_lower_header_stack_bytes = 64;

// Stack-backed results expose only fully written prefixes; an already-lowercase
// name continues to borrow the input, so unused scratch bytes are never read.
[[nodiscard]] inline std::string_view http2_lower_header_name(std::string_view name,
    std::array<char, http2_lower_header_stack_bytes>& stack, std::pmr::string& scratch) {
    const auto write_lower_from = [](std::string_view source_value, std::size_t first_upper,
                                      char* target) noexcept {
        if (first_upper != 0) {
            std::memcpy(target, source_value.data(), first_upper);
        }
        for (std::size_t i = first_upper; i < source_value.size(); ++i) {
            target[i] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(source_value[i])));
        }
    };

    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte < 'A' || byte > 'Z') {
            continue;
        }
        if (name.size() <= stack.size()) {
            write_lower_from(name, i, stack.data());
            return std::string_view(stack.data(), name.size());
        }
        scratch.resize(name.size());
        write_lower_from(name, i, scratch.data());
        return scratch;
    }
    return name;
}

// Capacity estimate for the octets an HPACK literal field adds beyond its name and
// value: the representation octet plus the name-length and value-length integers,
// each at most two octets with a 7-bit prefix below 255 (RFC 7541 §5.1, §6.2), and
// one spare octet. Longer fields only make the block grow on demand.
inline constexpr std::size_t hpack_literal_overhead_bytes = 6;

// Reserves `block` for its current bytes, `fixed_bytes` of separately estimated
// representations, and every field of `fields` as a literal. reserve() never
// shrinks, so an estimate within the small-string buffer allocates nothing.
template <typename field_range_type>
void http2_reserve_field_block(
    std::pmr::string& block, std::size_t fixed_bytes, const field_range_type& fields) {
    auto bytes = block.size() + fixed_bytes;
    for (const auto& field : fields) {
        bytes += field.name().size() + field.value().size() + hpack_literal_overhead_bytes;
    }
    block.reserve(bytes);
}

// HTTP/2 field names are lowercase on the wire (RFC 9113 §8.2.1).
template <typename field_range_type>
void http2_encode_lowercase_fields(std::pmr::string& block, const field_range_type& fields) {
    std::array<char, http2_lower_header_stack_bytes> lower_name_stack;
    std::pmr::string lower_name_scratch(block.get_allocator().resource());
    for (const auto& field : fields) {
        hpack_encoder::encode_header(block,
            http2_lower_header_name(field.name(), lower_name_stack, lower_name_scratch), field.value());
    }
}

// Request TE values are validated with the other request-field semantics
// (validate_request_field: only "trailers", RFC 9113 Section 8.2.2).
[[nodiscard]] inline bool http2_is_valid_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_binary_field_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

[[nodiscard]] inline bool http2_is_valid_outbound_regular_header(
    std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && is_valid_http_header_value(value) &&
           !is_forbidden_http_binary_connection_field(name);
}

// Decoded HTTP/2 field names are already required to be lowercase. TE is
// permitted only in requests (RFC 9113 Section 8.2.2); a response carrying TE
// is malformed even when its value is exactly "trailers".
[[nodiscard]] inline bool http2_is_valid_decoded_response_header(
    std::string_view name, std::string_view value) noexcept {
    return http2_is_valid_regular_header(name, value) && name != "te";
}
}  // namespace ruvia::detail
