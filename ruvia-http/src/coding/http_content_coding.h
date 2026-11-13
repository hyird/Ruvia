#pragma once

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_field_values.h"
#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ruvia/http/http_field_syntax.h"

namespace ruvia::detail {

struct http_content_coding_field_result_access final {
    using sequence_type = http_content_coding_field_result::content_coding_sequence;

    [[nodiscard]] static constexpr http_content_coding_field_result coding(
        sequence_type values) noexcept {
        return http_content_coding_field_result(values);
    }

    [[nodiscard]] static constexpr http_content_coding_field_result unsupported(
        bool depth_exceeded = false) noexcept {
        return http_content_coding_field_result(http_unsupported_content_coding(depth_exceeded));
    }

    [[nodiscard]] static constexpr http_content_coding_field_result invalid() noexcept {
        return http_content_coding_field_result(http_invalid_content_coding_field{});
    }
};

// Shared token walk for decoding and outbound acceptance. The caller decides
// whether an unknown extension requires a decoder or only a quality check.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status visit_http_content_coding_field(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    return http_visit_token_list(value, role, [&visitor](std::string_view token) {
        return visitor(token, http_content_coding_from_token(token));
    });
}

// Accumulates recipient list grammar across every Content-Encoding field line;
// empty list members are ignored.
class http_content_coding_field_parser final {
public:
    constexpr http_content_coding_field_parser() noexcept = default;

    void update(std::string_view value) noexcept;

    [[nodiscard]] http_content_coding_field_result finish() const noexcept;

private:
    http_content_coding_field_result_access::sequence_type codings_{};
    bool unknown_coding_{false};
    bool depth_exceeded_{false};
    bool invalid_{false};
};

// Validate list grammar without selecting decoders. Recipients/forwarders may
// retain empty members, but every nonempty member must still be a coding token.
[[nodiscard]] bool is_valid_http_content_encoding_field_value(
    std::string_view value, http_field_list_role role) noexcept;

template <typename headers_type>
[[nodiscard]] inline http_content_coding_field_result http_content_coding_from_headers(
    const headers_type& headers) noexcept {
    http_content_coding_field_parser parser;
    for (const auto& header : headers) {
        if (http_ascii_equals_ignore_case(header.name(), "Content-Encoding")) {
            parser.update(header.value());
        }
    }
    return parser.finish();
}

}  // namespace ruvia::detail

// A reusable codec slot: native compression libraries keep pointers into
// their stream, so the codec lives in one stable heap allocation.
// `codec_type` provides initialized() and reset().

namespace ruvia::detail {

// The slot's codec, created on first use. A codec that failed to initialize is
// not kept, so the next use retries; nullptr reports that failure.
template <typename codec_type, typename... args_type>
[[nodiscard]] codec_type* acquire_codec(codec_type*& slot, args_type&&... args) {
    if (slot == nullptr) {
        auto* created = new codec_type(std::forward<args_type>(args)...);
        if (!created->initialized()) {
            delete created;
            return nullptr;
        }
        slot = created;
    }
    return slot;
}

// Returns an acquired slot's codec to its initial state when the scope ends
// normally. A codec left by an exception, or one that cannot reset, is
// dropped and recreated by the next use: failed native state is never reused.
template <typename codec_type>
class codec_reset_guard final {
public:
    explicit codec_reset_guard(codec_type*& slot) noexcept
        : slot_(slot),
          uncaught_exceptions_(std::uncaught_exceptions()) {}
    codec_reset_guard(const codec_reset_guard&) = delete;
    codec_reset_guard& operator=(const codec_reset_guard&) = delete;

    ~codec_reset_guard() {
        if (std::uncaught_exceptions() != uncaught_exceptions_ || !slot_->reset()) {
            delete std::exchange(slot_, nullptr);
        }
    }

private:
    codec_type*& slot_;
    int uncaught_exceptions_;
};

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

enum class http_content_length_parse_status : std::uint8_t { ok,
    invalid,
    conflicting };

// Incremental Content-Length accumulator. Accepts comma-combined values with
// OWS and commits only when every repeated value agrees.
class http_content_length_state final {
public:
    [[nodiscard]] http_content_length_parse_status parse_field(std::string_view field_value) noexcept {
        auto status = http_content_length_parse_status::ok;
        bool saw_value = false;
        auto parsed_value = value_;
        http_visit_comma_separated_quoted_field_items(field_value, [&parsed_value, &status, &saw_value](
                                                                       std::string_view item) noexcept {
            status = accumulate(item, parsed_value);
            saw_value = true;
            return status == http_content_length_parse_status::ok;
        });
        if (status == http_content_length_parse_status::ok && !saw_value) {
            return http_content_length_parse_status::invalid;
        }
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed_value;
        }
        return status;
    }

