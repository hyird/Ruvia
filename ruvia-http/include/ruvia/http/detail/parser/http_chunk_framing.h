#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/protocol_byte_limit.h"

namespace ruvia::detail {

enum class chunk_framing_error : std::uint8_t {
    invalid_size,
    size_overflow,
    invalid_extension,
    invalid_crlf,
    invalid_trailer,
    trailer_limit_exceeded,
    body_limit_exceeded,
    framing_limit_exceeded,
};

enum class chunk_trailer_role : std::uint8_t { request,
    response };

// Framing has no cumulative budget: RFC 9112 places no limit on the chunk
// count, and an incremental caller retains at most one incomplete framing
// element. Each chunk-size line (including extensions) and the trailer section
// are bounded independently by max_http_header_bytes; a zero-size chunk ends
// the body, so empty chunks cannot repeat.
struct chunk_framing_config final {
    protocol_byte_limit body_limit_;
    // Optional additional bound on the encoded trailer section including its
    // terminating empty line; field validation always caps the trailer fields
    // at max_http_header_bytes.
    protocol_byte_limit trailer_section_limit_;
    chunk_trailer_role trailer_role_;
};

struct chunk_framing_need_more final {
    std::size_t consumed_bytes_;
};

struct chunk_framing_body final {
    std::size_t consumed_bytes_;
    std::string_view bytes_;
};

struct chunk_framing_complete final {
    std::size_t consumed_bytes_;
    std::string_view trailers_;
};

struct chunk_framing_failure final {
    std::size_t consumed_bytes_;
    chunk_framing_error error_;
};

using chunk_framing_result = std::variant<chunk_framing_need_more, chunk_framing_body,
    chunk_framing_complete, chunk_framing_failure>;

// The sole HTTP/1 chunk framing engine. Input is borrowed; incomplete framing
// remains unconsumed for the caller to retain. Body delivery never copies bytes.
class http_chunk_framing final {
public:
    explicit http_chunk_framing(chunk_framing_config config) noexcept
        : config_(config) {}

    [[nodiscard]] chunk_framing_result decode(std::string_view available, std::size_t max_body_bytes) noexcept;

private:
    enum class progress : std::uint8_t { size_line,
        body,
        delimiter,
        trailers,
        complete };

    [[nodiscard]] chunk_framing_result fail(std::size_t consumed_bytes, chunk_framing_error error) noexcept;
    [[nodiscard]] static std::optional<chunk_framing_error> consume_delimiter(std::string_view available) noexcept;
    [[nodiscard]] std::optional<chunk_framing_error> validate_trailers(std::string_view trailers) const noexcept;

    chunk_framing_config config_;
    std::variant<progress, chunk_framing_error> state_{progress::size_line};
    std::size_t trailer_search_offset_{0};
    std::size_t remaining_{0};
    std::size_t decoded_bytes_{0};
};

}  // namespace ruvia::detail
