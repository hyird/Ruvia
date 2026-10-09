#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"

namespace ruvia {

enum class http3_message_head_kind : std::uint8_t { request,
    response };

// Errors are scoped to processing a HEADERS field section: malformed decoded
// HTTP semantics map to H3_MESSAGE_ERROR; failures from QPACK decoding map to
// QPACK_DECOMPRESSION_FAILED. Resource exceptions propagate to the caller.
enum class http3_message_head_error : std::uint8_t {
    message_error,
    qpack_decompression_failed,
    field_section_too_large,
};

struct http3_message_header final {
    std::pmr::string name_;
    std::pmr::string value_;

    explicit http3_message_header(std::pmr::memory_resource* resource);
    http3_message_header(std::string_view name, std::string_view value, std::pmr::memory_resource* resource);
};

// All strings and headers are owned by resource. The resource must outlive this
// value; destroying the value returns its allocations to that resource.
struct http3_message_head final {
    explicit http3_message_head(std::pmr::memory_resource* resource);

    std::pmr::string method_;
    std::pmr::string protocol_;
    std::pmr::string scheme_;
    std::pmr::string authority_;
    std::pmr::string path_;
    std::uint16_t status_{0};
    std::optional<std::uint64_t> content_length_{};
    std::pmr::vector<http3_message_header> headers_;
};

struct http3_message_head_limits final {
    std::size_t max_field_section_size_{64 * 1024};
    std::size_t max_fields_{256};
    std::size_t max_encoded_bytes_{64 * 1024};
};

// Consumes a complete QPACK-encoded field section. max_field_section_size
// counts each field as name + value + 32, per RFC 9114 Section 4.2.2; it is not
// an encoded-byte limit. Only ordinary fields are returned in headers.
[[nodiscard]] std::variant<http3_message_head, http3_message_head_error> decode_http3_message_head(
    std::span<const char> field_section, http3_message_head_kind kind,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
    http3_message_head_limits limits = {});

class http3_qpack_decoder;
struct http3_qpack_blocked final {};
using http3_decoded_message_head_type = std::variant<http3_qpack_blocked, http3_message_head>;
// The shared decoder handles RFC 9204 state and acknowledgments. A blocked
// result contains no head: retain the section and retry after encoder input.
[[nodiscard]] std::variant<http3_decoded_message_head_type, http3_message_head_error> decode_http3_message_head(
    http3_qpack_decoder& decoder, std::uint64_t stream_id, std::span<const char> field_section,
    http3_message_head_kind kind, std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
    http3_message_head_limits limits = {});

}  // namespace ruvia
