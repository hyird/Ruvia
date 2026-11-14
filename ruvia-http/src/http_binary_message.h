#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "decimal_text.h"
#include "http1_server_response.h"
#include "http_client_request_view.h"
#include "http_date_parts.h"
#include "http_field_kind.h"
#include "http_field_text.h"
#include "http_field_validation.h"
#include "http_header.h"
#include "http_message.h"
#include "http_message_access.h"
#include "http_message_framing_fields.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_response.h"
#include "http_response_framing.h"
#include "http_status.h"
#include "http_uri_syntax.h"
#include "websocket_client_handshake.h"
#include "websocket_negotiation.h"

// Message semantics shared by HTTP/2 and HTTP/3: binary field-section sizing,
// request materialization from decoded fields, outbound request validation,
// response field generation and local content accounting. HTTP/1 translation
// units never include this header.
namespace ruvia::detail {

[[nodiscard]] inline bool is_http_request_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
}

// http(s) target URIs carry a mandatory, non-empty authority with Host
// syntax; other schemes may carry any RFC 3986 authority.
[[nodiscard]] inline bool is_valid_binary_request_authority(std::string_view scheme, std::string_view authority) noexcept {
    if (is_http_request_scheme(scheme)) {
        return !authority.empty() && is_valid_host_header(authority);
    }
    return is_valid_uri_authority(authority);
}

[[nodiscard]] inline bool binary_regular_request_requires_authority(std::string_view scheme, std::string_view path) noexcept {
    return path != "*" && is_http_request_scheme(scheme);
}

// HTTP(S) already uses Host syntax; other schemes can contain URI userinfo.
[[nodiscard]] inline bool binary_request_synthesizes_host(bool has_host, bool has_authority, std::string_view scheme, std::string_view authority) noexcept {
    return !has_host && has_authority && (is_http_request_scheme(scheme) || is_valid_host_header(authority));
}

// RFC 8441 §4 / RFC 9220 §3: websocket extended CONNECT requires an HTTP(S) URI
// scheme; other extended protocols retain the full RFC 3986 space.
[[nodiscard]] inline bool is_valid_extended_connect_scheme(std::string_view protocol, std::string_view scheme) noexcept {
    return http_canonical_extended_connect_protocol(protocol) != "websocket" || is_http_request_scheme(scheme);
}

[[nodiscard]] inline bool validate_binary_request_authority_and_host(std::string_view scheme, std::string_view path, bool has_authority, std::string_view authority, bool has_host, std::string_view host) noexcept {
    if (has_authority && !is_valid_binary_request_authority(scheme, authority)) {
        return false;
    }
    if (!has_authority && binary_regular_request_requires_authority(scheme, path) && (!has_host || !is_valid_binary_request_authority(scheme, host))) {
        return false;
    }
    return !has_authority || !has_host || authority_matches_host(authority, host, http_uri_scheme_default_port(scheme));
}

// Joins split binary (RFC 9113 §8.2.3, RFC 9114 §4.2.1) Cookie crumbs with "; "
// within max_http_header_bytes. When `first_crumb` is non-empty and `joined` is
// still empty, `first_crumb` is materialized before appending the second crumb.
[[nodiscard]] inline bool append_binary_cookie_crumb(std::string& joined, std::string_view value, bool has_existing_cookie, std::string_view first_crumb = {}) {
    if (value.empty()) {
        return true;
    }
    if (value.size() > max_http_header_bytes) {
        return false;
    }
    if (!has_existing_cookie) {
        reserve_geometric(joined, value.size());
        joined.append(value.data(), value.size());
        return true;
    }
    constexpr std::string_view cookie_separator = "; ";
    const auto prefix_bytes = joined.empty() ? first_crumb.size() : joined.size();
    if (prefix_bytes > max_http_header_bytes - cookie_separator.size() || value.size() > max_http_header_bytes - cookie_separator.size() - prefix_bytes) {
        return false;
    }
    reserve_geometric(joined, prefix_bytes + cookie_separator.size() + value.size());
    if (joined.empty() && !first_crumb.empty()) {
        joined.append(first_crumb.data(), first_crumb.size());
    }
    joined.append(cookie_separator.data(), cookie_separator.size());
    joined.append(value.data(), value.size());
    return true;
}

