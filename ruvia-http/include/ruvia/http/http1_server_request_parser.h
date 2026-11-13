#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/url_encoding.h"

namespace ruvia {

enum class http_transfer_coding : std::uint8_t { gzip,
    deflate };

// Parsing is bounded to prevent attacker-controlled unbounded state growth.
inline constexpr std::size_t max_transfer_codings = 8;

// A bounded inline sequence. It owns no memory resource, so a parsed framing
// plan may outlive the per-request storage its head was parsed from.
class http_transfer_codings final {
public:
    void push_back(http_transfer_coding coding) {
        if (size_ == max_transfer_codings) {
            throw std::length_error("too many transfer codings");
        }
        data_[size_++] = coding;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] http_transfer_coding* data() & noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    [[nodiscard]] const http_transfer_coding* data() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    http_transfer_coding* data() && = delete;
    const http_transfer_coding* data() const&& = delete;
    [[nodiscard]] http_transfer_coding* begin() & noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    [[nodiscard]] const http_transfer_coding* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data();
    }
    http_transfer_coding* begin() && = delete;
    const http_transfer_coding* begin() const&& = delete;
    [[nodiscard]] http_transfer_coding* end() & noexcept RUVIA_LIFETIMEBOUND {
        return data_.data() + size_;
    }
    [[nodiscard]] const http_transfer_coding* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return data_.data() + size_;
    }
    http_transfer_coding* end() && = delete;
    const http_transfer_coding* end() const&& = delete;
    [[nodiscard]] http_transfer_coding& operator[](std::size_t index) & noexcept RUVIA_LIFETIMEBOUND {
        return data_[index];
    }
    [[nodiscard]] const http_transfer_coding& operator[](std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return data_[index];
    }
    http_transfer_coding& operator[](std::size_t) && = delete;
    const http_transfer_coding& operator[](std::size_t) const&& = delete;

private:
    std::array<http_transfer_coding, max_transfer_codings> data_{};
    std::size_t size_{0};
};
}  // namespace ruvia

namespace ruvia {

enum class http_transfer_coding_decode_error : std::uint8_t {
    invalid_content,
    decoded_size_exceeded,
};
namespace detail {
class transfer_coding_decoder;
}

class http_transfer_coding_decode_need_input final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decode_need_input(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_output_view final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_output_view(std::size_t consumed_bytes, std::string_view bytes_value) noexcept
        : consumed_bytes_(consumed_bytes),
          bytes_(bytes_value) {}
    std::size_t consumed_bytes_;
    std::string_view bytes_;
};

class http_transfer_coding_decode_complete final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decode_complete(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }
    [[nodiscard]] constexpr http_transfer_coding_decode_error error() const noexcept {
        return error_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr http_transfer_coding_decode_failure(std::size_t consumed_bytes, http_transfer_coding_decode_error error) noexcept
        : consumed_bytes_(consumed_bytes),
          error_(error) {}
    std::size_t consumed_bytes_;
    http_transfer_coding_decode_error error_;
};

class http_transfer_coding_decoder_failure final {
public:
    [[nodiscard]] constexpr std::size_t consumed_bytes() const noexcept {
        return consumed_bytes_;
    }

private:
    friend class http_transfer_coding_decode_result;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr http_transfer_coding_decoder_failure(std::size_t consumed_bytes) noexcept
        : consumed_bytes_(consumed_bytes) {}
    std::size_t consumed_bytes_;
};

class http_transfer_coding_decode_result final {
public:
    [[nodiscard]] std::size_t consumed_bytes() const noexcept {
        return std::visit([](const auto& result_value) { return result_value.consumed_bytes(); }, value_);
    }
    [[nodiscard]] const http_transfer_coding_decode_need_input* need_input() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_need_input>(&value_);
    }
    const http_transfer_coding_decode_need_input* need_input() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_output_view* output() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_output_view>(&value_);
    }
    const http_transfer_coding_decode_output_view* output() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_complete* complete() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_complete>(&value_);
    }
    const http_transfer_coding_decode_complete* complete() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decode_failure* failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decode_failure>(&value_);
    }
    const http_transfer_coding_decode_failure* failure() const&& = delete;
    [[nodiscard]] const http_transfer_coding_decoder_failure* decoder_failure() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_transfer_coding_decoder_failure>(&value_);
    }
    const http_transfer_coding_decoder_failure* decoder_failure() const&& = delete;

private:
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    using value_type = std::variant<http_transfer_coding_decode_need_input, http_transfer_coding_decode_output_view, http_transfer_coding_decode_complete, http_transfer_coding_decode_failure, http_transfer_coding_decoder_failure>;
    template <typename result_type>
    explicit http_transfer_coding_decode_result(result_type result_value) noexcept
        : value_(std::move(result_value)) {}
    value_type value_;
};

