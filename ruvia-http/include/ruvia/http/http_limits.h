#pragma once

#include <cstddef>

namespace ruvia {

inline constexpr std::size_t max_http_header_bytes = std::size_t{64} * 1024;
// Field lines per HTTP/1 head or trailer section (the http_field_section_budget
// rule), received and generated, beside the max_http_header_bytes byte cap. The
// smallest field line costs 4 bytes, so the byte cap alone admits ~16 K fields
// whose descriptors outweigh the head; 256 matches Pingora's parser limit.
inline constexpr std::size_t max_http_header_fields = 256;
inline constexpr std::size_t default_max_buffered_body_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t default_max_websocket_message_bytes = std::size_t{16} * 1024 * 1024;

}  // namespace ruvia
