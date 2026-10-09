#pragma once

#include <cstdint>

#include "ruvia/http/http_protocol_error.h"

namespace ruvia {

enum class http_parse_error : std::uint8_t {
    header_too_large,
    body_too_large,
    invalid_request_line,
    unsupported_http_version,
    invalid_request_target,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    too_many_headers,
    missing_host,
    invalid_host,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding,
    invalid_chunk_size,
    chunk_size_overflow,
    invalid_chunk_extension,
    invalid_chunk_crlf,
    invalid_trailer
};

[[nodiscard]] http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept;

}  // namespace ruvia
