#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "router/RouteResolution.h"
#include "ruvia/http/HttpCommon.h"

namespace {

using ruvia::kMaxRouteParams;
using ruvia::detail::RouteEntry;
using ruvia::detail::RouteMatch;
using ruvia::detail::RouteResolution;

// A non-null, never-dereferenced RouteEntry pointer: found() only compares it
// against nullptr, and RouteEntry is incomplete here.
const RouteEntry* fakeRoute() noexcept {
    alignas(std::max_align_t) static unsigned char storage[8];
    return reinterpret_cast<const RouteEntry*>(&storage);
}

}  // namespace

RUVIA_TEST(route_match_add_and_values) {
    RouteMatch match;
    RUVIA_CHECK_EQ(match.size(), std::size_t{0});
    RUVIA_CHECK(match.add("alpha"));
    RUVIA_CHECK(match.add("beta"));
    RUVIA_CHECK_EQ(match.size(), std::size_t{2});
    RUVIA_CHECK_EQ(match.values().size(), std::size_t{2});
    RUVIA_CHECK_EQ(match.values()[0], std::string_view("alpha"));
    RUVIA_CHECK_EQ(match.values()[1], std::string_view("beta"));
}

RUVIA_TEST(route_match_truncate_and_clear) {
    RouteMatch match;
    RUVIA_CHECK(match.add("a"));
    RUVIA_CHECK(match.add("b"));
    RUVIA_CHECK(match.add("c"));
    match.truncate(2);
    RUVIA_CHECK_EQ(match.size(), std::size_t{2});
    // A count larger than the current size is clamped (no growth).
    match.truncate(10);
    RUVIA_CHECK_EQ(match.size(), std::size_t{2});
    match.clear();
    RUVIA_CHECK_EQ(match.size(), std::size_t{0});
}

RUVIA_TEST(route_match_add_rejects_when_full) {
    RouteMatch match;
    for (std::size_t i = 0; i < kMaxRouteParams; ++i) {
        RUVIA_CHECK(match.add("x"));
    }
    RUVIA_CHECK_EQ(match.size(), kMaxRouteParams);
    RUVIA_CHECK(!match.add("overflow"));  // capacity reached
    RUVIA_CHECK_EQ(match.size(), kMaxRouteParams);
}

RUVIA_TEST(route_resolution_found_static) {
    const auto resolution = RouteResolution::foundStatic(fakeRoute());
    RUVIA_CHECK(resolution.found());
    RUVIA_CHECK(!resolution.methodNotAllowed());
    RUVIA_CHECK(resolution.match() == nullptr);  // a static match carries no params
}

RUVIA_TEST(route_resolution_found_dynamic) {
    RouteMatch match;
    RUVIA_CHECK(match.add("id"));
    const auto resolution = RouteResolution::foundDynamic(fakeRoute(), match);
    RUVIA_CHECK(resolution.found());
    RUVIA_CHECK(!resolution.methodNotAllowed());
    RUVIA_CHECK(resolution.match() != nullptr);
    RUVIA_CHECK(resolution.match() != &match);
    RUVIA_CHECK_EQ(resolution.match()->size(), std::size_t{1});
    RUVIA_CHECK_EQ(resolution.match()->values()[0], std::string_view("id"));
}

RUVIA_TEST(route_resolution_method_not_allowed_vs_not_found) {
    // 405: no route, but a non-zero allowed-methods mask drives the Allow header.
    const auto notAllowed = RouteResolution::methodNotAllowed(0x5);
    RUVIA_CHECK(!notAllowed.found());
    RUVIA_CHECK(notAllowed.methodNotAllowed());
    RUVIA_CHECK_EQ(notAllowed.allowedMethods(), std::uint32_t{0x5});

    // 404: a default resolution is neither found nor method-not-allowed.
    const RouteResolution notFound;
    RUVIA_CHECK(!notFound.found());
    RUVIA_CHECK(!notFound.methodNotAllowed());
    RUVIA_CHECK_EQ(notFound.allowedMethods(), std::uint32_t{0});
}
