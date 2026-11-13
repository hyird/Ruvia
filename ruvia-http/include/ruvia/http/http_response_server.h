#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
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

namespace ruvia {

// An HTTP/1 response head this endpoint cannot send: an interim, final,
// CONNECT or CONNECT-UDP head that breaks a message rule (RFC 9110, RFC 9112)
// or the head size bound. It reports an application or runtime bug, never a
// peer error, so a server answers it like any other handler failure.
class http1_response_head_error final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

}  // namespace ruvia

namespace ruvia {

inline constexpr std::size_t response_head_stack_bytes = 512;
inline constexpr std::size_t response_head_retained_heap_bytes = std::size_t{4} * 1024;

// Reusable scratch storage for serialized HTTP response heads. Keeps small heads
// inline and retains only bounded heap capacity across requests.
class http_response_head_buffer final {
    struct stack_state_type final {
        std::size_t used_{0};
    };
    struct heap_state_type final {};

public:
    http_response_head_buffer() noexcept = default;

    http_response_head_buffer(const http_response_head_buffer& other);
    http_response_head_buffer(http_response_head_buffer&& other) noexcept;
    http_response_head_buffer& operator=(const http_response_head_buffer& other);
    http_response_head_buffer& operator=(http_response_head_buffer&& other) noexcept;
    void reset() noexcept;
    // value may alias storage, including a written range returned by stack_cursor().
    void append(std::string_view value);
    void append(char value);
    void append_unsigned(std::uint64_t value);
    // Reserve beyond the current size without spilling a fitting inline range.
    void reserve_additional(std::size_t size);
    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view view() const&& = delete;
    [[nodiscard]] bool can_append_on_stack(std::size_t size) const noexcept;

    template <typename writer_type>
        requires std::is_nothrow_invocable_r_v<void, writer_type&, char*>
    void append_generated(std::size_t size, writer_type&& writer) {
        if (char* cursor_value = stack_cursor(size); cursor_value != nullptr) {
            writer(cursor_value);
            commit_stack(cursor_value + size);
            return;
        }
        reserve_additional(size);
        const auto previous_size = heap_.size();
        heap_.resize(previous_size + size);
        writer(heap_.data() + previous_size);
    }

    // Returns uncommitted inline scratch at the current end if bound fits.
    // This is a capacity check, not a reservation; the buffer must stay alive
    // and unmoved, and intervening mutations may change the logical end.
    [[nodiscard]] char* stack_cursor(std::size_t bound) & noexcept RUVIA_LIFETIMEBOUND {
        auto* const stack_state = std::get_if<stack_state_type>(&state_);
        if (stack_state == nullptr || bound > stack_.size() - stack_state->used_) {
            return nullptr;
        }
        return stack_.data() + stack_state->used_;
    }
    [[nodiscard]] char* stack_cursor(std::size_t) && = delete;

    // Sets the inline length to end, including the array's one-past endpoint.
    // This may shorten existing content. A spilled buffer throws logic_error;
    // a null or foreign endpoint throws invalid_argument. Rejection preserves
    // the committed content. No pointer subtraction is performed on a foreign
    // array, even when an endpoint was taken from another live buffer.
    void commit_stack(const char* end) {
        auto* const stack_state = std::get_if<stack_state_type>(&state_);
        if (stack_state == nullptr) {
            throw std::logic_error("HTTP response head no longer uses inline storage");
        }
        const auto begin_address = reinterpret_cast<std::uintptr_t>(stack_.data());
        const auto end_address = reinterpret_cast<std::uintptr_t>(end);
        const auto offset = end_address - begin_address;
        if (end == nullptr || end_address < begin_address || offset > stack_.size()) {
            throw std::invalid_argument("HTTP response head endpoint is outside its inline storage");
        }
        stack_state->used_ = static_cast<std::size_t>(offset);
    }

private:
    void spill_to_heap(std::size_t min_capacity);
    void copy_committed_state(const http_response_head_buffer& other) noexcept;
    std::array<char, response_head_stack_bytes> stack_{};
    std::string heap_;
    std::variant<stack_state_type, heap_state_type> state_{stack_state_type{}};
};

}  // namespace ruvia

namespace ruvia {

class http1_response_head_plan;
class http1_buffered_response_plan;

// A complete buffered representation is length-delimited by the response writer.
class http1_buffered_response_head final {
public:
    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return content_length_;
    }

private:
    friend class http1_response_head_plan;

    explicit constexpr http1_buffered_response_head(std::uint64_t content_length) noexcept
        : content_length_(content_length) {}

    std::uint64_t content_length_{0};
};

// The response writer owns canonical Transfer-Encoding: chunked and the runtime
// emits the matching chunk frames. Application headers cannot redefine framing.
class http1_chunked_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_chunked_response_stream_head() noexcept = default;
};

