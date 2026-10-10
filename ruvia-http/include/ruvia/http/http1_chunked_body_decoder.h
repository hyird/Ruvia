#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/parser/http_chunk_framing.h"
#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http1_chunk_decode_error.h"
#include "ruvia/http/protocol_byte_limit.h"

namespace ruvia {

class http1_chunk_decode_need_more final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http1_chunk_decode_result;
    explicit constexpr http1_chunk_decode_need_more(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http1_chunk_decode_body_chunk_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class http1_chunk_decode_result;
    constexpr http1_chunk_decode_body_chunk_view(std::size_t consumed_bytes, std::string_view bytes_value) noexcept
        : consumed_bytes_(consumed_bytes),
          bytes_(bytes_value) {}
    std::size_t consumed_bytes_;
    std::string_view bytes_;
};

class http1_chunk_decode_complete_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr std::string_view trailers() const& noexcept {
        return trailers_;
    }
    std::string_view trailers() const&& = delete;

private:
    friend class http1_chunk_decode_result;
    explicit constexpr http1_chunk_decode_complete_view(
        std::size_t consumed_bytes, std::string_view trailers) noexcept
        : consumed_bytes_(consumed_bytes),
          trailers_(trailers) {}
    std::size_t consumed_bytes_;
    std::string_view trailers_;
};

class http1_chunk_decode_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr http1_chunk_decode_error error() const noexcept {
        return error_;
    }

private:
    friend class http1_chunk_decode_result;
    constexpr http1_chunk_decode_failure(
        std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept
        : consumed_bytes_(consumed_bytes),
          error_(error) {}
    std::size_t consumed_bytes_;
    http1_chunk_decode_error error_;
};

class http1_chunk_decode_result final {
public:
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return std::visit([](const auto& result_value) { return result_value.consumed_bytes(); }, value_);
    }
    [[nodiscard]] const http1_chunk_decode_need_more* need_more() const& noexcept {
        return std::get_if<http1_chunk_decode_need_more>(&value_);
    }
    const http1_chunk_decode_need_more* need_more() const&& = delete;
    [[nodiscard]] const http1_chunk_decode_body_chunk_view* body_chunk() const& noexcept {
        return std::get_if<http1_chunk_decode_body_chunk_view>(&value_);
    }
    const http1_chunk_decode_body_chunk_view* body_chunk() const&& = delete;
    [[nodiscard]] const http1_chunk_decode_complete_view* complete() const& noexcept {
        return std::get_if<http1_chunk_decode_complete_view>(&value_);
    }
    const http1_chunk_decode_complete_view* complete() const&& = delete;
    [[nodiscard]] const http1_chunk_decode_failure* failure() const& noexcept {
        return std::get_if<http1_chunk_decode_failure>(&value_);
    }
    const http1_chunk_decode_failure* failure() const&& = delete;

private:
    friend class http1_chunked_body_decoder;
    using value_type = std::variant<http1_chunk_decode_need_more, http1_chunk_decode_body_chunk_view,
        http1_chunk_decode_complete_view, http1_chunk_decode_failure>;
    template <typename result_type>
    explicit http1_chunk_decode_result(result_type result_value) noexcept
        : value_(std::move(result_value)) {}
    [[nodiscard]] static http1_chunk_decode_result make_need_more(std::size_t consumed_bytes) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_body_chunk(
        std::size_t consumed_bytes, std::string_view bytes) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_complete(
        std::size_t consumed_bytes, std::string_view trailers = {}) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_failure(
        std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept;
    value_type value_;
};

enum class http1_chunk_trailer_role : std::uint8_t {
    request,
    response,
};

struct http1_chunked_body_decoder_config final {
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http1_chunk_trailer_role trailer_role_{http1_chunk_trailer_role::request};
};

// Incremental sans-I/O HTTP/1 chunk framing decoder. Payload and trailer views
// borrow the supplied input and remain valid only until it is modified. The
// body limit counts chunk payload bytes, which may still be transfer-encoded.
// There is no chunk-count or cumulative framing budget: each chunk-size line
// (including extensions) and the trailer section are bounded independently by
// max_http_header_bytes. An over-long size line or unterminated trailer section
// reports framing_limit_exceeded; a complete oversized trailer section is
// invalid_framing.
class http1_chunked_body_decoder final {
public:
    explicit http1_chunked_body_decoder(http1_chunked_body_decoder_config config = {});
    ~http1_chunked_body_decoder();
    http1_chunked_body_decoder(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder& operator=(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept;
    http1_chunked_body_decoder& operator=(http1_chunked_body_decoder&&) noexcept;

    [[nodiscard]] http1_chunk_decode_result decode(std::string_view available);
    [[nodiscard]] http1_chunk_decode_result decode(std::string_view available, std::size_t max_body_bytes);
    template <detail::http_temporary_owning_char_string input_type>
    http1_chunk_decode_result decode(input_type&&) = delete;
    template <detail::http_temporary_owning_char_string input_type>
    http1_chunk_decode_result decode(input_type&&, std::size_t) = delete;

private:
    detail::http_chunk_framing framing_;
};

}  // namespace ruvia
