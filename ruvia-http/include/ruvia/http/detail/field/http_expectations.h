#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_protocol_error.h"

namespace ruvia::detail {

// The standardized Expect field member is defined once so parsers and writers
// cannot drift on its wire spelling.
inline constexpr std::string_view http_continue_expectation_token = "100-continue";

[[nodiscard]] inline bool http_expectation_token(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    for (const auto ch : token) {
        if (!is_http_token_char(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool http_expectation_quoted_string(std::string_view value) noexcept {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return false;
    }
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        auto ch = static_cast<unsigned char>(value[i]);
        if (ch == '\\') {
            ++i;
            if (i + 1 >= value.size()) {
                return false;
            }
            ch = static_cast<unsigned char>(value[i]);
        } else if (ch == '"') {
            return false;
        }
        if (!is_http_field_value_char(ch)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool http_expectation_value(std::string_view value) noexcept {
    return http_expectation_token(value) || http_expectation_quoted_string(value);
}

[[nodiscard]] inline bool http_expectation_item(std::string_view item) noexcept {
    item = http_trim_ows(item);
    if (item.empty()) {
        return false;
    }

    const auto parameters_start = http_find_unquoted_delimiter(item, 0, ';');
    const auto leading = http_trim_ows(item.substr(0, parameters_start));
    const auto equals = leading.find('=');
    if (equals == std::string_view::npos) {
        return parameters_start == item.size() && http_expectation_token(leading);
    }

    const auto name = http_trim_ows(leading.substr(0, equals));
    const auto value = http_trim_ows(leading.substr(equals + 1));
    if (!http_expectation_token(name) || !http_expectation_value(value)) {
        return false;
    }

    std::size_t start = parameters_start;
    while (start < item.size()) {
        ++start;
        const auto end = http_find_unquoted_delimiter(item, start, ';');
        const auto parameter = http_trim_ows(item.substr(start, end - start));
        const auto parameter_equals = parameter.find('=');
        if (parameter_equals == std::string_view::npos) {
            return false;
        }
        const auto parameter_name = http_trim_ows(parameter.substr(0, parameter_equals));
        const auto parameter_value = http_trim_ows(parameter.substr(parameter_equals + 1));
        if (!http_expectation_token(parameter_name) || !http_expectation_value(parameter_value)) {
            return false;
        }
        start = end;
    }
    return true;
}

// Sender-side Expect grammar. Recipients may apply a looser parsing policy for
// empty list members and unsupported extensions; a client must not generate
// syntactically invalid expectation values.
[[nodiscard]] inline bool is_valid_http_expect_field_value(std::string_view value) noexcept {
    bool valid = true;
    bool saw_item = false;
    http_visit_comma_separated_quoted_items(value, [&valid, &saw_item](std::string_view item) noexcept {
        saw_item = true;
        if (!http_expectation_item(item)) {
            valid = false;
            return false;
        }
        return true;
    });
    return valid && saw_item;
}

// Recipient-side Expect grammar. Empty list members are tolerated by the generic
// field-list parser, but every non-empty member still has to be a syntactically
// valid expectation before product policy decides whether unsupported extensions
// are rejected with 417 or ignored.
[[nodiscard]] inline bool is_valid_received_http_expect_field_value(std::string_view value) noexcept {
    bool valid = true;
    http_visit_comma_separated_quoted_items(value, [&valid](std::string_view item) noexcept {
        item = http_trim_ows(item);
        if (item.empty()) {
            return true;
        }
        if (!http_expectation_item(item)) {
            valid = false;
            return false;
        }
        return true;
    });
    return valid;
}

}  // namespace ruvia::detail

namespace ruvia {

// Whether the framing/lifecycle owner has established that request content will
// follow the initial head. Keep this typed: HTTP/1 derives it from its body plan,
// while HTTP/2 combines its receive-half and remaining-content states so an open
// metadata-only or known-empty stream cannot masquerade as pending content.
enum class http_request_content_indication : std::uint8_t { no_content,
    will_follow };

// RFC 9110 Section 10.1.1 forbids a client from generating 100-continue when
// the request has no content. Keep this sender check next to the recipient-side
// expectation state so HTTP/1 and HTTP/2 cannot derive different answers.
[[nodiscard]] constexpr bool http_client_expectation_is_valid(
    bool has_continue, http_request_content_indication content) noexcept {
    return !has_continue || content == http_request_content_indication::will_follow;
}

// Whether the product accepts unknown expectation extensions. Expect remains
// valid syntax either way; the HTTP contract owns the protocol response chosen
// by the explicit rejection policy.
enum class http_unsupported_expectation_policy : std::uint8_t { ignore,
    reject };

class http_server_expectation_plan;

class http_no_server_expectation_action final {
private:
    friend class http_server_expectation_plan;
    constexpr http_no_server_expectation_action() noexcept = default;
};

class http_send_continue final {
private:
    friend class http_server_expectation_plan;
    constexpr http_send_continue() noexcept = default;
};

class http_unsupported_expectation_rejection final {
public:
    [[nodiscard]] http_protocol_error protocol_error() const noexcept {
        return http_protocol_error(http_status::expectation_failed, "unsupported Expect header");
    }

private:
    friend class http_server_expectation_plan;
    constexpr http_unsupported_expectation_rejection() noexcept = default;
};

// No action, an interim response, and a final rejection are mutually exclusive
// protocol outcomes. Keep them typed so runtimes cannot compare a semantic enum
// and then reconstruct the required status themselves.
class http_server_expectation_plan final {
public:
    [[nodiscard]] constexpr const http_no_server_expectation_action* no_action() const& noexcept {
        return state_ == state_type::no_action ? &no_action_value : nullptr;
    }
    const http_no_server_expectation_action* no_action() const&& = delete;

    [[nodiscard]] constexpr const http_send_continue* send_continue() const& noexcept {
        return state_ == state_type::send_continue ? &send_continue_value : nullptr;
    }
    const http_send_continue* send_continue() const&& = delete;

    [[nodiscard]] constexpr const http_unsupported_expectation_rejection* rejection() const& noexcept {
        return state_ == state_type::rejection ? &rejection_value : nullptr;
    }
    const http_unsupported_expectation_rejection* rejection() const&& = delete;

private:
    friend class http_request_expectations;

    enum class state_type : std::uint8_t { no_action,
        send_continue,
        rejection };

    explicit constexpr http_server_expectation_plan(state_type state_value) noexcept
        : state_(state_value) {}

    [[nodiscard]] static constexpr http_server_expectation_plan no_action_plan() noexcept {
        return http_server_expectation_plan(state_type::no_action);
    }

    [[nodiscard]] static constexpr http_server_expectation_plan continue_plan() noexcept {
        return http_server_expectation_plan(state_type::send_continue);
    }

    [[nodiscard]] static constexpr http_server_expectation_plan rejection_plan() noexcept {
        return http_server_expectation_plan(state_type::rejection);
    }

    static inline constexpr http_no_server_expectation_action no_action_value{};
    static inline constexpr http_send_continue send_continue_value{};
    static inline constexpr http_unsupported_expectation_rejection rejection_value{};

    state_type state_;
};

// Incremental recipient-side state for the RFC 9110 Expect #list. Repeated field
// lines extend the same logical list, empty members are ignored, and all state is
// fixed-size. The only standardized member is 100-continue; every other non-empty
// member is retained as the single semantic fact "unsupported" for product policy.
class http_request_expectations final {
public:
    void parse_field(std::string_view value) noexcept {
        detail::http_visit_comma_separated_quoted(value, [this](std::string_view member) noexcept {
            if (detail::http_ascii_equals_ignore_case(member, detail::http_continue_expectation_token)) {
                flags_ |= continue_value;
            } else {
                flags_ |= unsupported;
            }
            return true;
        });
    }

    [[nodiscard]] bool has_continue() const noexcept {
        return (flags_ & continue_value) != 0;
    }

    [[nodiscard]] bool has_unsupported() const noexcept {
        return (flags_ & unsupported) != 0;
    }

    // RFC 9110 requires an HTTP/1.0 recipient to ignore 100-continue. Preserve
    // the independent unsupported-member fact so the Web product can still apply
    // its chosen extension-support policy.
    void ignore_continue() noexcept {
        flags_ &= static_cast<std::uint8_t>(~continue_value);
    }

    [[nodiscard]] http_server_expectation_plan server_plan(http_request_content_indication content,
        http_unsupported_expectation_policy unsupported_policy) const noexcept {
        if (has_unsupported() && unsupported_policy == http_unsupported_expectation_policy::reject) {
            return http_server_expectation_plan::rejection_plan();
        }
        if (has_continue() && content == http_request_content_indication::will_follow) {
            return http_server_expectation_plan::continue_plan();
        }
        return http_server_expectation_plan::no_action_plan();
    }

private:
    static constexpr std::uint8_t continue_value = 1U << 0;
    static constexpr std::uint8_t unsupported = 1U << 1;

    std::uint8_t flags_{0};
};

static_assert(std::is_trivially_copyable_v<http_request_expectations>);
static_assert(sizeof(http_request_expectations) <= 1);
static_assert(std::is_trivially_copyable_v<http_server_expectation_plan>);
static_assert(sizeof(http_server_expectation_plan) <= 2);

}  // namespace ruvia
