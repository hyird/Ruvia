#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <vector>

#include "context_request_fixture.h"
#include "model_field_fixture.h"

RUVIA_MODEL(header_spelling_model,
    RUVIA_REQUIRED_FIELD_NAME("x-oTHER", trace, ruvia::string));

RUVIA_TEST(context_request_priority_tracks_live_update_and_response_keeps_partial_hint) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Priority", "u=2"}, http_header_view{"priority", "i"}});
    request_memory request_memory(worker);
    std::optional<ruvia::http_priority> update;
    auto context_value = context_access::make(request_memory, request,
        ruvia::test::test_context_services().with_request_priority_update(update));
    RUVIA_CHECK(context_value.req().priority().urgency_ == 2 && context_value.req().priority().incremental_);
    update = ruvia::http_priority{.urgency_ = 0};
    RUVIA_CHECK(context_value.req().priority().urgency_ == 0 && !context_value.req().priority().incremental_);
    update = ruvia::http_priority{.urgency_ = 7, .incremental_ = true};
    RUVIA_CHECK(context_value.req().priority().urgency_ == 7 && context_value.req().priority().incremental_);
    update.reset();
    RUVIA_CHECK(context_value.req().priority().urgency_ == 2);
    context_value.priority({.incremental_ = false});
    bool invalid_rejected = false;
    try {
        context_value.priority({.urgency_ = 8});
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }
    RUVIA_CHECK(invalid_rejected);
    const auto response = context_value.text(std::string_view("priority"));
    const auto value = response.header("Priority");
    RUVIA_CHECK(value && *value == "i=?0");
}

RUVIA_TEST(context_request_priority_combines_repeated_field_values_before_parsing) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/",
        {http_header_view{"Priority", "extra=\"alpha"}, http_header_view{"X-Other", "ignored"},
            http_header_view{"priority", "beta\", u=1, i"}});
    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());
    RUVIA_CHECK(context_value.req().priority().urgency_ == 1);
    RUVIA_CHECK(context_value.req().priority().incremental_);
}

// Reading a request through context: cookies, query, route params and headers, and the caches each
// lookup shares.

RUVIA_TEST(context_request_cookie_single_lookup_does_not_materialize_cookie_list) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Cookie", "a=1; b=2; a=3"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto cookie = context_value.req().cookie("a");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(*cookie, std::string_view("3"));
    RUVIA_CHECK(!context_access::request_cookies_materialized(context_value));
}

RUVIA_TEST(context_request_cookie_single_lookup_scans_repeated_cookie_fields) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto cookie = context_value.req().cookie("a");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK_EQ(*cookie, std::string_view("1"));
    RUVIA_CHECK(!context_access::request_cookies_materialized(context_value));
}

RUVIA_TEST(context_request_cookie_fields_include_repeated_cookie_headers) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2; a=3"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto& cookies = context_value.req().cookie_fields();
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
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/?a=first&b=2&a=one+two");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto query = context_value.req().query("a");
    RUVIA_CHECK(query.has_value());
    RUVIA_CHECK_EQ(*query, std::string_view("one two"));
    RUVIA_CHECK(context_access::request_query_materialized(context_value));

    // Scalar, multivalue and field-binding access all borrow the same cache.
    // In particular, repeated encoded lookup must not append another arena list
    // node or return a different backing allocation.
    const auto* const stable_data = query->data();
    const auto all = context_value.req().queries("a");
    RUVIA_CHECK_EQ(all.size(), std::size_t{2});
    RUVIA_CHECK_EQ(all.back(), std::string_view("one two"));
    const auto& fields_value = context_value.req().query_fields();
    RUVIA_CHECK_EQ(*fields_value.get("a"), std::string_view("one two"));
    const auto repeated = context_value.req().query("a");
    RUVIA_CHECK(repeated.has_value());
    RUVIA_CHECK(repeated->data() == stable_data);
}

RUVIA_TEST(context_request_query_unencoded_fields_borrow_the_query_string) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/?tag=x&page=2");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto tag = context_value.req().query("tag");
    RUVIA_CHECK(tag.has_value());
    RUVIA_CHECK_EQ(*tag, std::string_view("x"));
    RUVIA_CHECK(tag->data() == request.query_string().data() + 4);
    const auto page = context_value.req().query("page");
    RUVIA_CHECK(page.has_value());
    RUVIA_CHECK(page->data() == request.query_string().data() + 11);
}

