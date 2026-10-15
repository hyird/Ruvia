#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
#include "ruvia/http/http3_headers_frame.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"

namespace ruvia {

enum class http3_response_head_error : std::uint8_t {
    invalid_field,
    forbidden_field,
    unsupported_status,
    field_section_error,
    // A connection-owned encoder refused the peer's decoded-size limit before
    // touching QPACK state; this is not an invalid application response.
    peer_field_section_limit,
};

struct http3_response_head final {
    // The complete HEADERS frame can be moved independently of the body metadata.
    http3_headers_frame headers_;
    http_response_body_plan body_plan_;
    std::optional<std::uint64_t> declared_content_length_{};
};

struct http3_streaming_response_head final {
    http3_response_head head_;
    http_response_stream_commit_plan commit_plan_;
};

struct http3_response_head_failure final {
    http3_response_head_error kind_;
    http3_field_section_error field_section_error_{http3_field_section_error::invalid_prefix};
};

// Validates and projects streaming metadata without inventing a buffered length.
[[nodiscard]] std::variant<http3_streaming_response_head, http3_response_head_failure>
encode_http3_streaming_response_head(http_response response, http_known_method method,
    http_response_stream_kind kind, http_response_trailer_intent trailers,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Projects an interim 1xx head, validating its bodyless message semantics.
[[nodiscard]] std::variant<http3_response_head, http3_response_head_failure>
encode_http3_interim_response_head(const http_interim_response_head& response,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Encodes a response head with the permanently empty dynamic table. Caller
// fields are borrowed only for this call. The returned HEADERS frame is owned
// by resource and carries :status followed by ordinary response fields.
[[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    http_status_code status, http_known_method request_method,
    std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Projects a buffered response into a QPACK field section. The body remains
// external; the returned body plan describes whether and how DATA may be sent.
[[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    const http_response& response, http_buffered_response_write_plan write_plan,
    http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Encodes an HTTP/3 trailing HEADERS frame using the permanently empty dynamic
// table (no FIN); the caller owns transmission and must keep resource alive
// until the returned frame is destroyed. Its decoded size is the uncompressed
// field-list size, independent of QPACK representation size.
[[nodiscard]] std::variant<http3_headers_frame, http3_response_head_failure>
encode_http3_response_trailers(std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

class http3_qpack_encoder;
[[nodiscard]] std::variant<http3_streaming_response_head, http3_response_head_failure>
encode_http3_streaming_response_head(http3_qpack_encoder& encoder, std::uint64_t stream_id, http_response response, http_known_method method,
    http_response_stream_kind kind, http_response_trailer_intent trailers, http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<http3_response_head, http3_response_head_failure>
encode_http3_interim_response_head(http3_qpack_encoder& encoder, std::uint64_t stream_id, const http_interim_response_head& response,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
// Dynamic forms share one encoder for every stream in the connection. Returned
// storage belongs to resource; encoder-stream output is drained separately.
[[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, http_status_code status, http_known_method method,
    std::span<const http3_field_section_field_view> fields_value, http3_field_section_limits limits = {},
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<http3_response_head, http3_response_head_failure> encode_http3_response_head(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, const http_response& response, http_buffered_response_write_plan plan,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
[[nodiscard]] std::variant<http3_headers_frame, http3_response_head_failure> encode_http3_response_trailers(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