// The one Extended CONNECT WebSocket bootstrap field list for HTTP/2 (RFC
// 8441) and HTTP/3 (RFC 9220): the negotiation's Sec-WebSocket-* request
// fields, then the application headers. The descriptors borrow both.
[[nodiscard]] inline http_header_descriptor_storage<http_header_view> stage_websocket_extended_connect_fields(const websocket_client_negotiation& negotiation, std::span<const http_header_view> headers) {
    const auto negotiation_fields = negotiation.request_fields();
    http_header_descriptor_storage<http_header_view> staged;
    staged.reserve(negotiation_fields.size() + headers.size());
    for (const auto& field : negotiation_fields) {
        staged.append(http_header_view{field.name(), field.value()});
    }
    for (const auto& field : headers) {
        staged.append(field);
    }
    return staged;
}

struct binary_request_materialization_spec final {
    http_protocol_version version_{http_protocol_version::http2};
    ::ruvia::http_request_target_form target_form_{::ruvia::http_request_target_form::http2};
    std::string_view method_{};
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view host_{};
    std::string_view path_{};
    std::string_view protocol_{};
    std::string_view cookie_{};
    std::string_view body_{};
    std::size_t regular_header_count_{0};
    bool has_authority_{false};
    bool has_host_{false};
    bool has_path_{false};
    bool has_cookie_{false};
    bool standard_connect_{false};
    const char* failure_message_{"validated binary HTTP request cannot be materialized"};
};

template <typename header_accessor_type>
[[nodiscard]] inline http_request materialize_binary_http_request(const binary_request_materialization_spec& spec, header_accessor_type&& header_at) {
    auto request = http_request_access::make();
    if (spec.method_.empty()) {
        throw std::logic_error(spec.failure_message_);
    }
    const auto target = spec.standard_connect_ ? spec.authority_ : spec.path_;
    if (target.empty() && (spec.standard_connect_ || !spec.has_path_)) {
        throw std::logic_error(spec.failure_message_);
    }
    std::string_view request_path = target;
    std::string_view query_string;
    if (!spec.standard_connect_ && target != "*") {
        if (const auto query = target.find('?'); query != std::string_view::npos) {
            request_path = target.substr(0, query);
            query_string = target.substr(query + 1);
        }
    }
    const auto effective_authority = spec.has_authority_ ? spec.authority_ : spec.host_;

    http_request_access::set_method(request, spec.method_);
    http_request_access::set_protocol_version(request, spec.version_);
    http_request_access::set_target(request, target);
    http_request_access::set_scheme(request, spec.scheme_);
    http_request_access::set_authority(request, effective_authority);
    http_request_access::set_protocol(request, spec.protocol_);
    http_request_access::set_target_form(request, spec.target_form_);
    http_request_access::set_path(request, request_path);
    http_request_access::set_query_string(request, query_string);
    http_request_access::set_body(request, std::as_bytes(std::span(spec.body_.data(), spec.body_.size())));

    // A non-HTTP target can carry an authority that is not legal Host syntax;
    // never manufacture an invalid regular field from that distinct grammar.
    const bool synthesize_host = binary_request_synthesizes_host(spec.has_host_, spec.has_authority_, spec.scheme_, spec.authority_);
    const auto header_count = spec.regular_header_count_ + static_cast<std::size_t>(synthesize_host) + static_cast<std::size_t>(spec.has_cookie_);
    if (header_count > (std::numeric_limits<std::uint16_t>::max)()) {
        throw std::logic_error(spec.failure_message_);
    }
    http_request_access::reserve_headers(request, header_count);
    for (std::size_t index = 0; index < spec.regular_header_count_; ++index) {
        const auto [field, kind] = header_at(index);
        if (!http_request_access::add_header(request, field, kind)) {
            throw std::logic_error(spec.failure_message_);
        }
    }
    if (synthesize_host && !http_request_access::add_header(request, http_header_view{"host", spec.authority_}, http_field_kind::host)) {
        throw std::logic_error(spec.failure_message_);
    }
    if (spec.has_cookie_ && !http_request_access::add_header(request, http_header_view{"cookie", spec.cookie_}, http_field_kind::cookie)) {
        throw std::logic_error(spec.failure_message_);
    }
    return request;
}

// RFC 9113 §6.5.2 / RFC 9114 §4.2.2: decoded fields, including
// pseudo-fields, cost their name and value lengths plus 32 bytes.
// Rejected additions leave the accumulated size unchanged.
class binary_field_section_size final {
public:
    explicit constexpr binary_field_section_size(std::uint64_t limit) noexcept :
        limit_(limit) {}