RUVIA_TEST(context_request_query_preserves_short_decoded_names_and_values) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/?%61=%31&%62=%32&%61=%33");
    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto& fields_value = context_value.req().query_fields();
    RUVIA_CHECK_EQ(fields_value.size(), std::size_t{3});
    RUVIA_CHECK_EQ(fields_value[0].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(fields_value[0].value(), std::string_view("1"));
    RUVIA_CHECK_EQ(fields_value[1].name(), std::string_view("b"));
    RUVIA_CHECK_EQ(fields_value[1].value(), std::string_view("2"));
    RUVIA_CHECK_EQ(fields_value[2].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(fields_value[2].value(), std::string_view("3"));
    RUVIA_CHECK_EQ(context_value.req().query("a").value_or("missing"), std::string_view("3"));
    RUVIA_CHECK_EQ(context_value.req().query("b").value_or("missing"), std::string_view("2"));
    const auto values = context_value.req().queries("a");
    RUVIA_CHECK_EQ(values.size(), std::size_t{2});
    if (values.size() == 2) {
        RUVIA_CHECK_EQ(values[0], std::string_view("1"));
        RUVIA_CHECK_EQ(values[1], std::string_view("3"));
    }
}

RUVIA_TEST(context_request_query_list_uses_last_duplicate_like_single_lookup) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/?a=1&b=2&a=3");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    // Single-value lookup resolves a duplicate name to its LAST value.
    RUVIA_CHECK_EQ(*context_value.req().query("a"), std::string_view("3"));

    // The query field list (used by controller field binding) preserves every
    // decoded occurrence, while its scalar get() agrees with query("a") by
    // taking the last value.
    const auto& list = context_value.req().query_fields();
    RUVIA_CHECK_EQ(list.size(), std::size_t{3});
    RUVIA_CHECK_EQ(list[0].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(list[0].value(), std::string_view("1"));
    RUVIA_CHECK_EQ(list[1].name(), std::string_view("b"));
    RUVIA_CHECK_EQ(list[1].value(), std::string_view("2"));
    RUVIA_CHECK_EQ(list[2].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(list[2].value(), std::string_view("3"));
    const auto via_list = list.get("a");
    RUVIA_CHECK(via_list.has_value());
    RUVIA_CHECK_EQ(*via_list, std::string_view("3"));
    RUVIA_CHECK_EQ(*list.get("b"), std::string_view("2"));

    // queries() still exposes every value in order (getAll semantics).
    const auto all = context_value.req().queries("a");
    RUVIA_CHECK_EQ(all.size(), std::size_t{2});
    RUVIA_CHECK_EQ(all[0], std::string_view("1"));
    RUVIA_CHECK_EQ(all[1], std::string_view("3"));
}

RUVIA_TEST(context_request_query_fields_preserve_duplicates_for_model_binding) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/?message=first&other=x&message=second");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto& fields_value = context_value.req().query_fields();
    RUVIA_CHECK_EQ(fields_value.size(), std::size_t{3});
    RUVIA_CHECK_EQ(*fields_value.get("message"), std::string_view("second"));

    RUVIA_CHECK(!ruvia::detail::model_parse_access::parse_form_fields<accessor_surface_request>(
        fields_value, request_memory.resource())
            .has_value());
    const auto parsed_value =
        ruvia::detail::model_parse_access::parse_form_fields_partial<accessor_surface_request>(
            fields_value, request_memory.resource());
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(parsed_value->get<"message">().has_value());
    RUVIA_CHECK_EQ(parsed_value->get<"message">()->view(), std::string_view("first"));
    RUVIA_CHECK(ruvia::detail::model_validation_access::field_state<"message">(*parsed_value) ==
                ruvia::detail::model_field_state::duplicate);
}

RUVIA_TEST(context_request_queries_use_empty_span_only_for_missing_name) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/?empty=&flag&value=x");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto empty_value = context_value.req().queries("empty");
    RUVIA_CHECK_EQ(empty_value.size(), std::size_t{1});
    RUVIA_CHECK(empty_value.front().empty());

    const auto valueless = context_value.req().queries("flag");
    RUVIA_CHECK_EQ(valueless.size(), std::size_t{1});
    RUVIA_CHECK(valueless.front().empty());

    RUVIA_CHECK(context_value.req().queries("missing").empty());
}

RUVIA_TEST(context_request_param_single_lookup_materializes_one_shared_cache) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/");

    const std::string_view names[] = {"unused", "id"};
    const std::string_view values[] = {"skip", "one%20two"};
    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, "/items/:id", names, values,
        std::size(names), 0, ruvia::test::test_context_services());

    const auto param = context_value.req().param("id");
    RUVIA_CHECK(param.has_value());
    RUVIA_CHECK_EQ(*param, std::string_view("one two"));
    RUVIA_CHECK(context_access::route_params_materialized(context_value));

    const auto* const stable_data = param->data();
    const auto& fields_value = context_value.req().param_fields();
    RUVIA_CHECK_EQ(*fields_value.get("id"), std::string_view("one two"));
    const auto repeated = context_value.req().param("id");
    RUVIA_CHECK(repeated.has_value());
    RUVIA_CHECK(repeated->data() == stable_data);
}

