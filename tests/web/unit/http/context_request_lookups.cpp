#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_header.h"

#include "context_request_fixture.h"

using ruvia::http_header_view;

namespace {

template <typename headers_type = std::initializer_list<http_header_view>>
ruvia::test_request make_request(std::string_view target, const headers_type& headers = {}, std::string_view method = "GET") {
    auto request = ruvia::test_request::method(method, target);
    for (const auto& header : headers) {
        request.header(header.name(), header.value());
    }
    return request;
}

}  // namespace

namespace {

std::optional<std::string_view> negotiate_encoding(
    std::initializer_list<http_header_view> headers,
    std::initializer_list<std::string_view> supported) {
    auto request = make_request("/", headers);
    std::optional<std::string_view> chosen;
    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        chosen = context_value.req().negotiate(ruvia::context_request::negotiable_type::encoding, supported);
        co_return;
    });
    return chosen;
}

}  // namespace

RUVIA_TEST(context_request_priority_combines_repeated_field_values_before_parsing) {
    auto request = make_request("/",
        {http_header_view{"Priority", "extra=\"alpha"}, http_header_view{"X-Other", "ignored"},
            http_header_view{"priority", "beta\", u=1, i"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        RUVIA_CHECK(context_value.req().priority().urgency_ == 1);
        RUVIA_CHECK(context_value.req().priority().incremental_);
        co_return;
    });
}

RUVIA_TEST(context_request_cookie_single_lookup_uses_last_duplicate) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1; b=2; a=3"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto cookie = context_value.req().cookie("a");
        RUVIA_CHECK(cookie.has_value());
        RUVIA_CHECK_EQ(*cookie, std::string_view("3"));
        co_return;
    });
}

RUVIA_TEST(context_request_cookie_single_lookup_scans_repeated_cookie_fields) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto cookie = context_value.req().cookie("a");
        RUVIA_CHECK(cookie.has_value());
        RUVIA_CHECK_EQ(*cookie, std::string_view("1"));
        co_return;
    });
}

RUVIA_TEST(context_request_cookie_fields_include_repeated_cookie_headers) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2; a=3"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
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
        co_return;
    });
}

RUVIA_TEST(context_request_query_accessors_agree_on_decoded_values) {
    auto request = make_request("/?a=first&b=2&a=one+two");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto query = context_value.req().query("a");
        RUVIA_CHECK(query.has_value());
        RUVIA_CHECK_EQ(*query, std::string_view("one two"));

        const auto all = context_value.req().queries("a");
        RUVIA_CHECK_EQ(all.size(), std::size_t{2});
        RUVIA_CHECK_EQ(all.back(), std::string_view("one two"));
        const auto& fields_value = context_value.req().query_fields();
        RUVIA_CHECK_EQ(*fields_value.get("a"), std::string_view("one two"));
        const auto repeated = context_value.req().query("a");
        RUVIA_CHECK(repeated.has_value());
        co_return;
    });
}

RUVIA_TEST(context_request_query_reads_unencoded_fields) {
    auto request = make_request("/?tag=x&page=2");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto tag = context_value.req().query("tag");
        RUVIA_CHECK(tag.has_value());
        RUVIA_CHECK_EQ(*tag, std::string_view("x"));
        const auto page = context_value.req().query("page");
        RUVIA_CHECK(page.has_value());
        co_return;
    });
}

RUVIA_TEST(context_request_query_preserves_short_decoded_names_and_values) {
    auto request = make_request("/?%61=%31&%62=%32&%61=%33");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
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
        co_return;
    });
}

RUVIA_TEST(context_request_query_list_uses_last_duplicate_like_single_lookup) {
    auto request = make_request("/?a=1&b=2&a=3");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
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
        co_return;
    });
}

RUVIA_TEST(context_request_queries_use_empty_span_only_for_missing_name) {
    auto request = make_request("/?empty=&flag&value=x");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto empty_value = context_value.req().queries("empty");
        RUVIA_CHECK_EQ(empty_value.size(), std::size_t{1});
        RUVIA_CHECK(empty_value.front().empty());

        const auto valueless = context_value.req().queries("flag");
        RUVIA_CHECK_EQ(valueless.size(), std::size_t{1});
        RUVIA_CHECK(valueless.front().empty());

        RUVIA_CHECK(context_value.req().queries("missing").empty());
        co_return;
    });
}