    [[nodiscard]] bool add(std::string_view name, std::string_view value) noexcept {
        constexpr std::uint64_t field_metadata_bytes = 32;
        const auto remaining = limit_ - bytes_;
        if (remaining < field_metadata_bytes || name.size() > remaining - field_metadata_bytes || value.size() > remaining - field_metadata_bytes - name.size()) {
            return false;
        }
        bytes_ += name.size() + value.size() + field_metadata_bytes;
        return true;
    }

private:
    std::uint64_t limit_;
    std::uint64_t bytes_{0};
};

// Borrow unchanged names; request writable storage only at the first uppercase
// byte. The provider must keep the input alive and return name.size() bytes.
template <typename prepare_type>
[[nodiscard]] inline std::string_view http_lower_header_name(std::string_view name, const prepare_type& prepare) {
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto byte = static_cast<unsigned char>(name[i]);
        if (byte < 'A' || byte > 'Z') {
            continue;
        }
        auto* target = prepare(name.size());
        if (i != 0) {
            std::memcpy(target, name.data(), i);
        }
        for (std::size_t index = i; index < name.size(); ++index) {
            target[index] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(name[index])));
        }
        return std::string_view(target, name.size());
    }
    return name;
}

// Local HTTP message content accounting is distinct from the peer-body accounting
// stored on the same bidirectional stream. In particular, a server receives request
// content while independently producing response content.
class http_local_content_known_length final {
public:
    [[nodiscard]] constexpr std::uint64_t declared_length() const noexcept {
        return declared_length_;
    }

private:
    friend class http_local_content_state;

    explicit constexpr http_local_content_known_length(std::uint64_t declared_length) noexcept :
        declared_length_(declared_length) {}

    std::uint64_t declared_length_;
};

enum class http_local_content_check : std::uint8_t { accepted,
    not_started,
    forbidden,
    length_exceeded,
    length_incomplete };

class http_local_content_state final {
    enum class phase_type : std::uint8_t { unset,
        forbidden,
        unbounded };

public:
    constexpr http_local_content_state() noexcept = default;

    void begin_forbidden() noexcept {
        reset(content_type(phase_type::forbidden));
    }

    void begin_unbounded() noexcept {
        reset(content_type(phase_type::unbounded));
    }

    void begin_known_length(std::uint64_t length) noexcept {
        reset(content_type(http_local_content_known_length(length)));
    }

    [[nodiscard]] constexpr const void* unset() const& noexcept {
        return has_phase(phase_type::unset) ? this : nullptr;
    }
    const void* unset() const&& = delete;

    [[nodiscard]] constexpr const void* forbidden() const& noexcept {
        return has_phase(phase_type::forbidden) ? this : nullptr;
    }
    const void* forbidden() const&& = delete;

    [[nodiscard]] constexpr const void* unbounded() const& noexcept {
        return has_phase(phase_type::unbounded) ? this : nullptr;
    }
    const void* unbounded() const&& = delete;

    [[nodiscard]] constexpr const http_local_content_known_length* known_length() const& noexcept {
        return std::get_if<http_local_content_known_length>(&content_);
    }
    const http_local_content_known_length* known_length() const&& = delete;

