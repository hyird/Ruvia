#include "http1_server_response.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "borrowed_text.h"
#include "http_connection_fields.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response.h"
#include "http_response_framing.h"
#include "http_wire_writer.h"

namespace ruvia::detail {

namespace {

inline constexpr std::string_view chunked_transfer_encoding_header = "Transfer-Encoding: chunked\r\n";

inline constexpr std::string_view content_length_name = "Content-Length";

// Every field line was validated when it was inserted into the response
// (name, value, per-field grammar, forbidden fields, singletons). Only the
// rules that span several lines or depend on this head's framing remain.
[[nodiscard]] bool is_valid_http1_emitted_response_header(std::string_view name, std::string_view value, bool trailer_header_allowed, http_connection_options& connection_options, http_upgrade_protocols& upgrade_protocols) noexcept {
    const auto kind = classify_http_field(name);
    if (kind == http_field_kind::trailer) {
        return trailer_header_allowed || http_trim_ows(value).empty();
    }
    return parse_http_response_control_field(kind, value, connection_options, upgrade_protocols) == http_response_control_field_status::ok;
}

void emit_response_head(const http_response& response, http_raw_wire_sink& sink_value, http_status_code response_status, std::string_view reason_phrase, std::string_view date_header, bool emit_chunked_transfer_encoding, std::string_view content_length_value) noexcept {
    emit_http1_status_line(sink_value, response_status, reason_phrase);

    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        // HTTP/1 framing belongs exclusively to http1_response_head_plan: the
        // handler's Content-Length and Transfer-Encoding (only ever the
        // replaceable chunked hint) yield to the plan's own framing fields.
        if (known_bit == response_header_transfer_encoding || known_bit == response_header_content_length) {
            continue;
        }
        emit_http1_field_line(sink_value, header.name(), header.value());
    }