// The response has no declared message-body length. When content is allowed, the
// runtime closes the connection to delimit it; application Content-Length and
// Transfer-Encoding fields therefore cannot survive into the wire head.
class http1_close_delimited_response_stream_head final {
private:
    friend class http1_response_head_plan;

    constexpr http1_close_delimited_response_stream_head() noexcept = default;
};

// Final HTTP/1 head framing is an exclusive protocol value, not the old
// "suppress automatic Content-Length" boolean. The body plan keeps method/status
// content semantics attached to the exact wire-framing alternative selected by
// the HTTP/1 planner.
class http1_response_head_plan final {
public:
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const& noexcept {
        return std::get_if<http1_buffered_response_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_buffered_response_head* buffered() const&& = delete;

    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const& noexcept {
        return std::get_if<http1_chunked_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_chunked_response_stream_head* chunked_stream() const&& = delete;

    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const& noexcept {
        return std::get_if<http1_close_delimited_response_stream_head>(&framing_);
    }
    [[nodiscard]] constexpr const http1_close_delimited_response_stream_head* close_delimited_stream()
        const&& = delete;

    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;
    friend constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
        http_response_body_plan, http1_request_connection_plan) noexcept;

    using framing_type = std::variant<http1_buffered_response_head,
        http1_chunked_response_stream_head, http1_close_delimited_response_stream_head>;

    [[nodiscard]] static constexpr framing_type buffered_framing(std::uint64_t content_length) noexcept {
        return framing_type(http1_buffered_response_head(content_length));
    }

    [[nodiscard]] static constexpr framing_type chunked_stream_framing() noexcept {
        return framing_type(http1_chunked_response_stream_head());
    }

    [[nodiscard]] static constexpr framing_type close_delimited_stream_framing() noexcept {
        return framing_type(http1_close_delimited_response_stream_head());
    }

    constexpr http1_response_head_plan(http_response_body_plan body_plan,
        http_protocol_version protocol_version, framing_type framing) noexcept
        : body_plan_(body_plan),
          protocol_version_(protocol_version),
          framing_(framing) {}

    http_response_body_plan body_plan_;
    http_protocol_version protocol_version_;
    framing_type framing_;
};

[[nodiscard]] constexpr http1_response_head_plan http1_chunked_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(
        body_plan, connection_plan.protocol_version(), http1_response_head_plan::chunked_stream_framing());
}

[[nodiscard]] constexpr http1_response_head_plan http1_close_delimited_response_stream_head_plan(
    http_response_body_plan body_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_response_head_plan(body_plan, connection_plan.protocol_version(),
        http1_response_head_plan::close_delimited_stream_framing());
}

// The runtime writes one inseparable HTTP/1 buffered response contract. The
// embedded head owns method/status/body semantics and representation length
// exactly once; direct fact access avoids restoring a parallel write-plan copy.
class http1_buffered_response_plan final {
public:
    [[nodiscard]] constexpr http_response_body_plan body_plan() const noexcept {
        return head_plan_.body_plan();
    }

    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan().response_status();
    }

    [[nodiscard]] constexpr std::uint64_t content_length() const noexcept {
        return head_plan_.buffered()->content_length();
    }

    [[nodiscard]] bool send_body() const noexcept {
        return detail::http_buffered_response_sends_body(body_plan(), content_length());
    }

    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const& noexcept {
        return head_plan_;
    }
    [[nodiscard]] constexpr const http1_response_head_plan& head_plan() const&& = delete;

private:
    friend http1_buffered_response_plan get_http1_buffered_response_plan(
        http_buffered_response_write_plan, http1_request_connection_plan) noexcept;

    explicit constexpr http1_buffered_response_plan(http1_response_head_plan head_plan) noexcept
        : head_plan_(head_plan) {}

    http1_response_head_plan head_plan_;
};

static_assert(std::is_trivially_copyable_v<http1_buffered_response_plan>);
static_assert(sizeof(http1_buffered_response_plan) == sizeof(http1_response_head_plan));

[[nodiscard]] inline http1_buffered_response_plan get_http1_buffered_response_plan(
    http_buffered_response_write_plan write_plan, http1_request_connection_plan connection_plan) noexcept {
    return http1_buffered_response_plan(
        http1_response_head_plan(write_plan.body_plan(), connection_plan.protocol_version(),
            http1_response_head_plan::buffered_framing(write_plan.content_length())));
}

}  // namespace ruvia

// Stable, non-detail entry points for HTTP response serialization plans. The
// plan implementations remain shared with the HTTP/1 and HTTP/2 protocol code.

