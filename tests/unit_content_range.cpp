#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "http/HttpResponseHeaderState.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpResponse.h"

namespace {

using ruvia::HttpMethod;
using ruvia::HttpResponse;
using ruvia::detail::setResponseAllowHeader;
using ruvia::detail::setResponseContentRange;
using ruvia::detail::setResponseContentRangeUnsatisfied;

constexpr std::uint32_t methodBit(HttpMethod method) {
    return std::uint32_t{1} << static_cast<std::uint32_t>(method);
}

HttpResponse makeResponse() {
    return HttpResponse(std::pmr::new_delete_resource());
}

}  // namespace

RUVIA_TEST(content_range_formats_satisfied_range) {
    // RFC 7233: bytes <first>-<last>/<total>, where last = offset + length - 1.
    auto whole = makeResponse();
    setResponseContentRange(whole, 0, 100, 1000);
    RUVIA_CHECK_EQ(whole.header("Content-Range"), std::string_view("bytes 0-99/1000"));

    auto mid = makeResponse();
    setResponseContentRange(mid, 500, 200, 1000);
    RUVIA_CHECK_EQ(mid.header("Content-Range"), std::string_view("bytes 500-699/1000"));

    // A single-byte range.
    auto one = makeResponse();
    setResponseContentRange(one, 0, 1, 1);
    RUVIA_CHECK_EQ(one.header("Content-Range"), std::string_view("bytes 0-0/1"));
}

RUVIA_TEST(content_range_formats_unsatisfied) {
    // 416 Range Not Satisfiable advertises the total with an unknown range.
    auto response = makeResponse();
    setResponseContentRangeUnsatisfied(response, 1000);
    RUVIA_CHECK_EQ(response.header("Content-Range"), std::string_view("bytes */1000"));
}

RUVIA_TEST(allow_header_lists_methods_in_canonical_order) {
    // The Allow header (405/OPTIONS) lists the mask's methods in method-enum
    // order, comma-separated.
    auto many = makeResponse();
    setResponseAllowHeader(many, methodBit(HttpMethod::kGet) | methodBit(HttpMethod::kPost) |
                                     methodBit(HttpMethod::kHead));
    RUVIA_CHECK_EQ(many.header("Allow"), std::string_view("GET, POST, HEAD"));

    // A single method has no separator.
    auto one = makeResponse();
    setResponseAllowHeader(one, methodBit(HttpMethod::kDelete));
    RUVIA_CHECK_EQ(one.header("Allow"), std::string_view("DELETE"));
}