    const auto known_bits = response_known_header_bits(response);
    if ((known_bits & response_header_date) == 0 && !date_header.empty()) {
        sink_value.append(date_header);
    }
    if (emit_chunked_transfer_encoding) {
        sink_value.append(chunked_transfer_encoding_header);
    }
    if (!content_length_value.empty()) {
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
void append_response_trailer_snapshot(std::string& output, std::span<const http_header_view> fields, std::size_t bytes) {
    std::array<char, max_http_header_bytes> scratch;
    http_raw_wire_sink sink(scratch.data());
    emit_response_trailer_fields(sink, fields);
    check_http_wire_end(sink.position(), scratch.data() + bytes);
    output.append(scratch.data(), bytes);
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

void append_http1_response_head(const http_response& response, http_response_head_buffer& head, const http1_response_head_plan& plan) {
    const auto body_plan = plan.body_plan();
    const auto* buffered = plan.buffered();
    const auto content_length_plan = buffered != nullptr ? detail::http_framed_buffered_response_head_plan(body_plan, buffered->content_length(), response) : detail::http_framed_streaming_response_head_plan(body_plan, response);
    const auto* framed = std::get_if<detail::http_framed_response_head_plan>(&content_length_plan);
    if (framed == nullptr) {
        switch (std::get<detail::http_framed_response_head_plan_error>(content_length_plan)) {
            case detail::http_framed_response_head_plan_error::invalid_content_length:
                throw http1_response_head_error("invalid explicit HTTP response Content-Length");
            case detail::http_framed_response_head_plan_error::response_status_mismatch:
                throw std::invalid_argument("HTTP/1 response plan status does not match response");
            case detail::http_framed_response_head_plan_error::response_representation_mismatch:
                throw std::invalid_argument("HTTP/1 response plan representation does not match response");
            case detail::http_framed_response_head_plan_error::connect_tunnel_required:
                throw http1_response_head_error("HTTP CONNECT tunnel response required");
        }
    }
    if (!detail::response_transfer_encoding_replaceable(response)) {
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }
    const auto response_status = body_plan.response_status();
    const bool chunked_payload_plan = plan.chunked_stream() != nullptr && body_plan.transfer_encoding_allowed() && !body_plan.body_suppressed();
    if (chunked_payload_plan && plan.protocol_version() == http_protocol_version::http10) {
        throw http1_response_head_error("HTTP/1.0 response cannot use chunked Transfer-Encoding");
    }
    const bool emit_chunked_transfer_encoding = chunked_payload_plan;
    // HTTP/1 alone can frame a stream with chunked coding or connection close.
    // Those delimiters replace Content-Length except suppressed HEAD metadata.
    const auto content_length = emit_chunked_transfer_encoding || (plan.close_delimited_stream() != nullptr && !body_plan.body_suppressed()) ? std::nullopt : framed->content_length();
    const auto known_bits = detail::response_known_header_bits(response);

    const auto reason_phrase = http_reason_phrase(response_status);
    const auto date_header = (known_bits & detail::response_header_date) == 0 ? detail::cached_date_header() : std::string_view{};
    // Reuse the formatted token for exact measurement and emission instead of
    // separately dividing the length to count its digits.
    // Buffered HEAD metadata retains the selected representation length.
    // A status-level no-content policy that still owns framing (205) is
    // canonicalized to zero for both buffered and streaming heads.
    std::array<char, http_max_unsigned_decimal_size> content_length_buffer;
    const auto content_length_value = content_length.has_value() ? http_format_unsigned(content_length_buffer, *content_length) : std::string_view{};

    // Measure the exact emitted head before touching reusable output storage.
    // This both bounds the unchecked raw stack sink and enforces the shared
    // field-section rule used by request parsing.
    http_field_section_budget head_budget;
    const auto charge = [](bool fits) {
        if (!fits) {
            throw http1_response_head_error("HTTP response head is too large");
        }
    };
    charge(head_budget.add(detail::http1_status_line_size(reason_phrase)));
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    for (const auto& header : response.headers()) {
        const auto known_bit = detail::http_response_header_access::known_bit(header);
        if (known_bit == detail::response_header_transfer_encoding || known_bit == detail::response_header_content_length) {
            continue;
        }
        if (!detail::is_valid_http1_emitted_response_header(header.name(), header.value(), emit_chunked_transfer_encoding, connection_options, upgrade_protocols)) {
            throw http1_response_head_error("invalid HTTP response header");
        }
        charge(head_budget.add_field(header.name(), header.value()));
    }
    if (!detail::http_response_upgrade_pair_valid(connection_options, upgrade_protocols)) {
        throw http1_response_head_error("HTTP Upgrade response requires Connection: Upgrade");
    }
    if ((known_bits & detail::response_header_date) == 0 && !date_header.empty()) {
        charge(head_budget.add(date_header.size()));
    }
    if (emit_chunked_transfer_encoding) {
        charge(head_budget.add(detail::chunked_transfer_encoding_header.size()));
    }
    if (!content_length_value.empty()) {
        charge(head_budget.add_field(detail::content_length_name.size(), content_length_value.size()));
    }
    charge(head_budget.add(2));

    head.append_generated(head_budget.bytes(), [&](char* cursor_value) noexcept {
        detail::http_raw_wire_sink sink_value(cursor_value);
        detail::emit_response_head(response, sink_value, response_status, reason_phrase, date_header, emit_chunked_transfer_encoding, content_length_value);
        detail::check_http_wire_end(sink_value.position(), cursor_value + head_budget.bytes());
    });
}

void append_http1_response_trailers(std::string& output, const http_response_trailer_section& trailers) {
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

namespace {

// Lists one generated option; an application Connection line is extended
// rather than replaced, so its own options survive.
void add_http1_connection_option(http_response& response, std::string_view option, bool field_present) {
    if (field_present) {
        response.header(detail::http_connection_field_name, option, http_response_header_options{.mode_ = http_response_header_mode::append});
    } else {
        response.header_stable_view(detail::http_connection_field_name, option);
    }
}

// Visits each protocol the Upgrade fields of `request` offer, in field order,
// until `visitor` returns false. Malformed members end the visit.
template <typename visitor_type>
void visit_offered_upgrade_protocols(const http_request& request, visitor_type&& visitor) noexcept {
    const auto headers = request.headers();
    bool visiting = true;
    for (std::size_t index = 0; visiting && index < headers.size(); ++index) {
        if (detail::http_request_access::header_kind(request, index) != detail::http_field_kind::upgrade) {
            continue;
        }
        (void)detail::http_upgrade_protocols{}.parse_field(headers[index].value(), detail::http_field_list_role::recipient, [&visiting, &visitor](const detail::http_upgrade_protocol& protocol) noexcept {
            visiting = visitor(protocol);
            return visiting;
        });
    }
}

[[nodiscard]] bool request_offers_upgrade_protocol(const http_request& request, const detail::http_upgrade_protocol& selected) noexcept {
    bool offered = false;
    visit_offered_upgrade_protocols(request, [&offered, &selected](const detail::http_upgrade_protocol& protocol) noexcept {
        offered = detail::http_upgrade_protocol_equals(protocol, selected);
        return !offered;
    });
    return offered;
}

}  // namespace

http1_request_connection_plan http1_commit_final_response(http_response& response, http1_request_connection_plan plan) {
    const auto http1_control = detail::http1_final_response_control_plan(response);
    const auto response_options = http1_control.connection_options();
    const auto upgrade_protocols = http1_control.upgrade_protocols();
    const bool preserve_upgrade = upgrade_protocols.has_field();
    // A failed CONNECT never becomes a tunnel. RFC 9931 section 8 requires
    // closing instead of interpreting optimistic tunnel data as more requests.
    if (response_options.close() || (plan.tunnel_request() && !response.status().is_successful())) {
        plan = plan.require_close();
    }
    if (plan.disposition() == http1_close_policy::close_after_response) {
        http1_mark_connection_close(response, preserve_upgrade ? http1_connection_close_field_policy::preserve_upgrade : http1_connection_close_field_policy::close_only);
    } else {
        const bool generate_upgrade_option = preserve_upgrade && !response_options.upgrade();
        if (generate_upgrade_option) {
            add_http1_connection_option(response, detail::http_connection_upgrade_option, response_options.has_field());
        }
        if (plan.protocol_version() == http_protocol_version::http10 && !response_options.keep_alive()) {
            add_http1_connection_option(response, detail::http_connection_keep_alive_option, response_options.has_field() || generate_upgrade_option);
        }
    }
    return plan;
}

http_response_stream_head prepare_http_response_stream_head(http_response response, http_response_stream_commit_plan commit_plan) {
    if (response.status() != commit_plan.response_status()) {
        throw std::invalid_argument("response stream commit plan status does not match response");
    }
    if (!commit_plan.trailer_intent_allowed()) {
        throw std::invalid_argument("response stream status does not permit trailers");
    }
    const auto framing = commit_plan.framing();
    if (!detail::response_transfer_encoding_replaceable(response)) {
        if (framing == http_response_stream_framing::http2_frames || framing == http_response_stream_framing::http3_frames) {
            throw std::invalid_argument("HTTP/2 or HTTP/3 response cannot carry a Transfer-Encoding other than chunked");
        }
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }
    const auto body_plan = commit_plan.body_plan();
    const bool writer_owns_http1_chunked = framing == http_response_stream_framing::http1_chunked && body_plan.transfer_encoding_allowed() && !body_plan.body_suppressed();

    // Keep the prepared response metadata consistent with the wire plan. The
    // framework's chunk writer is the only Transfer-Encoding producer, so the
    // handler's chunked hint survives only as that writer's own field; HTTP/2
    // and HTTP/3 frames and an HTTP/1.0 close-delimited body carry none. An HTTP/1.0
    // close-delimited body cannot retain Content-Length either. HEAD/304 may
    // retain Content-Length metadata because their body is suppressed.
    if (writer_owns_http1_chunked) {
        response.remove_header("Content-Length");
    } else {
        response.remove_header("Transfer-Encoding");
    }
    if (framing == http_response_stream_framing::http1_close_delimited && !body_plan.body_suppressed()) {
        response.remove_header("Content-Length");
    }
    // Without a chunked body the trailer section is discarded, so the head must
    // not announce trailer fields that will never follow (RFC 9110 6.6.2).
    if (!commit_plan.trailers_available()) {
        response.remove_header("Trailer");
    }

    if (writer_owns_http1_chunked) {
        if (!detail::response_has_known_header(response, detail::response_header_transfer_encoding)) {
            response.reserve_headers(response.headers().size() + 1);
        }
        response.header_stable_view("Transfer-Encoding", "chunked");
    }

    return http_response_stream_head(std::move(response), commit_plan);
}

namespace {

// The length a body-carrying stream declares for itself: a valid
// Content-Length without a Transfer-Encoding, which overrides it (RFC 9112
// §6.3). Body-suppressed and no-content responses keep their planned framing.
[[nodiscard]] std::optional<std::uint64_t> declared_stream_content_length(const http_response& response, http_response_body_plan body_plan) noexcept {
    if (body_plan.body_suppressed() || !body_plan.status_allows_body() || detail::response_has_known_header(response, detail::response_header_transfer_encoding)) {
        return std::nullopt;
    }
    const auto field = response.header("Content-Length");
    return field.has_value() ? http_parse_content_length(*field) : std::nullopt;
}

}  // namespace

prepared_http1_response_stream prepare_http1_response_stream_head(http_response response, const http1_response_stream_plan& plan, http_response_trailer_intent trailer_intent) {
    const auto content_length = declared_stream_content_length(response, plan_http_response_body(plan.request_method(), response.status()));
    const auto framing = content_length.has_value() ? http_response_stream_framing::http1_length_delimited : plan.framing();
    auto commit_plan = plan_http_response_stream_commit(framing, plan.request_method(), response.status(), trailer_intent);
    const auto body_plan = commit_plan.body_plan();
    // HTTP/1.0 cannot delimit an open-ended response stream without closing the
    // connection, but a declared length or a response whose method/status forbids
    // payload is already self-delimited. Make this decision at head commit, when
    // the response is finally known, instead of pessimistically baking close into
    // the pre-commit plan.
    const auto planned_connection = plan.request_connection_plan().disposition() == http1_close_policy::allow_reuse && plan.close_policy() == http1_close_policy::allow_reuse && (framing != http_response_stream_framing::http1_close_delimited || body_plan.body_suppressed()) ? plan.request_connection_plan() : plan.request_connection_plan().require_close();
    const auto connection_plan = http1_commit_final_response(response, planned_connection);
    auto head = prepare_http_response_stream_head(std::move(response), std::move(commit_plan));
    const auto head_body_plan = head.commit_plan().body_plan();
    const auto response_head_plan = content_length.has_value()   ? http1_length_delimited_response_stream_head_plan(head_body_plan, connection_plan, *content_length)
        : framing == http_response_stream_framing::http1_chunked ? http1_chunked_response_stream_head_plan(head_body_plan, connection_plan)
                                                                 : http1_close_delimited_response_stream_head_plan(head_body_plan, connection_plan);
    return prepared_http1_response_stream(std::move(head), response_head_plan, connection_plan);
}

bool http1_request_offers_upgrade(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 || request.known_method() == http_known_method::connect) {
        return false;
    }
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto kind = detail::http_request_access::header_kind(request, index);
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(headers[index].value(), detail::http_connection_field_role::request_recipient) != detail::http_field_list_parse_status::ok) {
                return false;
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(headers[index].value(), detail::http_field_list_role::recipient) != detail::http_field_list_parse_status::ok) {
                return false;
            }
        }
    }
    return connection_options.upgrade() && upgrade_protocols.has_protocol();
}

http1_response_head_plan prepare_http1_upgrade_response_head(http_response& response, const http_request& request) {
    if (!http1_request_offers_upgrade(request)) {
        throw http1_response_head_error("HTTP/1 request offers no protocol upgrade");
    }
    if (response.file_body() || response.has_multipart_file_body() || !response.body_bytes().empty() || detail::response_has_known_header(response, detail::response_header_content_length | detail::response_header_transfer_encoding)) {
        throw http1_response_head_error("HTTP/1 Switching Protocols response cannot carry content or framing fields");
    }
    const auto http1_control = detail::http1_final_response_control_plan(response);
    // RFC 9110 §7.8: the server switches only to protocols the client offered.
    if (http1_control.upgrade_protocols().has_field()) {
        bool offered = http1_control.upgrade_protocols().has_protocol();
        for (const auto& header : response.headers()) {
            if (offered && detail::http_response_header_access::known_bit(header) == detail::response_header_upgrade) {
                (void)detail::http_upgrade_protocols{}.parse_field(header.value(), detail::http_field_list_role::sender, [&offered, &request](const detail::http_upgrade_protocol& selected) noexcept {
                    offered = request_offers_upgrade_protocol(request, selected);
                    return offered;
                });
            }
        }
        if (!offered) {
            throw http1_response_head_error("HTTP/1 Switching Protocols response names a protocol the request did not offer");
        }
    } else {
        std::string_view offered_protocol;
        visit_offered_upgrade_protocols(request, [&offered_protocol](const detail::http_upgrade_protocol& protocol) noexcept {
            const auto end = protocol.version_.empty() ? protocol.name_.data() + protocol.name_.size() : protocol.version_.data() + protocol.version_.size();
            offered_protocol = std::string_view(protocol.name_.data(), static_cast<std::size_t>(end - protocol.name_.data()));
            return false;
        });
        response.header("Upgrade", offered_protocol);
    }
    if (!http1_control.connection_options().upgrade()) {
        add_http1_connection_option(response, detail::http_connection_upgrade_option, http1_control.connection_options().has_field());
    }
    response.status_code_ = http_status::switching_protocols;
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, http_status::switching_protocols), http1_request_connection_plan::http11_close());
}

