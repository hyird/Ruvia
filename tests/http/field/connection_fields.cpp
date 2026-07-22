#include "test_harness.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"
#include "ruvia/http/detail/field/HttpExpectations.h"

namespace {

using ruvia::detail::httpFindSemicolonParameterIgnoreCase;
using ruvia::detail::httpFindSemicolonParameterQuotedIgnoreCase;
using ruvia::detail::httpClientExpectationIsValid;
using ruvia::detail::HttpConnectionOptions;
using ruvia::detail::HttpFieldListParseStatus;
using ruvia::detail::HttpFieldListRole;
using ruvia::detail::HttpRequestContentIndication;
using ruvia::detail::HttpRequestExpectations;
using ruvia::detail::HttpUnsupportedExpectationPolicy;
using ruvia::detail::HttpUpgradeProtocols;

// {close, keepAlive, upgrade, te} after recipient-side parsing.
std::array<bool, 4> connectionOptions(std::string_view value) {
    HttpConnectionOptions options;
    if (options.parseField(value, HttpFieldListRole::kRecipient) !=
        HttpFieldListParseStatus::kOk) {
        throw std::runtime_error("test expected valid Connection options");
    }
    return {
        options.close(),
        options.keepAlive(),
        options.upgrade(),
        options.te()};
}

}  // namespace

// The Connection and Upgrade fields: list roles, and what one field state commits to.

RUVIA_TEST(connection_options_parse_tokens_case_insensitively) {
    using Arr = std::array<bool, 4>;  // {close, keepAlive, upgrade, te}

    // Single tokens, matched case-insensitively.
    RUVIA_CHECK((connectionOptions("close") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("CLOSE") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("keep-alive") == Arr{false, true, false, false}));
    RUVIA_CHECK((connectionOptions("Keep-Alive") == Arr{false, true, false, false}));
    RUVIA_CHECK((connectionOptions("Upgrade") == Arr{false, false, true, false}));
    RUVIA_CHECK((connectionOptions("UPGRADE") == Arr{false, false, true, false}));

    // A comma list sets each recognised token; OWS around tokens is trimmed.
    RUVIA_CHECK((connectionOptions("keep-alive, Upgrade") == Arr{false, true, true, false}));
    RUVIA_CHECK((connectionOptions("close , upgrade") == Arr{true, false, true, false}));
    RUVIA_CHECK((connectionOptions("close, keep-alive, upgrade") == Arr{true, true, true, false}));

    // Empty list items (leading / trailing / doubled comma) are skipped, not fatal.
    RUVIA_CHECK((connectionOptions(",close") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("close,") == Arr{true, false, false, false}));
    RUVIA_CHECK((connectionOptions("keep-alive,,upgrade") == Arr{false, true, true, false}));

    // Unrecognised tokens are ignored; a recognised neighbour still registers.
    RUVIA_CHECK((connectionOptions("TE, close") == Arr{true, false, false, true}));
    RUVIA_CHECK((connectionOptions("x-foo") == Arr{false, false, false, false}));
    RUVIA_CHECK((connectionOptions("") == Arr{false, false, false, false}));
}

RUVIA_TEST(connection_options_commit_presence_and_tokens_in_one_byte) {
    static_assert(sizeof(HttpConnectionOptions) == 1);

    HttpConnectionOptions options;
    RUVIA_CHECK(!options.hasField());
    RUVIA_CHECK(
        options.parseField(", ,", HttpFieldListRole::kRecipient) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(options.hasField());
    RUVIA_CHECK(!options.close());
    RUVIA_CHECK(!options.upgrade());

    RUVIA_CHECK(
        options.parseField("close, Upgrade", HttpFieldListRole::kRecipient) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(options.hasField());
    RUVIA_CHECK(options.close());
    RUVIA_CHECK(options.upgrade());
}

RUVIA_TEST(connection_options_enforce_sender_and_recipient_list_roles) {
    for (const auto value : {",close", "close,", "close,,Upgrade", ""}) {
        HttpConnectionOptions sender;
        RUVIA_CHECK(
            sender.parseField(value, HttpFieldListRole::kSender) ==
            HttpFieldListParseStatus::kMalformed);
    }

    HttpConnectionOptions repeated;
    RUVIA_CHECK(
        repeated.parseField("keep-alive", HttpFieldListRole::kSender) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(
        repeated.parseField("TE, Upgrade", HttpFieldListRole::kSender) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(repeated.keepAlive());
    RUVIA_CHECK(repeated.te());
    RUVIA_CHECK(repeated.upgrade());

    HttpConnectionOptions malformed;
    RUVIA_CHECK(
        malformed.parseField("close;param", HttpFieldListRole::kRecipient) ==
        HttpFieldListParseStatus::kMalformed);
}

RUVIA_TEST(upgrade_protocols_commit_one_explicit_field_state) {
    HttpUpgradeProtocols protocols;
    RUVIA_CHECK(!protocols.hasField());
    RUVIA_CHECK(!protocols.hasProtocol());

    const auto accept = [](const auto&) noexcept { return true; };
    RUVIA_CHECK(
        protocols.parseField(", ,", HttpFieldListRole::kRecipient, accept) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(protocols.hasField());
    RUVIA_CHECK(!protocols.hasProtocol());

    RUVIA_CHECK(
        protocols.parseField("websocket", HttpFieldListRole::kRecipient, accept) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(protocols.hasField());
    RUVIA_CHECK(protocols.hasProtocol());

    RUVIA_CHECK(
        protocols.parseField("", HttpFieldListRole::kRecipient, accept) ==
        HttpFieldListParseStatus::kOk);
    RUVIA_CHECK(protocols.hasProtocol());
}

RUVIA_TEST(upgrade_protocols_only_commit_successful_fields) {
    const auto accept = [](const auto&) noexcept { return true; };

    HttpUpgradeProtocols malformed;
    RUVIA_CHECK(
        malformed.parseField("", HttpFieldListRole::kSender, accept) ==
        HttpFieldListParseStatus::kMalformed);
    RUVIA_CHECK(!malformed.hasField());
    RUVIA_CHECK(!malformed.hasProtocol());

    HttpUpgradeProtocols rejected;
    RUVIA_CHECK(
        rejected.parseField(
            "websocket", HttpFieldListRole::kRecipient,
            [](const auto&) noexcept { return false; }) ==
        HttpFieldListParseStatus::kRejected);
    RUVIA_CHECK(!rejected.hasField());
    RUVIA_CHECK(!rejected.hasProtocol());
}
