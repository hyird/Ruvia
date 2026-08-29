#include "context_request_fixture.h"
#include "model_field_fixture.h"

#include <vector>

// Reading a request through Context: cookies, query, route params and headers, and the caches each
// lookup shares.

RUVIA_TEST(context_request_cookie_single_lookup_does_not_materialize_cookie_list) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "a=1; b=2; a=3"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

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
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

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
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const auto& cookies = context.req().cookieFields();
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
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

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
    const auto& fields = context.req().queryFields();
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
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    // Single-value lookup resolves a duplicate name to its LAST value.
    RUVIA_CHECK_EQ(*context.req().query("a"), std::string_view("3"));

    // The query field list (used by controller field binding) preserves every
    // decoded occurrence, while its scalar get() agrees with query("a") by
    // taking the last value.
    const auto& list = context.req().queryFields();
    RUVIA_CHECK_EQ(list.size(), std::size_t{3});
    RUVIA_CHECK_EQ(list[0].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(list[0].value(), std::string_view("1"));
    RUVIA_CHECK_EQ(list[1].name(), std::string_view("b"));
    RUVIA_CHECK_EQ(list[1].value(), std::string_view("2"));
    RUVIA_CHECK_EQ(list[2].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(list[2].value(), std::string_view("3"));
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

RUVIA_TEST(context_request_query_fields_preserve_duplicates_for_model_binding) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "message=first&other=x&message=second");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const auto& fields = context.req().queryFields();
    RUVIA_CHECK_EQ(fields.size(), std::size_t{3});
    RUVIA_CHECK_EQ(*fields.get("message"), std::string_view("second"));

    RUVIA_CHECK(!ruvia::detail::ModelParseAccess::parseFormFields<AccessorSurfaceRequest>(
        fields, requestMemory.resource())
            .has_value());
    const auto parsed =
        ruvia::detail::ModelParseAccess::parseFormFieldsPartial<AccessorSurfaceRequest>(
            fields, requestMemory.resource());
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->get<"message">().has_value());
    RUVIA_CHECK_EQ(parsed->get<"message">()->view(), std::string_view("first"));
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"message">(*parsed) ==
                ruvia::detail::ModelFieldState::kDuplicate);
}

RUVIA_TEST(context_request_queries_use_empty_span_only_for_missing_name) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "empty=&flag&value=x");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

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
    auto context = ContextAccess::make(requestMemory, request, "/items/:id", names, values,
        std::size(names), 0, ruvia::test::testContextServices());

    const auto param = context.req().param("id");
    RUVIA_CHECK(param.has_value());
    RUVIA_CHECK_EQ(*param, std::string_view("one two"));
    RUVIA_CHECK(ContextAccess::routeParamsMaterialized(context));

    const auto* const stableData = param->data();
    const auto& fields = context.req().paramFields();
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
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            (void)context.req().query("safe");
        } catch (const ruvia::HttpError& error) {
            threw = error.info().status() == ruvia::http_status::kBadRequest;
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
    auto context = ContextAccess::make(requestMemory, request, "/items/:id", names, values,
        std::size(names), 0, ruvia::test::testContextServices());

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            (void)context.req().param("id");
        } catch (const ruvia::HttpError& error) {
            threw = error.info().status() == ruvia::http_status::kBadRequest;
        }
        RUVIA_CHECK(threw);
    }
    const auto* storage = ContextAccess::requestStorage(context);
    RUVIA_CHECK(storage != nullptr);
    RUVIA_CHECK(storage->routeParamsInvalid);
    RUVIA_CHECK(!storage->routeParams.has_value());
}