http_response_head_buffer::http_response_head_buffer(const http_response_head_buffer& other) :
    heap_(other.heap_) {}

http_response_head_buffer::http_response_head_buffer(http_response_head_buffer&& other) noexcept :
    heap_(std::move(other.heap_)) {
    copy_committed_state(other);
}

http_response_head_buffer& http_response_head_buffer::operator=(const http_response_head_buffer& other) {
    if (this != &other) {
        if (other.heap_.size() > heap_.capacity()) {
            // Retire the old allocation only after the complete state is published.
            std::string prepared(other.heap_);
            heap_.swap(prepared);
            copy_committed_state(other);
            return *this;
        }
        heap_ = other.heap_;
        copy_committed_state(other);
    }
    return *this;
}

http_response_head_buffer& http_response_head_buffer::operator=(http_response_head_buffer&& other) noexcept {
    if (this != &other) {
        heap_ = std::move(other.heap_);
        copy_committed_state(other);
    }
    return *this;
}

void http_response_head_buffer::copy_committed_state(const http_response_head_buffer& other) noexcept {
    // Heap replacement must succeed before publishing the inline bytes and state.
    if (const auto* const stack_state = std::get_if<stack_state_type>(&other.state_)) {
        std::memcpy(stack_.data(), other.stack_.data(), stack_state->used_);
    }
    state_ = other.state_;
}

