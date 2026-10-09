#include <array>
#include <exception>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/http/http_set_cookie_plan.h"

#include "test_harness.h"

namespace {

using ruvia::http_response;

http_response make_response() {
    return http_response({.resource_ = std::pmr::new_delete_resource()});
}

}  // namespace

RUVIA_TEST(response_header_transfer_is_atomic_and_keeps_body) {
    auto destination = make_response();
    destination.header("Content-Type", "application/json");
    destination.body("payload");
    destination.header("X-Existing", "old");

    auto source_value = make_response();
    source_value.header("Content-Type", "text/plain");
    source_value.header("X-Existing", "new");
    source_value.header("X-Added", "yes");

    destination.transfer_headers_from(source_value, ruvia::http_response_header_transfer::assign);
    RUVIA_CHECK_EQ(destination.header("Content-Type").value_or(""), "application/json");
    RUVIA_CHECK_EQ(destination.header("X-Existing").value_or(""), "new");
    RUVIA_CHECK_EQ(destination.header("X-Added").value_or(""), "yes");
    RUVIA_CHECK_EQ(destination.header("Content-Length").value_or(""), "");
}

RUVIA_TEST(response_public_write_plan_distinguishes_head_from_bodyless_status) {
    auto response = make_response();
    response.body("head-representation");

    const auto head_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    RUVIA_CHECK(head_plan.body_suppressed());
    RUVIA_CHECK_EQ(head_plan.content_length(), std::uint64_t{19});
    RUVIA_CHECK(!head_plan.send_body());
    RUVIA_CHECK(head_plan.matches_response(response));
    RUVIA_CHECK_EQ(response.body_bytes(), "head-representation");
    RUVIA_CHECK(!response.file_body().has_value());

    const auto no_content_plan = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status::no_content);
    RUVIA_CHECK(!no_content_plan.status_allows_body());
    RUVIA_CHECK(no_content_plan.body_suppressed());
    const auto head_status_plan = ruvia::plan_http_response_body(
        ruvia::http_known_method::head, ruvia::http_status::ok);
    RUVIA_CHECK(head_status_plan.status_allows_body());
    RUVIA_CHECK(head_status_plan.body_suppressed());
}

RUVIA_TEST(response_public_server_plans_preserve_head_representation_length) {
    auto response = make_response();
    response.body("head-body");

    const auto write_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    RUVIA_CHECK(write_plan.body_suppressed());
    RUVIA_CHECK_EQ(write_plan.content_length(), std::uint64_t{9});
    RUVIA_CHECK(!write_plan.send_body());
    RUVIA_CHECK(write_plan.matches_response(response));

    const auto stream_plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http2_frames, ruvia::http_known_method::head,
        response.status(), ruvia::http_response_trailer_intent::none);
    RUVIA_CHECK(stream_plan.body_plan().body_suppressed());
    RUVIA_CHECK_EQ(stream_plan.head_disposition(), ruvia::http_response_stream_head_disposition::message_ended);
}

RUVIA_TEST(response_public_trailer_validation_returns_borrowed_section) {
    const std::array trailers{ruvia::http_header_view{"ETag", "\"v1\""},
        ruvia::http_header_view{"X-Trace", "trace-1"}};
    const auto section = ruvia::validate_http_response_trailers(trailers);
    RUVIA_CHECK(!section.empty());
    RUVIA_CHECK_EQ(section.fields().size(), std::size_t{2});
    RUVIA_CHECK_EQ(ruvia::response_trailer_intent(section), ruvia::http_response_trailer_intent::present);

    const std::array invalid_trailers{ruvia::http_header_view{"Content-Type", "text/plain"}};
    bool rejected = false;
    try {
        (void)ruvia::validate_http_response_trailers(invalid_trailers);
    } catch (const std::exception&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(response_public_cookie_and_metadata_capabilities) {
    auto response = make_response();
    const ruvia::cookie_options options{.path_ = "/"};
    const ruvia::set_cookie_plan plan("sid", "first", options);
    response.set_cookie(plan);
    const ruvia::set_cookie_plan replacement("sid", "second", options);
    response.set_cookie(replacement);
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
    RUVIA_CHECK(response.header("Set-Cookie").value_or("").find("second") != std::string_view::npos);

    response.content_range(5, 3, 10);
    response.add_vary_token("Accept-Encoding");
    RUVIA_CHECK_EQ(response.header("Content-Range").value_or(""), "bytes 5-7/10");
    RUVIA_CHECK_EQ(response.header("Vary").value_or(""), "Accept-Encoding");
}