RUVIA_TEST(context_request_param_rejects_and_remembers_malformed_percent_encoding) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/");
    const std::string_view names[] = {"id"};
    const std::string_view values[] = {"bad%zz"};

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, "/items/:id", names, values,
        std::size(names), 0, ruvia::test::test_context_services());

    for (int attempt_value = 0; attempt_value < 2; ++attempt_value) {
        bool threw = false;
        try {
            (void)context_value.req().param("id");
        } catch (const ruvia::http_error& error) {
            threw = error.info().status() == ruvia::http_status::bad_request;
        }
        RUVIA_CHECK(threw);
    }
    const auto* storage = context_access::request_storage(context_value);
    RUVIA_CHECK(storage != nullptr);
    RUVIA_CHECK(storage->route_params_invalid_);
    RUVIA_CHECK(!storage->route_params_.has_value());
}

RUVIA_TEST(context_request_accepts_merges_multiple_accept_field_lines) {
    const auto accepts = [](std::vector<std::string_view> accept_lines, std::string_view media_type) {
        worker_memory worker;
        request_memory memory(worker);
        std::vector<http_header_view> headers;
        headers.reserve(accept_lines.size());
        for (const auto line : accept_lines) {
            headers.emplace_back("Accept", line);
        }
        http_request request = make_request(std::pmr::get_default_resource(), "/", headers);
        auto context_value = context_access::make(memory, request, ruvia::test::test_context_services());
        return context_value.req().accepts(media_type);
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
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"X-Trace", "first"}, http_header_view{"x-trace", "second"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    asio::io_context& io = ruvia::test::new_test_io_context();
    std::string header;
    asio::co_spawn(io, read_header_value(context_value, header), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(header, std::string("second"));
}

RUVIA_TEST(context_request_header_lookup_is_case_insensitive_and_presence_aware) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"X-Empty", ""}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto present_empty = context_value.req().header("x-EMPTY");
    RUVIA_CHECK(present_empty.has_value());
    RUVIA_CHECK(present_empty.value_or("missing").empty());
    RUVIA_CHECK(!context_value.req().header("X-Missing").has_value());
}

RUVIA_TEST(context_request_preserves_exact_extension_method_token) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {}, "PROPFIND");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    asio::io_context& io = ruvia::test::new_test_io_context();
    method_observation observation;
    asio::co_spawn(io, read_method(context_value, observation), asio::detached);
    io.run();

    RUVIA_CHECK_EQ(observation.method_, std::string("PROPFIND"));
    RUVIA_CHECK(observation.known_method_ == http_known_method::unknown);
}

// The four bulk accessors are the enumeration path promoted out of detail:: onto
// context_request. Each must present every field in request order, keep
// duplicates, and stay callable on the by-value facade req() returns.

RUVIA_TEST(context_request_header_fields_enumerate_every_field_in_order) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"X-Trace", "a"}, http_header_view{"X-Trace", "b"}, http_header_view{"X-Other", "c"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    // Called on the prvalue req() returns: the borrowed list belongs to the
    // context, so this must not be an rvalue-deleted overload.
    const auto& headers = context_value.req().header_fields();
    // Enumeration preserves spelling; both lookup surfaces ignore ASCII case.
    RUVIA_CHECK_EQ(headers.count("x-trace"), std::size_t(2));
    RUVIA_CHECK_EQ(headers.count("X-Trace"), std::size_t(2));
    RUVIA_CHECK_EQ(headers[0].name(), std::string_view("X-Trace"));
    RUVIA_CHECK_EQ(headers[0].value(), std::string_view("a"));
    RUVIA_CHECK_EQ(headers[1].value(), std::string_view("b"));
    RUVIA_CHECK_EQ(headers[2].name(), std::string_view("X-Other"));
    // Scalar lookup keeps last-occurrence semantics across the same list.
    RUVIA_CHECK_EQ(*headers.get("x-trace"), std::string_view("b"));
    RUVIA_CHECK(headers.data() == request.headers().data());
    RUVIA_CHECK_EQ(headers.size(), request.headers().size());
    // ...while the named lookup still accepts the sent spelling, and both paths
    // agree on last-occurrence-wins for a repeated name.
    RUVIA_CHECK_EQ(*context_value.req().header("X-Trace"), std::string_view("b"));
    const auto model = ruvia::detail::model_parse_access::parse_form_fields<header_spelling_model>(headers, context_value.arena());
    RUVIA_CHECK(model.has_value());
    RUVIA_CHECK_EQ(model->get<"trace">().view(), std::string_view("c"));
}