namespace ruvia {

namespace detail {

[[nodiscard]] bool is_forbidden_response_trailer_name(std::string_view name) noexcept;
[[nodiscard]] bool is_forbidden_http_binary_response_field(std::string_view name) noexcept;

}  // namespace detail

class http_response_trailer_section;

// Borrowed proof that the complete terminal section passed the shared response-
// trailer rules. The source span must outlive its synchronous consumption.
class http_response_trailer_section final {
public:
    [[nodiscard]] std::span<const http_header_view> fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return fields_.empty();
    }

private:
    friend http_response_trailer_section validate_http_response_trailers(
        std::span<const http_header_view>);
    explicit http_response_trailer_section(std::span<const http_header_view> fields_value) noexcept
        : fields_(fields_value) {}

    std::span<const http_header_view> fields_;
};

// Serialize a finalized HTTP/1 response head into caller-owned reusable scratch.
// The plan must describe the same response representation.
// Canonical Content-Length is measured and emitted from the same decimal token.
// An explicit Date is preserved without consulting the clock; otherwise a
// cached current Date is generated when a usable clock is available. A head
// that breaks a message rule or the head size bound throws
// http1_response_head_error. The plan owns framing: an application
// Transfer-Encoding is accepted only as exactly `chunked` (OWS and ASCII case
// aside) and is replaced by the plan's framing; any other value throws.
void append_http1_response_head(
    const http_response& response, http_response_head_buffer& head,
    const http1_response_head_plan& plan);

// Append only field lines; chunk delimiters belong to the caller. Inputs may
// alias output. Allocation failure leaves the existing output bytes unchanged.
void append_http1_response_trailers(
    std::string& output, const http_response_trailer_section& trailers);

// Validate a complete response trailer section and return a borrowed proof for
// synchronous protocol submission. The input storage must outlive its use.
[[nodiscard]] http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers);

template <detail::http_temporary_owning_range<http_header_view> headers_type>
http_response_trailer_section validate_http_response_trailers(headers_type&&) = delete;
http_response_trailer_section validate_http_response_trailers(
    std::initializer_list<http_header_view>) = delete;

}  // namespace ruvia

namespace ruvia {

// Immutable, bodyless response-head view for 1xx progress messages that precede
// a final response. 101 is deliberately excluded: switching protocols transfers
// connection ownership and therefore requires its dedicated protocol driver.
// Header elements and their strings are borrowed and must remain stable through
// the synchronous sans-I/O submit call.
class http_interim_response_head final {
public:
    explicit http_interim_response_head(http_status_code status_code, borrowed_http_headers headers = {});

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_code_;
    }

    [[nodiscard]] constexpr std::span<const http_header_view> headers() const noexcept {
        return headers_;
    }

private:
    http_status_code status_code_;
    std::span<const http_header_view> headers_;
};

// Connection lifecycle after the encoded interim response. RFC 9112 requires
// a sender of Connection: close to begin closing after the response containing
// that option, even though doing so leaves the request without a final response.
enum class http1_interim_connection_disposition : std::uint8_t {
    unchanged,
    close_after_interim_response,
};

class http1_interim_response_buffer_too_small final {
public:
    [[nodiscard]] constexpr std::size_t required_head_bytes() const noexcept {
        return required_head_bytes_;
    }

private:
    friend std::variant<class prepared_http1_interim_response, http1_interim_response_buffer_too_small>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

    explicit constexpr http1_interim_response_buffer_too_small(std::size_t required_head_bytes) noexcept
        : required_head_bytes_(required_head_bytes) {}

    std::size_t required_head_bytes_;
};

// A transactionally encoded HTTP/1.1 interim head. The byte view points into
// the caller's output buffer. When Connection: close is present, the owner must
// initiate connection closure as soon as this interim head has been written.
class prepared_http1_interim_response final {
public:
    [[nodiscard]] constexpr std::string_view head() const noexcept {
        return head_;
    }

    [[nodiscard]] constexpr http1_interim_connection_disposition connection_disposition()
        const noexcept {
        return connection_disposition_;
    }

private:
    friend std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
    prepare_http1_interim_response_head(const http_interim_response_head&, std::span<char>);

    constexpr prepared_http1_interim_response(
        std::string_view head, http1_interim_connection_disposition connection_disposition) noexcept
        : head_(head),
          connection_disposition_(connection_disposition) {}

    std::string_view head_;
    http1_interim_connection_disposition connection_disposition_;
};

// Allocation-free HTTP/1.1 interim-head writer. It validates the complete
// borrowed field set and exact 64-field/64-KiB bounds before touching the
// output buffer, throwing http1_response_head_error for a head it cannot send.
// No Server or Date field is injected: the encoded fields are exactly those
// represented by http_interim_response_head.
[[nodiscard]] std::variant<prepared_http1_interim_response, http1_interim_response_buffer_too_small>
prepare_http1_interim_response_head(const http_interim_response_head& response, std::span<char> head_buffer);

}  // namespace ruvia

