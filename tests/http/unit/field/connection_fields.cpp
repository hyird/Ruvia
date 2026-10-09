#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_expectations.h"

#include "test_harness.h"

namespace {

using ruvia::http_client_expectation_is_valid;
using ruvia::http_request_content_indication;
using ruvia::http_request_expectations;
using ruvia::http_unsupported_expectation_policy;
using ruvia::detail::http_connection_options;
using ruvia::detail::http_field_list_parse_status;
using ruvia::detail::http_field_list_role;
using ruvia::detail::http_find_semicolon_parameter_ignore_case;
using ruvia::detail::http_find_semicolon_parameter_quoted_ignore_case;
using ruvia::detail::http_upgrade_protocols;

// {close, keep_alive, upgrade, te} after recipient-side parsing.
std::array<bool, 4> connection_options(std::string_view value) {
    http_connection_options options;
    if (options.parse_field(value, http_field_list_role::recipient) != http_field_list_parse_status::ok) {
        throw std::runtime_error("test expected valid Connection options");
    }
    return {options.close(), options.keep_alive(), options.upgrade(), options.te()};
}

}  // namespace

// The Connection and Upgrade fields: list roles, and what one field state commits to.

RUVIA_TEST(connection_options_parse_tokens_case_insensitively) {
    using arr_type = std::array<bool, 4>;  // {close, keep_alive, upgrade, te}

    // Single tokens, matched case-insensitively.
    RUVIA_CHECK((connection_options("close") == arr_type{true, false, false, false}));
    RUVIA_CHECK((connection_options("CLOSE") == arr_type{true, false, false, false}));
    RUVIA_CHECK((connection_options("keep-alive") == arr_type{false, true, false, false}));
    RUVIA_CHECK((connection_options("Keep-Alive") == arr_type{false, true, false, false}));
    RUVIA_CHECK((connection_options("Upgrade") == arr_type{false, false, true, false}));
    RUVIA_CHECK((connection_options("UPGRADE") == arr_type{false, false, true, false}));

    // A comma list sets each recognised token; OWS around tokens is trimmed.
    RUVIA_CHECK((connection_options("keep-alive, Upgrade") == arr_type{false, true, true, false}));
    RUVIA_CHECK((connection_options("close , upgrade") == arr_type{true, false, true, false}));
    RUVIA_CHECK((connection_options("close, keep-alive, upgrade") == arr_type{true, true, true, false}));

    // Empty list items (leading / trailing / doubled comma) are skipped, not fatal.
    RUVIA_CHECK((connection_options(",close") == arr_type{true, false, false, false}));
    RUVIA_CHECK((connection_options("close,") == arr_type{true, false, false, false}));
    RUVIA_CHECK((connection_options("keep-alive,,upgrade") == arr_type{false, true, true, false}));

    // Unrecognised tokens are ignored; a recognised neighbour still registers.
    RUVIA_CHECK((connection_options("TE, close") == arr_type{true, false, false, true}));
    RUVIA_CHECK((connection_options("x-foo") == arr_type{false, false, false, false}));
    RUVIA_CHECK((connection_options("") == arr_type{false, false, false, false}));
}

RUVIA_TEST(connection_options_commit_presence_and_tokens_in_one_byte) {
    http_connection_options options;
    RUVIA_CHECK(!options.has_field());
    RUVIA_CHECK(
        options.parse_field(", ,", http_field_list_role::recipient) == http_field_list_parse_status::ok);
    RUVIA_CHECK(options.has_field());
    RUVIA_CHECK(!options.close());
    RUVIA_CHECK(!options.upgrade());

    RUVIA_CHECK(options.parse_field("close, Upgrade", http_field_list_role::recipient) ==
                http_field_list_parse_status::ok);
    RUVIA_CHECK(options.has_field());
    RUVIA_CHECK(options.close());
    RUVIA_CHECK(options.upgrade());
}

RUVIA_TEST(connection_options_enforce_sender_and_recipient_list_roles) {
    for (const auto value : {",close", "close,", "close,,Upgrade", ""}) {
        http_connection_options sender;
        RUVIA_CHECK(sender.parse_field(value, http_field_list_role::sender) ==
                    http_field_list_parse_status::malformed);
    }

    http_connection_options repeated;
    RUVIA_CHECK(repeated.parse_field("keep-alive", http_field_list_role::sender) ==
                http_field_list_parse_status::ok);
    RUVIA_CHECK(repeated.parse_field("TE, Upgrade", http_field_list_role::sender) ==
                http_field_list_parse_status::ok);
    RUVIA_CHECK(repeated.keep_alive());
    RUVIA_CHECK(repeated.te());
    RUVIA_CHECK(repeated.upgrade());

    http_connection_options malformed;
    RUVIA_CHECK(malformed.parse_field("close;param", http_field_list_role::recipient) ==
                http_field_list_parse_status::malformed);
}

RUVIA_TEST(upgrade_protocols_commit_one_explicit_field_state) {
    http_upgrade_protocols protocols;
    RUVIA_CHECK(!protocols.has_field());
    RUVIA_CHECK(!protocols.has_protocol());

    const auto accept = [](const auto&) noexcept { return true; };
    RUVIA_CHECK(protocols.parse_field(", ,", http_field_list_role::recipient, accept) ==
                http_field_list_parse_status::ok);
    RUVIA_CHECK(protocols.has_field());
    RUVIA_CHECK(!protocols.has_protocol());

    RUVIA_CHECK(protocols.parse_field("websocket", http_field_list_role::recipient, accept) ==
                http_field_list_parse_status::ok);
    RUVIA_CHECK(protocols.has_field());
    RUVIA_CHECK(protocols.has_protocol());

    RUVIA_CHECK(protocols.parse_field("", http_field_list_role::recipient, accept) ==
                http_field_list_parse_status::ok);
    RUVIA_CHECK(protocols.has_protocol());
}

RUVIA_TEST(upgrade_protocols_only_commit_successful_fields) {
    const auto accept = [](const auto&) noexcept { return true; };

    http_upgrade_protocols malformed;
    RUVIA_CHECK(malformed.parse_field("", http_field_list_role::sender, accept) ==
                http_field_list_parse_status::malformed);
    RUVIA_CHECK(!malformed.has_field());
    RUVIA_CHECK(!malformed.has_protocol());

    http_upgrade_protocols rejected;
    RUVIA_CHECK(
        rejected.parse_field("websocket", http_field_list_role::recipient,
            [](const auto&) noexcept { return false; }) == http_field_list_parse_status::rejected);
    RUVIA_CHECK(!rejected.has_field());
    RUVIA_CHECK(!rejected.has_protocol());
}