RUVIA_TEST(context_request_query_fields_enumerate_repeated_names) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/?tag=x&tag=y&page=2");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto& queries = context_value.req().query_fields();
    RUVIA_CHECK_EQ(queries.size(), std::size_t(3));
    RUVIA_CHECK_EQ(queries.count("tag"), std::size_t(2));
    RUVIA_CHECK_EQ(queries[0].value(), std::string_view("x"));
    RUVIA_CHECK_EQ(queries[1].value(), std::string_view("y"));
    RUVIA_CHECK_EQ(*queries.get("page"), std::string_view("2"));
}

RUVIA_TEST(context_request_bulk_accessors_share_the_named_lookup_cache) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Cookie", "a=1; b=2"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    // A named lookup alone must not materialize the list...
    RUVIA_CHECK(!context_access::request_cookies_materialized(context_value));
    (void)context_value.req().cookie("a");
    RUVIA_CHECK(!context_access::request_cookies_materialized(context_value));

    // ...but the bulk accessor does, and a later named lookup reuses it rather
    // than building a second copy.
    const auto& cookies = context_value.req().cookie_fields();
    RUVIA_CHECK(context_access::request_cookies_materialized(context_value));
    RUVIA_CHECK_EQ(cookies.size(), std::size_t(2));
    RUVIA_CHECK_EQ(context_value.req().cookie_fields().data(), cookies.data());
}

// Content negotiation: accepts() answers "would this one do?", negotiate()
// answers "which of mine does the client want most?". Looping accepts() cannot
// substitute -- it yields the server's first acceptable option, not the
// client's preferred one.

namespace {

std::optional<std::string_view> negotiate_encoding(
    std::initializer_list<http_header_view> headers,
    std::initializer_list<std::string_view> supported) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", headers);
    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());
    return context_value.req().negotiate(ruvia::context_request::negotiable_type::encoding, supported);
}

}  // namespace

RUVIA_TEST(context_request_negotiate_encoding_preserves_identity_defaults) {
    RUVIA_CHECK_EQ(negotiate_encoding({}, {"gzip", "identity"}).value_or("missing"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", ""}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "gzip"}}, {"identity"}).value_or("missing"), std::string_view("identity"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5, *;q=0.2"}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5, identity;q=0.1"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5"}, {"accept-encoding", "identity;q=0"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "*;q=0"}, {"Accept-Encoding", "identity;q=0.7"}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    RUVIA_CHECK(!negotiate_encoding({{"Accept-Encoding", "*;q=0"}}, {"identity"}).has_value());
    RUVIA_CHECK(!negotiate_encoding({{"Accept-Encoding", ""}}, {"gzip"}).has_value());
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "extension;q=0.8, identity;q=0.2"}}, {"identity", "extension"}).value_or("missing"), std::string_view("extension"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "gzip, identity"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("identity"));
}

RUVIA_TEST(context_request_negotiate_encoding_preserves_coding_aliases) {
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "x-gzip"}}, {"gzip"}).value_or("missing"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "gzip"}}, {"x-gzip"}).value_or("missing"), std::string_view("x-gzip"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "x-gzip;q=0, *;q=1"}}, {"gzip", "br"}).value_or("missing"), std::string_view("br"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.3"}, {"Accept-Encoding", "X-GZIP;q=0.9, br;q=0.5"}}, {"br", "gzip"}).value_or("missing"), std::string_view("gzip"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "x-compress"}}, {"compress"}).value_or("missing"), std::string_view("compress"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "compress"}}, {"x-compress"}).value_or("missing"), std::string_view("x-compress"));
    RUVIA_CHECK_EQ(negotiate_encoding({{"Accept-Encoding", "x-compress;q=0, *;q=1"}}, {"compress", "gzip"}).value_or("missing"), std::string_view("gzip"));
}

RUVIA_TEST(context_request_negotiate_picks_the_client_preferred_media_type) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Accept", "text/html;q=0.3, application/json;q=0.9"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    // Server order lists html first, but the client prefers json.
    const std::string_view supported[] = {"text/html", "application/json"};
    const auto chosen =
        context_value.req().negotiate(ruvia::context_request::negotiable_type::media_type, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("application/json"));

    // accepts() would have said yes to the first one tried, which is the bug
    // this API exists to remove.
    RUVIA_CHECK(context_value.req().accepts("text/html"));
}

