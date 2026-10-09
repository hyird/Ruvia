#pragma once
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/http/http_response.h"

namespace ruvia::detail {

struct http_response_header_state_access final {
    [[nodiscard]] static http_response clone_headers_for_transaction(
        const http_response& response, std::size_t additional_headers) {
        return response.clone_headers_for_transaction(additional_headers);
    }

    // Staged headers must belong to the response's resource domain.
    static void commit_headers(http_response& response, http_response&& staged) noexcept {
        response.commit_headers_from(std::move(staged));
    }

    [[nodiscard]] static http_response clone_for_transaction(const http_response& response) {
        return response.clone_for_transaction();
    }

    static void set_validated(http_response& response, std::string_view key, std::string_view value,
        std::uint32_t known_bit) {
        response.set_header_validated(key, value, known_bit);
    }

    static void append_validated(http_response& response, std::string_view key,
        std::string_view value, std::uint32_t known_bit) {
        response.append_header_validated(key, value, known_bit);
    }

    static void upsert_set_cookie_validated(http_response& response, std::string_view value) {
        response.upsert_set_cookie_header_validated(value);
    }

    static void set_unsigned(
        http_response& response, std::string_view key, std::uint64_t value, std::uint32_t known_bit) {
        response.set_header_unsigned(key, value, known_bit);
    }

    static void set_content_range(
        http_response& response, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
        response.set_content_range(offset, length, size);
    }

    static void set_content_range_unsatisfied(http_response& response, std::uint64_t size) {
        response.set_content_range_unsatisfied(size);
    }

    static void reserve(http_response& response, std::size_t count) {
        response.reserve_headers(count);
    }

    static void apply_content_encoding(http_response& response, std::string_view content_encoding) {
        response.apply_content_encoding(content_encoding);
    }

    static void replace_body_with_content_encoding(
        http_response& response, std::pmr::string&& value, std::string_view content_encoding) {
        response.replace_body_with_content_encoding(std::move(value), content_encoding);
    }

    [[nodiscard]] static std::uint32_t known_bits(const http_response& response) noexcept {
        return response.known_header_bits_;
    }

    [[nodiscard]] static bool has_known(const http_response& response, std::uint32_t bit) noexcept {
        return (response.known_header_bits_ & bit) != 0;
    }

    [[nodiscard]] static std::string_view known_value(
        const http_response& response, std::uint32_t bit) noexcept {
        return response.known_header_value(bit);
    }

    [[nodiscard]] static std::pmr::memory_resource* resource(
        const http_response& response) noexcept {
        return response.resource();
    }
};

inline void set_response_header_validated(
    http_response& response, std::string_view key, std::string_view value, std::uint32_t known_bit) {
    http_response_header_state_access::set_validated(response, key, value, known_bit);
}

inline void append_response_header_validated(
    http_response& response, std::string_view key, std::string_view value, std::uint32_t known_bit) {
    http_response_header_state_access::append_validated(response, key, value, known_bit);
}

inline void upsert_response_set_cookie_validated(http_response& response, std::string_view value) {
    http_response_header_state_access::upsert_set_cookie_validated(response, value);
}

inline void set_response_header_unsigned(
    http_response& response, std::string_view key, std::uint64_t value, std::uint32_t known_bit) {
    http_response_header_state_access::set_unsigned(response, key, value, known_bit);
}

inline void set_response_content_range(
    http_response& response, std::uint64_t offset, std::uint64_t length, std::uint64_t size) {
    http_response_header_state_access::set_content_range(response, offset, length, size);
}

inline void set_response_content_range_unsatisfied(http_response& response, std::uint64_t size) {
    http_response_header_state_access::set_content_range_unsatisfied(response, size);
}

inline void reserve_response_headers(http_response& response, std::size_t count) {
    http_response_header_state_access::reserve(response, count);
}

// Announces a non-identity content-coding whose encoded body is not yet known
// (streaming). Sets Content-Encoding, drops identity Content-Length, and
// weakens a strong ETag. Vary is a negotiation/product header and is left to
// the caller.
inline void apply_response_content_encoding(
    http_response& response, std::string_view content_encoding) {
    http_response_header_state_access::apply_content_encoding(response, content_encoding);
}

// Atomically prepares the three representation fields affected by buffered
// content-coding (Content-Encoding, Content-Length and a strong ETag's weak
// replacement) before publishing the owned body. Web compression uses this
// boundary so an allocation failure cannot leave identity bytes carrying
// compressed metadata, or compressed bytes carrying an identity length.
inline void replace_response_body_with_content_encoding(
    http_response& response, std::pmr::string&& value, std::string_view content_encoding) {
    http_response_header_state_access::replace_body_with_content_encoding(
        response, std::move(value), content_encoding);
}

[[nodiscard]] inline std::uint32_t response_known_header_bits(const http_response& response) noexcept {
    return http_response_header_state_access::known_bits(response);
}

[[nodiscard]] inline bool response_has_known_header(
    const http_response& response, std::uint32_t bit) noexcept {
    return http_response_header_state_access::has_known(response, bit);
}

[[nodiscard]] inline std::string_view response_known_header(
    const http_response& response, std::uint32_t bit) noexcept {
    return http_response_header_state_access::known_value(response, bit);
}

[[nodiscard]] inline std::pmr::memory_resource* response_resource(
    const http_response& response) noexcept {
    return http_response_header_state_access::resource(response);
}

}  // namespace ruvia::detail
