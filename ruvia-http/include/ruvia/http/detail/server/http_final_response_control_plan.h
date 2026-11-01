#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_response_head_error.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

enum class http2_final_response_control_plan_error : std::uint8_t {
    invalid_status,
    upgrade_unavailable,
    connection_specific_field_forbidden
};

class http1_final_response_control;

[[nodiscard]] http1_final_response_control http1_final_response_control_plan(const http_response& response);

[[nodiscard]] std::optional<http2_final_response_control_plan_error> http2_final_response_control_plan(
    const http_response& response) noexcept;
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

    http1_final_response_control(
        http_connection_options connection_options, http_upgrade_protocols upgrade_protocols) noexcept
        : connection_options_(connection_options),
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

    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    for (const auto& header : response.headers()) {
        switch (parse_http_response_control_field(
            classify_request_header(header.name()), header.value(), connection_options, upgrade_protocols)) {
            case http_response_control_field_status::invalid_connection:
                throw http1_response_head_error("invalid HTTP Connection header");
            case http_response_control_field_status::invalid_upgrade:
                throw http1_response_head_error("invalid HTTP Upgrade header");
            case http_response_control_field_status::te_forbidden:
                throw http1_response_head_error("TE is not a response field");
            case http_response_control_field_status::ok:
                break;
        }
    }
    // RFC 9110 §15.5.22: a 426 response MUST send an Upgrade field.
    if (status_code == http_status::upgrade_required && !upgrade_protocols.has_protocol()) {
        throw http1_response_head_error("Upgrade Required response requires an Upgrade protocol");
    }
    return http1_final_response_control(connection_options, upgrade_protocols);
}

// Validate HTTP/2 control semantics before HPACK or stream state is mutated.
// The success token proves that no connection-specific field exists; RFC 9113
// section 8.2.2 requires rejection rather than silent filtering.
[[nodiscard]] inline std::optional<http2_final_response_control_plan_error>
http2_final_response_control_plan(const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!status_code.is_final()) {
        return http2_final_response_control_plan_error::invalid_status;
    }
    if (status_code == http_status::upgrade_required) {
        return http2_final_response_control_plan_error::upgrade_unavailable;
    }
    for (const auto& header : response.headers()) {
        if (is_forbidden_http_binary_response_field(header.name())) {
            return http2_final_response_control_plan_error::connection_specific_field_forbidden;
        }
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