RUVIA_TEST(context_request_negotiate_reports_no_acceptable_representation) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Accept", "image/png"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const std::string_view supported[] = {"text/html", "application/json"};
    // nullopt is the 406 signal, which is why this cannot fall back to front().
    RUVIA_CHECK(!context_value.req()
            .negotiate(ruvia::context_request::negotiable_type::media_type, supported)
            .has_value());
}

RUVIA_TEST(context_request_negotiate_without_the_field_takes_server_preference) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const std::string_view supported[] = {"application/json", "text/html"};
    const auto chosen =
        context_value.req().negotiate(ruvia::context_request::negotiable_type::media_type, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("application/json"));
}

RUVIA_TEST(context_request_negotiate_language_uses_basic_prefix_filtering) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Accept-Language", "fr;q=0.4, en;q=0.8"}});

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    // RFC 4647 basic filtering: the range "en" matches the tag "en-US".
    const std::string_view supported[] = {"fr-CA", "en-US"};
    const auto chosen =
        context_value.req().negotiate(ruvia::context_request::negotiable_type::language, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("en-US"));
}

RUVIA_TEST(context_request_negotiate_honours_explicit_zero_quality_exclusion) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Accept-Encoding", "*, gzip;q=0"}});
    // "*" accepts everything except the explicitly excluded, more specific tag.

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const std::string_view supported[] = {"gzip", "br"};
    const auto chosen =
        context_value.req().negotiate(ruvia::context_request::negotiable_type::encoding, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("br"));
}

RUVIA_TEST(context_request_negotiate_folds_repeated_field_lines) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), "/", {http_header_view{"Accept-Language", "de;q=0.2"}, http_header_view{"Accept-Language", "ja;q=0.9"}});
    // RFC 9110 5.3: equivalent to one comma-joined value.

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const std::string_view supported[] = {"de", "ja"};
    const auto chosen =
        context_value.req().negotiate(ruvia::context_request::negotiable_type::language, supported);
    RUVIA_CHECK(chosen.has_value());
    RUVIA_CHECK_EQ(*chosen, std::string_view("ja"));
}

RUVIA_TEST(context_request_query_decodes_names_and_values_in_wire_order) {
    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(),
        "/?%61=one+two&%62=three%20four&%61=five+six&plain=borrowed");

    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto& fields_value = context_value.req().query_fields();
    RUVIA_CHECK_EQ(fields_value.size(), std::size_t{4});
    RUVIA_CHECK_EQ(fields_value[0].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(fields_value[0].value(), std::string_view("one two"));
    RUVIA_CHECK_EQ(fields_value[1].name(), std::string_view("b"));
    RUVIA_CHECK_EQ(fields_value[1].value(), std::string_view("three four"));
    RUVIA_CHECK_EQ(fields_value[2].name(), std::string_view("a"));
    RUVIA_CHECK_EQ(fields_value[2].value(), std::string_view("five six"));
    RUVIA_CHECK_EQ(fields_value[3].value(), std::string_view("borrowed"));

    const auto values = context_value.req().queries("a");
    RUVIA_CHECK_EQ(values.size(), std::size_t{2});
    if (values.size() == 2) {
        RUVIA_CHECK_EQ(values[0], std::string_view("one two"));
        RUVIA_CHECK_EQ(values[1], std::string_view("five six"));
    }
    const auto value = context_value.req().query("a");
    RUVIA_CHECK(value.has_value());
    if (value) {
        RUVIA_CHECK_EQ(*value, std::string_view("five six"));
        RUVIA_CHECK(value->data() == fields_value[2].value().data());
    }
}

RUVIA_TEST(context_request_query_preserves_many_decoded_values) {
    constexpr std::size_t count = 257;
    std::string target = "/?";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            target += '&';
        }
        target += "%74ag=v+" + std::to_string(i);
    }

    worker_memory worker;
    http_request request = make_request(std::pmr::get_default_resource(), target);
    request_memory request_memory(worker);
    auto context_value = context_access::make(request_memory, request, ruvia::test::test_context_services());

    const auto& fields_value = context_value.req().query_fields();
    const auto values = context_value.req().queries("tag");
    RUVIA_CHECK_EQ(fields_value.size(), count);
    RUVIA_CHECK_EQ(values.size(), count);
    if (fields_value.size() == count && values.size() == count) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto expected = "v " + std::to_string(i);
            RUVIA_CHECK_EQ(fields_value[i].name(), std::string_view("tag"));
            RUVIA_CHECK_EQ(fields_value[i].value(), std::string_view(expected));
            RUVIA_CHECK_EQ(values[i], std::string_view(expected));
        }
    }
}
