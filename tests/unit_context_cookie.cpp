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

RUVIA_TEST(context_request_query_single_lookup_decodes_without_materializing_query_tables) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "a=one+two&b=2&a=3");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto query = context.req().query("a");
    RUVIA_CHECK(query.has_value());
    RUVIA_CHECK_EQ(*query, std::string_view("one two"));
    RUVIA_CHECK(!ContextAccess::requestQueryMaterialized(context));
}

RUVIA_TEST(context_request_param_single_lookup_decodes_without_materializing_param_table) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);

    const std::string_view names[] = {"unused", "id"};
    const std::string_view values[] = {"skip", "one%20two"};
    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(
        requestMemory,
        request,
        "/items/:id",
        names,
        values,
        std::size(names),
        ruvia::HttpMethod::kGet,
        0,
        0);

    const auto param = context.req().param("id");
    RUVIA_CHECK(param.has_value());
    RUVIA_CHECK_EQ(*param, std::string_view("one two"));
    RUVIA_CHECK(!ContextAccess::routeParamsMaterialized(context));
}