    // Transactional preflight for one submit_data input. No counters change here.
    // A terminal known-length submission must complete the declared length exactly;
    // callers can retry a rejected input with a corrected terminal flag or size.
    [[nodiscard]] http_local_content_check check_accept(std::size_t bytes_value, bool terminal) const noexcept {
        if (unset() != nullptr) {
            return http_local_content_check::not_started;
        }
        if (forbidden() != nullptr) {
            return http_local_content_check::forbidden;
        }

        const auto amount = static_cast<std::uint64_t>(bytes_value);
        if (amount > std::numeric_limits<std::uint64_t>::max() - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        const auto* known_length_content = known_length();
        if (known_length_content == nullptr) {
            return http_local_content_check::accepted;
        }
        const auto declared_length = known_length_content->declared_length();
        if (accepted_bytes_ > declared_length || amount > declared_length - accepted_bytes_) {
            return http_local_content_check::length_exceeded;
        }
        if (terminal && accepted_bytes_ + amount != declared_length) {
            return http_local_content_check::length_incomplete;
        }
        return http_local_content_check::accepted;
    }

    void commit(std::size_t bytes_value) noexcept {
        accepted_bytes_ += static_cast<std::uint64_t>(bytes_value);
    }

    [[nodiscard]] bool length_complete() const noexcept {
        if (unset() != nullptr) {
            return false;
        }
        const auto* known_length_content = known_length();
        return known_length_content == nullptr || accepted_bytes_ == known_length_content->declared_length();
    }

private:
    using content_type = std::variant<phase_type, http_local_content_known_length>;

    [[nodiscard]] constexpr bool has_phase(phase_type phase) const noexcept {
        const auto* current = std::get_if<phase_type>(&content_);
        return current != nullptr && *current == phase;
    }

    void reset(content_type content) noexcept {
        content_ = content;
        accepted_bytes_ = 0;
    }

    content_type content_{phase_type::unset};
    std::uint64_t accepted_bytes_{0};
};

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_header(std::string_view name, std::string_view value) noexcept {
    return is_valid_http_header_name(name) && is_valid_http_header_value(value) && !is_forbidden_http_binary_connection_field(name);
}

// Cross-field constraints only. Decoded :path syntax is already checked;
// senders combine this predicate with the origin-form grammar below.
[[nodiscard]] inline bool http_binary_request_path_matches_method_and_scheme(http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    if (path.empty()) {
        return !is_http_request_scheme(scheme);
    }
    return path != "*" || method == http_known_method::options;
}

[[nodiscard]] inline bool http_binary_is_valid_regular_request_path(http_known_method method, std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(method, scheme, path) && (path.empty() || path == "*" || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_extended_connect_path(std::string_view scheme, std::string_view path) noexcept {
    return http_binary_request_path_matches_method_and_scheme(http_known_method::connect, scheme, path) && (path.empty() || is_valid_origin_form_target(path));
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_method(std::string_view method, http_known_method known_method) noexcept {
    if (known_method == http_known_method::connect) {
        return false;
    }
    return known_method != http_known_method::unknown || is_valid_http_method_token(method);
}

[[nodiscard]] inline bool http_binary_are_valid_outbound_request_headers(std::optional<std::string_view> authority, std::uint16_t default_port, std::span<const http_header_view> headers, bool allow_host, bool allow_trailers, binary_field_section_size& section_size, request_field_state& fields, http_request_field_source source, bool websocket_connect = false) noexcept {
    if (!is_valid_http_request_field_source(source)) {
        return false;
    }
    websocket_request_header_validator websocket_headers(http_field_list_role::sender);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!http_binary_is_valid_outbound_regular_header(header.name(), header.value()) || !section_size.add(header.name(), header.value())) {
            return false;
        }
        const auto kind = classify_http_field(header.name());
        if ((!allow_trailers && (kind == http_field_kind::trailer || kind == http_field_kind::te)) || (!allow_host && kind == http_field_kind::host)) {
            return false;
        }
        // Content-Length and Expect belong exclusively to the typed request
        // content and expectation plans; the validator refuses raw fields.
        if (validate_request_field(kind, header.value(), http_field_list_role::sender, fields, source) != request_field_status::ok) {
            return false;
        }
        if (kind == http_field_kind::host && authority.has_value() && !authority_matches_host(*authority, header.value(), default_port)) {
            return false;
        }
        // Host is already refused for websocket extended CONNECT by the outbound field
        // check (:authority is the sole target authority there).
        if (websocket_connect && (kind == http_field_kind::sec_websocket_key || (kind == http_field_kind::other && http_ascii_equals_ignore_case(header.name(), "sec-websocket-accept")) || !websocket_headers.observe_header(kind, header.value()))) {
            return false;
        }
    }
    return !websocket_connect || websocket_headers.client_offers_valid();
}

// The one outbound extended CONNECT request check for HTTP/2 (RFC 8441
// section 4) and HTTP/3 (RFC 9220 section 3): :protocol token, scheme,
// authority, path and application fields, with every field sized against
// `limit`. Returns the :protocol value to send, or no value for an invalid
// request. A websocket request needs an HTTP(S) scheme, carries no Host field
// and offers a valid WebSocket handshake.
[[nodiscard]] inline std::optional<std::string_view> http_binary_validate_extended_connect_request(std::string_view protocol, std::string_view scheme, std::string_view authority, std::string_view path, std::span<const http_header_view> headers, http_request_field_source source, std::uint64_t limit) noexcept {
    const auto encoded_protocol = http_canonical_extended_connect_protocol(protocol);
    const bool websocket = encoded_protocol == "websocket";
    const auto default_port = http_uri_scheme_default_port(scheme);
    binary_field_section_size section_size(limit);
    request_field_state fields{request_field_framing::binary};
    if (!is_valid_http_header_name(protocol) || !is_valid_uri_scheme(scheme) || (websocket && default_port == 0) || !is_valid_binary_request_authority(scheme, authority) || !http_binary_is_valid_extended_connect_path(scheme, path) || !section_size.add(":method", "CONNECT") || !section_size.add(":protocol", encoded_protocol) || !section_size.add(":scheme", scheme) || !section_size.add(":authority", authority) || !section_size.add(":path", path) ||
        !http_binary_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/!websocket,
            /*allow_trailers=*/false, section_size, fields, source,
            /*websocket_connect=*/websocket)) {
        return std::nullopt;
    }
    return encoded_protocol;
}

[[nodiscard]] inline bool http_binary_is_valid_outbound_regular_request_head(std::string_view method, http_known_method known_method, std::string_view scheme, std::uint16_t default_port, std::optional<std::string_view> authority, std::string_view path, std::span<const http_header_view> headers, http_request_content_indication content_indication, std::string_view content_length_value, http_client_request_expectation expectation, http_request_field_source source, std::uint64_t limit) noexcept {
    if (!http_binary_is_valid_outbound_method(method, known_method) || !is_valid_uri_scheme(scheme) || (authority.has_value() && !is_valid_binary_request_authority(scheme, *authority)) || !http_binary_is_valid_regular_request_path(known_method, scheme, path) || (!authority.has_value() && binary_regular_request_requires_authority(scheme, path))) {
        return false;
    }
    // An asterisk-form target carries no authority information; binary senders
    // omit :authority (RFC 9113 §8.3.1 / RFC 9114 §4.3.1).
    if (path == "*" && authority.has_value()) {
        return false;
    }
    binary_field_section_size section_size(limit);
    bool expect_continue = false;
    switch (expectation) {
        case http_client_request_expectation::none:
            break;
        case http_client_request_expectation::continue_value:
            expect_continue = true;
            break;
        default:
            return false;
    }
    if (!http_client_expectation_is_valid(expect_continue, content_indication) || !section_size.add(":method", method) || !section_size.add(":scheme", scheme) || (authority.has_value() && !section_size.add(":authority", *authority)) || !section_size.add(":path", path) || (expect_continue && !section_size.add("expect", http_continue_expectation_token))) {
        return false;
    }
    request_field_state fields{request_field_framing::binary};
    if (!http_binary_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/true,
            /*allow_trailers=*/true, section_size, fields, source)) {
        return false;
    }
    if (!content_length_value.empty() && !section_size.add("content-length", content_length_value)) {
        return false;
    }
    if (content_indication == http_request_content_indication::no_content) {
        return true;
    }
    const auto content_semantics = classify_http_request_content_semantics(method);
    // RFC 9110 section 9.3.7 applies the Content-Type requirement to generation.
    return content_semantics != http_request_content_semantics::forbidden && (source == http_request_field_source::forwarded || content_semantics != http_request_content_semantics::content_type_required || fields.seen(http_field_kind::content_type));
}

