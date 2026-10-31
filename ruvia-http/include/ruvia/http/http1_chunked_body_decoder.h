#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/detail/util/borrowed_view.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
#include "ruvia/http/protocol_byte_limit.h"
namespace ruvia {

enum class http1_chunk_decode_error : std::uint8_t {
    invalid_framing,
    body_limit_exceeded,
    framing_limit_exceeded,
};
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

// How a received trailer section treats obs-fold (RFC 9112 section 5.2). A
// server may reject a request carrying it (the default); a user agent must
// replace each obs-fold in a response with SP before interpreting the value,
// so a response decoder accepts it. An accepted fold is validated, against the
// field syntax and the section budget, as its HTTP unfolding: the obs-fold and
// adjacent OWS become one SP. The published trailer block keeps the received
// bytes; its consumer unfolds them (http1_client_response_body_decoder does).
enum class http1_trailer_obs_fold : std::uint8_t {
    reject,
    accept,
};

struct http1_chunked_body_decoder_config final {
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http1_trailer_obs_fold obs_fold_{http1_trailer_obs_fold::reject};
};

// Incremental sans-I/O HTTP/1 chunk framing decoder. Payload and trailer views
// borrow the supplied input and remain valid only until it is modified;
// incomplete framing stays unconsumed for the caller to retain. The body limit
// counts chunk payload bytes, which may still be transfer-encoded.
// There is no chunk-count or cumulative framing budget: RFC 9112 places no limit
// on the chunk count, and a zero-size chunk ends the body, so empty chunks cannot
// repeat. Each chunk-size line (including extensions) and the encoded trailer
// section (including its final empty line) are bounded independently by
// max_http_header_bytes. Searches stop at those caps, and reaching a cap
// with no terminator reports framing_limit_exceeded without inspecting
// later bytes. Malformed framing within the permitted prefix still reports
// invalid_framing.
// Trailer sections must also fit the http_field_section_budget byte rule
// (name.size() + value.size() + 4 per field, plus 2 for the terminating empty
// line; see http_trailer_section_budget()). obs-fold in the trailer section
// follows http1_chunked_body_decoder_config::obs_fold_. The complete view
// publishes the raw trailer block; fields a sender was not permitted to generate as trailers are
// not framing errors and are discarded by the block's consumers
// (http_request_trailers::append_http1, visit_http_response_trailers).
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
    enum class progress : std::uint8_t {
        size_line,
        body,
        delimiter,
        trailers,
        complete,
    };

    [[nodiscard]] http1_chunk_decode_result fail(std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept;
    [[nodiscard]] bool consume_delimiter(std::string_view available, std::size_t& cursor) noexcept;
    [[nodiscard]] bool trailers_valid(std::string_view trailers) const noexcept;

    protocol_byte_limit body_limit_;
    http1_trailer_obs_fold obs_fold_;
    // A failed decoder keeps reporting its error without consuming input.
    std::variant<progress, http1_chunk_decode_error> state_{progress::size_line};
    // Bytes of the pending size line or trailer section already scanned for LF.
    std::size_t size_line_search_offset_{0};
    std::size_t trailer_search_offset_{0};
    std::size_t remaining_{0};
    std::size_t decoded_bytes_{0};
};

// A request-body failure detected by a server driver while enforcing the
// HTTP-owned content contract. The driver supplies runtime byte limits and I/O
// completion facts; HTTP owns their final response status and diagnostic.
enum class http_request_body_failure : std::uint8_t {
    too_large,
    incomplete,
};

[[nodiscard]] inline http_protocol_error http_request_body_protocol_error(
    http_request_body_failure failure) noexcept {
    switch (failure) {
        case http_request_body_failure::too_large:
            return http_protocol_error(
                http_status::content_too_large, "request_body_too_large", "request body is too large");
        case http_request_body_failure::incomplete:
            return http_protocol_error(
                http_status::bad_request, "incomplete_request_body", "incomplete request body");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request_body", "invalid request body");
}

[[nodiscard]] inline http_protocol_error http_request_chunk_decode_error(
    http1_chunk_decode_error error) noexcept {
    switch (error) {
        case http1_chunk_decode_error::invalid_framing:
            return http_protocol_error(
                http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
        case http1_chunk_decode_error::body_limit_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
        case http1_chunk_decode_error::framing_limit_exceeded:
            return http_protocol_error(http_status::content_too_large, "chunked_framing_too_large",
                "request body framing is too large");
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
}

[[nodiscard]] inline http_protocol_error http_request_transfer_coding_error(
    http_transfer_coding_decode_error error) noexcept {
    switch (error) {
        case http_transfer_coding_decode_error::invalid_content:
            return http_protocol_error(http_status::bad_request, "invalid_transfer_coding_body",
                "invalid transfer-coding body");
        case http_transfer_coding_decode_error::decoded_size_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_transfer_coding_body", "invalid transfer-coding body");
}

// Status for a request body whose Content-Encoding could not be decoded. A
// decoder_failure is a codec fault rather than a protocol error; callers report
// it as an internal failure before mapping, so it is not given a status here.
[[nodiscard]] inline http_protocol_error http_request_content_decode_error(
    http_content_decode_error error) noexcept {
    switch (error) {
        case http_content_decode_error::unsupported_coding:
            return http_protocol_error(http_status::unsupported_media_type,
                "unsupported_content_coding", "request Content-Encoding is not supported");
        case http_content_decode_error::invalid_content:
            return http_protocol_error(
                http_status::bad_request, "invalid_content_coding", "failed to decode request body");
        case http_content_decode_error::decoded_size_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
        case http_content_decode_error::decoder_failure:
            break;
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_content_coding", "failed to decode request body");
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_size_failure(
    std::size_t size, protocol_byte_limit limit) noexcept {
    return limit.exceeds(size)
               ? std::optional<http_request_body_failure>(http_request_body_failure::too_large)
               : std::nullopt;
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_addition_failure(
    std::size_t current_size, std::size_t additional_size, protocol_byte_limit limit) noexcept {
    return limit.addition_exceeds(current_size, additional_size)
               ? std::optional<http_request_body_failure>(http_request_body_failure::too_large)
               : std::nullopt;
}

}  // namespace ruvia
