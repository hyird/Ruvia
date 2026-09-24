#include <array>
#include <exception>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/http/HttpSetCookiePlan.h"

#include "test_harness.h"

namespace {

using ruvia::HttpResponse;

HttpResponse makeResponse() {
    return HttpResponse({.resource = std::pmr::new_delete_resource()});
}

}  // namespace

RUVIA_TEST(response_header_transfer_is_atomic_and_keeps_body) {
    auto destination = makeResponse();
    destination.header("Content-Type", "application/json");
    destination.body("payload");
    destination.header("X-Existing", "old");

    auto source = makeResponse();
    source.header("Content-Type", "text/plain");
    source.header("X-Existing", "new");
    source.header("X-Added", "yes");

    destination.transferHeadersFrom(source, ruvia::HttpResponseHeaderTransfer::kAssign);
    RUVIA_CHECK_EQ(destination.header("Content-Type").value_or(""), "application/json");
    RUVIA_CHECK_EQ(destination.header("X-Existing").value_or(""), "new");
    RUVIA_CHECK_EQ(destination.header("X-Added").value_or(""), "yes");
    RUVIA_CHECK_EQ(destination.header("Content-Length").value_or(""), "");
}

RUVIA_TEST(response_public_write_plan_distinguishes_head_from_bodyless_status) {
    auto response = makeResponse();
    response.body("head-representation");

    const auto headPlan = ruvia::planBufferedHttpResponseWrite(
        ruvia::HttpKnownMethod::kHead, response);
    RUVIA_CHECK(headPlan.bodySuppressed());
    RUVIA_CHECK_EQ(headPlan.contentLength(), std::uint64_t{19});
    RUVIA_CHECK(!headPlan.sendBody());
    RUVIA_CHECK(headPlan.matchesResponse(response));
    RUVIA_CHECK_EQ(response.bodyBytes(), "head-representation");
    RUVIA_CHECK(!response.fileBody().has_value());

    const auto noContentPlan = ruvia::planHttpResponseBody(
        ruvia::HttpKnownMethod::kGet, ruvia::http_status::kNoContent);
    RUVIA_CHECK(!noContentPlan.statusAllowsBody());
    RUVIA_CHECK(noContentPlan.bodySuppressed());
    const auto headStatusPlan = ruvia::planHttpResponseBody(
        ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk);
    RUVIA_CHECK(headStatusPlan.statusAllowsBody());
    RUVIA_CHECK(headStatusPlan.bodySuppressed());
}

RUVIA_TEST(response_public_server_plans_preserve_head_representation_length) {
    auto response = makeResponse();
    response.body("head-body");

    const auto writePlan = ruvia::planHttpServerBufferedResponseWrite(
        ruvia::HttpKnownMethod::kHead, response);
    RUVIA_CHECK(writePlan.bodySuppressed());
    RUVIA_CHECK_EQ(writePlan.contentLength(), std::uint64_t{9});
    RUVIA_CHECK(!writePlan.sendBody());
    RUVIA_CHECK(writePlan.matchesResponse(response));

    const auto streamPlan = ruvia::planHttpResponseStreamCommit(
        ruvia::ResponseStreamFraming::kHttp2Frames, ruvia::HttpKnownMethod::kHead,
        response.status(), ruvia::ResponseTrailerIntent::kNone);
    RUVIA_CHECK(streamPlan.bodyPlan().bodySuppressed());
    RUVIA_CHECK_EQ(streamPlan.headDisposition(), ruvia::ResponseStreamHeadDisposition::kMessageEnded);
}

RUVIA_TEST(response_public_trailer_validation_returns_borrowed_section) {
    const std::array trailers{ruvia::HttpHeaderView{"ETag", "\"v1\""},
        ruvia::HttpHeaderView{"X-Trace", "trace-1"}};
    const auto section = ruvia::validateHttpResponseTrailers(trailers);
    RUVIA_CHECK(!section.empty());
    RUVIA_CHECK_EQ(section.fields().size(), std::size_t{2});
    RUVIA_CHECK_EQ(ruvia::httpResponseTrailerIntent(section), ruvia::ResponseTrailerIntent::kPresent);

    const std::array invalidTrailers{ruvia::HttpHeaderView{"Content-Type", "text/plain"}};
    bool rejected = false;
    try {
        (void)ruvia::validateHttpResponseTrailers(invalidTrailers);
    } catch (const std::exception&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(response_public_cookie_and_metadata_capabilities) {
    auto response = makeResponse();
    const ruvia::CookieOptions options{.path = "/"};
    const ruvia::SetCookiePlan plan("sid", "first", options);
    response.setCookie(plan);
    const ruvia::SetCookiePlan replacement("sid", "second", options);
    response.setCookie(replacement);
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
    RUVIA_CHECK(response.header("Set-Cookie").value_or("").find("second") != std::string_view::npos);

    response.contentRange(5, 3, 10);
    response.addVaryToken("Accept-Encoding");
    RUVIA_CHECK_EQ(response.header("Content-Range").value_or(""), "bytes 5-7/10");
    RUVIA_CHECK_EQ(response.header("Vary").value_or(""), "Accept-Encoding");
}
