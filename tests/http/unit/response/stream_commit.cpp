#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_response_stream.h"

namespace {

using ruvia::http_known_method;
using ruvia::http_response;
using ruvia::http_response_stream_framing;
using ruvia::http_response_trailer_intent;

}  // namespace

TEST(Ruvia, response_stream_commit_discards_trailers_without_wire_framing) {
    constexpr std::array framings{
        http_response_stream_framing::http1_close_delimited,
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames};
    constexpr std::array methods{http_known_method::get, http_known_method::head};
    for (const auto framing : framings) {
        for (const auto method : methods) {
            if ((framing == http_response_stream_framing::http1_chunked ||
                    framing == http_response_stream_framing::http2_frames) &&
                method == http_known_method::get) {
                continue;
            }
            http_response response;
            response.header("Trailer", "X-Checksum");
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, method, response.status(), http_response_trailer_intent::present);
            EXPECT_TRUE(plan.trailer_intent_allowed());
            EXPECT_TRUE(!plan.trailers_available());
            const auto prepared = ruvia::prepare_http_response_stream_head(
                std::move(response), plan);
            EXPECT_TRUE(!prepared.response().header("Trailer").has_value());
            if (method == http_known_method::head) {
                EXPECT_EQ(prepared.commit_plan().head_disposition(), ruvia::http_response_stream_head_disposition::message_ended);
            }
        }
    }
}

TEST(Ruvia, response_stream_commit_keeps_allowed_trailers_open) {
    constexpr std::array framings{
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames};
    for (const auto framing : framings) {
        http_response response;
        response.header("Trailer", "X-Checksum");
        const auto plan = ruvia::plan_http_response_stream_commit(
            framing, http_known_method::get, response.status(), http_response_trailer_intent::present);
        EXPECT_TRUE(plan.trailer_intent_allowed());
        EXPECT_TRUE(plan.trailers_available());
        const auto prepared = ruvia::prepare_http_response_stream_head(
            std::move(response), plan);
        EXPECT_TRUE(prepared.response().header("Trailer").has_value());
        EXPECT_EQ(prepared.commit_plan().head_disposition(), ruvia::http_response_stream_head_disposition::body_open);
    }
}

TEST(Ruvia, response_stream_commit_rejects_trailers_for_bodyless_statuses) {
    constexpr std::array framings{
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames};
    constexpr std::array statuses{
        ruvia::http_status::no_content,
        ruvia::http_status::reset_content,
        ruvia::http_status::not_modified};
    for (const auto framing : framings) {
        for (const auto status : statuses) {
            http_response response;
            response.status(status);
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, http_known_method::get, status, http_response_trailer_intent::present);
            EXPECT_TRUE(!plan.trailer_intent_allowed());
            bool rejected = false;
            try {
                (void)ruvia::prepare_http_response_stream_head(
                    std::move(response), plan);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
        }
    }
}
