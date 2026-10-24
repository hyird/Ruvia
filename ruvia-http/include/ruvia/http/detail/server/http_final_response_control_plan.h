#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_status.h"

namespace ruvia::detail {

enum class http1_final_response_control_plan_error : std::uint8_t {
    invalid_status,
    invalid_connection_field,
    invalid_upgrade_field,
    upgrade_required,
    te_field_forbidden,
};

enum class http2_final_response_control_plan_error : std::uint8_t {
    invalid_status,
    upgrade_unavailable,
    connection_specific_field_forbidden
};

class http1_final_response_control;

using http1_final_response_control_plan_result_type =
    std::variant<http1_final_response_control, http1_final_response_control_plan_error>;

[[nodiscard]] http1_final_response_control_plan_result_type http1_final_response_control_plan(
    const http_response& response) noexcept;

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
    friend http1_final_response_control_plan_result_type http1_final_response_control_plan(
        const http_response&) noexcept;

    http1_final_response_control(
        http_connection_options connection_options, http_upgrade_protocols upgrade_protocols) noexcept
        : connection_options_(connection_options),
          upgrade_protocols_(upgrade_protocols) {}

    http_connection_options connection_options_;
    http_upgrade_protocols upgrade_protocols_;
};

// Validate HTTP/1 control fields before the response mutates Connection state.
// Success owns the parsed repeated Connection and Upgrade fields.
[[nodiscard]] inline http1_final_response_control_plan_result_type http1_final_response_control_plan(
    const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!http_final_status_code_valid(status_code)) {
        return http1_final_response_control_plan_error::invalid_status;
    }

    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    for (const auto& header : response.headers()) {
        switch (classify_request_header(header.name())) {
            case request_header_kind::connection:
                if (connection_options.parse_field(header.value(), http_field_list_role::sender) !=
                    http_field_list_parse_status::ok) {
                    return http1_final_response_control_plan_error::invalid_connection_field;
                }
                break;
            case request_header_kind::upgrade:
                if (upgrade_protocols.parse_field(header.value(), http_field_list_role::sender) !=
                    http_field_list_parse_status::ok) {
                    return http1_final_response_control_plan_error::invalid_upgrade_field;
                }
                break;
            case request_header_kind::te:
                return http1_final_response_control_plan_error::te_field_forbidden;
            default:
                break;
        }
    }
    if (status_code == http_status::upgrade_required && !upgrade_protocols.has_protocol()) {
        return http1_final_response_control_plan_error::upgrade_required;
    }
    return http1_final_response_control(connection_options, upgrade_protocols);
}

// Validate HTTP/2 control semantics before HPACK or stream state is mutated.
// The success token proves that no connection-specific field exists; RFC 9113
// section 8.2.2 requires rejection rather than silent filtering.
[[nodiscard]] inline std::optional<http2_final_response_control_plan_error>
http2_final_response_control_plan(const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!http_final_status_code_valid(status_code)) {
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
