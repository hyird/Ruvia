#include <array>
#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpResponseStream.h"

#include "test_harness.h"

namespace {

using ruvia::http_response_stream_framing;
using ruvia::http_response_stream_kind;
using ruvia::http_response_trailer_intent;
using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;

}  // namespace

RUVIA_TEST(response_stream_commit_rejects_trailers_without_wire_framing) {
    constexpr std::array framings{
        http_response_stream_framing::http1_known_length,
        http_response_stream_framing::http1_close_delimited,
        http_response_stream_framing::http1_chunked};
    constexpr std::array methods{HttpKnownMethod::kGet, HttpKnownMethod::kHead};
    for (const auto framing : framings) {
        for (const auto method : methods) {
            if (framing == http_response_stream_framing::http1_chunked && method == HttpKnownMethod::kGet) {
                continue;
            }
            HttpResponse response;
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, method, response.status(), http_response_trailer_intent::present);
            RUVIA_CHECK(!plan.trailer_intent_allowed());
            bool rejected = false;
            try {
                (void)ruvia::prepare_http_response_stream_head(
                    std::move(response), http_response_stream_kind::generic, plan);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        }
    }
}

RUVIA_TEST(response_stream_commit_keeps_allowed_trailers_open) {
    constexpr std::array framings{
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames,
        http_response_stream_framing::http3_frames};
    constexpr std::array methods{HttpKnownMethod::kGet, HttpKnownMethod::kHead};
    for (const auto framing : framings) {
        for (const auto method : methods) {
            if (framing == http_response_stream_framing::http1_chunked && method == HttpKnownMethod::kHead) {
                continue;
            }
            HttpResponse response;
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, method, response.status(), http_response_trailer_intent::present);
            RUVIA_CHECK(plan.trailer_intent_allowed());
            const auto prepared = ruvia::prepare_http_response_stream_head(
                std::move(response), http_response_stream_kind::generic, plan);
            RUVIA_CHECK_EQ(prepared.commit_plan().head_disposition(),
                method == HttpKnownMethod::kHead
                    ? ruvia::http_response_stream_head_disposition::trailers_only
                    : ruvia::http_response_stream_head_disposition::body_open);
        }
    }
}

RUVIA_TEST(response_stream_commit_rejects_trailers_for_bodyless_statuses) {
    constexpr std::array framings{
        http_response_stream_framing::http1_chunked,
        http_response_stream_framing::http2_frames,
        http_response_stream_framing::http3_frames};
    constexpr std::array statuses{
        ruvia::http_status::kNoContent,
        ruvia::http_status::kNotModified};
    for (const auto framing : framings) {
        for (const auto status : statuses) {
            HttpResponse response;
            response.status(status);
            const auto plan = ruvia::plan_http_response_stream_commit(
                framing, HttpKnownMethod::kGet, status, http_response_trailer_intent::present);
            RUVIA_CHECK(!plan.trailer_intent_allowed());
            bool rejected = false;
            try {
                (void)ruvia::prepare_http_response_stream_head(
                    std::move(response), http_response_stream_kind::generic, plan);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
        }
    }
}