namespace ruvia {

enum class http_response_stream_framing : std::uint8_t {
    http1_chunked,
    // RFC 9112 6.1: a server MUST NOT send Transfer-Encoding to a client that did
    // not indicate HTTP/1.1. An HTTP/1.0 stream therefore carries no chunk framing;
    // the body is delimited by the connection close (RFC 9112 6.3), so it also
    // announces Connection: close and the session shuts the socket afterwards.
    http1_close_delimited,
    http2_frames,
    // RFC 9114 §4.1: HTTP/3 delimits content with DATA frames and carries a
    // trailer section in a trailing HEADERS frame, exactly like HTTP/2.
    http3_frames
};
// A trailer section is terminal message metadata, not an independently queued
// side channel. The caller declares whether this head commit is reserving a
// terminal trailer section. Trailers are optional metadata a recipient may
// discard (RFC 9110 6.5), so framing that cannot carry them (HTTP/1.0
// close-delimited, known length, or a suppressed body) drops them; only a
// status that can never carry content rejects the intent before the head.
enum class http_response_trailer_intent : std::uint8_t { none,
    present };

// Whether a validated trailer section commits the response to sending trailers.
// An empty section is not "no decision": it is the decision to send none, which
// the head must state before any body byte goes out.
[[nodiscard]] inline http_response_trailer_intent response_trailer_intent(
    const http_response_trailer_section& section) noexcept {
    return section.empty() ? http_response_trailer_intent::none : http_response_trailer_intent::present;
}

// Authoritative phase immediately after the initial response head is submitted.
// Body-suppressed responses (HEAD / 304) complete at the initial head
// (message_ended) and discard trailers.
enum class http_response_stream_head_disposition : std::uint8_t { body_open,
    message_ended };

class http_response_stream_commit_plan final {
public:
    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan_.response_status();
    }

    [[nodiscard]] http_response_stream_framing framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // HTTP/1.1 requires chunked framing for trailers (RFC 9112 7.1.2).
    // HTTP/2 and HTTP/3 carry them in a trailing field section. A body-suppressed
    // response (HEAD / 304) ends at its initial head and discards trailers.
    [[nodiscard]] bool trailers_available() const noexcept {
        return !body_plan_.body_suppressed() &&
               (framing_ == http_response_stream_framing::http1_chunked ||
                   framing_ == http_response_stream_framing::http2_frames ||
                   framing_ == http_response_stream_framing::http3_frames);
    }

    [[nodiscard]] http_response_stream_head_disposition head_disposition() const noexcept {
        return body_plan_.body_suppressed() ? http_response_stream_head_disposition::message_ended
                                            : http_response_stream_head_disposition::body_open;
    }

    // A status that can never carry content (1xx/204/205/304) has no message
    // body to terminate with a trailer section; asking for one is a handler
    // error. Every other unavailable framing discards the trailers instead.
    [[nodiscard]] bool status_permits_trailers() const noexcept {
        return body_plan_.status_allows_body();
    }

    [[nodiscard]] bool trailer_intent_allowed() const noexcept {
        return !trailer_intent_present_ || status_permits_trailers();
    }

private:
    friend http_response_stream_commit_plan plan_http_response_stream_commit(
        http_response_stream_framing, http_known_method, http_status_code, http_response_trailer_intent) noexcept;

    http_response_stream_commit_plan(http_response_stream_framing framing, http_response_body_plan body_plan,
        bool trailer_intent_present) noexcept
        : framing_(framing),
          body_plan_(body_plan),
          trailer_intent_present_(trailer_intent_present) {}

    http_response_stream_framing framing_{http_response_stream_framing::http1_chunked};
    http_response_body_plan body_plan_;
    bool trailer_intent_present_{false};
};

[[nodiscard]] inline http_response_stream_commit_plan plan_http_response_stream_commit(
    http_response_stream_framing framing, http_known_method request_method, http_status_code response_status,
    http_response_trailer_intent trailer_intent) noexcept {
    const auto body_plan = plan_http_response_body(request_method, response_status);
    return http_response_stream_commit_plan(framing, body_plan,
        trailer_intent == http_response_trailer_intent::present);
}

// Preparation normalizes the initial response metadata to the supplied commit
// plan. Mutating the response afterward can invalidate that relationship; this
// object is not a permanent proof over a mutable response.
class http_response_stream_head final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return response_;
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return response_;
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return commit_plan_;
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