void http_response_head_buffer::reset() noexcept {
    // Publish the empty inline state before retiring storage can reenter this buffer.
    state_.emplace<stack_state_type>();
    http_clear_string_retaining_small(heap_, response_head_retained_heap_bytes);
}

void http_response_head_buffer::spill_to_heap(std::size_t min_capacity) {
    heap_.reserve(std::max(stack_.size() * 2, min_capacity));
    heap_.assign(stack_.data(), std::get<stack_state_type>(state_).used_);
    state_.emplace<heap_state_type>();
}

void http_response_head_buffer::append(std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (value.size() <= stack_.size() - stack_state->used_) {
            std::memmove(stack_.data() + stack_state->used_, value.data(), value.size());
            stack_state->used_ += value.size();
            return;
        }
        if (value.size() > heap_.max_size() - stack_state->used_) {
            throw std::length_error("HTTP response head is too large");
        }
        spill_to_heap(stack_state->used_ + value.size());
        heap_.append(value);
        return;
    }
    if (value.size() > heap_.max_size() - heap_.size()) {
        throw std::length_error("HTTP response head is too large");
    }
    if (detail::http_text_aliases_string(value, heap_)) {
        std::string copy(value);
        heap_.append(copy);
        return;
    }
    heap_.append(value);
}

void http_response_head_buffer::append(char value) {
    if (auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        if (stack_state->used_ < stack_.size()) {
            stack_[stack_state->used_++] = value;
            return;
        }
        spill_to_heap(stack_state->used_ + 1);
    }
    heap_.push_back(value);
}

