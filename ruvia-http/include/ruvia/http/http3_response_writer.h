#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http3_field_section.h"
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

struct http3_response_field_section final {
    // Owned by the supplied PMR resource; that resource must outlive this value.
    std::pmr::vector<char> field_section_;

    // RFC 9114 field-list size from emitted names and values, not QPACK bytes.
    [[nodiscard]] std::size_t decoded_field_section_size() const noexcept {
        return decoded_field_section_size_;
    }

    http3_response_field_section(std::pmr::vector<char> bytes_value, std::size_t decoded_size)
        : field_section_(std::move(bytes_value)),
          decoded_field_section_size_(decoded_size) {}
    http3_response_field_section(const http3_response_field_section&) = delete;
    http3_response_field_section& operator=(const http3_response_field_section&) = delete;
    http3_response_field_section(http3_response_field_section&& other) noexcept
        : field_section_(std::move(other.field_section_)),
          decoded_field_section_size_(std::exchange(other.decoded_field_section_size_, 0)) {
        other.field_section_.clear();
    }
    http3_response_field_section& operator=(http3_response_field_section&& other) {
        if (this != &other) {
            if (field_section_.get_allocator() == other.field_section_.get_allocator()) {
                field_section_ = std::move(other.field_section_);
            } else {
                std::pmr::vector<char> replacement(field_section_.get_allocator().resource());
                replacement.assign(other.field_section_.begin(), other.field_section_.end());
                field_section_.swap(replacement);
            }
            decoded_field_section_size_ = std::exchange(other.decoded_field_section_size_, 0);
            other.field_section_.clear();
        }
        return *this;
    }

private:
    std::size_t decoded_field_section_size_;
};

struct http3_response_head final {
    // The encoded section can be moved independently of the body metadata.
    http3_response_field_section field_section_;
    http_response_body_plan body_plan_;
    std::optional<std::uint64_t> declared_content_length_{};

    http3_response_head(std::pmr::vector<char> bytes_value, http_response_body_plan plan,
        std::size_t decoded_size, std::optional<std::uint64_t> length = {})
        : field_section_(std::move(bytes_value), decoded_size),
          body_plan_(plan),
          declared_content_length_(length) {}
    http3_response_head(const http3_response_head&) = delete;
    http3_response_head& operator=(const http3_response_head&) = delete;
    http3_response_head(http3_response_head&&) = default;
    http3_response_head& operator=(http3_response_head&&) = default;
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

// Encodes a response QPACK field section with the permanently empty dynamic
// table. Caller fields are borrowed only for this call. The returned bytes are
// the field section payload (not a complete HTTP/3 HEADERS frame), owned by
// resource, and contain :status followed by ordinary response fields.
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

// Encodes an HTTP/3 trailing HEADERS QPACK field section using the permanently
// empty dynamic table. The returned bytes are payload only (no HEADERS frame or
// FIN); the caller owns transmission and must keep resource alive until the
// returned field section is destroyed. Its decoded size is the uncompressed
// field-list size, independent of QPACK representation size.
[[nodiscard]] std::variant<http3_response_field_section, http3_response_head_failure>
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
[[nodiscard]] std::variant<http3_response_field_section, http3_response_head_failure> encode_http3_response_trailers(
    http3_qpack_encoder& encoder, std::uint64_t stream_id, std::span<const http3_field_section_field_view> fields_value,
    http3_field_section_limits limits = {}, std::pmr::memory_resource* resource = std::pmr::get_default_resource());

}  // namespace ruvia
