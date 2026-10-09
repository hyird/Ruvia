#include "server/http_response_head.h"

#include <array>
#include <charconv>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/util/http_ows.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "coding/http_content_coding.h"
#include "coding/http_content_length.h"
#include "field/http_media_type.h"
#include "http1/http1_chunked_framing.h"
#include "response/http_response_header_access.h"
#include "server/http_date_cache.h"

namespace ruvia::detail {

namespace {

struct response_head_flags {
    http_protocol_version protocol_version_{http_protocol_version::http11};
    bool emit_chunked_transfer_encoding_{false};
    bool emit_content_length_{false};
    std::uint64_t canonical_content_length_{0};
};

inline constexpr std::string_view chunked_transfer_encoding_header = "Transfer-Encoding: chunked\r\n";

[[nodiscard]] std::optional<std::uint64_t> explicit_content_length(const http_response& response) {
    // A ok parse_field always populates the state's value, and a Content-Length
    // that fails to parse throws below, so the accumulated optional already
    // encodes presence: empty means no Content-Length line was seen.
    http_content_length_state<> state;
    for (const auto& header : response.headers()) {
        if (response_header_known_bit(header) != response_header_content_length) {
            continue;
        }
        if (state.parse_field(header.value()) != http_content_length_parse_status::ok) {
            throw std::invalid_argument("invalid explicit HTTP response Content-Length");
        }
    }
    const auto value = state.value();
    if (!value.has_value()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*value);
}

// Unchecked sink writing through a raw cursor; the caller guarantees capacity
// via response_head_buffer_type::append_generated. Constant-size appends inline to stores.
struct raw_head_sink {
    char* out_;

    void append(std::string_view value) noexcept {
        // A default-constructed empty string_view may carry a null data pointer.
        // libc annotates memcpy arguments as nonnull even when the byte count is
        // zero, so avoid passing that representation across the C boundary.
        if (value.empty()) {
            return;
        }
        std::memcpy(out_, value.data(), value.size());
        out_ += value.size();
    }

    void append(char value) noexcept {
        *out_++ = value;
    }

    void append_unsigned(std::uint64_t value) noexcept {
        // Format into a local buffer and copy. The measured head reserves only
        // decimal_digits(value) bytes for these digits, so std::to_chars(out,
        // out + 20, ...) would form a pointer past the 512-byte stack buffer's
        // end (undefined per [expr.add]) when the head nearly fills it, even
        // though no byte beyond the digits is written. Mirrors
        // response_head_buffer_type::append_unsigned.
        std::array<char, 20> digits;
        const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
        append(std::string_view(digits.data(), static_cast<std::size_t>(end - digits.data())));
    }
};

void add_response_head_bytes(std::size_t& total, std::size_t bytes_value) {
    if (bytes_value > max_http_header_bytes - total) {
        throw std::length_error("HTTP response head is too large");
    }
    total += bytes_value;
}

[[nodiscard]] std::size_t decimal_digits(std::uint64_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

[[nodiscard]] bool is_valid_http1_emitted_response_header(std::string_view name, std::string_view value,
    std::uint32_t known_bit, bool trailer_header_allowed) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return false;
    }
    if (known_bit == response_header_content_type && !is_valid_http_content_type_field_value(value)) {
        return false;
    }
    if (known_bit == response_header_content_encoding &&
        !is_valid_http_content_encoding_field_value(value, http_field_list_role::sender)) {
        return false;
    }
    if (http_ascii_equals_ignore_case(name, "Trailer")) {
        return is_valid_http_response_trailer_field_value(value, http_field_list_role::sender) &&
               (trailer_header_allowed || http_trim_ows(value).empty());
    }
    if (http_ascii_equals_ignore_case(name, "Connection")) {
        http_connection_options options;
        return options.parse_field(
                   value, http_field_list_role::sender, [](std::string_view option) noexcept {
                       return !http_connection_option_conflicts_with_managed_field(option);
                   }) == http_field_list_parse_status::ok;
    }
    if (http_ascii_equals_ignore_case(name, "Upgrade")) {
        http_upgrade_protocols protocols;
        return protocols.parse_field(value, http_field_list_role::sender,
                   [](const http_upgrade_protocol&) noexcept { return true; }) ==
               http_field_list_parse_status::ok;
    }
    if (http_ascii_equals_ignore_case(name, "TE")) {
        return false;
    }
    return true;
}

void emit_response_head(const http_response& response, raw_head_sink& sink_value, http_status_code response_status,
    std::string_view reason_phrase, std::string_view date_header, response_head_flags flags) noexcept {
    sink_value.append(flags.protocol_version_ == http_protocol_version::http10
                          ? std::string_view("HTTP/1.0 ")
                          : std::string_view("HTTP/1.1 "));
    const auto status_token = http_status_code_token(response_status);
    sink_value.append(http_status_code_token_view(status_token));
    // RFC 9112 requires this SP even when the optional reason phrase is empty.
    sink_value.append(' ');
    sink_value.append(reason_phrase);
    sink_value.append(std::string_view("\r\n"));

    for (const auto& header : response.headers()) {
        const auto known_bit = response_header_known_bit(header);
        // HTTP/1 framing belongs exclusively to http1_response_head_plan. A handler
        // cannot override canonical chunked framing, invent Transfer-Encoding on
        // an HTTP/1.0 response, or attach Content-Length to a body-open
        // close-delimited stream.
        if (known_bit == response_header_transfer_encoding ||
            known_bit == response_header_content_length) {
            continue;
        }
        sink_value.append(header.name());
        sink_value.append(std::string_view(": "));
        sink_value.append(header.value());
        sink_value.append(std::string_view("\r\n"));
    }

    const auto known_bits = response_known_header_bits(response);
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        sink_value.append(date_header);
    }
    if (flags.emit_chunked_transfer_encoding_) {
        sink_value.append(chunked_transfer_encoding_header);
    }
    if (flags.emit_content_length_) {
        sink_value.append(std::string_view("Content-Length: "));
        sink_value.append_unsigned(flags.canonical_content_length_);
        sink_value.append(std::string_view("\r\n"));
    }
    sink_value.append(std::string_view("\r\n"));
}

}  // namespace