void http_response_head_buffer::append_unsigned(std::uint64_t value) {
    std::array<char, http_max_unsigned_decimal_size> digits;
    append(http_format_unsigned(digits, value));
}

void http_response_head_buffer::reserve_additional(std::size_t size) {
    if (std::holds_alternative<heap_state_type>(state_)) {
        if (size > heap_.max_size() - heap_.size()) {
            throw std::length_error("HTTP response head is too large");
        }
        heap_.reserve(heap_.size() + size);
        return;
    }
    const auto used = std::get<stack_state_type>(state_).used_;
    if (size <= stack_.size() - used) {
        return;
    }
    if (size > heap_.max_size() - used) {
        throw std::length_error("HTTP response head is too large");
    }
    spill_to_heap(used + size);
}

std::string_view http_response_head_buffer::view() const& noexcept {
    if (const auto* const stack_state = std::get_if<stack_state_type>(&state_)) {
        return std::string_view(stack_.data(), stack_state->used_);
    }
    return std::string_view(heap_);
}

bool http_response_head_buffer::can_append_on_stack(std::size_t size) const noexcept {
    const auto* const stack_state = std::get_if<stack_state_type>(&state_);
    return stack_state != nullptr && size <= stack_.size() - stack_state->used_;
}

}  // namespace ruvia