// Sinks expose clear(), reserve(count), add(lowercase_name, borrowed_value),
// add_lowercase(name, borrowed_value), and add_owned_value(lowercase_name, value).
// add_owned_value copies temporary formatting/date bytes; the other values must
// outlive submission. Each sink applies http_field_never_indexed to every field.
// Validation and size preflight complete before any sink mutation.
template <typename sink_type>
inline void append_http_binary_response_field(sink_type& fields, std::string_view name, std::string_view value, std::uint32_t known_bit) {
    const auto known_name = response_known_header_lowercase_name(known_bit);
    if (known_name.empty()) {
        fields.add_lowercase(name, value);
    } else {
        fields.add(known_name, value);
    }
}

[[nodiscard]] inline bool validate_http_binary_interim_response_fields(const http_interim_response_head& response, std::uint64_t limit) noexcept {
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", http_status_code_token_view(status_token))) {
        return false;
    }
    http_interim_response_sender_validator validator;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto name = headers[i].name();
        const auto value = headers[i].value();
        if (validator.validate(name, value) != http_interim_response_header_validation_status::ok || is_forbidden_http_binary_response_field(name) || !section_size.add(name, value)) {
            return false;
        }
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_interim_response_fields(sink_type& fields, const http_interim_response_head& response, std::uint64_t limit) {
    if (!validate_http_binary_interim_response_fields(response, limit)) {
        return false;
    }
    fields.clear();
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    fields.reserve(headers.size() + 1);
    fields.add_owned_value(":status", http_status_code_token_view(status_token));
    for (std::size_t i = 0; i < headers.size(); ++i) {
        append_http_binary_response_field(fields, headers[i].name(), headers[i].value(), classify_response_header_name(headers[i].name()));
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_response_fields(sink_type& fields, const http_response& response, const http_framed_response_head_plan& plan, std::uint64_t limit) {
    // The owning header block already enforces syntax, sender metadata grammar
    // and singleton rules. Only binary framing and section limits remain here.
    const auto known_bits = response_known_header_bits(response);
    const auto status_token = http_status_code_token(plan.body_plan().response_status());
    const auto status_value = http_status_code_token_view(status_token);
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", status_value)) {
        return false;
    }
    // A replaceable chunked hint has no binary-protocol form; Content-Length
    // comes exclusively from the prepared plan used for content accounting.
    constexpr auto writer_framed_bits = response_header_content_length | response_header_transfer_encoding;
    for (const auto& header : response.headers()) {
        if ((http_response_header_access::known_bit(header) & writer_framed_bits) != 0) {
            continue;
        }
        if (!section_size.add(header.name(), header.value())) {
            return false;
        }
    }
    std::string_view generated_date;
    if ((known_bits & response_header_date) == 0) {
        generated_date = cached_date_value();
        if (!generated_date.empty() && !section_size.add("date", generated_date)) {
            return false;
        }
    }
    std::array<char, http_max_unsigned_decimal_size> content_length_bytes;
    std::string_view content_length_value;
    if (const auto content_length = plan.content_length()) {
        content_length_value = http_format_unsigned(content_length_bytes, *content_length);
        if (!section_size.add("content-length", content_length_value)) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response.headers().size() + 3);
    fields.add_owned_value(":status", status_value);
    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        if ((known_bit & writer_framed_bits) != 0) {
            continue;
        }
        append_http_binary_response_field(fields, header.name(), header.value(), known_bit);
    }
    if (!generated_date.empty()) {
        fields.add_owned_value("date", generated_date);
    }
    if (!content_length_value.empty()) {
        fields.add_owned_value("content-length", content_length_value);
    }
    return true;
}