private:
    friend http_response_stream_head prepare_http_response_stream_head(
        http_response, http_response_stream_commit_plan);

    http_response_stream_head(http_response response, http_response_stream_commit_plan commit_plan)
        : response_(std::move(response)),
          commit_plan_(commit_plan) {}

    http_response response_;
    http_response_stream_commit_plan commit_plan_;
};

// An application Transfer-Encoding must be exactly the `chunked` coding (OWS
// and ASCII case aside): it is a hint the framework's writer replaces with its
// own framing. Any other value throws http1_response_head_error for an HTTP/1
// framing and std::invalid_argument for HTTP/2 or HTTP/3 before the response
// changes.
[[nodiscard]] http_response_stream_head prepare_http_response_stream_head(
    http_response response, http_response_stream_commit_plan commit_plan);
}  // namespace ruvia

namespace ruvia {

class http1_response_stream_plan final {
public:
    [[nodiscard]] http_response_stream_framing framing() const noexcept {
        return framing_;
    }

    [[nodiscard]] http1_request_connection_plan request_connection_plan() const noexcept {
        return request_connection_plan_;
    }

    [[nodiscard]] http1_close_policy close_policy() const noexcept {
        return close_policy_;
    }

    [[nodiscard]] http_known_method request_method() const noexcept {
        return request_method_;
    }

private:
    friend http1_response_stream_plan http1_plan_response_stream(
        const http1_server_request_parse_state&, http1_close_policy, http1_request_body_consumption) noexcept;

    http1_response_stream_plan(http_response_stream_framing framing,
        http1_request_connection_plan request_connection_plan, http1_close_policy close_policy,
        http_known_method request_method) noexcept
        : framing_(framing),
          request_connection_plan_(request_connection_plan),
          close_policy_(close_policy),
          request_method_(request_method) {}

    http_response_stream_framing framing_;
    http1_request_connection_plan request_connection_plan_;
    http1_close_policy close_policy_{http1_close_policy::close_after_response};
    http_known_method request_method_{http_known_method::unknown};
};

// Pure HTTP/1 response-stream planning. The runtime contributes only its typed
// product policy (for example, a per-connection request limit) and the request
// body's consumption state; HTTP retains the request disposition and candidate
// framing until the response status is known. Commit can therefore distinguish
// a body-allowed HTTP/1.0 stream, which requires close delimiting, from a
// body-suppressed response that is already self-delimited. A route that consumed
// the complete request body before streaming passes `complete`; the parser's
// body plan still describes the original request framing, so planning for an
// unread body passes `body_plan_.consumption_if_unread()`.
[[nodiscard]] inline http1_response_stream_plan http1_plan_response_stream(
    const http1_server_request_parse_state& parsed_value, http1_close_policy close_policy,
    http1_request_body_consumption consumption) noexcept {
    const auto framing = parsed_value.request().protocol_version() == http_protocol_version::http11
                             ? http_response_stream_framing::http1_chunked
                             : http_response_stream_framing::http1_close_delimited;
    return http1_response_stream_plan(framing,
        apply_request_body_consumption(parsed_value.connection_plan(), consumption), close_policy,
        parsed_value.request().known_method());
}

enum class http1_connection_close_field_policy : std::uint8_t { close_only,
    preserve_upgrade };

inline void http1_mark_connection_close(http_response& response,
    http1_connection_close_field_policy field_policy = http1_connection_close_field_policy::close_only) {
    // A runtime close verdict dominates keep-alive. Collapse repeated fields
    // after the socket lifecycle is decided, while preserving the Upgrade
    // option required by any retained Upgrade field and any custom hop-by-hop
    // field names declared in Connection (RFC 9110 section 7.6.1).
    const std::string_view base =
        field_policy == http1_connection_close_field_policy::preserve_upgrade ? "close, Upgrade"
                                                                              : "close";
    std::string merged;
    for (const auto& header : response.headers()) {
        if (!http_ascii_equals_ignore_case(header.name(), "Connection")) {
            continue;
        }
        detail::http_visit_comma_separated_quoted(header.value(), [&merged, base](std::string_view option) {
            if (!http_ascii_equals_ignore_case(option, "close") &&
                !http_ascii_equals_ignore_case(option, "keep-alive") &&
                !http_ascii_equals_ignore_case(option, "Upgrade")) {
                if (merged.empty()) {
                    merged.assign(base);
                }
                merged.append(", ");
                merged.append(option);
            }
            return true;
        });
    }
    if (merged.empty()) {
        response.header_stable_view("Connection", base);
    } else {
        response.header("Connection", std::move(merged));
    }
}

class prepared_http1_response_stream;

// Commit response-side HTTP/1 persistence after the runtime has folded in
// request-body completion and server policy. This is the sole protocol mutation:
// it honors an application-provided Connection: close and emits the version-
// appropriate Connection field. Success retains the exact request version for
// head serialization; a head this endpoint cannot send (for example a 426
// without an Upgrade protocol, or a Transfer-Encoding other than exactly
// `chunked`) throws http1_response_head_error before the response changes.
[[nodiscard]] http1_request_connection_plan http1_commit_final_response(
    http_response& response, http1_request_connection_plan plan);

// Commit-time result. This object combines request/version/runtime constraints with
// the response method/status and Connection options, then binds the exact header
// bytes to the authoritative connection disposition.
class prepared_http1_response_stream final {
public:
    [[nodiscard]] http_response& response() & noexcept {
        return head_.response();
    }
    [[nodiscard]] http_response& response() && = delete;

