#pragma once

#include <cstdint>
#include <type_traits>

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
class http2_final_response_control;
class http1_final_response_commit_failure;
class http1_final_response_control_plan_failure;
class http2_final_response_control_plan_failure;

template <typename control_type, typename failure_type>
class http_final_response_control_plan_result;

using http1_final_response_control_plan_result_type =
    http_final_response_control_plan_result<http1_final_response_control,
        http1_final_response_control_plan_failure>;
using http2_final_response_control_plan_result_type =
    http_final_response_control_plan_result<http2_final_response_control,
        http2_final_response_control_plan_failure>;

[[nodiscard]] http1_final_response_control_plan_result_type http1_final_response_control_plan(
    const http_response& response) noexcept;

[[nodiscard]] http2_final_response_control_plan_result_type http2_final_response_control_plan(
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

class http2_final_response_control final {
private:
    friend http2_final_response_control_plan_result_type http2_final_response_control_plan(
        const http_response&) noexcept;

    constexpr http2_final_response_control() noexcept = default;
};

class http1_final_response_control_plan_failure final {
public:
    [[nodiscard]] constexpr http1_final_response_control_plan_error error() const noexcept {
        return error_;
    }

private:
    friend class http1_final_response_commit_failure;
    template <typename control_type, typename failure_type>
    friend class http_final_response_control_plan_result;
    friend http1_final_response_control_plan_result_type http1_final_response_control_plan(
        const http_response&) noexcept;

    explicit constexpr http1_final_response_control_plan_failure(
        http1_final_response_control_plan_error error) noexcept
        : error_(error) {}

    http1_final_response_control_plan_error error_;
};

class http2_final_response_control_plan_failure final {
public:
    [[nodiscard]] constexpr http2_final_response_control_plan_error error() const noexcept {
        return error_;
    }

private:
    template <typename control_type, typename failure_type>
    friend class http_final_response_control_plan_result;
    friend http2_final_response_control_plan_result_type http2_final_response_control_plan(
        const http_response&) noexcept;

    explicit constexpr http2_final_response_control_plan_failure(
        http2_final_response_control_plan_error error) noexcept
        : error_(error) {}

    http2_final_response_control_plan_error error_;
};

// Each protocol-specific entry point returns only its validated control token or
// one typed failure. The caller already owns the protocol, so the result does not
// repeat that discriminator or admit the other protocol's impossible branch.
template <typename control_type, typename failure_type>
class http_final_response_control_plan_result final {
public:
    [[nodiscard]] const control_type* control() const& noexcept {
        return has_control_ ? &value_.control_ : nullptr;
    }
    [[nodiscard]] const control_type* control() const&& = delete;

    [[nodiscard]] const failure_type* failure() const& noexcept {
        return has_control_ ? nullptr : &value_.failure_;
    }
    [[nodiscard]] const failure_type* failure() const&& = delete;

private:
    friend http1_final_response_control_plan_result_type http1_final_response_control_plan(
        const http_response&) noexcept;
    friend http2_final_response_control_plan_result_type http2_final_response_control_plan(
        const http_response&) noexcept;

    union value {
        constexpr explicit value(control_type value) noexcept
            : control_(value) {}
        constexpr explicit value(failure_type value) noexcept
            : failure_(value) {}

        control_type control_;
        failure_type failure_;
    };

    explicit constexpr http_final_response_control_plan_result(control_type control) noexcept
        : value_(control),
          has_control_(true) {}

    explicit constexpr http_final_response_control_plan_result(failure_type failure) noexcept
        : value_(failure),
          has_control_(false) {}

    value value_;
    bool has_control_;
};

static_assert(std::is_trivially_copyable_v<http1_final_response_control_plan_result_type>);
static_assert(sizeof(http1_final_response_control_plan_result_type) <= 8);
static_assert(std::is_trivially_copyable_v<http2_final_response_control_plan_result_type>);
static_assert(sizeof(http2_final_response_control_plan_result_type) <= 2);

// Validate HTTP/1 control fields before the response mutates Connection state.
// Success owns the parsed repeated Connection and Upgrade fields.
[[nodiscard]] inline http1_final_response_control_plan_result_type http1_final_response_control_plan(
    const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!http_final_status_code_valid(status_code)) {
        return http1_final_response_control_plan_result_type(http1_final_response_control_plan_failure(
            http1_final_response_control_plan_error::invalid_status));
    }

    http_connection_options connection_options;
    http_upgrade_protocols upgrade_protocols;
    for (const auto& header : response.headers()) {
        if (http_ascii_equals_ignore_case(header.name(), "Connection")) {
            if (connection_options.parse_field(header.value(), http_field_list_role::sender,
                    [](std::string_view option) noexcept {
                        return !http_connection_option_conflicts_with_managed_field(option);
                    }) != http_field_list_parse_status::ok) {
                return http1_final_response_control_plan_result_type(http1_final_response_control_plan_failure(
                    http1_final_response_control_plan_error::invalid_connection_field));
            }
            continue;
        }
        if (http_ascii_equals_ignore_case(header.name(), "Upgrade")) {
            if (upgrade_protocols.parse_field(header.value(), http_field_list_role::sender,
                    [](const http_upgrade_protocol&) noexcept { return true; }) !=
                http_field_list_parse_status::ok) {
                return http1_final_response_control_plan_result_type(http1_final_response_control_plan_failure(
                    http1_final_response_control_plan_error::invalid_upgrade_field));
            }
            continue;
        }
        if (http_ascii_equals_ignore_case(header.name(), "TE")) {
            return http1_final_response_control_plan_result_type(http1_final_response_control_plan_failure(
                http1_final_response_control_plan_error::te_field_forbidden));
        }
    }
    if (status_code == http_status::upgrade_required && !upgrade_protocols.has_protocol()) {
        return http1_final_response_control_plan_result_type(http1_final_response_control_plan_failure(
            http1_final_response_control_plan_error::upgrade_required));
    }
    return http1_final_response_control_plan_result_type(
        http1_final_response_control(connection_options, upgrade_protocols));
}

// Validate HTTP/2 control semantics before HPACK or stream state is mutated.
// The success token proves that no connection-specific field exists; RFC 9113
// section 8.2.2 requires rejection rather than silent filtering.
[[nodiscard]] inline http2_final_response_control_plan_result_type http2_final_response_control_plan(
    const http_response& response) noexcept {
    const auto status_code = response.status();
    if (!http_final_status_code_valid(status_code)) {
        return http2_final_response_control_plan_result_type(http2_final_response_control_plan_failure(
            http2_final_response_control_plan_error::invalid_status));
    }
    if (status_code == http_status::upgrade_required) {
        return http2_final_response_control_plan_result_type(http2_final_response_control_plan_failure(
            http2_final_response_control_plan_error::upgrade_unavailable));
    }
    for (const auto& header : response.headers()) {
        if (is_forbidden_http_binary_response_field(header.name())) {
            return http2_final_response_control_plan_result_type(http2_final_response_control_plan_failure(
                http2_final_response_control_plan_error::connection_specific_field_forbidden));
        }
    }
    return http2_final_response_control_plan_result_type(http2_final_response_control{});
}

}  // namespace ruvia::detail