    [[nodiscard]] http_content_length_parse_status parse_single_value(std::string_view input) noexcept {
        auto parsed = value_;
        const auto status = accumulate(input, parsed);
        if (status == http_content_length_parse_status::ok) {
            value_ = parsed;
        }
        return status;
    }

    [[nodiscard]] std::optional<std::uint64_t> value() const noexcept {
        return value_;
    }

private:
    [[nodiscard]] static http_content_length_parse_status accumulate(
        std::string_view item, std::optional<std::uint64_t>& value) noexcept {
        if (item.empty()) {
            return http_content_length_parse_status::invalid;
        }
        std::uint64_t parsed_value = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed_value);
        if (ec != std::errc{} || end != item.data() + item.size()) {
            return http_content_length_parse_status::invalid;
        }
        if (value.has_value() && *value != parsed_value) {
            return http_content_length_parse_status::conflicting;
        }
        value = parsed_value;
        return http_content_length_parse_status::ok;
    }

    std::optional<std::uint64_t> value_;
};

}  // namespace ruvia::detail

// One compression library binding per coding, behind a uniform signature: decode
// or encode a whole buffer, bounded by an
// explicit output ceiling. Decode reports data errors as values; encode returns
// nullopt only for the ceiling. Both throw http_content_codec_error when the
// codec itself cannot run. Which coding a field asks for is decided elsewhere;
// this is only the machinery each one runs on. decode_http_content maps an
// empty representation to empty content before any decoder below runs.

namespace ruvia::detail {

class brotli_encoder_stream;
class zlib_deflate_stream;
class zstd_encoder_stream;

// Append decoder output while enforcing the ceiling; false means the ceiling
// would be exceeded and the decode must fail.
[[nodiscard]] inline bool append_decoded_bytes(
    std::string& output, const char* bytes_value, std::size_t size, std::size_t max_decoded_bytes) {
    if (output.size() > max_decoded_bytes || size > max_decoded_bytes - output.size()) {
        return false;
    }
    output.append(bytes_value, size);
    return true;
}

template <typename stream_type, typename operation_type>
[[nodiscard]] inline std::optional<std::string> encode_whole_content_with_stream(
    stream_type& encoder, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes) {
    std::string output;
    if (!encoder.encode(input, output, finish_operation, max_encoded_bytes)) {
        return std::nullopt;
    }
    return output;
}

template <typename stream_type, typename operation_type, typename... arg_types>
[[nodiscard]] inline std::optional<std::string> encode_whole_content(
    const char* init_error, operation_type finish_operation, std::string_view input,
    std::size_t max_encoded_bytes, arg_types&&... args) {
    stream_type encoder(std::forward<arg_types>(args)...);
    if (!encoder.initialized()) {
        throw http_content_codec_error(init_error);
    }
    return encode_whole_content_with_stream(
        encoder, finish_operation, input, max_encoded_bytes);
}

[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_brotli_content(
    std::string_view input, std::size_t max_decoded_bytes);
[[nodiscard]] std::variant<std::string, http_content_decode_error> decode_zstd_content(
    std::string_view input, std::size_t max_decoded_bytes);

[[nodiscard]] std::optional<std::string> encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(
    std::string_view input, std::size_t max_encoded_bytes);

// A whole-buffer encode, one finishing pass of the stream's single encode
// loop, over caller-owned codec state that is freshly initialized or reset.
// Every path leaves the state consumed (finished or mid-stream), so an owner
// that reuses it resets it afterwards.
[[nodiscard]] std::optional<std::string> encode_zlib_content(zlib_deflate_stream& deflater,
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_brotli_content(brotli_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes);
[[nodiscard]] std::optional<std::string> encode_zstd_content(zstd_encoder_stream& encoder,
    std::string_view input, std::size_t max_encoded_bytes);
}  // namespace ruvia::detail
