#include "server/http_response_head.h"

#include <array>
#include <optional>
#include <stdexcept>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http1_status_line.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
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
    return state.value();
}

inline constexpr std::string_view content_length_name = "Content-Length";

// Every field line was validated when it was inserted into the response
// (name, value, per-field grammar, forbidden fields, singletons). Only the
// rules that span several lines or depend on this head's framing remain.
[[nodiscard]] bool is_valid_http1_emitted_response_header(std::string_view name, std::string_view value,
    std::uint32_t known_bit, bool trailer_header_allowed, http_connection_options& connection_options,
    http_upgrade_protocols& upgrade_protocols) noexcept {
    switch (classify_response_field(name, known_bit)) {
        case request_header_kind::connection:
            return connection_options.parse_field(value, http_field_list_role::sender) == http_field_list_parse_status::ok;
        case request_header_kind::trailer:
            return trailer_header_allowed || http_trim_ows(value).empty();
        case request_header_kind::upgrade:
            return upgrade_protocols.parse_field(value, http_field_list_role::sender) == http_field_list_parse_status::ok;
        default:
            return true;
    }
}

void emit_response_head(const http_response& response, http_raw_wire_sink& sink_value, http_status_code response_status,
    std::string_view reason_phrase, std::string_view date_header, response_head_flags flags,
    std::string_view content_length_value) noexcept {
    emit_http1_status_line(sink_value, response_status, reason_phrase);

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
        sink_value.append(content_length_value);
        sink_value.append(std::string_view("\r\n"));
    }
    sink_value.append(std::string_view("\r\n"));
}

template <typename sink_type>
void emit_response_trailer_fields(sink_type& sink, std::span<const http_header_view> fields) {
    for (const auto& field : fields) {
        emit_http1_field_line(sink, field.name(), field.value());
    }
}

// Consume aliased names, values and descriptors before append can reallocate.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void append_response_trailer_snapshot(std::pmr::string& output,
    std::span<const http_header_view> fields, std::size_t bytes) {
    std::array<char, max_http_header_bytes> scratch;
    http_raw_wire_sink sink(scratch.data());
    emit_response_trailer_fields(sink, fields);
    check_http_wire_end(sink.position(), scratch.data() + bytes);
    output.append(scratch.data(), bytes);
}

}  // namespace