// The sole transfer-decoding entry point for one or more codings. Copies the
// protocol-order sequence during construction and decodes it in reverse.
// Input is never retained; output views borrow the caller's scratch storage.
// Consume each view before reusing it. Keep the decoder at a stable address
// until destruction. Drain output with decode({}, scratch)
// before requesting more input; finish_input() commits framing EOF only after
// all input/output is drained. Each layer enforces decoded_limit independently.
// A layer that received no coded bytes before framing EOF completes as empty.
// decode/finish_input return typed failures because incremental drivers must
// retain the exact wire consumption even when a layer fails; failure is terminal.
class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(
        std::span<const http_transfer_coding> codings, protocol_byte_limit decoded_limit);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(
        std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;

private:
    struct impl_type;
    impl_type* impl_{nullptr};
};

}  // namespace ruvia

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

}  // namespace ruvia

namespace ruvia {

// A request-body failure detected by a server driver while enforcing the
// HTTP-owned content contract. The driver supplies runtime byte limits and I/O
// completion facts; HTTP owns their final response status and diagnostic.
enum class http_request_body_failure : std::uint8_t {
    too_large,
    incomplete,
    buffer_limit,
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
        case http_request_body_failure::buffer_limit:
            return http_protocol_error(http_status::service_unavailable, "inbound_buffer_limit",
                "inbound buffer capacity exhausted");
    }
    return http_protocol_error(http_status::bad_request, "invalid_request_body", "invalid request body");
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

// Status for a request body whose Content-Encoding could not be decoded.
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
    // The received trailer section, empty when there was none. The fields
    // borrow the decoder that produced this result (see
    // http1_chunked_body_decoder).
    [[nodiscard]] constexpr std::span<const http_header> trailers() const& noexcept {
        return trailers_;
    }
    std::span<const http_header> trailers() const&& = delete;

private:
    friend class http1_chunk_decode_result;
    explicit constexpr http1_chunk_decode_complete_view(
        std::size_t consumed_bytes, std::span<const http_header> trailers) noexcept
        : consumed_bytes_(consumed_bytes),
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
        std::size_t consumed_bytes, std::span<const http_header> trailers = {}) noexcept;
    [[nodiscard]] static http1_chunk_decode_result make_failure(
        std::size_t consumed_bytes, http1_chunk_decode_error error) noexcept;
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

}  // namespace ruvia

namespace ruvia {

class http1_server_request_parse_state;

// Whether the runtime consumed the complete request message before attempting
// to reuse its transport. This is a protocol lifecycle fact, not a product
// policy bool: RFC 9112 section 9.3 forbids reuse with unread request content.
enum class http1_request_body_consumption : std::uint8_t { complete,
    incomplete };

class http1_request_without_body final {
private:
    friend class http1_request_body_plan;
    constexpr http1_request_without_body() noexcept = default;
};

class http1_known_length_request_body final {
public:
    [[nodiscard]] constexpr std::size_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_request_body_plan;

    explicit constexpr http1_known_length_request_body(std::size_t content_length) noexcept
        : content_length_(content_length) {}

    std::size_t content_length_;
};

class http1_chunked_request_body final {
public:
    [[nodiscard]] const http_transfer_codings& transfer_codings() const& noexcept RUVIA_LIFETIMEBOUND {
        return transfer_codings_;
    }
    const http_transfer_codings& transfer_codings() const&& = delete;

private:
    friend class http1_request_body_plan;

    explicit http1_chunked_request_body(http_transfer_codings transfer_codings)
        : transfer_codings_(std::move(transfer_codings)) {}

    http_transfer_codings transfer_codings_;
};

// Immutable framing contract produced only by the HTTP/1 parser and consumed
// by any runtime driver. Content-Length belongs only to the known-length
// alternative; transfer-coding order belongs only to the final-chunked
// alternative. A caller cannot synthesize a plan that bypasses wire validation.
class http1_request_body_plan final {
public:
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_request_without_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_request_without_body* without_body() const&& = delete;

    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_known_length_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_known_length_request_body* known_length() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http1_chunked_request_body>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_request_body* chunked() const&& = delete;

    // Chunked framing requires consuming the terminating zero chunk even when
    // the decoded content is empty.
    [[nodiscard]] constexpr bool requires_consumption() const noexcept {
        if (const auto* known = known_length()) {
            return known->content_length() != 0;
        }
        return chunked() != nullptr;
    }

