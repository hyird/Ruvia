#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/ProtocolByteLimit.h"

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

struct chunk_framing_config final {
    ProtocolByteLimit body_limit;
    std::size_t framing_limit;
    // Whole-message scanning bounds the encoded trailer section separately.
    // Incremental decoding first validates fields, then charges framing bytes.
    ProtocolByteLimit trailer_section_limit;
    chunk_trailer_role trailer_role;
};

struct chunk_framing_need_more final {
    std::size_t consumed_bytes;
};

struct chunk_framing_body final {
    std::size_t consumed_bytes;
    std::string_view bytes;
};

struct chunk_framing_complete final {
    std::size_t consumed_bytes;
    std::string_view trailers;
};

struct chunk_framing_failure final {
    std::size_t consumed_bytes;
    chunk_framing_error error;
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
    [[nodiscard]] std::optional<chunk_framing_error> account_framing(std::size_t bytes) noexcept;
    [[nodiscard]] std::optional<chunk_framing_error> consume_delimiter(std::string_view available) noexcept;
    [[nodiscard]] std::optional<chunk_framing_error> validate_trailers(std::string_view trailers) const noexcept;

    chunk_framing_config config_;
    std::variant<progress, chunk_framing_error> state_{progress::size_line};
    std::size_t trailer_search_offset_{0};
    std::size_t remaining_{0};
    std::size_t decoded_bytes_{0};
    std::size_t framing_bytes_{0};
};

}  // namespace ruvia::detail