RUVIA_TEST(context_request_accepts_merges_multiple_accept_field_lines) {
    const auto accepts = [](std::vector<std::string_view> acceptLines, std::string_view mediaType) {
        WorkerMemory worker;
        RequestMemory memory(worker);
        HttpRequest request = HttpRequestAccess::make();
        HttpRequestAccess::reset(request);
        HttpRequestAccess::setMethod(request, "GET");
        HttpRequestAccess::setResource(request, memory.resource());
        const auto slot = HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kAccept);
        for (const auto line : acceptLines) {
            HttpRequestAccess::addHeader(request, HttpHeaderView{"Accept", line}, slot);
        }
        auto context = ContextAccess::make(memory, request, ruvia::test::testContextServices());
        return context.req().accepts(mediaType);
    };

    // No Accept header -> the client accepts anything.
    RUVIA_CHECK(accepts({}, "text/html"));
    // A present but empty Accept list is distinct from an absent field: it has
    // no matching media range. Empty members remain harmless when another field
    // line supplies an actual range.
    RUVIA_CHECK(!accepts({""}, "text/html"));
    RUVIA_CHECK(accepts({"", "text/html"}, "text/html"));
    // A single line behaves as before.
    RUVIA_CHECK(accepts({"text/html"}, "text/html"));
    RUVIA_CHECK(!accepts({"text/html"}, "application/json"));

    // RFC 9110 5.3: two Accept lines are equivalent to their comma-join. A type
    // offered only on the SECOND line must be accepted -- previously the stored
    // known-header slot held one line and the other was ignored.
    RUVIA_CHECK(accepts({"text/html", "application/json"}, "application/json"));
    RUVIA_CHECK(accepts({"text/html", "application/json"}, "text/html"));
    RUVIA_CHECK(!accepts({"text/html", "application/json"}, "image/png"));

    // A q=0 exclusion whose range is more specific than an accepting range on
    // another line must win, exactly as if joined "text/*, text/html;q=0" -- which
    // a naive per-line OR would get wrong.
    RUVIA_CHECK(!accepts({"text/*", "text/html;q=0"}, "text/html"));
    RUVIA_CHECK(accepts({"text/*", "text/html;q=0"}, "text/plain"));
}

RUVIA_TEST(context_request_header_lookup_uses_last_match) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Trace", "first"}));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"x-trace", "second"}));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

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
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

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
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    asio::io_context& io = ruvia::test::newTestIoContext();
    MethodObservation observation;
    asio::co_spawn(io, readMethod(context, observation), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(observation.method, std::string("PROPFIND"));
    RUVIA_CHECK(observation.knownMethod == HttpKnownMethod::kUnknown);
}

// The four bulk accessors are the enumeration path promoted out of detail:: onto
// ContextRequest. Each must present every field in request order, keep
// duplicates, and stay callable on the by-value facade req() returns.

RUVIA_TEST(context_request_header_fields_enumerate_every_field_in_order) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Trace", "a"}));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Trace", "b"}));
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Other", "c"}));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    // Called on the prvalue req() returns: the borrowed list belongs to the
    // Context, so this must not be an rvalue-deleted overload.
    const auto& headers = context.req().headerFields();
    // Names are normalized to lower case here, unlike the case-insensitive
    // header(name) lookup over the request as received.
    RUVIA_CHECK_EQ(headers.count("x-trace"), std::size_t(2));
    RUVIA_CHECK_EQ(headers.count("X-Trace"), std::size_t(0));
    RUVIA_CHECK_EQ(headers[0].name(), std::string_view("x-trace"));
    RUVIA_CHECK_EQ(headers[0].value(), std::string_view("a"));
    RUVIA_CHECK_EQ(headers[1].value(), std::string_view("b"));
    RUVIA_CHECK_EQ(headers[2].name(), std::string_view("x-other"));
    // Scalar lookup keeps last-occurrence semantics across the same list.
    RUVIA_CHECK_EQ(*headers.get("x-trace"), std::string_view("b"));
    // ...while the named lookup still accepts the sent spelling, and both paths
    // agree on last-occurrence-wins for a repeated name.
    RUVIA_CHECK_EQ(*context.req().header("X-Trace"), std::string_view("b"));
}

RUVIA_TEST(context_request_query_fields_enumerate_repeated_names) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setQueryString(request, "tag=x&tag=y&page=2");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const auto& queries = context.req().queryFields();
    RUVIA_CHECK_EQ(queries.size(), std::size_t(3));
    RUVIA_CHECK_EQ(queries.count("tag"), std::size_t(2));
    RUVIA_CHECK_EQ(queries[0].value(), std::string_view("x"));
    RUVIA_CHECK_EQ(queries[1].value(), std::string_view("y"));
    RUVIA_CHECK_EQ(*queries.get("page"), std::string_view("2"));
}