    [[nodiscard]] const http_response& response() const& noexcept {
        return head_.response();
    }
    [[nodiscard]] const http_response& response() const&& = delete;

    [[nodiscard]] const http1_response_head_plan& response_head_plan() const& noexcept {
        return response_head_plan_;
    }
    [[nodiscard]] const http1_response_head_plan& response_head_plan() const&& = delete;

    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const& noexcept {
        return head_.commit_plan();
    }
    [[nodiscard]] const http_response_stream_commit_plan& commit_plan() const&& = delete;

    [[nodiscard]] http1_request_connection_plan connection_plan() const noexcept {
        return connection_plan_;
    }

private:
    friend prepared_http1_response_stream prepare_http1_response_stream_head(
        http_response, const http1_response_stream_plan&, http_response_trailer_intent);

    prepared_http1_response_stream(http_response_stream_head head, http1_response_head_plan response_head_plan,
        http1_request_connection_plan connection_plan) noexcept
        : head_(std::move(head)),
          response_head_plan_(response_head_plan),
          connection_plan_(connection_plan) {}

    http_response_stream_head head_;
    http1_response_head_plan response_head_plan_;
    http1_request_connection_plan connection_plan_;
};

[[nodiscard]] prepared_http1_response_stream prepare_http1_response_stream_head(
    http_response response, const http1_response_stream_plan& plan,
    http_response_trailer_intent trailer_intent = http_response_trailer_intent::none);

// The successful head ends HTTP framing and transfers the connection to a
// duplex byte stream. Payload belongs to that stream, not to an HTTP response.
// A 2xx head with content or framing fields throws http1_response_head_error.
[[nodiscard]] http1_response_head_plan prepare_http1_connect_response_head(
    const http_response& response, http_protocol_version version);

// RFC 9110 §7.8: whether an HTTP/1.1 `request` asks to switch protocols -- an
// Upgrade field offering at least one protocol, sent with the Upgrade
// connection option. Any other request's Upgrade field is ignored, as is a
// CONNECT's: its 2xx establishes a tunnel instead.
[[nodiscard]] bool http1_request_offers_upgrade(const http_request& request) noexcept;

// Makes `response` -- a head without content or framing fields -- the 101
// (Switching Protocols) head after which an HTTP/1.1 connection carries the
// protocol it names (RFC 9110 §7.8, §15.2.2): the response's own Upgrade
// field, else the first protocol `request` offered, with the Upgrade
// connection option added when missing. A request that offers no upgrade, a
// protocol it did not offer, content or framing fields throw
// http1_response_head_error before `response` changes.
[[nodiscard]] http1_response_head_plan prepare_http1_upgrade_response_head(
    http_response& response, const http_request& request);

}  // namespace ruvia

namespace ruvia {
// Already-expanded proxy URI path, borrowed until open_udp_tunnel() returns.
struct http_client_udp_tunnel_request_view final {
    borrowed_text target_{};
    borrowed_http_headers headers_{};
};

struct http_connect_udp_target_view final {
    borrowed_text host_{};
    port_number port_{};
};
struct http_connect_udp_target final {
    http_connect_udp_target() noexcept = default;
    http_connect_udp_target(std::string host, port_number port = {}) noexcept;
    http_connect_udp_target(const http_connect_udp_target&) = default;
    http_connect_udp_target(http_connect_udp_target&&) noexcept = default;
    http_connect_udp_target& operator=(const http_connect_udp_target&) = default;
    http_connect_udp_target& operator=(http_connect_udp_target&&) noexcept = default;

