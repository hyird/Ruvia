#include "test_harness.h"

#include <array>
#include <cstdint>
#include <concepts>
#include <memory_resource>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/response/HttpResponseHeaderState.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/StaticFileMetadata.h"
#include "ruvia/web/detail/http/StaticRootIndex.h"
#include "ruvia/web/detail/server/file/HttpFileOpen.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::HttpResponse;
using ruvia::detail::setResponseAllowHeader;
using ruvia::detail::setResponseContentRange;
using ruvia::detail::setResponseContentRangeUnsatisfied;

constexpr std::uint32_t methodBit(HttpKnownMethod method) {
    return std::uint32_t{1} << static_cast<std::uint32_t>(method);
}

HttpResponse makeResponse() {
    return HttpResponse(std::pmr::new_delete_resource());
}

}  // namespace

// Response fields Ruvia formats itself: Content-Range and Allow.

RUVIA_TEST(content_range_formats_satisfied_range) {
    // RFC 7233: bytes <first>-<last>/<total>, where last = offset + length - 1.
    auto whole = makeResponse();
    setResponseContentRange(whole, 0, 100, 1000);
    RUVIA_CHECK_EQ(whole.header("Content-Range").value_or(""), std::string_view("bytes 0-99/1000"));

    auto mid = makeResponse();
    setResponseContentRange(mid, 500, 200, 1000);
    RUVIA_CHECK_EQ(mid.header("Content-Range").value_or(""), std::string_view("bytes 500-699/1000"));

    // A single-byte range.
    auto one = makeResponse();
    setResponseContentRange(one, 0, 1, 1);
    RUVIA_CHECK_EQ(one.header("Content-Range").value_or(""), std::string_view("bytes 0-0/1"));

    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    auto boundary = makeResponse();
    setResponseContentRange(boundary, maximum - 1, 1, maximum);
    RUVIA_CHECK_EQ(
        boundary.header("Content-Range").value_or(""),
        std::string_view(
            "bytes 18446744073709551614-18446744073709551614/"
            "18446744073709551615"));
}

RUVIA_TEST(content_range_rejects_out_of_bounds_and_overflow) {
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();

    for (const auto values : {
             std::array<std::uint64_t, 3>{maximum, 2, maximum},
             std::array<std::uint64_t, 3>{10, 1, 10},
             std::array<std::uint64_t, 3>{11, 1, 10}}) {
        auto response = makeResponse();
        bool rejected = false;
        try {
            setResponseContentRange(
                response, values[0], values[1], values[2]);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK(!response.header("Content-Range").has_value());
    }
}

RUVIA_TEST(content_range_formats_unsatisfied) {
    // 416 Range Not Satisfiable advertises the total with an unknown range.
    auto response = makeResponse();
    setResponseContentRangeUnsatisfied(response, 1000);
    RUVIA_CHECK_EQ(response.header("Content-Range").value_or(""), std::string_view("bytes */1000"));
}

RUVIA_TEST(allow_header_lists_methods_in_canonical_order) {
    // The Allow header (405/OPTIONS) lists the mask's methods in method-enum
    // order, comma-separated.
    auto many = makeResponse();
    setResponseAllowHeader(many, methodBit(HttpKnownMethod::kGet) | methodBit(HttpKnownMethod::kPost) |
                                     methodBit(HttpKnownMethod::kHead));
    RUVIA_CHECK_EQ(many.header("Allow").value_or(""), std::string_view("GET, POST, HEAD"));

    // A single method has no separator.
    auto one = makeResponse();
    setResponseAllowHeader(one, methodBit(HttpKnownMethod::kDelete));
    RUVIA_CHECK_EQ(one.header("Allow").value_or(""), std::string_view("DELETE"));
}
