#include "server/http_response_head.h"

#include <exception>
#include <optional>
#include <stdexcept>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_whitespace.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"

#include "coding/http_content_length.h"
#include "response/http_response_header_access.h"
#include "server/http_date_cache.h"
#include "util/http_wire_sink.h"

namespace ruvia::detail {

namespace {

struct response_head_flags {
    http_protocol_version protocol_version_{http_protocol_version::http11};
    bool emit_chunked_transfer_encoding_{false};
    bool emit_content_length_{false};
    std::uint64_t canonical_content_length_{0};
};

inline constexpr std::string_view chunked_transfer_encoding_header = "Transfer-Encoding: chunked\r\n";
inline constexpr std::string_view http10_status_prefix = "HTTP/1.0 ";
inline constexpr std::string_view http11_status_prefix = "HTTP/1.1 ";
// The head is measured before the protocol version is chosen for emission.
static_assert(http10_status_prefix.size() == http11_status_prefix.size());

[[nodiscard]] std::optional<std::uint64_t> explicit_content_length(const http_response& response) {
    // A ok parse_field always populates the state's value, and a Content-Length
    // that fails to parse throws below, so the accumulated optional already
    // encodes presence: empty means no Content-Length line was seen.
    http_content_length_state state;
    for (const auto& header : response.headers()) {
        if (http_response_header_access::known_bit(header) != response_header_content_length) {
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

inline constexpr std::string_view content_length_name = "Content-Length";

// Every field line was validated when it was inserted into the response
// (name, value, per-field grammar, forbidden fields, singletons). Only the
// rules that span several lines or depend on this head's framing remain.
[[nodiscard]] bool is_valid_http1_emitted_response_header(std::string_view name, std::string_view value,
    std::uint32_t known_bit, bool trailer_header_allowed, http_connection_options& connection_options,
    http_upgrade_protocols& upgrade_protocols) noexcept {
    if (known_bit == response_header_connection) {
        return connection_options.parse_field(value, http_field_list_role::sender) == http_field_list_parse_status::ok;
    }
    if (known_bit != 0) {
        return true;
    }
    if (http_ascii_equals_ignore_case(name, "Trailer")) {
        return trailer_header_allowed || http_trim_ows(value).empty();
    }
    if (http_ascii_equals_ignore_case(name, "Upgrade")) {
        return upgrade_protocols.parse_field(value, http_field_list_role::sender) == http_field_list_parse_status::ok;
    }
    return true;
}

void emit_response_head(const http_response& response, http_raw_wire_sink& sink_value, http_status_code response_status,
    std::string_view reason_phrase, std::string_view date_header, response_head_flags flags) noexcept {
    sink_value.append(
        flags.protocol_version_ == http_protocol_version::http10 ? http10_status_prefix : http11_status_prefix);
    const auto status_token = http_status_code_token(response_status);
    sink_value.append(http_status_code_token_view(status_token));
    // RFC 9112 requires this SP even when the optional reason phrase is empty.
    sink_value.append(' ');
    sink_value.append(reason_phrase);
    sink_value.append(std::string_view("\r\n"));

    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        // HTTP/1 framing belongs exclusively to http1_response_head_plan. A handler
        // cannot override canonical chunked framing, invent Transfer-Encoding on
        // an HTTP/1.0 response, or attach Content-Length to a body-open
        // close-delimited stream.
        if (known_bit == response_header_transfer_encoding ||
            known_bit == response_header_content_length) {
            continue;
        }
        emit_http1_field_line(sink_value, header.name(), header.value());
    }

    const auto known_bits = response_known_header_bits(response);
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        sink_value.append(date_header);
    }
    if (flags.emit_chunked_transfer_encoding_) {
        sink_value.append(chunked_transfer_encoding_header);
    }
    if (flags.emit_content_length_) {
        sink_value.append(content_length_name);
        sink_value.append(std::string_view(": "));
        sink_value.append_unsigned(flags.canonical_content_length_);
        sink_value.append(std::string_view("\r\n"));
    }
    sink_value.append(std::string_view("\r\n"));
}

}  // namespace

void append_response_head(
    const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan) {
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
    // A HEAD handler may describe the GET representation without producing it.
    // Its empty suppressed body then has no length of its own, so the declared
    // Content-Length is the only truthful value (RFC 9110 section 8.6).
    const bool suppressed_empty_buffered_body = buffered != nullptr && body_plan.body_suppressed() &&
                                                body_plan.status_allows_body() &&
                                                body_plan.explicit_content_length_allowed() &&
                                                buffered->content_length() == 0;
    const auto known_bits = response_known_header_bits(response);
    const auto declared_content_length =
        (explicit_content_length_allowed || suppressed_empty_buffered_body) &&
                (known_bits & response_header_content_length) != 0
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
    // This both bounds the unchecked raw stack sink and enforces the shared
    // field-section rule used by request parsing.
    http_field_section_budget head_budget;
    const auto charge = [](bool fits) {
        if (!fits) {
            throw std::length_error("HTTP response head is too large");
        }
    };
    charge(head_budget.add(
        http11_status_prefix.size() + http_status_code_token_size + 1 + reason_phrase.size() + 2));
    std::size_t field_count = 0;
    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        if (known_bit == response_header_transfer_encoding ||
            known_bit == response_header_content_length) {
            continue;
        }
        if (!is_valid_http1_emitted_response_header(header.name(), header.value(), known_bit,
                emit_chunked_transfer_encoding, connection_options, upgrade_protocols)) {
            throw std::invalid_argument("invalid HTTP response header");
        }
        ++field_count;
        charge(head_budget.add_field(header.name(), header.value()));
    }
    if (upgrade_protocols.has_field() && !connection_options.upgrade()) {
        throw std::invalid_argument("HTTP Upgrade response requires Connection: Upgrade");
    }
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        ++field_count;
        charge(head_budget.add(date_header.size()));
    }
    if (emit_chunked_transfer_encoding) {
        ++field_count;
        charge(head_budget.add(chunked_transfer_encoding_header.size()));
    }
    if (flags.emit_content_length_) {
        ++field_count;
        charge(head_budget.add_field(content_length_name.size(),
            http_unsigned_decimal_size(flags.canonical_content_length_)));
    }
    if (field_count > max_http_header_fields) {
        throw std::length_error("too many HTTP response headers");
    }
    charge(head_budget.add(2));

    head.append_generated(head_budget.bytes(), [&](char* cursor_value) noexcept {
        http_raw_wire_sink sink_value(cursor_value);
        emit_response_head(response, sink_value, response_status, reason_phrase, date_header, flags);
        // A mismatch has already overrun or under-filled the reserved bytes; the
        // writer is noexcept and the heap path has already grown the buffer, so
        // failing here cannot leave the output unchanged or the memory trusted.
        if (sink_value.position() != cursor_value + head_budget.bytes()) {
            std::terminate();
        }
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
    detail::http_string_wire_sink sink(output);
    for (const auto& trailer : trailers.fields()) {
        detail::emit_http1_field_line(sink, trailer.name(), trailer.value());
    }
}

}  // namespace ruvia
