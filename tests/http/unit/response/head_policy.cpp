#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>

#include "ruvia/http/http1_request_connection_plan.h"
#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

TEST(Ruvia, response_write_plan_unifies_method_status_and_body_size) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::ok);
    response.body("hello");

    const auto get_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(get_plan.body_plan().request_method() == ruvia::http_known_method::get);
    EXPECT_TRUE(get_plan.matches_response(response));
    EXPECT_EQ(get_plan.body_plan().response_status(), ruvia::http_status::ok);
    EXPECT_TRUE(get_plan.body_plan().status_allows_body());
    EXPECT_TRUE(get_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_TRUE(!get_plan.body_plan().body_suppressed());
    EXPECT_TRUE(get_plan.send_body());
    EXPECT_EQ(get_plan.content_length(), static_cast<std::uint64_t>(5));

    const auto head_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    EXPECT_EQ(head_plan.body_plan().response_status(), ruvia::http_status::ok);
    EXPECT_TRUE(head_plan.body_plan().status_allows_body());
    EXPECT_TRUE(head_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::without_content);
    EXPECT_TRUE(head_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!head_plan.send_body());
    EXPECT_EQ(head_plan.content_length(), static_cast<std::uint64_t>(5));

    response.status(ruvia::http_status::no_content);
    const auto no_content_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_EQ(no_content_plan.body_plan().response_status(), ruvia::http_status::no_content);
    EXPECT_TRUE(!no_content_plan.body_plan().status_allows_body());
    EXPECT_TRUE(no_content_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::without_content);
    EXPECT_TRUE(no_content_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!no_content_plan.send_body());
    EXPECT_EQ(no_content_plan.content_length(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::reset_content);
    const auto reset_content_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(!reset_content_plan.body_plan().status_allows_body());
    EXPECT_TRUE(reset_content_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_TRUE(reset_content_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!reset_content_plan.send_body());
    EXPECT_EQ(reset_content_plan.content_length(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::ok);
    const auto connect_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, response);
    EXPECT_TRUE(connect_plan.body_plan().status_allows_body());
    EXPECT_TRUE(connect_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::connect_tunnel);
    EXPECT_TRUE(connect_plan.body_plan().body_suppressed());
    EXPECT_TRUE(!connect_plan.send_body());
    EXPECT_EQ(connect_plan.content_length(), static_cast<std::uint64_t>(0));
}

TEST(Ruvia, response_body_plan_classifies_protocol_response_states) {
    const auto informational = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status_code::from_value(199));
    EXPECT_TRUE(informational.content_semantics() ==
                ruvia::http_response_content_semantics::informational);
    EXPECT_TRUE(informational.body_suppressed());
    EXPECT_TRUE(!informational.status_allows_body());

    const auto protocol_switch = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status::switching_protocols);
    EXPECT_TRUE(protocol_switch.content_semantics() ==
                ruvia::http_response_content_semantics::protocol_switch);
    EXPECT_TRUE(protocol_switch.body_suppressed());

    const auto connect_tunnel = ruvia::plan_http_response_body(
        ruvia::http_known_method::connect, ruvia::http_status::ok);
    EXPECT_TRUE(connect_tunnel.content_semantics() ==
                ruvia::http_response_content_semantics::connect_tunnel);
    EXPECT_TRUE(connect_tunnel.body_suppressed());
    EXPECT_TRUE(connect_tunnel.status_allows_body());

    const auto failed_connect = ruvia::plan_http_response_body(
        ruvia::http_known_method::connect, ruvia::http_status::bad_request);
    EXPECT_TRUE(failed_connect.content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_TRUE(!failed_connect.body_suppressed());
}

TEST(Ruvia, response_stream_plan_restricts_trailers_by_status_and_method) {
    for (const auto status : {ruvia::http_status_code::from_value(199),
             ruvia::http_status::no_content, ruvia::http_status::not_modified}) {
        const auto plan = ruvia::plan_http_response_stream_commit(
            ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::get,
            status, ruvia::http_response_trailer_intent::present);
        EXPECT_TRUE(!plan.trailer_intent_allowed());
    }

    const auto head_plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http1_chunked, ruvia::http_known_method::head,
        ruvia::http_status::ok, ruvia::http_response_trailer_intent::present);
    EXPECT_TRUE(head_plan.trailer_intent_allowed());
    EXPECT_TRUE(!head_plan.trailers_available());
    EXPECT_TRUE(head_plan.head_disposition() == ruvia::http_response_stream_head_disposition::message_ended);
}

TEST(Ruvia, response_write_plan_rejects_mutated_response_snapshot) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http_response response(&resource);
    response.status(ruvia::http_status::multi_status);
    response.body("old");
    const auto plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    EXPECT_TRUE(plan.matches_response(response));

    response.body("longer");
    EXPECT_TRUE(!plan.matches_response(response));
    response.body("old");
    response.status(ruvia::http_status::already_reported);
    EXPECT_TRUE(!plan.matches_response(response));
}

TEST(Ruvia, http1_response_head_framing_is_an_exclusive_plan) {
    ruvia::http_response response(std::pmr::get_default_resource());
    response.body("hello");
    const auto body_plan =
        ruvia::plan_http_response_body(ruvia::http_known_method::get, ruvia::http_status::ok);
    const auto connection_plan = ruvia::plan_http11_request_connection(false);
    const auto write_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    const auto combined = ruvia::get_http1_buffered_response_plan(write_plan, connection_plan);
    const auto& buffered = combined.head_plan();
    const auto chunked =
        ruvia::http1_chunked_response_stream_head_plan(body_plan, connection_plan);
    const auto close_delimited =
        ruvia::http1_close_delimited_response_stream_head_plan(body_plan, connection_plan);

    EXPECT_TRUE(buffered.buffered() != nullptr);
    EXPECT_TRUE(buffered.chunked_stream() == nullptr);
    EXPECT_TRUE(buffered.close_delimited_stream() == nullptr);
    EXPECT_TRUE(chunked.buffered() == nullptr);
    EXPECT_TRUE(chunked.chunked_stream() != nullptr);
    EXPECT_TRUE(chunked.close_delimited_stream() == nullptr);
    EXPECT_TRUE(close_delimited.buffered() == nullptr);
    EXPECT_TRUE(close_delimited.chunked_stream() == nullptr);
    EXPECT_TRUE(close_delimited.close_delimited_stream() != nullptr);
    EXPECT_TRUE(close_delimited.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    EXPECT_EQ(buffered.buffered()->content_length(), std::uint64_t{5});
    EXPECT_EQ(combined.content_length(), std::uint64_t{5});
    EXPECT_EQ(combined.body_plan().response_status(), ruvia::http_status::ok);
    EXPECT_TRUE(combined.send_body());
    EXPECT_TRUE(combined.body_plan().request_method() == ruvia::http_known_method::get);
    EXPECT_TRUE(buffered.protocol_version() == ruvia::http_protocol_version::http11);

    EXPECT_EQ(combined.content_length(), combined.head_plan().buffered()->content_length());
}
