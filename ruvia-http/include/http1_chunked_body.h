#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "binary_text_encoding.h"
#include "borrowed_text.h"
#include "decode_step.h"
#include "http_header.h"
#include "http_protocol.h"
#include "http_request_body_failure.h"
#include "http_status.h"

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

enum class http1_chunk_decode_error : std::uint8_t {
    invalid_framing,
    body_limit_exceeded,
    framing_limit_exceeded,
};

// The end of a chunked body, zero-size chunk and trailer section included.
class http1_chunk_decode_complete final {
public:
    // The received trailer section, empty when there was none. The fields
    // borrow the decoder that produced this result (see
    // http1_chunked_body_decoder).
    [[nodiscard]] constexpr std::span<const http_header> trailers() const noexcept {
        return trailers_;
    }

private:
    friend class http1_chunked_body_decoder;
    explicit constexpr http1_chunk_decode_complete(std::span<const http_header> trailers) noexcept :
        trailers_(trailers) {}
    std::span<const http_header> trailers_;
};

using http1_chunk_decode_failure = decode_failure<http1_chunk_decode_error>;

// One decode() outcome: more input needed, chunk payload bytes borrowed from
// the input, the end of the body, or a framing failure.
using http1_chunk_decode_result = decode_step<decode_need_input, decode_output, http1_chunk_decode_complete, http1_chunk_decode_failure>;

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
// The complete alternative publishes the remaining fields in received order,
// names lowercase and values without surrounding OWS. The decoder owns them:
// storage is allocated only for a section that keeps a field, and the view
// stays valid until the next decode() or reset() call, or until the decoder is
// moved from, assigned to or destroyed.
// A finished or failed decoder consumes no further input: decode() repeats
// the terminal result (a finished one without its trailers) until reset().
class http1_chunked_body_decoder final {
public:
    explicit http1_chunked_body_decoder(http1_chunked_body_decoder_config config = {});
    ~http1_chunked_body_decoder();
    http1_chunked_body_decoder(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder& operator=(const http1_chunked_body_decoder&) = delete;
    http1_chunked_body_decoder(http1_chunked_body_decoder&&) noexcept;
    http1_chunked_body_decoder& operator=(http1_chunked_body_decoder&&) noexcept;

    // At most max_body_bytes of chunk payload are published per call.
    [[nodiscard]] http1_chunk_decode_result decode(std::string_view available, std::size_t max_body_bytes = (std::numeric_limits<std::size_t>::max)());
    template <detail::http_temporary_owning_char_string input_type>
    http1_chunk_decode_result decode(input_type&&, std::size_t = 0) = delete;

    [[nodiscard]] decoder_state state() const noexcept;
    // Starts a new body with the constructed configuration, keeping trailer
    // storage capacity.
    void reset() noexcept;

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

[[nodiscard]] inline http_protocol_error make_http_protocol_error(http1_chunk_decode_error error) noexcept {
    switch (error) {
        case http1_chunk_decode_error::invalid_framing:
            return http_protocol_error(http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
        case http1_chunk_decode_error::body_limit_exceeded:
            return make_http_protocol_error(http_request_body_failure::too_large);
        case http1_chunk_decode_error::framing_limit_exceeded:
            return http_protocol_error(http_status::content_too_large, "chunked_framing_too_large", "request body framing is too large");
    }
    return http_protocol_error(http_status::bad_request, "invalid_chunked_body", "invalid chunked request body");
}

}  // namespace ruvia