    std::string host_;
    std::uint16_t port_{0};
};
enum class http_connect_udp_error : std::uint8_t {
    invalid_target,
    invalid_path,
    invalid_request,
    invalid_response,
    invalid_capsule_protocol,
};
// RFC 9298's default URI template. IPv6 hosts have no brackets or zone ID;
// all reserved bytes are percent-encoded during template expansion.
[[nodiscard]] std::variant<std::string, http_connect_udp_error> encode_http_connect_udp_path(
    http_connect_udp_target_view target);
[[nodiscard]] std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(
    std::string_view path);

struct http_connect_udp_request_view final {
    // HTTP/1.1 Upgrade versus HTTP/2 Extended CONNECT.
    http_protocol_version version_{http_protocol_version::http11};
    borrowed_text method_{"GET"};
    borrowed_text protocol_{"connect-udp"};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text path_{};
    borrowed_http_headers headers_{};
};
// Recipient checks. The connect-udp token selects the Capsule Protocol, so
// Capsule-Protocol is optional; a non-Boolean, ?0 or repeated field is treated as
// absent (RFC 9297 section 3.4). Responses with 204/205/206 are malformed (RFC 9297 section 3.2).
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept;
// Sender check for a CONNECT-UDP request this endpoint emits: the recipient
// rules plus exactly one Capsule-Protocol: ?1 field (RFC 9297 section 3.4
// SHOULD, kept mandatory for Ruvia senders on HTTP/1.1 and HTTP/2).
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(
    http_connect_udp_request_view request) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header> headers) noexcept;
// Classifies the HTTP/1 Upgrade token; full validation remains mandatory.
[[nodiscard]] bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept;
[[nodiscard]] std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept;
// Takes application response metadata, validates it (2xx other than 204/205/206;
// an application Capsule-Protocol field must be a single ?1), and supplies the
// Capsule-Protocol and version-specific status/Upgrade fields. HTTP/1 status 101
// can only be produced by this dedicated driver boundary.
[[nodiscard]] std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version);
// The HTTP/1 head plan of a response prepare_http_connect_udp_response()
// produced; throws http1_response_head_error for any other response.
[[nodiscard]] http1_response_head_plan prepare_http1_connect_udp_response_head(const http_response& response);
}  // namespace ruvia

namespace ruvia {
enum class http_capsule_codec_error : std::uint8_t { need_more_data,
    value_out_of_range,
    output_too_small };
// RFC 9297 Capsule-Protocol is a Structured Field Boolean Item.
[[nodiscard]] std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept;

inline constexpr std::uint64_t http_datagram_capsule_type = 0;
struct http_udp_datagram_view final {
    std::uint64_t context_id_{0};
    std::span<const char> payload_{};
};
// RFC 9298: context ID zero carries a UDP payload; unknown contexts are dropped.
// The payload view borrows `input`.
[[nodiscard]] std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept;
template <detail::http_temporary_owning_range<char> input_type>
std::optional<http_udp_datagram_view> decode_http_udp_datagram(input_type&&) noexcept = delete;
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id = 0) noexcept;

struct http_capsule_event final {
    std::uint64_t type_{0};
    std::span<const char> payload_{};
    bool end_capsule_{false};
};
using http_capsule_callback_type = void (*)(void*, http_capsule_event);
enum class http_capsule_status : std::uint8_t { need_more_data,
    end,
    truncated,
    limit };
struct http_capsule_feed_result final {
    http_capsule_status status_{http_capsule_status::need_more_data};
    std::size_t consumed_bytes_{};
    bool capsule_complete_{};
};
// max_capsule_length_ bounds the capsules a reader keeps (see
// http_capsule_retention); dropped capsules are skipped at any length.
struct http_capsule_config final {
    std::uint64_t max_capsule_length_{16 * 1024 * 1024};
};
// Which capsules a reader keeps. RFC 9297 section 3.2: a recipient silently
// drops a capsule it does not use and skips over it. Dropped capsules produce
// no callback, are not buffered and are not subject to max_capsule_length_.
enum class http_capsule_retention : std::uint8_t {
    // Every capsule: a generic Capsule Protocol reader handles unknown types.
    all,
    // DATAGRAM capsules (RFC 9297 section 3.5) only.
    datagrams,
    // DATAGRAM capsules carrying RFC 9298 Context ID 0 (UDP payloads); other
    // Context IDs are unknown and dropped. The Context ID is inspected before
    // the length limit applies. A DATAGRAM too short for its Context ID is kept
    // so that the reader rejects it as malformed.
    udp_payloads,
};

