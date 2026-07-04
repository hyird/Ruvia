#include "test_harness.h"

#include <string_view>

#include "http/ContextInternal.h"
#include "http/HttpRequestInternal.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RequestKnownHeader;

}  // namespace

RUVIA_TEST(context_request_cookie_single_lookup_does_not_materialize_cookie_list) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Cookie", "a=1; b=2; a=3"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto cookie = context.req().cookie("a");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(*cookie, std::string_view("3"));
    RUVIA_CHECK(!ContextAccess::requestCookiesMaterialized(context));
}
