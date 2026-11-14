#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

#include "http1_server_response.h"
#include "http_connection_fields.h"
#include "http_field_kind.h"
#include "http_message_access.h"
#include "http_message_framing_fields.h"
#include "http_response.h"

// Final-response control and Content-Length framing decided before any wire
// encoding: the HTTP/1 Connection/Upgrade commit, the HTTP/2 and HTTP/3
// connection-specific field ban, and the shared framed head plan.
namespace ruvia::detail {

enum class http_binary_final_response_control_plan_error : std::uint8_t { invalid_status,
    upgrade_unavailable,
    connection_specific_field_forbidden };

// The protocol writer alone frames a response (RFC 9112 section 6.1, RFC 9113
// section 8.2.2), so an application Transfer-Encoding is accepted only as a
// hint that names exactly the single `chunked` coding, which every writer
// replaces with its own framing. Any other value claims a coding the writer
// never applies and fails closed on both protocols.
[[nodiscard]] inline bool response_transfer_encoding_replaceable(const http_response& response) noexcept {
    if (!response_has_known_header(response, response_header_transfer_encoding)) {
        return true;
    }
    return http_ascii_equals_ignore_case(http_trim_ows(http_response_header_state_access::known_header_value(response.headers(), response_header_transfer_encoding)), "chunked");
}

class http1_final_response_control final {
public:
    [[nodiscard]] http_connection_options connection_options() const noexcept {
        return connection_options_;
    }

    [[nodiscard]] http_upgrade_protocols upgrade_protocols() const noexcept {
        return upgrade_protocols_;
    }

private:
    friend http1_final_response_control http1_final_response_control_plan(const http_response&);

    http1_final_response_control(http_connection_options connection_options, http_upgrade_protocols upgrade_protocols) noexcept :
        connection_options_(connection_options),
        upgrade_protocols_(upgrade_protocols) {}

    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
};

// Validate HTTP/1 control fields before the response mutates Connection state.
// Success owns the parsed repeated Connection and Upgrade fields. http_response
// already rejects a non-final status and malformed Connection, Upgrade and TE
// fields on insertion; these checks keep the commit self-contained.
[[nodiscard]] inline http1_final_response_control http1_final_response_control_plan(const http_response& response) {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        throw http1_response_head_error("invalid final HTTP response status");
    }
    if (!response_transfer_encoding_replaceable(response)) {
        throw http1_response_head_error("HTTP response Transfer-Encoding must be exactly chunked");
    }

    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    if (response_has_known_header(response, response_header_connection | response_header_upgrade)) {
        for (const auto& header : response.headers()) {
            switch (http_response_header_access::known_bit(header)) {
                case response_header_connection:
                    if (connection_options.parse_field(header.value(), http_connection_field_role::sender) != http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Connection header");
                    }
                    break;
                case response_header_upgrade:
                    if (upgrade_protocols.parse_field(header.value(), http_field_list_role::sender) != http_field_list_parse_status::ok) {
                        throw http1_response_head_error("invalid HTTP Upgrade header");
                    }
                    break;
                default:
                    break;
            }
        }
    }
    // RFC 9110 §15.5.22: a 426 response MUST send an Upgrade field.
    if (status_code == http_status::upgrade_required && !upgrade_protocols.has_protocol()) {
        throw http1_response_head_error("Upgrade Required response requires an Upgrade protocol");
    }
    return http1_final_response_control(connection_options, upgrade_protocols);
}

// Validate HTTP/2 and HTTP/3 control semantics before field encoding or stream
// state is mutated. Success proves that no connection-specific field exists;
// RFC 9113 section 8.2.2 and RFC 9114 section 4.2 require rejection rather than
// silent filtering. A replaceable Transfer-Encoding hint is not application
// metadata but writer-owned framing, so both encoders omit it.
[[nodiscard]] inline std::optional<http_binary_final_response_control_plan_error> http_binary_final_response_control_plan(const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        return http_binary_final_response_control_plan_error::invalid_status;
    }
    if (status_code == http_status::upgrade_required) {
        return http_binary_final_response_control_plan_error::upgrade_unavailable;
    }
    if (response_has_known_header(response, response_header_connection | response_header_upgrade | response_header_keep_alive | response_header_proxy_connection) || !response_transfer_encoding_replaceable(response)) {
        return http_binary_final_response_control_plan_error::connection_specific_field_forbidden;
    }
    return std::nullopt;
}

// Shared response Content-Length semantics. HTTP/1 adds its chunked and
// close-delimited framing policy; HTTP/2 and HTTP/3 delimit content with DATA
// frames but enforce the same normalized field value.

