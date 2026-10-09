#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/server/http_response_head_policy.h"
#include "ruvia/http/detail/server/http_response_write_plan.h"
#include "ruvia/http/http1_response_head_plan.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_stream.h"

#include "test_harness.h"

namespace {

using ruvia::detail::get_response_write_policy;
using ruvia::detail::response_write_policy;

}  // namespace

RUVIA_TEST(response_write_plan_unifies_method_status_and_body_size) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::ok);
    response.body("hello");

    const auto get_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(get_plan.request_method() == ruvia::http_known_method::get);
    RUVIA_CHECK(get_plan.body_plan().request_method() == ruvia::http_known_method::get);
    RUVIA_CHECK(get_plan.matches_response(response));
    RUVIA_CHECK_EQ(get_plan.response_status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(get_plan.body_plan().response_status(), ruvia::http_status::ok);
    RUVIA_CHECK(get_plan.status_allows_body());
    RUVIA_CHECK(get_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK(!get_plan.body_suppressed());
    RUVIA_CHECK(get_plan.send_body());
    RUVIA_CHECK_EQ(get_plan.content_length(), static_cast<std::uint64_t>(5));

    const auto head_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::head, response);
    RUVIA_CHECK_EQ(head_plan.response_status(), ruvia::http_status::ok);
    RUVIA_CHECK(head_plan.body_plan().status_allows_body());
    RUVIA_CHECK(head_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::without_content);
    RUVIA_CHECK(head_plan.body_suppressed());
    RUVIA_CHECK(!head_plan.send_body());
    RUVIA_CHECK_EQ(head_plan.content_length(), static_cast<std::uint64_t>(5));

    response.status(ruvia::http_status::no_content);
    const auto no_content_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK_EQ(no_content_plan.response_status(), ruvia::http_status::no_content);
    RUVIA_CHECK(!no_content_plan.body_plan().status_allows_body());
    RUVIA_CHECK(no_content_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::without_content);
    RUVIA_CHECK(no_content_plan.body_suppressed());
    RUVIA_CHECK(!no_content_plan.send_body());
    RUVIA_CHECK_EQ(no_content_plan.content_length(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::reset_content);
    const auto reset_content_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(!reset_content_plan.body_plan().status_allows_body());
    RUVIA_CHECK(reset_content_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK(reset_content_plan.body_suppressed());
    RUVIA_CHECK(!reset_content_plan.send_body());
    RUVIA_CHECK_EQ(reset_content_plan.content_length(), static_cast<std::uint64_t>(0));

    response.status(ruvia::http_status::ok);
    const auto connect_plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::connect, response);
    RUVIA_CHECK(connect_plan.status_allows_body());
    RUVIA_CHECK(connect_plan.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::connect_tunnel);
    RUVIA_CHECK(connect_plan.body_suppressed());
    RUVIA_CHECK(!connect_plan.send_body());
    RUVIA_CHECK_EQ(connect_plan.content_length(), static_cast<std::uint64_t>(0));
}

RUVIA_TEST(response_body_plan_classifies_protocol_response_states) {
    const auto informational = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status_code::from_value(199));
    RUVIA_CHECK(informational.content_semantics() ==
                ruvia::http_response_content_semantics::informational);
    RUVIA_CHECK(informational.body_suppressed());
    RUVIA_CHECK(!informational.status_allows_body());

    const auto protocol_switch = ruvia::plan_http_response_body(
        ruvia::http_known_method::get, ruvia::http_status::switching_protocols);
    RUVIA_CHECK(protocol_switch.content_semantics() ==
                ruvia::http_response_content_semantics::protocol_switch);
    RUVIA_CHECK(protocol_switch.body_suppressed());

    const auto connect_tunnel = ruvia::plan_http_response_body(
        ruvia::http_known_method::connect, ruvia::http_status::ok);
    RUVIA_CHECK(connect_tunnel.content_semantics() ==
                ruvia::http_response_content_semantics::connect_tunnel);
    RUVIA_CHECK(connect_tunnel.body_suppressed());
    RUVIA_CHECK(connect_tunnel.status_allows_body());

    const auto failed_connect = ruvia::plan_http_response_body(
        ruvia::http_known_method::connect, ruvia::http_status::bad_request);
    RUVIA_CHECK(failed_connect.content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK(!failed_connect.body_suppressed());
}

RUVIA_TEST(response_stream_plan_restricts_trailers_by_status_not_method) {
    for (const auto status : {ruvia::http_status_code::from_value(199),
             ruvia::http_status::no_content, ruvia::http_status::not_modified}) {
        const auto plan = ruvia::plan_http_response_stream_commit(
            ruvia::http_response_stream_framing::http2_frames, ruvia::http_known_method::get,
            status, ruvia::http_response_trailer_intent::present);
        RUVIA_CHECK(!plan.trailer_intent_allowed());
    }

    const auto head_plan = ruvia::plan_http_response_stream_commit(
        ruvia::http_response_stream_framing::http2_frames, ruvia::http_known_method::head,
        ruvia::http_status::ok, ruvia::http_response_trailer_intent::present);
    RUVIA_CHECK(head_plan.trailer_intent_allowed());
    RUVIA_CHECK(head_plan.head_disposition() == ruvia::http_response_stream_head_disposition::trailers_only);
}

RUVIA_TEST(response_write_plan_rejects_mutated_response_snapshot) {
    std::pmr::monotonic_buffer_resource resource;
    ruvia::http_response response({.resource_ = &resource});
    response.status(ruvia::http_status::multi_status);
    response.body("old");
    const auto plan =
        ruvia::plan_buffered_http_response_write(ruvia::http_known_method::get, response);
    RUVIA_CHECK(plan.matches_response(response));

    response.body("longer");
    RUVIA_CHECK(!plan.matches_response(response));
    response.body("old");
    response.status(ruvia::http_status::already_reported);
    RUVIA_CHECK(!plan.matches_response(response));
}

RUVIA_TEST(response_policy_normal_status_allows_everything) {
    for (const ruvia::http_status_code status :
        {ruvia::http_status::ok, ruvia::http_status::partial_content,
            ruvia::http_status::not_found, ruvia::http_status::internal_server_error}) {
        const auto policy = get_response_write_policy(status);
        RUVIA_CHECK(policy.normal() != nullptr);
        RUVIA_CHECK(policy.body_forbidden() == nullptr);
        RUVIA_CHECK(policy.zero_length() == nullptr);
        RUVIA_CHECK(policy.not_modified() == nullptr);
        RUVIA_CHECK(policy.body_allowed());
        RUVIA_CHECK(policy.auto_content_length_allowed());
        RUVIA_CHECK(policy.explicit_content_length_allowed());
        RUVIA_CHECK(policy.transfer_encoding_allowed());
    }
}

RUVIA_TEST(response_policy_bodyless_statuses_forbid_all_framing) {
    // 1xx informational and 204 are terminated by the empty line regardless of
    // headers (RFC 9112 §6.3 rule 1), so they carry no body and no framing headers.
    for (const ruvia::http_status_code status :
        {ruvia::http_status::continue_value, ruvia::http_status::switching_protocols,
            ruvia::http_status_code::from_value(199), ruvia::http_status::no_content}) {
        const auto policy = get_response_write_policy(status);
        RUVIA_CHECK(policy.normal() == nullptr);
        RUVIA_CHECK(policy.body_forbidden() != nullptr);
        RUVIA_CHECK(policy.zero_length() == nullptr);
        RUVIA_CHECK(policy.not_modified() == nullptr);
        RUVIA_CHECK(!policy.body_allowed());
        RUVIA_CHECK(!policy.auto_content_length_allowed());
        RUVIA_CHECK(!policy.explicit_content_length_allowed());
        RUVIA_CHECK(!policy.transfer_encoding_allowed());
    }
}

RUVIA_TEST(response_policy_reset_content_owns_zero_length_framing) {
    // RFC 9110 §15.3.6 forbids content in 205. HTTP/1 does not infer a zero
    // length from that status, so the writer owns one canonical Content-Length:
    // 0 and rejects both caller-owned length and transfer coding declarations.
    const auto policy = get_response_write_policy(ruvia::http_status::reset_content);
    RUVIA_CHECK(policy.normal() == nullptr);
    RUVIA_CHECK(policy.body_forbidden() == nullptr);
    RUVIA_CHECK(policy.zero_length() != nullptr);
    RUVIA_CHECK(policy.not_modified() == nullptr);
    RUVIA_CHECK(!policy.body_allowed());
    RUVIA_CHECK(policy.auto_content_length_allowed());
    RUVIA_CHECK(!policy.explicit_content_length_allowed());
    RUVIA_CHECK(!policy.transfer_encoding_allowed());
}

RUVIA_TEST(response_policy_not_modified_keeps_explicit_content_length) {
    // 304 has no body, but may echo the Content-Length of the selected
    // representation; auto length and transfer-encoding stay forbidden.
    const auto policy = get_response_write_policy(ruvia::http_status::not_modified);
    RUVIA_CHECK(policy.normal() == nullptr);
    RUVIA_CHECK(policy.body_forbidden() == nullptr);
    RUVIA_CHECK(policy.zero_length() == nullptr);
    RUVIA_CHECK(policy.not_modified() != nullptr);
    RUVIA_CHECK(!policy.body_allowed());
    RUVIA_CHECK(!policy.auto_content_length_allowed());
    RUVIA_CHECK(policy.explicit_content_length_allowed());
    RUVIA_CHECK(!policy.transfer_encoding_allowed());
}

RUVIA_TEST(http1_response_head_framing_is_an_exclusive_plan) {
    ruvia::http_response response({.resource_ = std::pmr::get_default_resource()});
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

    RUVIA_CHECK(buffered.buffered() != nullptr);
    RUVIA_CHECK(buffered.chunked_stream() == nullptr);
    RUVIA_CHECK(buffered.close_delimited_stream() == nullptr);
    RUVIA_CHECK(chunked.buffered() == nullptr);
    RUVIA_CHECK(chunked.chunked_stream() != nullptr);
    RUVIA_CHECK(chunked.close_delimited_stream() == nullptr);
    RUVIA_CHECK(close_delimited.buffered() == nullptr);
    RUVIA_CHECK(close_delimited.chunked_stream() == nullptr);
    RUVIA_CHECK(close_delimited.close_delimited_stream() != nullptr);
    RUVIA_CHECK(close_delimited.body_plan().content_semantics() ==
                ruvia::http_response_content_semantics::with_content);
    RUVIA_CHECK_EQ(buffered.buffered()->content_length(), std::uint64_t{5});
    RUVIA_CHECK_EQ(combined.content_length(), std::uint64_t{5});
    RUVIA_CHECK_EQ(combined.response_status(), ruvia::http_status::ok);
    RUVIA_CHECK(combined.send_body());
    RUVIA_CHECK(combined.body_plan().request_method() == ruvia::http_known_method::get);
    RUVIA_CHECK(buffered.protocol_version() == ruvia::http_protocol_version::http11);

    RUVIA_CHECK_EQ(combined.content_length(), combined.head_plan().buffered()->content_length());
}
