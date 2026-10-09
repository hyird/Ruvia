#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/http1_request_parser.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_status.h"

#include "test_harness.h"

namespace {

using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::http_protocol_error;
using ruvia::http_reason_phrase;

}  // namespace

RUVIA_TEST(http_status_code_validates_the_wire_value_boundary) {
    RUVIA_CHECK(ruvia::http_status_code::try_from_value(100) == ruvia::http_status::continue_value);
    RUVIA_CHECK(ruvia::http_status_code::try_from_value(599) == ruvia::http_status_code::from_value(599));
    RUVIA_CHECK(!ruvia::http_status_code::try_from_value(99).has_value());
    RUVIA_CHECK(!ruvia::http_status_code::try_from_value(600).has_value());

    bool threw = false;
    try {
        (void)ruvia::http_status_code::from_value(600);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(http_reason_phrase_is_conventional_http1_presentation) {
    RUVIA_CHECK_EQ(http_reason_phrase(ruvia::http_status::ok), std::string_view("OK"));
    RUVIA_CHECK_EQ(http_reason_phrase(ruvia::http_status::not_found), std::string_view("Not Found"));
    RUVIA_CHECK_EQ(http_reason_phrase(ruvia::http_status::content_too_large),
        std::string_view("Content Too Large"));
    RUVIA_CHECK_EQ(http_reason_phrase(ruvia::http_status::internal_server_error),
        std::string_view("Internal Server Error"));
    RUVIA_CHECK_EQ(
        http_reason_phrase(ruvia::http_status::reset_content), std::string_view("Reset Content"));
    RUVIA_CHECK_EQ(
        http_reason_phrase(ruvia::http_status::bad_gateway), std::string_view("Bad Gateway"));
    RUVIA_CHECK_EQ(
        http_reason_phrase(ruvia::http_status::gateway_timeout), std::string_view("Gateway Timeout"));
}

RUVIA_TEST(http_reason_phrase_does_not_mislabel_extension_statuses) {
    // 104 is still a temporary draft registration, so its unstable name is not
    // promoted into the framework's stable public vocabulary.
    RUVIA_CHECK(http_reason_phrase(ruvia::http_status_code::from_value(104)).empty());
    RUVIA_CHECK(http_reason_phrase(ruvia::http_status_code::from_value(299)).empty());
    RUVIA_CHECK(http_reason_phrase(ruvia::http_status_code::from_value(499)).empty());
    RUVIA_CHECK(http_reason_phrase(ruvia::http_status_code::from_value(599)).empty());
}

RUVIA_TEST(http_protocol_error_owns_bounded_diagnostic_without_allocation) {
    std::string source_value(200, 'x');
    const http_protocol_error error(ruvia::http_status::content_too_large, source_value);
    source_value.assign(200, 'y');

    RUVIA_CHECK_EQ(error.status(), ruvia::http_status::content_too_large);
    const auto diagnostic = std::string_view(error.what());
    RUVIA_CHECK_EQ(diagnostic.size(), std::size_t{127});
    RUVIA_CHECK(diagnostic.find_first_not_of('x') == std::string_view::npos);
}

RUVIA_TEST(parse_error_status_mapping) {
    // Size limits map to their specific statuses.
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::header_too_large).status(),
        ruvia::http_status::request_header_fields_too_large);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::too_many_headers).status(),
        ruvia::http_status::request_header_fields_too_large);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::body_too_large).status(),
        ruvia::http_status::content_too_large);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::unsupported_transfer_encoding).status(),
        ruvia::http_status::not_implemented);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::unsupported_http_version).status(),
        ruvia::http_status::http_version_not_supported);
    // Everything else is a 400 Bad Request.
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::missing_host).status(),
        ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::invalid_connection).status(),
        ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::invalid_upgrade).status(),
        ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::invalid_chunk_size).status(),
        ruvia::http_status::bad_request);
    RUVIA_CHECK_EQ(http_parse_protocol_error(http_parse_error::conflicting_content_length).status(),
        ruvia::http_status::bad_request);
}