enum class http_framed_response_head_plan_error : std::uint8_t { invalid_content_length,
    connect_tunnel_required,
    response_status_mismatch,
    response_representation_mismatch };

class http_framed_response_head_plan final {
public:
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }

    // The normalized Content-Length field to encode, if any.
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::omit ? std::nullopt : std::optional<std::uint64_t>(content_length_);
    }

    // Only an application-declared streaming length constrains subsequent DATA.
    // Framework-generated buffered lengths are already bound to their response
    // representation and therefore do not create a streaming accounting limit.
    [[nodiscard]] std::optional<std::uint64_t> streaming_content_length() const noexcept {
        return content_length_mode_ == content_length_mode_type::explicit_value ? std::optional<std::uint64_t>(content_length_) : std::nullopt;
    }

    [[nodiscard]] static http_framed_response_head_plan canonical(http_response_body_plan body_plan, std::uint64_t value) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::canonical, value);
    }

    [[nodiscard]] static std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error> preserve_explicit(http_response_body_plan body_plan, const http_response& response) noexcept {
        if (!response_has_known_header(response, response_header_content_length)) {
            return omit(body_plan);
        }

        const auto value = http_response_header_state_access::known_header_value(response.headers(), response_header_content_length);
        http_content_length_state state;
        if (state.parse_field(value) != http_content_length_parse_status::ok) {
            return http_framed_response_head_plan_error::invalid_content_length;
        }
        return http_framed_response_head_plan(body_plan, content_length_mode_type::explicit_value, *state.value());
    }

    [[nodiscard]] static http_framed_response_head_plan omit(http_response_body_plan body_plan) noexcept {
        return http_framed_response_head_plan(body_plan, content_length_mode_type::omit);
    }

private:
    enum class content_length_mode_type : std::uint8_t {
        omit,
        canonical,
        explicit_value,
    };

    http_framed_response_head_plan(http_response_body_plan body_plan, content_length_mode_type content_length_mode, std::uint64_t content_length = 0) noexcept :
        body_plan_(body_plan),
        content_length_mode_(content_length_mode),
        content_length_(content_length) {}

    http_response_body_plan body_plan_;
    content_length_mode_type content_length_mode_;
    std::uint64_t content_length_{0};
};

static_assert(std::is_trivially_copyable_v<http_framed_response_head_plan>);
static_assert(sizeof(http_framed_response_head_plan) <= 24);

using http_framed_response_head_plan_result = std::variant<http_framed_response_head_plan, http_framed_response_head_plan_error>;

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(const http_response_body_plan& body_plan, std::uint64_t content_length, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (content_length != body_plan.buffered_representation_length(response)) {
        return http_framed_response_head_plan_error::response_representation_mismatch;
    }
    // An empty suppressed HEAD body describes the GET representation only
    // through its declared Content-Length, if any (RFC 9110 section 8.6).
    if (body_plan.body_suppressed() && body_plan.status_allows_body() && body_plan.explicit_content_length_allowed() && content_length == 0 && (response.has_head_metadata_body() || response_has_known_header(response, response_header_content_length))) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    if (body_plan.auto_content_length_allowed()) {
        return http_framed_response_head_plan::canonical(body_plan, body_plan.status_allows_body() ? content_length : 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_buffered_response_head_plan(const http_buffered_response_write_plan& write_plan, const http_response& response) noexcept {
    return http_framed_buffered_response_head_plan(write_plan.body_plan(), write_plan.content_length(), response);
}

[[nodiscard]] inline http_framed_response_head_plan_result http_framed_streaming_response_head_plan(const http_response_body_plan& body_plan, const http_response& response) noexcept {
    if (body_plan.response_status() != response.status()) {
        return http_framed_response_head_plan_error::response_status_mismatch;
    }
    if (body_plan.auto_content_length_allowed() && !body_plan.status_allows_body()) {
        return http_framed_response_head_plan::canonical(body_plan, 0);
    }
    if (body_plan.explicit_content_length_allowed()) {
        return http_framed_response_head_plan::preserve_explicit(body_plan, response);
    }
    return http_framed_response_head_plan::omit(body_plan);
}

// A successful CONNECT opens a tunnel and carries no Content-Length (RFC 9110
// §9.3.6); any other outcome must take the generic final-response path.
[[nodiscard]] inline http_framed_response_head_plan_result http_framed_connect_response_head_plan(const http_response_body_plan& body_plan) noexcept {
    if (body_plan.content_semantics() == ::ruvia::http_response_content_semantics::connect_tunnel) {
        return http_framed_response_head_plan::omit(body_plan);
    }
    return http_framed_response_head_plan_error::connect_tunnel_required;
}

}  // namespace ruvia::detail