void append_response_head(
    const http_response& response, response_head_buffer_type& head, const http1_response_head_plan& plan) {
    const auto body_plan = plan.body_plan();
    if (response.status() != body_plan.response_status()) {
        throw std::invalid_argument("HTTP/1 response plan status does not match response");
    }
    const auto* buffered = plan.buffered();
    const auto* known_length_stream = plan.known_length_stream();
    if (buffered != nullptr &&
        buffered->content_length() != body_plan.buffered_representation_length(response)) {
        throw std::invalid_argument("HTTP/1 response plan representation does not match response");
    }
    const auto response_status = body_plan.response_status();
    const bool chunked_payload_plan = plan.chunked_stream() != nullptr &&
                                      body_plan.transfer_encoding_allowed() && !body_plan.body_suppressed();
    if (chunked_payload_plan && plan.protocol_version() == http_protocol_version::http10) {
        throw std::invalid_argument("HTTP/1.0 response cannot use chunked Transfer-Encoding");
    }
    const bool emit_chunked_transfer_encoding = chunked_payload_plan;
    const bool auto_content_length_owned_by_writer =
        body_plan.auto_content_length_allowed() && !emit_chunked_transfer_encoding &&
        (buffered != nullptr || known_length_stream != nullptr || !body_plan.status_allows_body());
    const bool explicit_content_length_allowed =
        body_plan.explicit_content_length_allowed() && !emit_chunked_transfer_encoding &&
        !auto_content_length_owned_by_writer &&
        (plan.close_delimited_stream() == nullptr || body_plan.body_suppressed());
    const auto known_bits = response_known_header_bits(response);
    const auto declared_content_length =
        explicit_content_length_allowed && (known_bits & response_header_content_length) != 0
            ? explicit_content_length(response)
            : std::nullopt;
    const response_head_flags flags{.protocol_version_ = plan.protocol_version(),
        .emit_chunked_transfer_encoding_ = emit_chunked_transfer_encoding,
        .emit_content_length_ = auto_content_length_owned_by_writer || declared_content_length.has_value(),
        // Buffered HEAD metadata retains the selected representation length.
        // A status-level no-content policy that still owns framing (205) is
        // canonicalized to zero for both buffered and streaming heads.
        .canonical_content_length_ = declared_content_length.value_or(
            body_plan.status_allows_body()
                ? (buffered != nullptr
                          ? buffered->content_length()
                          : (known_length_stream != nullptr ? known_length_stream->content_length()
                                                            : std::uint64_t{0}))
                : std::uint64_t{0})};

    const auto reason_phrase = http_reason_phrase(response_status);
    const auto date_header = cached_date_header();

    // Measure the exact emitted head before touching reusable output storage.
    // This both bounds the unchecked raw stack sink and enforces the same 64 KiB
    // field-section ceiling used by request and HTTP/2 paths.
    std::size_t head_bytes = 9;
    add_response_head_bytes(head_bytes, http_status_code_token_size);
    add_response_head_bytes(head_bytes, 1);
    add_response_head_bytes(head_bytes, reason_phrase.size());
    add_response_head_bytes(head_bytes, 2);
    std::size_t field_count = 0;
    for (const auto& header : response.headers()) {
        const auto known_bit = response_header_known_bit(header);
        if (known_bit == response_header_transfer_encoding ||
            known_bit == response_header_content_length) {
            continue;
        }
        if (!is_valid_http1_emitted_response_header(
                header.name(), header.value(), known_bit, emit_chunked_transfer_encoding)) {
            throw std::invalid_argument("invalid HTTP response header");
        }
        ++field_count;
        add_response_head_bytes(head_bytes, header.name().size());
        add_response_head_bytes(head_bytes, header.value().size());
        add_response_head_bytes(head_bytes, 4);
    }
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        ++field_count;
        add_response_head_bytes(head_bytes, date_header.size());
    }
    if (emit_chunked_transfer_encoding) {
        ++field_count;
        add_response_head_bytes(head_bytes, chunked_transfer_encoding_header.size());
    }
    if (flags.emit_content_length_) {
        ++field_count;
        add_response_head_bytes(head_bytes, 16);
        add_response_head_bytes(head_bytes, decimal_digits(flags.canonical_content_length_));
        add_response_head_bytes(head_bytes, 2);
    }
    if (field_count > max_http_header_fields) {
        throw std::length_error("too many HTTP response headers");
    }
    add_response_head_bytes(head_bytes, 2);

    head.append_generated(head_bytes, [&](char* cursor_value) noexcept {
        raw_head_sink sink_value{cursor_value};
        emit_response_head(response, sink_value, response_status, reason_phrase, date_header, flags);
    });
}

}  // namespace ruvia::detail

namespace ruvia {

void append_http1_response_head(const http_response& response, http_response_head_buffer& head,
    const http1_response_head_plan& plan) {
    detail::append_response_head(response, head, plan);
}

void append_http1_response_trailers(
    std::pmr::string& output, const http_response_trailer_section& trailers) {
    detail::append_http1_trailer_section(output, trailers);
}

}  // namespace ruvia
