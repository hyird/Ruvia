#include "test_harness.h"

#include <memory_resource>
#include <string_view>

#include "net/http2/Http2RequestBuilder.h"

namespace {

using ruvia::HttpMethod;
using ruvia::detail::Http2RequestBuilder;
using ruvia::detail::Http2StreamState;

Http2StreamState makeStream() {
    return Http2StreamState(1, std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(h2_request_builder_method_is_stream_method_when_not_ws_connect) {
    auto stream = makeStream();
    stream.setRequestMethod(HttpMethod::kPost);
    RUVIA_CHECK(Http2RequestBuilder::requestMethod(stream) == HttpMethod::kPost);
    stream.setRequestMethod(HttpMethod::kDelete);
    RUVIA_CHECK(Http2RequestBuilder::requestMethod(stream) == HttpMethod::kDelete);
}

RUVIA_TEST(h2_request_builder_target_is_path_and_splits_query) {
    auto stream = makeStream();
    stream.assignRequestPath("/search?q=hello&x=1");
    // For a non-CONNECT request the target is the :path pseudo-header verbatim.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestTarget(stream),
                   std::string_view("/search?q=hello&x=1"));
    // The path is everything before the first '?'.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("/search"));
}

RUVIA_TEST(h2_request_builder_path_without_query) {
    auto stream = makeStream();
    stream.assignRequestPath("/index.html");
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("/index.html"));
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestTarget(stream), std::string_view("/index.html"));
}

RUVIA_TEST(h2_request_builder_asterisk_form_target) {
    auto stream = makeStream();
    stream.assignRequestPath("*");
    // The asterisk-form target (OPTIONS *) keeps "*" as the path.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("*"));
}

RUVIA_TEST(h2_request_builder_empty_query_after_question_mark) {
    auto stream = makeStream();
    stream.assignRequestPath("/a?");
    // A trailing '?' with no query still yields the path up to it.
    RUVIA_CHECK_EQ(Http2RequestBuilder::requestPath(stream), std::string_view("/a"));
}