void append_response_head(
    const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan) {
    const auto body_plan = plan.body_plan();
    if (response.status() != body_plan.response_status()) {
        throw std::invalid_argument("HTTP/1 response plan status does not match response");
    }
    const auto* buffered = plan.buffered();
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
        (buffered != nullptr || !body_plan.status_allows_body());
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
                ? (buffered != nullptr ? buffered->content_length() : std::uint64_t{0})
                : std::uint64_t{0})};

    const auto reason_phrase = http_reason_phrase(response_status);
    const auto date_header =
        (known_bits & response_header_date) == 0 ? cached_date_header() : std::string_view{};
    // Reuse the formatted token for exact measurement and emission instead of
    // separately dividing the length to count its digits.
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value = flags.emit_content_length_
                                          ? http_format_unsigned(content_length_buffer, flags.canonical_content_length_)
                                          : std::string_view{};

    // Measure the exact emitted head before touching reusable output storage.
    // This both bounds the unchecked raw stack sink and enforces the shared
    // field-section rule used by request parsing.
    http_field_section_budget head_budget;
    const auto charge = [](bool fits) {
        if (!fits) {
            throw std::length_error("HTTP response head is too large");
        }
    };
    charge(head_budget.add(http1_status_line_size(reason_phrase)));
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
        charge(head_budget.add_field(header.name(), header.value()));
    }
    if (upgrade_protocols.has_field() && !connection_options.upgrade()) {
        throw std::invalid_argument("HTTP Upgrade response requires Connection: Upgrade");
    }
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        charge(head_budget.add(date_header.size()));
    }
    if (emit_chunked_transfer_encoding) {
        charge(head_budget.add(chunked_transfer_encoding_header.size()));
    }
    if (flags.emit_content_length_) {
        charge(head_budget.add_field(content_length_name.size(), content_length_value.size()));
    }
    charge(head_budget.add(2));

    head.append_generated(head_budget.bytes(), [&](char* cursor_value) noexcept {
        http_raw_wire_sink sink_value(cursor_value);
        emit_response_head(response, sink_value, response_status, reason_phrase, date_header, flags,
            content_length_value);
        check_http_wire_end(sink_value.position(), cursor_value + head_budget.bytes());
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
    const auto fields = trailers.fields();
    if (fields.empty()) {
        return;
    }
    auto budget = http_trailer_section_budget();
    for (const auto& field : fields) {
        if (!budget.add_field(field.name(), field.value())) {
            throw std::length_error("HTTP response trailers are too large");
        }
    }
    const auto bytes = budget.bytes() - std::string_view("\r\n").size();
    if (bytes > output.max_size() - output.size()) {
        throw std::length_error("HTTP response trailers are too large");
    }
    if (detail::http_wire_inputs_overlap(output, fields)) {
        detail::append_response_trailer_snapshot(output, fields, bytes);
        return;
    }
    output.reserve(output.size() + bytes);
    detail::http_string_wire_sink sink(output);
    detail::emit_response_trailer_fields(sink, fields);
}

http_response_trailer_section validate_http_response_trailers(
    std::span<const http_header_view> trailers) {
    auto section_size = http_trailer_section_budget();
    for (const auto& trailer : trailers) {
        if (!detail::response_trailer_field_valid(trailer.name(), trailer.value()) ||
            !section_size.add_field(trailer.name(), trailer.value())) {
            throw std::invalid_argument("invalid HTTP response trailer section");
        }
    }
    return http_response_trailer_section(trailers);
}

const char* http1_final_response_commit_error::what() const noexcept {
    switch (error_) {
        case detail::http1_final_response_control_plan_error::invalid_status:
            return "invalid final HTTP response status";
        case detail::http1_final_response_control_plan_error::invalid_connection_field:
            return "invalid HTTP Connection header";
        case detail::http1_final_response_control_plan_error::invalid_upgrade_field:
            return "invalid HTTP Upgrade header";
        case detail::http1_final_response_control_plan_error::upgrade_required:
            return "Upgrade Required response requires an Upgrade protocol";
        case detail::http1_final_response_control_plan_error::te_field_forbidden:
            return "TE is not a response field";
    }
    return "unknown HTTP final response commit failure";
}

namespace {

// Lists one generated option; an application Connection line is extended
// rather than replaced, so its own options survive.
void add_http1_connection_option(http_response& response, std::string_view option, bool field_present) {
    if (field_present) {
        response.header(detail::http_connection_field_name, option,
            http_response_header_options{.mode_ = http_response_header_mode::append});
    } else {
        response.header_stable_view(detail::http_connection_field_name, option);
    }
}

}  // namespace

http1_request_connection_plan http1_commit_final_response(
    http_response& response, http1_request_connection_plan plan) {
    const auto control_result = detail::http1_final_response_control_plan(response);
    if (const auto* error = std::get_if<detail::http1_final_response_control_plan_error>(&control_result)) {
        throw http1_final_response_commit_error(*error);
    }
    const auto& http1_control = std::get<detail::http1_final_response_control>(control_result);
    const auto response_options = http1_control.connection_options();
    const auto upgrade_protocols = http1_control.upgrade_protocols();
    const bool preserve_upgrade = upgrade_protocols.has_field();
    const bool generate_upgrade_option = preserve_upgrade && !response_options.upgrade();
    if (generate_upgrade_option) {
        add_http1_connection_option(response, detail::http_connection_upgrade_option, response_options.has_field());
    }
    // A failed CONNECT never becomes a tunnel. RFC 9931 section 8 requires
    // closing instead of interpreting optimistic tunnel data as more requests.
    if (response_options.close() || (plan.tunnel_request() && !response.status().is_successful())) {
        plan = plan.require_close();
    }
    if (plan.disposition() == http1_close_policy::close_after_response) {
        http1_mark_connection_close(response, preserve_upgrade
                                                  ? http1_connection_close_field_policy::preserve_upgrade
                                                  : http1_connection_close_field_policy::close_only);
    } else if (plan.protocol_version() == http_protocol_version::http10 &&
               !response_options.keep_alive()) {
        add_http1_connection_option(response, detail::http_connection_keep_alive_option,
            response_options.has_field() || generate_upgrade_option);
    }
    return plan;
}

prepared_http1_response_stream prepare_http1_response_stream_head(
    http_response response, const http1_response_stream_plan& plan,
    http_response_trailer_intent trailer_intent) {
    auto commit_plan = plan_http_response_stream_commit(
        plan.framing(), plan.request_method(), response.status(), trailer_intent);
    const auto body_plan = commit_plan.body_plan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a response whose method/status forbids payload is already
    // self-delimited. Make this decision at head commit, when the response status is
    // finally known, instead of pessimistically baking close into the pre-commit plan.
    const auto planned_connection =
        plan.request_connection_plan().disposition() == http1_close_policy::allow_reuse &&
                plan.close_policy() == http1_close_policy::allow_reuse &&
                (plan.framing() != http_response_stream_framing::http1_close_delimited ||
                    body_plan.body_suppressed())
            ? plan.request_connection_plan()
            : plan.request_connection_plan().require_close();
    const auto connection_plan = http1_commit_final_response(response, planned_connection);
    auto head = prepare_http_response_stream_head(std::move(response), std::move(commit_plan));
    const auto response_head_plan =
        plan.framing() == http_response_stream_framing::http1_chunked
            ? http1_chunked_response_stream_head_plan(head.commit_plan().body_plan(), connection_plan)
            : http1_close_delimited_response_stream_head_plan(
                  head.commit_plan().body_plan(), connection_plan);
    return prepared_http1_response_stream(std::move(head), response_head_plan, connection_plan);
}

}  // namespace ruvia
