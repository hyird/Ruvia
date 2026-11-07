#include <gtest/gtest.h>

#include <array>
#include <exception>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"

#include "test_exceptions.h"

namespace {

using ruvia::http_response;

http_response make_response() {
    return http_response(std::pmr::new_delete_resource());
}

}  // namespace

TEST(Ruvia, response_header_transfer_is_atomic_and_keeps_body) {
    auto destination = make_response();
    destination.header("Content-Type", "application/json");
    destination.body("payload");
    destination.header("X-Existing", "old");

    auto source_value = make_response();
    source_value.header("Content-Type", "text/plain");
    source_value.header("X-Existing", "new");
    source_value.header("X-Added", "yes");

    destination.transfer_headers_from(source_value, ruvia::http_response_header_transfer::assign);
    EXPECT_EQ(destination.header("Content-Type").value_or(""), "application/json");
    EXPECT_EQ(destination.header("X-Existing").value_or(""), "new");
    EXPECT_EQ(destination.header("X-Added").value_or(""), "yes");
    EXPECT_EQ(destination.header("Content-Length").value_or(""), "");
}

TEST(Ruvia, response_public_write_plan_distinguishes_head_from_bodyless_status) {
    auto response = make_response();
    response.body("head-representation");

    const auto head_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    EXPECT_TRUE(head_plan.body_plan().body_suppressed());
    EXPECT_EQ(head_plan.content_length(), std::uint64_t{19});
    EXPECT_TRUE(!head_plan.send_body());
    EXPECT_TRUE(head_plan.matches_response(response));
    EXPECT_EQ(response.body_bytes(), "head-representation");
    EXPECT_EQ(response.body(), "head-representation");
    EXPECT_TRUE(!response.file_body().has_value());

    const auto no_content_plan = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status::no_content);
    EXPECT_TRUE(!no_content_plan.status_allows_body());
    EXPECT_TRUE(no_content_plan.body_suppressed());
    const auto head_status_plan = ruvia::plan_http_response_body(
        ruvia::http_known_method::head, ruvia::http_status::ok);
    EXPECT_TRUE(head_status_plan.status_allows_body());
    EXPECT_TRUE(head_status_plan.body_suppressed());
}

TEST(Ruvia, response_public_server_plans_preserve_head_representation_length) {
    auto response = make_response();
    response.body("head-body");

    const auto write_plan = ruvia::plan_buffered_http_response_write(
        ruvia::http_known_method::head, response);
    EXPECT_TRUE(write_plan.body_plan().body_suppressed());
    EXPECT_EQ(write_plan.content_length(), std::uint64_t{9});
    EXPECT_TRUE(!write_plan.send_body());
    EXPECT_TRUE(write_plan.matches_response(response));

    constexpr std::array framings{
        ruvia::http_response_stream_framing::http1_chunked,
        ruvia::http_response_stream_framing::http2_frames};
    for (const auto framing : framings) {
        const auto stream_plan = ruvia::plan_http_response_stream_commit(
            framing, ruvia::http_known_method::head,
            response.status(), ruvia::http_response_trailer_intent::none);
        EXPECT_TRUE(stream_plan.body_plan().body_suppressed());
        EXPECT_EQ(stream_plan.head_disposition(), ruvia::http_response_stream_head_disposition::message_ended);
    }
}

TEST(Ruvia, response_public_trailer_validation_returns_borrowed_section) {
    const std::array trailers{ruvia::http_header_view{"ETag", "\"v1\""},
        ruvia::http_header_view{"X-Trace", "trace-1"}};
    const auto section = ruvia::validate_http_response_trailers(trailers);
    EXPECT_TRUE(!section.empty());
    EXPECT_EQ(section.fields().size(), std::size_t{2});
    EXPECT_EQ(ruvia::response_trailer_intent(section), ruvia::http_response_trailer_intent::present);

    const std::array invalid_trailers{ruvia::http_header_view{"Content-Type", "text/plain"}};
    bool rejected = false;
    try {
        (void)ruvia::validate_http_response_trailers(invalid_trailers);
    } catch (const std::exception&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, response_public_cookie_and_metadata_capabilities) {
    auto response = make_response();
    const ruvia::cookie_options options{.path_ = "/"};
    const ruvia::set_cookie_plan plan("sid", "first", options);
    response.set_cookie(plan);
    const ruvia::set_cookie_plan replacement("sid", "second", options);
    response.set_cookie(replacement);
    EXPECT_EQ(response.headers().size(), std::size_t{1});
    EXPECT_TRUE(response.header("Set-Cookie").value_or("").find("second") != std::string_view::npos);

    response.content_range(5, 3, 10);
    response.add_vary_token("Accept-Encoding");
    EXPECT_EQ(response.header("Content-Range").value_or(""), "bytes 5-7/10");
    EXPECT_EQ(response.header("Vary").value_or(""), "Accept-Encoding");
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(ruvia::testing::throws_on([&] { response.content_range(invalid, 3, 10); }));
        EXPECT_TRUE(ruvia::testing::throws_on([&] { response.content_range(5, invalid, 10); }));
        EXPECT_TRUE(ruvia::testing::throws_on([&] { response.content_range(5, 3, invalid); }));
        EXPECT_TRUE(ruvia::testing::throws_on([&] { response.content_range_unsatisfied(invalid); }));
    }
    EXPECT_EQ(response.header("Content-Range").value_or(""), "bytes 5-7/10");
    response.add_vary_token(" Accept-Encoding ");
    EXPECT_EQ(response.header("Vary").value_or(""), "Accept-Encoding");
    bool invalid_vary_rejected = false;
    try {
        response.add_vary_token("bad token");
    } catch (const std::invalid_argument&) {
        invalid_vary_rejected = true;
    }
    EXPECT_TRUE(invalid_vary_rejected);

    auto staged = response.headers().clone();
    staged.reserve(12);
    staged.allow_methods(ruvia::http_known_method_bit(ruvia::http_known_method::get));
    staged.content_range(0, 4, 10);
    staged.add_vary_token("Origin");
    EXPECT_EQ(staged.header("Allow").value_or(""), "GET");
    EXPECT_EQ(staged.header("Content-Range").value_or(""), "bytes 0-3/10");
    staged.content_range_unsatisfied(10);
    EXPECT_EQ(staged.header("Content-Range").value_or(""), "bytes */10");
    auto merged = response.headers().clone();
    merged.transfer_from(staged, ruvia::http_response_header_transfer::apply);
    EXPECT_EQ(merged.header("Allow").value_or(""), "GET");
    EXPECT_EQ(merged.header("Content-Range").value_or(""), "bytes */10");
}