    // The consumption a response must record when the runtime never read the
    // request body: incomplete only if there is anything to read.
    [[nodiscard]] constexpr http1_request_body_consumption consumption_if_unread() const noexcept {
        return requires_consumption() ? http1_request_body_consumption::incomplete
                                      : http1_request_body_consumption::complete;
    }

    [[nodiscard]] http_request_expectations expectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] http_server_expectation_plan expectation_plan(
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        return expectations_.server_plan(requires_consumption()
                                             ? http_request_content_indication::will_follow
                                             : http_request_content_indication::no_content,
            unsupported_policy);
    }

private:
    friend class http1_server_request_parse_state;

    using framing_type =
        std::variant<http1_request_without_body, http1_known_length_request_body, http1_chunked_request_body>;

    explicit http1_request_body_plan(http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_request_without_body()), expectations) {}

    http1_request_body_plan(std::size_t content_length, http_request_expectations expectations) noexcept
        : http1_request_body_plan(framing_type(http1_known_length_request_body(content_length)), expectations) {}

    http1_request_body_plan(
        http_transfer_codings transfer_codings, http_request_expectations expectations)
        : http1_request_body_plan(framing_type(http1_chunked_request_body(std::move(transfer_codings))), expectations) {}

    http1_request_body_plan(framing_type framing, http_request_expectations expectations)
        : framing_(std::move(framing)),
          expectations_(expectations) {}

    framing_type framing_;
    http_request_expectations expectations_;
};

}  // namespace ruvia

namespace ruvia {

// Whether a connection is reused after a completed HTTP/1 exchange. Both the
// client request writer and the server response planner express the same wire
// decision with this single shared policy: allow_reuse keeps the connection
// poolable, close_after_response emits Connection: close and closes it.
enum class http1_close_policy : std::uint8_t {
    allow_reuse,
    close_after_response,
};
// Immutable request-side HTTP/1 persistence contract. Parsing establishes the
// version and initial disposition; later body-consumption policy may only close.
class http1_request_connection_plan final {
public:
    [[nodiscard]] static constexpr http1_request_connection_plan http11_close() noexcept {
        return {http_protocol_version::http11, http1_close_policy::close_after_response, false};
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return version_;
    }
    [[nodiscard]] constexpr http1_close_policy disposition() const noexcept {
        return disposition_;
    }
    [[nodiscard]] constexpr http1_request_connection_plan require_close() const noexcept {
        return {version_, http1_close_policy::close_after_response, tunnel_request_};
    }

    // A CONNECT request. A 2xx final response turns the connection into a
    // tunnel; any other final response must close it, because a client may
    // already have sent tunnel bytes optimistically (RFC 9110 9.3.6) and they
    // must never be parsed as further requests.
    [[nodiscard]] constexpr bool tunnel_request() const noexcept {
        return tunnel_request_;
    }
    [[nodiscard]] constexpr http1_request_connection_plan with_tunnel_request() const noexcept {
        return {version_, disposition_, true};
    }

private:
    friend constexpr http1_request_connection_plan plan_http10_request_connection(bool, bool) noexcept;
    friend constexpr http1_request_connection_plan plan_http11_request_connection(bool) noexcept;

    constexpr http1_request_connection_plan(http_protocol_version version,
        http1_close_policy disposition, bool tunnel_request) noexcept
        : version_(version),
          disposition_(disposition),
          tunnel_request_(tunnel_request) {}

    http_protocol_version version_;
    http1_close_policy disposition_;
    bool tunnel_request_;
};

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http10_request_connection(
    bool close, bool keep_alive) noexcept {
    return {http_protocol_version::http10,
        !close && keep_alive ? http1_close_policy::allow_reuse : http1_close_policy::close_after_response, false};
}

[[nodiscard]] inline constexpr http1_request_connection_plan plan_http11_request_connection(
    bool close) noexcept {
    return {http_protocol_version::http11,
        close ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse, false};
}

[[nodiscard]] inline constexpr http1_request_connection_plan apply_request_body_consumption(
    http1_request_connection_plan plan, http1_request_body_consumption consumption) noexcept {
    return consumption == http1_request_body_consumption::complete ? plan : plan.require_close();
}

}  // namespace ruvia

namespace ruvia {

enum class http1_server_request_parse_failure_source : std::uint8_t { request_line,
    message };

// Cleartext listeners can silently discard inputs whose request-line failure
// shows the peer was not speaking HTTP (for example, TLS bytes on an HTTP
// port). Input whose first line (after ignored empty lines; the available
// bytes while no CRLF has arrived) has a whitespace-separated word naming an
// HTTP version ("HTTP/" in any case, or token "/" DIGITs "." DIGITs) is HTTP
// and is answered with 400 (RFC 9112 sections 2.2 and 3). Otherwise input is
// dropped once its first line is CRLF-terminated, or at once when its first
// byte cannot begin a method token.
[[nodiscard]] bool should_drop_invalid_cleartext_http1_input(
    std::string_view buffer, http1_server_request_parse_failure_source source_value) noexcept;

class http1_server_need_request_head final {
private:
    friend class http1_server_request_parse_state;

