#include "test_harness.h"

#include <cstddef>

#include "net/http2/Http2StreamBodyPolicy.h"

namespace {

using ruvia::detail::HttpRequestBodyMode;
using ruvia::detail::Http2StreamBodyPolicy;

}  // namespace

RUVIA_TEST(stream_body_policy_defaults_to_buffered) {
    Http2StreamBodyPolicy policy;
    RUVIA_CHECK(policy.bodyMode() == HttpRequestBodyMode::kBuffered);
    RUVIA_CHECK(!policy.usesStreamRequestBody());
}

RUVIA_TEST(stream_body_policy_body_mode_predicate) {
    Http2StreamBodyPolicy policy;
    policy.setBodyMode(HttpRequestBodyMode::kStream);
    RUVIA_CHECK(policy.bodyMode() == HttpRequestBodyMode::kStream);
    RUVIA_CHECK(policy.usesStreamRequestBody());
    policy.setBodyMode(HttpRequestBodyMode::kBuffered);
    RUVIA_CHECK(!policy.usesStreamRequestBody());
}

RUVIA_TEST(stream_body_policy_reset_to_buffered_clears_mode) {
    Http2StreamBodyPolicy policy;
    policy.setBodyMode(HttpRequestBodyMode::kStream);
    policy.resetToBuffered();
    RUVIA_CHECK(policy.bodyMode() == HttpRequestBodyMode::kBuffered);
    RUVIA_CHECK(!policy.usesStreamRequestBody());
}
