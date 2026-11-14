#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "http_content_coding.h"
#include "http_header.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_response.h"
#include "http_status.h"
#include "http_transfer_coding.h"

namespace ruvia {

inline constexpr std::string_view http1_chunk_data_terminator = "\r\n";
inline constexpr std::string_view http1_last_chunk_prefix = "0\r\n";
inline constexpr std::string_view http1_trailer_section_terminator = "\r\n";

// The HTTP/1 chunk-size line, including its terminating CRLF. The caller owns
// framing decisions; this value only formats one chunk's wire prefix.
class http1_chunk_header final {
public:
    explicit http1_chunk_header(std::size_t chunk_size) noexcept {
        auto* cursor_value = storage_.data() + storage_.size();
        *--cursor_value = '\n';
        *--cursor_value = '\r';
        do {
            *--cursor_value = http_lower_hex_digit(static_cast<int>(chunk_size & 0x0fU));
            chunk_size >>= 4U;
        } while (chunk_size != 0);
        offset_ = static_cast<std::size_t>(cursor_value - storage_.data());
    }

    [[nodiscard]] std::string_view view() const& noexcept {
        return std::string_view(storage_.data() + offset_, storage_.size() - offset_);
    }
    [[nodiscard]] std::string_view view() const&& = delete;

private:
    // Construction writes every byte exposed by view(), plus the final offset.
    std::array<char, sizeof(std::size_t) * 2 + 2> storage_;
    std::size_t offset_;
};

// A request-body failure detected by a server driver while enforcing the
// HTTP-owned content contract. The driver supplies runtime byte limits and I/O
// completion facts; HTTP owns their final response status and diagnostic.
enum class http_request_body_failure : std::uint8_t {
    too_large,
    incomplete,
    buffer_limit,
};

[[nodiscard]] inline http_protocol_error http_request_body_protocol_error(http_request_body_failure failure) noexcept {
    switch (failure) {
        case http_request_body_failure::too_large:
            return http_protocol_error(http_status::content_too_large, "request_body_too_large", "request body is too large");
        case http_request_body_failure::incomplete:
            return http_protocol_error(http_status::bad_request, "incomplete_request_body", "incomplete request body");
        case http_request_body_failure::buffer_limit:
            return http_protocol_error(http_status::service_unavailable, "inbound_buffer_limit", "inbound buffer capacity exhausted");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request_body", "invalid request body");
}

[[nodiscard]] inline http_protocol_error http_request_transfer_coding_error(http_transfer_coding_decode_error error) noexcept {
    switch (error) {
        case http_transfer_coding_decode_error::invalid_content:
            return http_protocol_error(http_status::bad_request, "invalid_transfer_coding_body", "invalid transfer-coding body");
        case http_transfer_coding_decode_error::decoded_size_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
    }
    return http_protocol_error(http_status::bad_request, "invalid_transfer_coding_body", "invalid transfer-coding body");
}

// Status for a request body whose Content-Encoding could not be decoded.
[[nodiscard]] inline http_protocol_error http_request_content_decode_error(http_content_decode_error error) noexcept {
    switch (error) {
        case http_content_decode_error::unsupported_coding:
            return http_protocol_error(http_status::unsupported_media_type, "unsupported_content_coding", "request Content-Encoding is not supported");
        case http_content_decode_error::invalid_content:
            return http_protocol_error(http_status::bad_request, "invalid_content_coding", "failed to decode request body");
        case http_content_decode_error::decoded_size_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
    }
    return http_protocol_error(http_status::bad_request, "invalid_content_coding", "failed to decode request body");
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_size_failure(std::size_t size, protocol_byte_limit limit) noexcept {
    return limit.exceeds(size) ? std::optional<http_request_body_failure>(http_request_body_failure::too_large) : std::nullopt;
}

[[nodiscard]] inline std::optional<http_request_body_failure> http_request_body_addition_failure(std::size_t current_size, std::size_t additional_size, protocol_byte_limit limit) noexcept {
    return limit.addition_exceeds(current_size, additional_size) ? std::optional<http_request_body_failure>(http_request_body_failure::too_large) : std::nullopt;
}

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
    explicit constexpr http1_chunk_decode_need_more(std::size_t consumed_bytes) noexcept :
        consumed_bytes_(consumed_bytes) {}
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
    constexpr http1_chunk_decode_body_chunk_view(std::size_t consumed_bytes, std::string_view bytes_value) noexcept :
        consumed_bytes_(consumed_bytes),
        bytes_(bytes_value) {}
    std::size_t consumed_bytes_;
    std::string_view bytes_;
};

class http1_chunk_decode_complete_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    // The received trailer section, empty when there was none. The fields
    // borrow the decoder that produced this result (see
    // http1_chunked_body_decoder).
    [[nodiscard]] constexpr std::span<const http_header> trailers() const& noexcept {
        return trailers_;
    }
    std::span<const http_header> trailers() const&& = delete;

private:
    friend class http1_chunk_decode_result;
    explicit constexpr http1_chunk_decode_complete_view(std::size_t consumed_bytes, std::span<const http_header> trailers) noexcept :
        consumed_bytes_(consumed_bytes),
        trailers_(trailers) {}
    std::size_t consumed_bytes_;
    std::span<const http_header> trailers_;
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
    constexpr http1_chunk_decode_failure(std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept :
        consumed_bytes_(consumed_bytes),
        error_(error) {}
    std::size_t consumed_bytes_;
    http1_chunk_decode_error error_;
};

class http1_chunk_decode_result final {
public:
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return std::visit([](const auto& result_value) {
            return result_value.consumed_bytes();
        }, value_);
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
    using value_type = std::variant<http1_chunk_decode_need_more, http1_chunk_decode_body_chunk_view, http1_chunk_decode_complete_view, http1_chunk_decode_failure>;
    template <typename result_type>
    explicit http1_chunk_decode_result(result_type result_value) noexcept :
        value_(std::move(result_value)) {}
    [[nodiscard]] static http1_chunk_decode_result make_need_more(std::size_t consumed_bytes) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_body_chunk(std::size_t consumed_bytes, std::string_view bytes) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_complete(std::size_t consumed_bytes, std::span<const http_header> trailers = {}) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_failure(std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept;
    value_type value_;
};

// How a received trailer section treats obs-fold (RFC 9112 section 5.2). A
// server may reject a request carrying it (the default); a user agent must
// replace each obs-fold in a response with SP before interpreting the value,
// so a response decoder accepts it. An accepted fold is validated, against the
// field syntax and the section budget, and published as its HTTP unfolding:
// the obs-fold and adjacent OWS become one SP.
enum class http1_trailer_obs_fold : std::uint8_t {
    reject,
    accept,
};

// The message whose trailer section a decoder receives. A received field its
// sender was not permitted to generate as a trailer of that message (RFC 9110
// section 6.5.1) is not a framing error; it is discarded rather than published
// (RFC 9112 section 7.1.2).
enum class http1_trailer_message : std::uint8_t {
    request,
    response,
};

struct http1_chunked_body_decoder_config final {
    protocol_byte_limit body_limit_{protocol_byte_limit::unlimited()};
    http1_trailer_obs_fold obs_fold_{http1_trailer_obs_fold::reject};
    http1_trailer_message trailer_message_{http1_trailer_message::request};
};

// Incremental sans-I/O HTTP/1 chunk framing decoder. Payload views borrow the
// supplied input and remain valid only until it is modified;
// incomplete framing stays unconsumed for the caller to retain. The body limit
// counts chunk payload bytes, which may still be transfer-encoded.
// RFC 9112 places no limit on the chunk count, and a zero-size chunk ends the
// body, so empty chunks cannot repeat. Each chunk-size line (including
// extensions) and the encoded trailer section (including its final empty line)
// are bounded independently by max_http_header_bytes. Searches stop at those
// caps, and reaching a cap with no terminator reports framing_limit_exceeded
// without inspecting later bytes. Size-line bytes beyond the shortest hex size
// (leading zeros and chunk-ext) also share one per-message budget of
// max_http_header_bytes (RFC 9112 section 7.1.1); exceeding it reports
// framing_limit_exceeded. Malformed framing within the permitted prefix still
// reports invalid_framing.
// Trailer sections must also fit the http_field_section_budget byte rule
// (name.size() + value.size() + 4 per field, plus 2 for the terminating empty
// line; see http_trailer_section_budget()). obs-fold in the trailer section
// follows http1_chunked_body_decoder_config::obs_fold_, and fields the sender
// was not permitted to generate follow trailer_message_.
// The complete view publishes the remaining fields in received order, names
// lowercase and values without surrounding OWS. The decoder owns them: storage
// is allocated only for a section that keeps a
// field, and the view stays valid until the next decode() call, or until the
// decoder is moved from, assigned to or destroyed.
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
    [[nodiscard]] bool publish_trailers(std::string_view trailers);

    protocol_byte_limit body_limit_;
    http1_trailer_obs_fold obs_fold_;
    // A failed decoder keeps reporting its error without consuming input.
    std::variant<progress, http1_chunk_decode_error> state_{progress::size_line};
    // Bytes of the pending size line or trailer section already scanned for LF.
    std::size_t size_line_search_offset_{0};
    std::size_t trailer_search_offset_{0};
    std::size_t remaining_{0};
    std::size_t decoded_bytes_{0};
    // Charged size-line bytes beyond the shortest hex size; never exceeds
    // max_http_header_bytes.
    std::size_t framing_overhead_bytes_{0};
    http1_trailer_message trailer_message_;
    std::vector<http_header> trailers_;
};

[[nodiscard]] inline http_protocol_error http_request_chunk_decode_error(http1_chunk_decode_error error) noexcept {
    switch (error) {
        case http1_chunk_decode_error::invalid_framing:
            return http_protocol_error(http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
        case http1_chunk_decode_error::body_limit_exceeded:
            return http_request_body_protocol_error(http_request_body_failure::too_large);
        case http1_chunk_decode_error::framing_limit_exceeded:
            return http_protocol_error(http_status::content_too_large, "chunked_framing_too_large", "request body framing is too large");
    }
    return http_protocol_error(http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
}

}  // namespace ruvia