RUVIA_TEST(context_request_bulk_accessors_share_the_named_lookup_cache) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Cookie", "a=1; b=2"},
        HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie)));

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    // A named lookup alone must not materialize the list...
    RUVIA_CHECK(!ContextAccess::requestCookiesMaterialized(context));
    (void)context.req().cookie("a");
    RUVIA_CHECK(!ContextAccess::requestCookiesMaterialized(context));

    // ...but the bulk accessor does, and a later named lookup reuses it rather
    // than building a second copy.
    const auto& cookies = context.req().cookieFields();
    RUVIA_CHECK(ContextAccess::requestCookiesMaterialized(context));
    RUVIA_CHECK_EQ(cookies.size(), std::size_t(2));
    RUVIA_CHECK_EQ(context.req().cookieFields().data(), cookies.data());
}

// Content negotiation: accepts() answers "would this one do?", negotiate()
// answers "which of mine does the client want most?". Looping accepts() cannot
// substitute -- it yields the server's first acceptable option, not the
// client's preferred one.

namespace {

void setAcceptHeader(HttpRequest& request, std::string_view name, std::string_view value) {
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{name, value});
}

}  // namespace

RUVIA_TEST(context_request_negotiate_picks_the_client_preferred_media_type) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    setAcceptHeader(request, "Accept", "text/html;q=0.3, application/json;q=0.9");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    // Server order lists html first, but the client prefers json.
    const std::string_view supported[] = {"text/html", "application/json"};
    const auto chosen =
        context.req().negotiate(ruvia::ContextRequest::Negotiable::kMediaType, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("application/json"));

    // accepts() would have said yes to the first one tried, which is the bug
    // this API exists to remove.
    RUVIA_CHECK(context.req().accepts("text/html"));
}

RUVIA_TEST(context_request_negotiate_reports_no_acceptable_representation) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    setAcceptHeader(request, "Accept", "image/png");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const std::string_view supported[] = {"text/html", "application/json"};
    // nullopt is the 406 signal, which is why this cannot fall back to front().
    RUVIA_CHECK(!context.req()
            .negotiate(ruvia::ContextRequest::Negotiable::kMediaType, supported)
            .has_value());
}

RUVIA_TEST(context_request_negotiate_without_the_field_takes_server_preference) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const std::string_view supported[] = {"application/json", "text/html"};
    const auto chosen =
        context.req().negotiate(ruvia::ContextRequest::Negotiable::kMediaType, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("application/json"));
}

RUVIA_TEST(context_request_negotiate_language_uses_basic_prefix_filtering) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    setAcceptHeader(request, "Accept-Language", "fr;q=0.4, en;q=0.8");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    // RFC 4647 basic filtering: the range "en" matches the tag "en-US".
    const std::string_view supported[] = {"fr-CA", "en-US"};
    const auto chosen =
        context.req().negotiate(ruvia::ContextRequest::Negotiable::kLanguage, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("en-US"));
}

RUVIA_TEST(context_request_negotiate_honours_explicit_zero_quality_exclusion) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // "*" accepts everything except the explicitly excluded, more specific tag.
    setAcceptHeader(request, "Accept-Encoding", "*, gzip;q=0");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const std::string_view supported[] = {"gzip", "br"};
    const auto chosen =
        context.req().negotiate(ruvia::ContextRequest::Negotiable::kEncoding, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("br"));
}

RUVIA_TEST(context_request_negotiate_folds_repeated_field_lines) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // RFC 9110 5.3: equivalent to one comma-joined value.
    setAcceptHeader(request, "Accept-Language", "de;q=0.2");
    setAcceptHeader(request, "Accept-Language", "ja;q=0.9");

    RequestMemory requestMemory(worker);
    HttpRequestAccess::setResource(request, requestMemory.resource());
    auto context = ContextAccess::make(requestMemory, request, ruvia::test::testContextServices());

    const std::string_view supported[] = {"de", "ja"};
    const auto chosen =
        context.req().negotiate(ruvia::ContextRequest::Negotiable::kLanguage, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("ja"));
}