RUVIA_TEST(context_request_accepts_merges_multiple_accept_field_lines) {
    const auto accepts = [](std::vector<std::string_view> accept_lines, std::string_view media_type) {
        std::vector<http_header_view> headers;
        headers.reserve(accept_lines.size());
        for (const auto line : accept_lines) {
            headers.emplace_back("Accept", line);
        }
        auto request = make_request("/", headers);
        bool accepted = false;
        (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
            accepted = context_value.req().accepts(media_type);
            co_return;
        });
        return accepted;
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
    auto request = make_request("/", {http_header_view{"X-Trace", "first"}, http_header_view{"x-trace", "second"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        RUVIA_CHECK_EQ(context_value.req().header("X-Trace"), std::string_view("second"));
        co_return;
    });
}

RUVIA_TEST(context_request_header_lookup_is_case_insensitive_and_presence_aware) {
    auto request = make_request("/", {http_header_view{"X-Empty", ""}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto present_empty = context_value.req().header("x-EMPTY");
        RUVIA_CHECK(present_empty.has_value());
        RUVIA_CHECK(present_empty.value_or("missing").empty());
        RUVIA_CHECK(!context_value.req().header("X-Missing").has_value());
        co_return;
    });
}

RUVIA_TEST(context_request_preserves_exact_extension_method_token) {
    auto request = make_request("/", {}, "PROPFIND");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        RUVIA_CHECK_EQ(context_value.req().method(), std::string_view("PROPFIND"));
        RUVIA_CHECK(context_value.req().known_method() == ruvia::http_known_method::unknown);
        co_return;
    });
}

RUVIA_TEST(context_request_header_fields_enumerate_every_field_in_order) {
    auto request = make_request("/", {http_header_view{"X-Trace", "a"}, http_header_view{"X-Trace", "b"}, http_header_view{"X-Other", "c"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
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
        // ...while the named lookup still accepts the sent spelling, and both paths
        // agree on last-occurrence-wins for a repeated name.
        RUVIA_CHECK_EQ(*context_value.req().header("X-Trace"), std::string_view("b"));
        co_return;
    });
}

RUVIA_TEST(context_request_query_fields_enumerate_repeated_names) {
    auto request = make_request("/?tag=x&tag=y&page=2");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& queries = context_value.req().query_fields();
        RUVIA_CHECK_EQ(queries.size(), std::size_t(3));
        RUVIA_CHECK_EQ(queries.count("tag"), std::size_t(2));
        RUVIA_CHECK_EQ(queries[0].value(), std::string_view("x"));
        RUVIA_CHECK_EQ(queries[1].value(), std::string_view("y"));
        RUVIA_CHECK_EQ(*queries.get("page"), std::string_view("2"));
        co_return;
    });
}

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
    auto request = make_request("/", {http_header_view{"Accept", "text/html;q=0.3, application/json;q=0.9"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // Server order lists html first, but the client prefers json.
        const std::string_view supported[] = {"text/html", "application/json"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_type::media_type, supported);
        RUVIA_CHECK(chosen.has_value());
        RUVIA_CHECK_EQ(*chosen, std::string_view("application/json"));

        // accepts() would have said yes to the first one tried, which is the bug
        // this API exists to remove.
        RUVIA_CHECK(context_value.req().accepts("text/html"));
        co_return;
    });
}

RUVIA_TEST(context_request_negotiate_reports_no_acceptable_representation) {
    auto request = make_request("/", {http_header_view{"Accept", "image/png"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"text/html", "application/json"};
        // nullopt is the 406 signal, which is why this cannot fall back to front().
        RUVIA_CHECK(!context_value.req()
                .negotiate(ruvia::context_request::negotiable_type::media_type, supported)
                .has_value());
        co_return;
    });
}

RUVIA_TEST(context_request_negotiate_without_the_field_takes_server_preference) {
    auto request = make_request("/");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"application/json", "text/html"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_type::media_type, supported);
        RUVIA_CHECK(chosen.has_value());
        RUVIA_CHECK_EQ(*chosen, std::string_view("application/json"));
        co_return;
    });
}

RUVIA_TEST(context_request_negotiate_language_uses_basic_prefix_filtering) {
    auto request = make_request("/", {http_header_view{"Accept-Language", "fr;q=0.4, en;q=0.8"}});

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // RFC 4647 basic filtering: the range "en" matches the tag "en-US".
        const std::string_view supported[] = {"fr-CA", "en-US"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_type::language, supported);
        RUVIA_CHECK(chosen.has_value());
        RUVIA_CHECK_EQ(*chosen, std::string_view("en-US"));
        co_return;
    });
}

RUVIA_TEST(context_request_negotiate_honours_explicit_zero_quality_exclusion) {
    auto request = make_request("/", {http_header_view{"Accept-Encoding", "*, gzip;q=0"}});
    // "*" accepts everything except the explicitly excluded, more specific tag.

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"gzip", "br"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_type::encoding, supported);
        RUVIA_CHECK(chosen.has_value());
        RUVIA_CHECK_EQ(*chosen, std::string_view("br"));
        co_return;
    });
}

RUVIA_TEST(context_request_negotiate_folds_repeated_field_lines) {
    auto request = make_request("/", {http_header_view{"Accept-Language", "de;q=0.2"}, http_header_view{"Accept-Language", "ja;q=0.9"}});
    // RFC 9110 5.3: equivalent to one comma-joined value.

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"de", "ja"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_type::language, supported);
        RUVIA_CHECK(chosen.has_value());
        RUVIA_CHECK_EQ(*chosen, std::string_view("ja"));
        co_return;
    });
}

RUVIA_TEST(context_request_query_decodes_names_and_values_in_wire_order) {
    auto request = make_request(
        "/?%61=one+two&%62=three%20four&%61=five+six&plain=borrowed");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
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
        }
        co_return;
    });
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

    auto request = make_request(target);

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
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
        co_return;
    });
}
