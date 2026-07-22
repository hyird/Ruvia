#include "context_request_fixture.h"

// Reading a request through Context: cookies, query, route params and headers, and the caches each lookup shares.

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

RUVIA_TEST(context_request_cookie_single_lookup_scans_repeated_cookie_fields) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "a=1"}, slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "b=2"}, slot));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto cookie = context.req().cookie("a");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(*cookie, std::string_view("1"));
    RUVIA_CHECK(!ContextAccess::requestCookiesMaterialized(context));
}

RUVIA_TEST(context_request_cookie_fields_include_repeated_cookie_headers) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "a=1"}, slot));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "b=2; a=3"}, slot));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto& cookies = ruvia::detail::requestCookieFields(context.req());
    RUVIA_CHECK_EQ(cookies.size(), std::size_t{3});
    RUVIA_CHECK_EQ(cookies[0].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(cookies[0].value(), std::string_view("1"));
    RUVIA_CHECK_EQ(cookies[1].name(), std::string_view("b"));
    RUVIA_CHECK_EQ(cookies[1].value(), std::string_view("2"));
    RUVIA_CHECK_EQ(cookies[2].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(cookies[2].value(), std::string_view("3"));
    const auto latest = cookies.get("a");
    RUVIA_CHECK(latest.has_value());
    RUVIA_CHECK_EQ(*latest, std::string_view("3"));
}

RUVIA_TEST(context_request_query_single_lookup_materializes_one_shared_cache) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "a=first&b=2&a=one+two");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto query = context.req().query("a");
    RUVIA_CHECK(query.has_value());
    RUVIA_CHECK_EQ(*query, std::string_view("one two"));
    RUVIA_CHECK(ContextAccess::requestQueryMaterialized(context));

    // Scalar, multivalue and field-binding access all borrow the same cache.
    // In particular, repeated encoded lookup must not append another arena list
    // node or return a different backing allocation.
    const auto* const stableData = query->data();
    const auto all = context.req().queries("a");
    RUVIA_CHECK_EQ(all.size(), std::size_t{2});
    RUVIA_CHECK_EQ(all.back(), std::string_view("one two"));
    const auto& fields = ruvia::detail::requestQueryFields(context.req());
    RUVIA_CHECK_EQ(*fields.get("a"), std::string_view("one two"));
    const auto repeated = context.req().query("a");
    RUVIA_CHECK(repeated.has_value());
    RUVIA_CHECK(repeated->data() == stableData);
}

RUVIA_TEST(context_request_query_list_uses_last_duplicate_like_single_lookup) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "a=1&b=2&a=3");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    // Single-value lookup resolves a duplicate name to its LAST value.
    RUVIA_CHECK_EQ(*context.req().query("a"), std::string_view("3"));

    // The flattened query field list (used by controller field binding) must
    // agree. It previously kept the first occurrence ("1"), so a caller binding
    // fields from the list and one calling query("a") saw different values for
    // ?a=1&a=3 -- the inconsistency this pins.
    const auto& list = ruvia::detail::requestQueryFields(context.req());
    RUVIA_CHECK_EQ(list.size(), std::size_t{2});  // deduped to unique names a, b
    const auto viaList = list.get("a");
    RUVIA_CHECK(viaList.has_value());
    RUVIA_CHECK_EQ(*viaList, std::string_view("3"));
    RUVIA_CHECK_EQ(*list.get("b"), std::string_view("2"));

    // queries() still exposes every value in order (getAll semantics).
    const auto all = context.req().queries("a");
    RUVIA_CHECK_EQ(all.size(), std::size_t{2});
    RUVIA_CHECK_EQ(all[0], std::string_view("1"));
    RUVIA_CHECK_EQ(all[1], std::string_view("3"));
}

RUVIA_TEST(context_request_queries_use_empty_span_only_for_missing_name) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "empty=&flag&value=x");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto emptyValue = context.req().queries("empty");
    RUVIA_CHECK_EQ(emptyValue.size(), std::size_t{1});
    RUVIA_CHECK(emptyValue.front().empty());

    const auto valueless = context.req().queries("flag");
    RUVIA_CHECK_EQ(valueless.size(), std::size_t{1});
    RUVIA_CHECK(valueless.front().empty());

    RUVIA_CHECK(context.req().queries("missing").empty());
}

RUVIA_TEST(context_request_param_single_lookup_materializes_one_shared_cache) {
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
        0);

    const auto param = context.req().param("id");
    RUVIA_CHECK(param.has_value());
    RUVIA_CHECK_EQ(*param, std::string_view("one two"));
    RUVIA_CHECK(ContextAccess::routeParamsMaterialized(context));

    const auto* const stableData = param->data();
    const auto& fields = ruvia::detail::requestParamFields(context.req());
    RUVIA_CHECK_EQ(*fields.get("id"), std::string_view("one two"));
    const auto repeated = context.req().param("id");
    RUVIA_CHECK(repeated.has_value());
    RUVIA_CHECK(repeated->data() == stableData);
}

RUVIA_TEST(context_request_query_rejects_and_remembers_malformed_percent_encoding) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "safe=1&bad=%zz");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            (void)context.req().query("safe");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    const auto* storage = ContextAccess::requestStorage(context);
    RUVIA_CHECK(storage != nullptr);
    RUVIA_CHECK(storage->queryInvalid);
    RUVIA_CHECK(!storage->query.has_value());
}

RUVIA_TEST(context_request_param_rejects_and_remembers_malformed_percent_encoding) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const std::string_view names[] = {"id"};
    const std::string_view values[] = {"bad%zz"};

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(
        requestMemory,
        request,
        "/items/:id",
        names,
        values,
        std::size(names),
        0);

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            (void)context.req().param("id");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    const auto* storage = ContextAccess::requestStorage(context);
    RUVIA_CHECK(storage != nullptr);
    RUVIA_CHECK(storage->routeParamsInvalid);
    RUVIA_CHECK(!storage->routeParams.has_value());
}

RUVIA_TEST(context_request_header_lookup_uses_last_match) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Trace", "first"}));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"x-trace", "second"}));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    asio::io_context& io = ruvia::test::newTestIoContext();
    std::string header;
    asio::co_spawn(io, readHeaderValue(context, header), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(header, std::string("second"));
}

RUVIA_TEST(context_request_header_lookup_is_case_insensitive_and_presence_aware) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Empty", ""}));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    const auto presentEmpty = context.req().header("x-EMPTY");
    RUVIA_CHECK(presentEmpty.has_value());
    RUVIA_CHECK(presentEmpty.value_or("missing").empty());
    RUVIA_CHECK(!context.req().header("X-Missing").has_value());
}

RUVIA_TEST(context_request_preserves_exact_extension_method_token) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, "PROPFIND");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request);

    asio::io_context& io = ruvia::test::newTestIoContext();
    MethodObservation observation;
    asio::co_spawn(io, readMethod(context, observation), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(observation.method, std::string("PROPFIND"));
    RUVIA_CHECK(observation.knownMethod == HttpKnownMethod::kUnknown);
}
