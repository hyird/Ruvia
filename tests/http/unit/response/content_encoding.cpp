#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"

#include "test_harness.h"

namespace {

using ruvia::HttpResponse;
using ruvia::detail::applyResponseContentEncoding;
using ruvia::detail::replaceResponseBodyWithContentEncoding;
using ruvia::detail::responseBody;

HttpResponse makeResponse() {
    return HttpResponse({.resource = std::pmr::new_delete_resource()});
}

}  // namespace

RUVIA_TEST(apply_content_encoding_sets_coding_drops_identity_length_and_weakens_etag) {
    auto response = makeResponse();
    response.body("identity");
    response.header("Content-Length", "8");
    response.header("ETag", "\"v1\"");

    applyResponseContentEncoding(response, "gzip");

    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK(!response.header("Content-Length").has_value());
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("identity"));
}

RUVIA_TEST(apply_content_encoding_leaves_weak_malformed_and_absent_etags) {
    auto weak = makeResponse();
    weak.header("ETag", "W/\"v1\"");
    applyResponseContentEncoding(weak, "br");
    RUVIA_CHECK_EQ(weak.header("ETag"), std::string_view("W/\"v1\""));

    auto malformed = makeResponse();
    malformed.header("ETag", "v1");
    applyResponseContentEncoding(malformed, "zstd");
    RUVIA_CHECK_EQ(malformed.header("ETag"), std::string_view("v1"));

    auto absent = makeResponse();
    applyResponseContentEncoding(absent, "gzip");
    RUVIA_CHECK(!absent.header("ETag").has_value());
    RUVIA_CHECK_EQ(absent.header("Content-Encoding"), std::string_view("gzip"));
}

RUVIA_TEST(apply_content_encoding_rejects_empty_coding) {
    auto response = makeResponse();
    response.header("ETag", "\"v1\"");
    bool rejected = false;
    try {
        applyResponseContentEncoding(response, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("\"v1\""));
    RUVIA_CHECK(!response.header("Content-Encoding").has_value());
}

RUVIA_TEST(replace_body_with_content_encoding_commits_representation_and_weakens_etag) {
    auto response = makeResponse();
    response.body("identity");
    response.header("Content-Length", "8");
    response.header("ETag", "\"v1\"");

    std::pmr::string encoded("compressed", std::pmr::new_delete_resource());
    replaceResponseBodyWithContentEncoding(response, std::move(encoded), "gzip");

    RUVIA_CHECK_EQ(response.header("Content-Encoding"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(response.header("Content-Length"), std::string_view("10"));
    RUVIA_CHECK_EQ(response.header("ETag"), std::string_view("W/\"v1\""));
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("compressed"));
}