    // Resumable scan progress: leading empty-line bytes already skipped and,
    // once accepted, the end of the request-line. Each input byte is thereby
    // classified once across repeated parse_head calls.
    std::size_t leading_bytes_{0};
    std::size_t request_line_end_{0};
};

class http1_server_request_head_ready final {
public:
    [[nodiscard]] constexpr std::size_t header_bytes() const noexcept {
        return header_bytes_;
    }

private:
    friend class http1_server_request_parse_state;

    explicit constexpr http1_server_request_head_ready(std::size_t header_bytes) noexcept
        : header_bytes_(header_bytes) {
        if (header_bytes_ == 0) {
            std::terminate();
        }
    }

    std::size_t header_bytes_;
};

class http1_server_request_parse_failure final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        return http_parse_protocol_error(error_);
    }

    [[nodiscard]] constexpr http1_server_request_parse_failure_source source() const noexcept {
        return error_ == http_parse_error::invalid_request_line ||
                       error_ == http_parse_error::unsupported_http_version
                   ? http1_server_request_parse_failure_source::request_line
                   : http1_server_request_parse_failure_source::message;
    }

private:
    friend class http1_server_request_parse_state;

    explicit constexpr http1_server_request_parse_failure(http_parse_error error) noexcept
        : error_(error) {}

    http_parse_error error_;
};

class http1_server_request_parse_state final {
public:
    http1_server_request_parse_state() noexcept;

    // 分配失败以 std::bad_alloc 传出且不发布 head_ready/成功状态；释放资源后可用同一 buffer 与 offset 重试，但部分元数据可能残留。
    void parse_head(std::string_view buffer, std::size_t header_search_offset = 0);

    template <detail::http_temporary_owning_char_string buffer_type>
    void parse_head(buffer_type&&, std::size_t = 0) = delete;
    [[nodiscard]] const http1_server_need_request_head* need_request_head() const& noexcept {
        return std::get_if<http1_server_need_request_head>(&progress_);
    }
    [[nodiscard]] const http1_server_need_request_head* need_request_head() const&& = delete;

    [[nodiscard]] const http1_server_request_head_ready* head_ready() const& noexcept {
        return std::get_if<http1_server_request_head_ready>(&progress_);
    }
    [[nodiscard]] const http1_server_request_head_ready* head_ready() const&& = delete;

    [[nodiscard]] const http1_server_request_parse_failure* failure() const& noexcept {
        return std::get_if<http1_server_request_parse_failure>(&progress_);
    }
    [[nodiscard]] const http1_server_request_parse_failure* failure() const&& = delete;

    // The request, body plan and connection plan parse_head() derived from
    // its last input. A failure keeps the accepted request version and a
    // connection plan that requires close; an incomplete head keeps an empty
    // request.
    [[nodiscard]] const http_request& request() const& noexcept RUVIA_LIFETIMEBOUND {
        return request_;
    }
    const http_request& request() const&& = delete;
    [[nodiscard]] const http1_request_body_plan& body_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_plan_;
    }
    const http1_request_body_plan& body_plan() const&& = delete;
    [[nodiscard]] const http1_request_connection_plan& connection_plan() const& noexcept RUVIA_LIFETIMEBOUND {
        return connection_plan_;
    }
    const http1_request_connection_plan& connection_plan() const&& = delete;

    // Each extraction moves one result out and leaves the default value
    // behind; the state then serves only parse_head() or destruction. Taking
    // the request releases its descriptor storage.
    [[nodiscard]] http_request take_request() && noexcept;
    [[nodiscard]] http1_request_body_plan take_body_plan() && noexcept;
    [[nodiscard]] http1_request_connection_plan take_connection_plan() && noexcept;

private:
    http_request request_;
    http1_request_body_plan body_plan_{http1_request_body_plan(http_request_expectations{})};
    http1_request_connection_plan connection_plan_{http1_request_connection_plan::http11_close()};
    // The compact request owns its descriptor block across parse attempts.
    // Only this small progress value changes, so scan/head/error metadata
    // exists solely in the alternative where it is meaningful.
    using progress = std::variant<http1_server_need_request_head, http1_server_request_head_ready,
        http1_server_request_parse_failure>;

    progress progress_{http1_server_need_request_head{}};
};

}  // namespace ruvia
