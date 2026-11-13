#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_status.h"

namespace {

using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::http_protocol_error;
using ruvia::http_reason_phrase;

template <typename value_type>
constexpr bool can_construct_received_status = requires(value_type value) {
    ruvia::http_received_status::try_from_value(value);
};

enum received_status_enum : unsigned { received_ok = 200 };

static_assert(can_construct_received_status<std::int64_t>);
static_assert(can_construct_received_status<std::uint64_t>);
static_assert(can_construct_received_status<char>);
static_assert(can_construct_received_status<bool>);
static_assert(!can_construct_received_status<float>);
static_assert(!can_construct_received_status<double>);
static_assert(!can_construct_received_status<long double>);
static_assert(!can_construct_received_status<received_status_enum>);

}  // namespace

TEST(Ruvia, http_status_code_validates_the_wire_value_boundary) {
    EXPECT_TRUE(ruvia::http_status_code::try_from_value(100) == ruvia::http_status::continue_value);
    EXPECT_TRUE(ruvia::http_status_code::try_from_value(599) == ruvia::http_status_code::from_value(599));
    EXPECT_TRUE(!ruvia::http_status_code::try_from_value(99).has_value());
    EXPECT_TRUE(!ruvia::http_status_code::try_from_value(600).has_value());

    bool threw = false;
    try {
        (void)ruvia::http_status_code::from_value(600);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, http_status_code_rejects_original_integers_before_wire_narrowing) {
    const auto check_rejected = [&](auto input) {
        EXPECT_TRUE(!ruvia::http_status_code::is_valid_value(input));
        EXPECT_TRUE(!ruvia::http_status_code::try_from_value(input).has_value());
        bool threw = false;
        try {
            (void)ruvia::http_status_code::from_value(input);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    };
    check_rejected(std::int64_t{65736});
    check_rejected(std::int64_t{-65336});
    check_rejected((std::numeric_limits<std::int64_t>::min)());
    check_rejected((std::numeric_limits<std::int64_t>::max)());
    check_rejected(std::uint64_t{65736});
    check_rejected((std::numeric_limits<std::uint64_t>::max)());

    EXPECT_TRUE(ruvia::http_status_code::try_from_value(std::int64_t{100}) ==
                ruvia::http_status::continue_value);
    EXPECT_TRUE(ruvia::http_status_code::try_from_value(std::uint64_t{599}) ==
                ruvia::http_status_code::from_value(599));
}

TEST(Ruvia, http_received_status_rejects_original_integers_before_wire_narrowing) {
    for (const auto value : {std::int64_t{-1}, std::int64_t{-65336},
             std::int64_t{1000}, std::int64_t{65736}, std::int64_t{4'294'967'496},
             (std::numeric_limits<std::int64_t>::min)(), (std::numeric_limits<std::int64_t>::max)()}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(ruvia::http_received_status::try_from_value(value).has_value());
    }
    for (const auto value : {std::uint64_t{1000}, std::uint64_t{65736},
             std::uint64_t{4'294'967'496}, (std::numeric_limits<std::uint64_t>::max)() - 65'335,
             (std::numeric_limits<std::uint64_t>::max)()}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(ruvia::http_received_status::try_from_value(value).has_value());
    }
}

TEST(Ruvia, http_received_status_preserves_every_three_digit_value_and_its_semantics) {
    for (std::uint64_t value = 0; value <= 999; ++value) {
        SCOPED_TRACE(value);
        const auto received = ruvia::http_received_status::try_from_value(value);
        ASSERT_TRUE(received.has_value());
        EXPECT_EQ(received->value(), value);
        const auto expected = value >= 100 && value <= 599
                                  ? ruvia::http_status_code::from_value(value)
                                  : ruvia::http_status::internal_server_error;
        EXPECT_EQ(received->status(), expected);
    }
}

TEST(Ruvia, http_reason_phrase_is_conventional_http1_presentation) {
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::ok), std::string_view("OK"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::not_found), std::string_view("Not Found"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::content_too_large), std::string_view("Content Too Large"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::internal_server_error), std::string_view("Internal Server Error"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::reset_content), std::string_view("Reset Content"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::bad_gateway), std::string_view("Bad Gateway"));
    EXPECT_EQ(http_reason_phrase(ruvia::http_status::gateway_timeout), std::string_view("Gateway Timeout"));
}

TEST(Ruvia, http_reason_phrase_does_not_mislabel_extension_statuses) {
    // 104 is still a temporary draft registration, so its unstable name is not
    // promoted into the framework's stable public vocabulary.
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(104)).empty());
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(299)).empty());
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(499)).empty());
    EXPECT_TRUE(http_reason_phrase(ruvia::http_status_code::from_value(599)).empty());
}

TEST(Ruvia, http_protocol_error_owns_bounded_diagnostic_without_allocation) {
    std::string source_value(200, 'x');
    const http_protocol_error error(
        ruvia::http_status::content_too_large, "body_too_large", source_value);
    source_value.assign(200, 'y');

    EXPECT_EQ(error.status(), ruvia::http_status::content_too_large);
    EXPECT_EQ(error.code(), std::string_view("body_too_large"));
    const auto diagnostic = std::string_view(error.what());
    EXPECT_EQ(diagnostic.size(), std::size_t{127});
    EXPECT_TRUE(diagnostic.find_first_not_of('x') == std::string_view::npos);
}

TEST(Ruvia, parse_error_status_mapping) {
    // Size limits map to their specific statuses.
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::header_too_large).status(), ruvia::http_status::request_header_fields_too_large);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::uri_too_long).status(), ruvia::http_status::uri_too_long);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::unsupported_transfer_encoding).status(), ruvia::http_status::not_implemented);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::unsupported_http_version).status(), ruvia::http_status::http_version_not_supported);
    // Everything else is a 400 Bad Request.
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::missing_host).status(), ruvia::http_status::bad_request);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::invalid_connection).status(), ruvia::http_status::bad_request);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::invalid_upgrade).status(), ruvia::http_status::bad_request);
    EXPECT_EQ(http_parse_protocol_error(http_parse_error::conflicting_content_length).status(), ruvia::http_status::bad_request);
}