[[nodiscard]] inline bool is_valid_http_binary_connect_response_head(const http_response& response) noexcept {
    const auto& body = response_body(response);
    if (!response.status().is_successful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    return !response_has_known_header(response, response_header_content_length | response_header_transfer_encoding);
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_websocket_response_fields(sink_type& fields, const websocket_server_negotiation& negotiation, std::uint64_t limit) {
    // Negotiation fields are already lowercase and validated by their owner.
    const auto response_fields = negotiation.response_headers();
    const bool has_date = std::ranges::any_of(response_fields, [](const http_header& field) {
        return field.name() == "date";
    });
    const auto date = has_date ? std::string_view{} : cached_date_value();
    const auto subprotocol = negotiation.subprotocol();
    const auto extensions = negotiation.extensions();
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", "200") || (!date.empty() && !section_size.add("date", date)) || (!subprotocol.empty() && !section_size.add("sec-websocket-protocol", subprotocol)) || (!extensions.empty() && !section_size.add("sec-websocket-extensions", extensions))) {
        return false;
    }
    for (const auto& field : response_fields) {
        if (!section_size.add(field.name(), field.value())) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response_fields.size() + 4);
    fields.add(":status", "200");
    if (!date.empty()) {
        fields.add_owned_value("date", date);
    }
    if (!subprotocol.empty()) {
        fields.add("sec-websocket-protocol", subprotocol);
    }
    if (!extensions.empty()) {
        fields.add("sec-websocket-extensions", extensions);
    }
    for (const auto& field : response_fields) {
        fields.add(field.name(), field.value());
    }
    return true;
}

}  // namespace ruvia::detail