// RFC 9297 incremental Capsule Protocol framing for HTTP/1 tunnels. Header state is inline; payload
// fragments borrow feed input (or, for a kept CONNECT-UDP Context ID, decoder
// storage) only during callbacks. No owned payload or event queue is created.
// The retention of a capsule is decided from the feed call that completes its
// header (and, for udp_payloads, its Context ID).
class http_capsule_decoder final {
public:
    explicit http_capsule_decoder(http_capsule_config config = {});
    [[nodiscard]] http_capsule_status feed(std::span<const char> input, bool fin, http_capsule_callback_type callback,
        void* context, http_capsule_retention retention = http_capsule_retention::all);
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_status feed(std::span<const char> input, bool fin,
        visitor_type&& visitor,
        http_capsule_retention retention = http_capsule_retention::all) {
        auto& target = visitor;
        return feed(
            input, fin,
            +[](void* context, http_capsule_event event) {
                (*static_cast<std::remove_reference_t<visitor_type>*>(context))(event);
            },
            std::addressof(target), retention);
    }
    // Stops at the first complete kept capsule. The unconsumed suffix stays with
    // the caller; FIN is committed only after all input is consumed. This permits
    // pull-based adapters without buffering a queue of complete capsules.
    [[nodiscard]] http_capsule_feed_result feed_one(std::span<const char> input, bool fin,
        http_capsule_callback_type callback, void* context,
        http_capsule_retention retention = http_capsule_retention::all);
    template <typename visitor_type>
        requires std::invocable<visitor_type&, http_capsule_event>
    [[nodiscard]] http_capsule_feed_result feed_one(std::span<const char> input, bool fin,
        visitor_type&& visitor,
        http_capsule_retention retention = http_capsule_retention::all) {
        auto& target = visitor;
        return feed_one(
            input, fin,
            +[](void* context, http_capsule_event event) {
                (*static_cast<std::remove_reference_t<visitor_type>*>(context))(event);
            },
            std::addressof(target), retention);
    }

private:
    enum class payload_mode : std::uint8_t { deliver,
        skip,
        context_id };
    [[nodiscard]] http_capsule_feed_result feed_impl(std::span<const char> input, bool fin,
        http_capsule_callback_type callback, void* context, bool one, http_capsule_retention retention);
    http_capsule_config config_;
    std::array<char, 16> header_{};
    std::array<char, 8> context_id_{};
    std::size_t header_size_{0}, type_size_{0}, context_id_size_{0};
    std::uint64_t type_{0}, remaining_{0};
    payload_mode mode_{payload_mode::deliver};
    bool payload_{false}, feeding_{false};
    http_capsule_status status_{http_capsule_status::need_more_data};
};
[[nodiscard]] std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept;
enum class http_datagram_error : std::uint8_t {
    send_closed,
    payload_too_large,
    malformed,
};
// A valid output snapshot produced by http_datagram_session. The inline prefix
// belongs to this plan; payload() continues to borrow the caller's input.
class http_datagram_write_plan final {
public:
    [[nodiscard]] constexpr std::string_view prefix() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::string_view(prefix_.data(), prefix_size_);
    }
    std::string_view prefix() const&& = delete;

    [[nodiscard]] constexpr std::span<const char> payload() const noexcept {
        return payload_;
    }

private:
    friend class http_datagram_session;
    http_datagram_write_plan() noexcept = default;

    std::array<char, 24> prefix_{};
    std::size_t prefix_size_{0};
    std::span<const char> payload_{};
};
// One accepted HTTP Datagram channel, with optional CONNECT-UDP semantics. Construct after request negotiation (clients
// may optimistically send before the response). Owns no memory or transport.
// Returned payloads borrow caller input; unknown context IDs are dropped.
class http_datagram_session final {
public:
    http_datagram_session() noexcept = default;
    void close_send() noexcept {
        send_open_ = false;
    }
    void close_receive() noexcept {
        receive_open_ = false;
    }
    [[nodiscard]] std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(
        std::span<const char> payload) const noexcept;
    template <detail::http_temporary_owning_range<char> payload_type>
    std::variant<http_datagram_write_plan, http_datagram_error> prepare_datagram(payload_type&&) const noexcept = delete;
    [[nodiscard]] std::optional<std::span<const char>> receive_datagram(
        std::span<const char> input) const noexcept;
    template <detail::http_temporary_owning_range<char> input_type>
    std::optional<std::span<const char>> receive_datagram(input_type&&) const noexcept = delete;
    [[nodiscard]] std::variant<http_datagram_write_plan, http_datagram_error> prepare_udp_datagram(
        std::span<const char> payload) const noexcept;
    template <detail::http_temporary_owning_range<char> payload_type>
    std::variant<http_datagram_write_plan, http_datagram_error> prepare_udp_datagram(payload_type&&) const noexcept = delete;
    // Capsule input is the complete DATAGRAM capsule value, excluding type/length.
    // Empty optional means silently dropped.
    [[nodiscard]] std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> receive_udp_datagram(
        std::span<const char> input) const noexcept;
    template <detail::http_temporary_owning_range<char> input_type>
    std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> receive_udp_datagram(input_type&&) const noexcept = delete;

private:
    [[nodiscard]] static std::variant<http_datagram_write_plan, http_datagram_error> plan_datagram_capsule(
        std::span<const char> payload, std::string_view context_prefix) noexcept;

    bool send_open_{true};
    bool receive_open_{true};
};
}  // namespace ruvia
