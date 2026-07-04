#include "test_harness.h"

#include <cstddef>
#include <cstdint>

#include "net/http2/Http2StreamRouting.h"
#include "ruvia/http/HttpCommon.h"

namespace {

using ruvia::RequestBodyMode;
using ruvia::detail::Http2StreamRouting;
using ruvia::detail::RouteResolution;

}  // namespace

RUVIA_TEST(stream_routing_defaults_are_buffered_and_unresolved) {
    Http2StreamRouting routing;
    RUVIA_CHECK(routing.bodyMode() == RequestBodyMode::kBuffered);
    RUVIA_CHECK(!routing.usesStreamRequestBody());
    RUVIA_CHECK(!routing.resolution().found());
    RUVIA_CHECK_EQ(routing.match().size(), std::size_t{0});
}

RUVIA_TEST(stream_routing_body_mode_predicate) {
    Http2StreamRouting routing;
    routing.setBodyMode(RequestBodyMode::kStream);
    RUVIA_CHECK(routing.bodyMode() == RequestBodyMode::kStream);
    RUVIA_CHECK(routing.usesStreamRequestBody());
    routing.setBodyMode(RequestBodyMode::kBuffered);
    RUVIA_CHECK(!routing.usesStreamRequestBody());
}

RUVIA_TEST(stream_routing_set_resolution_is_reflected) {
    Http2StreamRouting routing;
    routing.setResolution(RouteResolution::methodNotAllowed(0x5));
    RUVIA_CHECK(routing.resolution().methodNotAllowed());
    RUVIA_CHECK_EQ(routing.resolution().allowedMethods(), std::uint32_t{0x5});
}

RUVIA_TEST(stream_routing_reset_to_buffered_clears_all_state) {
    Http2StreamRouting routing;
    RUVIA_CHECK(routing.match().add("id"));
    routing.setBodyMode(RequestBodyMode::kStream);
    routing.setResolution(RouteResolution::methodNotAllowed(0x5));

    // Reusing a pooled stream slot must not leak the previous request's routing.
    routing.resetToBuffered();
    RUVIA_CHECK_EQ(routing.match().size(), std::size_t{0});
    RUVIA_CHECK(routing.bodyMode() == RequestBodyMode::kBuffered);
    RUVIA_CHECK(!routing.usesStreamRequestBody());
    RUVIA_CHECK(!routing.resolution().found());
    RUVIA_CHECK(!routing.resolution().methodNotAllowed());
}
