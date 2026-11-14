#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_header.h"
#include "http_server_fixture.h"
using ruvia::http_header_view;

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iterator>
#include <new>
#include <span>
#include <stdexcept>
#include <system_error>

#include <asio.hpp>
#include <asio/io_context.hpp>

#include "app.h"
#include "context.h"
#include "controller.h"
#include "event_loop.h"
#include "http_header.h"
#include "http_response.h"
#include "middleware.h"
#include "model.h"
#include "task.h"
#include "task_scope.h"
#include "worker_task_fixture.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "db.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "redis.h"
#endif

namespace {

template <typename headers_type = std::initializer_list<http_header_view>>
ruvia::test_request make_request(std::string_view target, const headers_type& headers = {}, std::string_view method = "GET") {
    auto request = ruvia::test_request::method(method, target);
    for (const auto& header : headers) {
        request.header(header.name(), header.value());
    }
    return request;
}

std::optional<std::string_view> negotiate_encoding(std::initializer_list<http_header_view> headers, std::initializer_list<std::string_view> supported) {
    auto request = make_request("/", headers);
    std::optional<std::string_view> chosen;
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::encoding, supported);
        co_return;
    });
    return chosen;
}

}  // namespace

TEST(Ruvia, context_request_priority_combines_repeated_field_values_before_parsing) {
    auto request = make_request("/", {http_header_view{"Priority", "extra=\"alpha"}, http_header_view{"X-Other", "ignored"}, http_header_view{"priority", "beta\", u=1, i"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_TRUE(context_value.req().priority().urgency_ == 1);
        EXPECT_TRUE(context_value.req().priority().incremental_);
        co_return;
    });
}

TEST(Ruvia, context_request_cookie_single_lookup_uses_last_duplicate) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1; b=2; a=3"}});

    const auto response = ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto cookie = context_value.req().cookie("a");
        EXPECT_TRUE(cookie.has_value());
        EXPECT_EQ(cookie.value_or("missing"), std::string_view("3"));
        context_value.respond(context_value.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_single_lookup_scans_repeated_cookie_fields) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2"}});

    const auto response = ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto cookie = context_value.req().cookie("a");
        EXPECT_TRUE(cookie.has_value());
        EXPECT_EQ(cookie.value_or("missing"), std::string_view("1"));
        context_value.respond(context_value.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_include_repeated_cookie_headers) {
    auto request = make_request("/", {http_header_view{"Cookie", "a=1"}, http_header_view{"Cookie", "b=2; a=3"}});

    const auto response = ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& cookies = context_value.req().cookie_fields();
        EXPECT_EQ(cookies.size(), std::size_t{3});
        if (cookies.size() == 3) {
            EXPECT_EQ(cookies[0].name(), std::string_view("a"));
            EXPECT_EQ(cookies[0].value(), std::string_view("1"));
            EXPECT_EQ(cookies[1].name(), std::string_view("b"));
            EXPECT_EQ(cookies[1].value(), std::string_view("2"));
            EXPECT_EQ(cookies[2].name(), std::string_view("a"));
            EXPECT_EQ(cookies[2].value(), std::string_view("3"));
        }
        const auto latest = cookies.get("a");
        EXPECT_TRUE(latest.has_value());
        EXPECT_EQ(latest.value_or("missing"), std::string_view("3"));
        context_value.respond(context_value.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_preserve_literal_syntax_and_case_sensitive_names) {
    auto request = make_request("/", {http_header_view{"Cookie", "x=first; ignored; = unnamed; q=\"a;b\""}, http_header_view{"X-Other", "unrelated=value"}, http_header_view{"cOoKiE", "X=upper; x = last ; empty=;"}});
    const auto response = ruvia::test::with_context(request, [](ruvia::context& context) -> ruvia::task<void> {
        EXPECT_EQ(context.req().cookie("x").value_or("missing"), "last");
        EXPECT_EQ(context.req().cookie("X").value_or("missing"), "upper");
        const auto& fields = context.req().cookie_fields();
        const http_header_view expected[] = {{"x", "first"}, {"", "unnamed"}, {"q", "\"a"}, {"X", "upper"}, {"x", "last"}, {"empty", ""}};
        EXPECT_EQ(fields.size(), std::size_t{6});
        if (fields.size() == 6) {
            for (std::size_t i = 0; i != fields.size(); ++i) {
                EXPECT_EQ(fields[i].name(), expected[i].name());
                EXPECT_EQ(fields[i].value(), expected[i].value());
            }
        }
        EXPECT_EQ(fields.count("x"), std::size_t{2});
        EXPECT_EQ(fields.get("x").value_or("missing"), "last");
        EXPECT_EQ(fields.get("X").value_or("missing"), "upper");
        EXPECT_EQ(fields.get("").value_or("missing"), "unnamed");
        EXPECT_EQ(context.req().cookie("q").value_or("missing"), "\"a");
        EXPECT_EQ(context.req().cookie("empty").value_or("missing"), "");
        context.respond(context.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_keep_all_values_across_growth_and_cache_reads) {
    std::string value{};
    for (std::size_t i = 0; i != 128; ++i) {
        if (!value.empty()) {
            value.append("; ");
        }
        value.append(std::move(std::string("c").append(std::to_string(i))) + std::move(std::string("=v").append(std::to_string(i))));
    }
    auto request = make_request("/", {http_header_view{"Cookie", value}, http_header_view{"Cookie", "c0=latest; tail=end"}});
    const auto response = ruvia::test::with_context(request, [](ruvia::context& context) -> ruvia::task<void> {
        {
            const auto before = context.req().cookie("c0");
            const auto& fields = context.req().cookie_fields();
            EXPECT_EQ(fields.size(), std::size_t{130});
            if (fields.size() == 130) {
                for (std::size_t i = 0; i != 128; ++i) {
                    EXPECT_EQ(fields[i].name(), std::move(std::string("c").append(std::to_string(i))));
                    EXPECT_EQ(fields[i].value(), std::move(std::string("v").append(std::to_string(i))));
                }
                EXPECT_EQ(fields[128].name(), "c0");
                EXPECT_EQ(fields[128].value(), "latest");
                EXPECT_EQ(fields[129].name(), "tail");
                EXPECT_EQ(fields[129].value(), "end");
                const auto first = fields[0].value();
                EXPECT_EQ(context.req().cookie("c0"), before);
                EXPECT_EQ(context.req().cookie_fields().get("tail").value_or("missing"), "end");
                EXPECT_EQ(first, "v0");
            }
            EXPECT_EQ(fields.count("c0"), std::size_t{2});
            EXPECT_EQ(fields.get("c0").value_or("missing"), "latest");
        }
        context.respond(context.text(ruvia::static_text("ok")));
        co_return;
    });
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, context_request_cookie_fields_distinguish_no_pairs_from_missing_values) {
    for (const bool include_header : {false, true}) {
        auto request = ruvia::test_request::get("/");
        if (include_header) {
            request.header("Cookie", "; ignored ; flag ;");
        }
        const auto response = ruvia::test::with_context(request, [](ruvia::context& context) -> ruvia::task<void> {
            EXPECT_TRUE(context.req().cookie_fields().empty());
            EXPECT_FALSE(context.req().cookie("missing").has_value());
            EXPECT_FALSE(context.req().cookie_fields().get("flag").has_value());
            context.respond(context.text(ruvia::static_text("empty")));
            co_return;
        });
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.body(), "empty");
    }
}

TEST(Ruvia, context_request_query_accessors_agree_on_decoded_values) {
    auto request = make_request("/?a=first&b=2&a=one+two");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto query = context_value.req().query("a");
        EXPECT_TRUE(query.has_value());
        EXPECT_EQ(*query, std::string_view("one two"));

        const auto all = context_value.req().queries("a");
        EXPECT_EQ(all.size(), std::size_t{2});
        EXPECT_EQ(all.back(), std::string_view("one two"));
        const auto& fields_value = context_value.req().query_fields();
        EXPECT_EQ(*fields_value.get("a"), std::string_view("one two"));
        const auto repeated = context_value.req().query("a");
        EXPECT_TRUE(repeated.has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_query_views_survive_multivalue_lookups) {
    auto request = make_request("/?z=first&%61=one+two&z=last&a=three&empty=&=unnamed");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto scalar = context_value.req().query("a");
        const auto& fields = context_value.req().query_fields();
        const auto decoded = fields[1].value();
        const auto z = context_value.req().queries("z");
        const auto a = context_value.req().queries("a");
        EXPECT_TRUE(context_value.req().queries("missing").empty());
        const auto empty = context_value.req().queries("empty");
        const auto unnamed = context_value.req().queries("");
        EXPECT_EQ(scalar.value_or("missing"), "three");
        EXPECT_EQ(decoded, "one two");
        EXPECT_EQ(fields.size(), std::size_t{6});
        EXPECT_EQ(fields[0].name(), "z");
        EXPECT_EQ(fields[1].name(), "a");
        EXPECT_EQ(z.size(), std::size_t{2});
        EXPECT_EQ(a.size(), std::size_t{2});
        if (z.size() == 2 && a.size() == 2) {
            EXPECT_EQ(z[0], "first");
            EXPECT_EQ(z[1], "last");
            EXPECT_EQ(a[0], "one two");
            EXPECT_EQ(a[1], "three");
        }
        EXPECT_EQ(empty.size(), std::size_t{1});
        EXPECT_EQ(unnamed.size(), std::size_t{1});
        if (!empty.empty() && !unnamed.empty()) {
            EXPECT_TRUE(empty.front().empty());
            EXPECT_EQ(unnamed.front(), "unnamed");
        }
        co_return;
    });
}

TEST(Ruvia, context_request_query_reads_unencoded_fields) {
    auto request = make_request("/?tag=x&page=2");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto tag = context_value.req().query("tag");
        EXPECT_TRUE(tag.has_value());
        EXPECT_EQ(*tag, std::string_view("x"));
        const auto page = context_value.req().query("page");
        EXPECT_TRUE(page.has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_query_preserves_short_decoded_names_and_values) {
    auto request = make_request("/?%61=%31&%62=%32&%61=%33");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& fields_value = context_value.req().query_fields();
        EXPECT_EQ(fields_value.size(), std::size_t{3});
        EXPECT_EQ(fields_value[0].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[0].value(), std::string_view("1"));
        EXPECT_EQ(fields_value[1].name(), std::string_view("b"));
        EXPECT_EQ(fields_value[1].value(), std::string_view("2"));
        EXPECT_EQ(fields_value[2].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[2].value(), std::string_view("3"));
        EXPECT_EQ(context_value.req().query("a").value_or("missing"), std::string_view("3"));
        EXPECT_EQ(context_value.req().query("b").value_or("missing"), std::string_view("2"));
        const auto values = context_value.req().queries("a");
        EXPECT_EQ(values.size(), std::size_t{2});
        if (values.size() == 2) {
            EXPECT_EQ(values[0], std::string_view("1"));
            EXPECT_EQ(values[1], std::string_view("3"));
        }
        co_return;
    });
}

TEST(Ruvia, context_request_query_list_uses_last_duplicate_like_single_lookup) {
    auto request = make_request("/?a=1&b=2&a=3");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // Single-value lookup resolves a duplicate name to its LAST value.
        EXPECT_EQ(*context_value.req().query("a"), std::string_view("3"));

        // The query field list (used by controller field binding) preserves every
        // decoded occurrence, while its scalar get() agrees with query("a") by
        // taking the last value.
        const auto& list = context_value.req().query_fields();
        EXPECT_EQ(list.size(), std::size_t{3});
        EXPECT_EQ(list[0].name(), std::string_view("a"));
        EXPECT_EQ(list[0].value(), std::string_view("1"));
        EXPECT_EQ(list[1].name(), std::string_view("b"));
        EXPECT_EQ(list[1].value(), std::string_view("2"));
        EXPECT_EQ(list[2].name(), std::string_view("a"));
        EXPECT_EQ(list[2].value(), std::string_view("3"));
        const auto via_list = list.get("a");
        EXPECT_TRUE(via_list.has_value());
        EXPECT_EQ(*via_list, std::string_view("3"));
        EXPECT_EQ(*list.get("b"), std::string_view("2"));

        // queries() still exposes every value in order (getAll semantics).
        const auto all = context_value.req().queries("a");
        EXPECT_EQ(all.size(), std::size_t{2});
        EXPECT_EQ(all[0], std::string_view("1"));
        EXPECT_EQ(all[1], std::string_view("3"));
        co_return;
    });
}

TEST(Ruvia, context_request_queries_use_empty_span_only_for_missing_name) {
    auto request = make_request("/?empty=&flag&value=x");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto empty_value = context_value.req().queries("empty");
        EXPECT_EQ(empty_value.size(), std::size_t{1});
        EXPECT_TRUE(empty_value.front().empty());

        const auto valueless = context_value.req().queries("flag");
        EXPECT_EQ(valueless.size(), std::size_t{1});
        EXPECT_TRUE(valueless.front().empty());

        EXPECT_TRUE(context_value.req().queries("missing").empty());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_merges_multiple_accept_field_lines) {
    const auto accepts = [](std::initializer_list<std::string_view> accept_lines, std::string_view media_type) {
        std::vector<http_header_view> headers{};
        headers.reserve(accept_lines.size());
        for (const auto line : accept_lines) {
            headers.emplace_back("Accept", line);
        }
        auto request = make_request("/", headers);
        bool accepted = false;
        (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
            accepted = context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, {media_type}).has_value();
            co_return;
        });
        return accepted;
    };

    // No Accept header -> the client accepts anything.
    EXPECT_TRUE(accepts({}, "text/html"));
    // A present but empty Accept list is distinct from an absent field: it has
    // no matching media range. Empty members remain harmless when another field
    // line supplies an actual range.
    EXPECT_TRUE(!accepts({""}, "text/html"));
    EXPECT_TRUE(accepts({"", "text/html"}, "text/html"));
    // A single line behaves as before.
    EXPECT_TRUE(accepts({"text/html"}, "text/html"));
    EXPECT_TRUE(!accepts({"text/html"}, "application/json"));

    // RFC 9110 5.3: two Accept lines are equivalent to their comma-join. A type
    // offered only on the SECOND line must be accepted -- previously the stored
    // known-header slot held one line and the other was ignored.
    EXPECT_TRUE(accepts({"text/html", "application/json"}, "application/json"));
    EXPECT_TRUE(accepts({"text/html", "application/json"}, "text/html"));
    EXPECT_TRUE(!accepts({"text/html", "application/json"}, "image/png"));

    // A q=0 exclusion whose range is more specific than an accepting range on
    // another line must win, exactly as if joined "text/*, text/html;q=0" -- which
    // a naive per-line OR would get wrong.
    EXPECT_TRUE(!accepts({"text/*", "text/html;q=0"}, "text/html"));
    EXPECT_TRUE(accepts({"text/*", "text/html;q=0"}, "text/plain"));
}

TEST(Ruvia, context_request_header_lookup_uses_last_match) {
    auto request = make_request("/", {http_header_view{"X-Trace", "first"}, http_header_view{"x-trace", "second"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_EQ(context_value.req().header("X-Trace"), std::string_view("second"));
        co_return;
    });
}

TEST(Ruvia, context_request_header_lookup_is_case_insensitive_and_presence_aware) {
    auto request = make_request("/", {http_header_view{"X-Empty", ""}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto present_empty = context_value.req().header("x-EMPTY");
        EXPECT_TRUE(present_empty.has_value());
        EXPECT_TRUE(present_empty.value_or("missing").empty());
        EXPECT_TRUE(!context_value.req().header("X-Missing").has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_preserves_exact_extension_method_token) {
    auto request = make_request("/", {}, "PROPFIND");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_EQ(context_value.req().method(), std::string_view("PROPFIND"));
        EXPECT_TRUE(context_value.req().known_method() == ruvia::http_known_method::unknown);
        co_return;
    });
}

TEST(Ruvia, context_request_header_fields_enumerate_every_field_in_order) {
    auto request = make_request("/", {http_header_view{"X-Trace", "a"}, http_header_view{"X-Trace", "b"}, http_header_view{"X-Other", "c"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& headers = context_value.req().header_fields();
        // Enumeration preserves spelling; both lookup surfaces ignore ASCII case.
        EXPECT_EQ(headers.count("x-trace"), std::size_t(2));
        EXPECT_EQ(headers.count("X-Trace"), std::size_t(2));
        EXPECT_EQ(headers[0].name(), std::string_view("X-Trace"));
        EXPECT_EQ(headers[0].value(), std::string_view("a"));
        EXPECT_EQ(headers[1].value(), std::string_view("b"));
        EXPECT_EQ(headers[2].name(), std::string_view("X-Other"));
        // Scalar lookup keeps last-occurrence semantics across the same list.
        EXPECT_EQ(*headers.get("x-trace"), std::string_view("b"));
        // ...while the named lookup still accepts the sent spelling, and both paths
        // agree on last-occurrence-wins for a repeated name.
        EXPECT_EQ(*context_value.req().header("X-Trace"), std::string_view("b"));
        co_return;
    });
}

TEST(Ruvia, context_request_query_fields_enumerate_repeated_names) {
    auto request = make_request("/?tag=x&tag=y&page=2");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& queries = context_value.req().query_fields();
        EXPECT_EQ(queries.size(), std::size_t(3));
        EXPECT_EQ(queries.count("tag"), std::size_t(2));
        EXPECT_EQ(queries[0].value(), std::string_view("x"));
        EXPECT_EQ(queries[1].value(), std::string_view("y"));
        EXPECT_EQ(*queries.get("page"), std::string_view("2"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_encoding_preserves_identity_defaults) {
    EXPECT_EQ(negotiate_encoding({}, {"gzip", "identity"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", ""}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip"}}, {"identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5, *;q=0.2"}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5, identity;q=0.1"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.5"}, {"accept-encoding", "identity;q=0"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "*;q=0"}, {"Accept-Encoding", "identity;q=0.7"}}, {"gzip", "identity"}).value_or("missing"), std::string_view("identity"));
    EXPECT_TRUE(!negotiate_encoding({{"Accept-Encoding", "*;q=0"}}, {"identity"}).has_value());
    EXPECT_TRUE(!negotiate_encoding({{"Accept-Encoding", ""}}, {"gzip"}).has_value());
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "extension;q=0.8, identity;q=0.2"}}, {"identity", "extension"}).value_or("missing"), std::string_view("extension"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip, identity"}}, {"identity", "gzip"}).value_or("missing"), std::string_view("identity"));
}

TEST(Ruvia, context_request_negotiate_encoding_preserves_coding_aliases) {
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-gzip"}}, {"gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip"}}, {"x-gzip"}).value_or("missing"), std::string_view("x-gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-gzip;q=0, *;q=1"}}, {"gzip", "br"}).value_or("missing"), std::string_view("br"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "gzip;q=0.3"}, {"Accept-Encoding", "X-GZIP;q=0.9, br;q=0.5"}}, {"br", "gzip"}).value_or("missing"), std::string_view("gzip"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-compress"}}, {"compress"}).value_or("missing"), std::string_view("compress"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "compress"}}, {"x-compress"}).value_or("missing"), std::string_view("x-compress"));
    EXPECT_EQ(negotiate_encoding({{"Accept-Encoding", "x-compress;q=0, *;q=1"}}, {"compress", "gzip"}).value_or("missing"), std::string_view("gzip"));
}

TEST(Ruvia, context_request_negotiate_picks_the_client_preferred_media_type) {
    auto request = make_request("/", {http_header_view{"Accept", "text/html;q=0.3, application/json;q=0.9"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // Server order lists html first, but the client prefers json.
        const std::string_view supported[] = {"text/html", "application/json"};
        const auto chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("application/json"));

        // A first-acceptable-offer check would have taken html; negotiation
        // ranks the offers by the client's weights instead.
        EXPECT_TRUE(context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, {"text/html"}).has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_reports_no_acceptable_representation) {
    auto request = make_request("/", {http_header_view{"Accept", "image/png"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"text/html", "application/json"};
        // nullopt is the 406 signal, which is why this cannot fall back to front().
        EXPECT_TRUE(!context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, supported).has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_without_the_field_takes_server_preference) {
    auto request = make_request("/");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"application/json", "text/html"};
        const auto chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("application/json"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_language_uses_basic_prefix_filtering) {
    auto request = make_request("/", {http_header_view{"Accept-Language", "fr;q=0.4, en;q=0.8"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        // RFC 4647 basic filtering: the range "en" matches the tag "en-US".
        const std::string_view supported[] = {"fr-CA", "en-US"};
        const auto chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::language, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("en-US"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_honours_explicit_zero_quality_exclusion) {
    auto request = make_request("/", {http_header_view{"Accept-Encoding", "*, gzip;q=0"}});
    // "*" accepts everything except the explicitly excluded, more specific tag.

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"gzip", "br"};
        const auto chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::encoding, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("br"));
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_folds_repeated_field_lines) {
    auto request = make_request("/", {http_header_view{"Accept-Language", "de;q=0.2"}, http_header_view{"Accept-Language", "ja;q=0.9"}});
    // RFC 9110 5.3: equivalent to one comma-joined value.

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"de", "ja"};
        const auto chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::language, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("ja"));
        co_return;
    });
}

TEST(Ruvia, context_request_query_decodes_names_and_values_in_wire_order) {
    auto request = make_request("/?%61=one+two&%62=three%20four&%61=five+six&plain=borrowed");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& fields_value = context_value.req().query_fields();
        EXPECT_EQ(fields_value.size(), std::size_t{4});
        EXPECT_EQ(fields_value[0].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[0].value(), std::string_view("one two"));
        EXPECT_EQ(fields_value[1].name(), std::string_view("b"));
        EXPECT_EQ(fields_value[1].value(), std::string_view("three four"));
        EXPECT_EQ(fields_value[2].name(), std::string_view("a"));
        EXPECT_EQ(fields_value[2].value(), std::string_view("five six"));
        EXPECT_EQ(fields_value[3].value(), std::string_view("borrowed"));

        const auto values = context_value.req().queries("a");
        EXPECT_EQ(values.size(), std::size_t{2});
        if (values.size() == 2) {
            EXPECT_EQ(values[0], std::string_view("one two"));
            EXPECT_EQ(values[1], std::string_view("five six"));
        }
        const auto value = context_value.req().query("a");
        EXPECT_TRUE(value.has_value());
        if (value) {
            EXPECT_EQ(*value, std::string_view("five six"));
        }
        co_return;
    });
}

TEST(Ruvia, context_request_query_preserves_many_decoded_values) {
    constexpr std::size_t count = 257;
    std::string target{"/?"};
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            target += '&';
        }
        target += std::move(std::string("%74ag=v+").append(std::to_string(i)));
    }

    auto request = make_request(target);

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto& fields_value = context_value.req().query_fields();
        const auto values = context_value.req().queries("tag");
        EXPECT_EQ(fields_value.size(), count);
        EXPECT_EQ(values.size(), count);
        if (fields_value.size() == count && values.size() == count) {
            for (std::size_t i = 0; i < count; ++i) {
                const auto expected = std::move(std::string("v ").append(std::to_string(i)));
                EXPECT_EQ(fields_value[i].name(), std::string_view("tag"));
                EXPECT_EQ(fields_value[i].value(), std::string_view(expected));
                EXPECT_EQ(values[i], std::string_view(expected));
            }
        }
        co_return;
    });
}

#if defined(RUVIA_ENABLE_DATABASE) || defined(RUVIA_ENABLE_REDIS)
TEST(Ruvia, context_rejects_unconfigured_worker_clients_consistently) {
    const auto response = ruvia::test::with_context(ruvia::test_request::get("/capabilities"), [&](ruvia::context& context_value) -> ruvia::task<void> {
#ifdef RUVIA_ENABLE_DATABASE
        bool database_rejected = false;
        try {
            static_cast<void>(context_value.db());
        } catch (const ruvia::db_error& error) {
            database_rejected = error.code() == ruvia::db_error::code_type::not_configured;
        }
        EXPECT_TRUE(database_rejected);
#endif

#ifdef RUVIA_ENABLE_REDIS
        bool redis_rejected = false;
        try {
            static_cast<void>(context_value.redis());
        } catch (const ruvia::redis_error& error) {
            redis_rejected = error.code() == ruvia::redis_error::code_type::not_configured;
        }
        EXPECT_TRUE(redis_rejected);
#endif
        co_return;
    });
    (void)response;
}
#endif

TEST(Ruvia, context_session_capability_requires_explicit_middleware_binding) {
    const auto response = ruvia::test::with_context(ruvia::test_request::get("/capabilities"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        bool rejected = false;
        try {
            static_cast<void>(context_value.session());
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);

        co_return;
    });
    (void)response;
}

// Setting response cookies, including prefixes and signatures.

TEST(Ruvia, context_set_cookie_serializes_all_attributes) {
    auto request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const ruvia::cookie_options host{
            .path_ = "/",
            .same_site_ = ruvia::cookie_same_site::strict,
            .priority_ = ruvia::cookie_priority::high,
            .max_age_ = std::chrono::seconds(3600),
            .prefix_ = ruvia::cookie_prefix::host,
            .http_only_ = ruvia::cookie_attribute_policy::emit,
            .secure_ = ruvia::cookie_attribute_policy::emit,
            .partitioned_ = ruvia::cookie_attribute_policy::emit,
        };
        context_value.draft().set_cookie({.name_ = "id", .value_ = "abc", .attributes_ = host});
        const auto host_response = context_value.text("ok");
        EXPECT_EQ(host_response.header("Set-Cookie"),
            std::string_view("__Host-id=abc; Path=/; Max-Age=3600; HttpOnly; Secure; "
                             "SameSite=Strict; Priority=High; Partitioned"));

        co_return;
    });

    // Case B: __Secure- prefix carrying Domain and a fixed Expires (the well-known
    // instant 1234567890 = Fri 13 Feb 2009 23:31:30 UTC, formatted as a
    // locale-independent IMF-fixdate) plus SameSite=None. Covers the Domain and
    // Expires branches Case A omits.
    const ruvia::cookie_options secure{
        .path_ = "/app",
        .domain_ = "example.com",
        .same_site_ = ruvia::cookie_same_site::none,
        .expires_ = std::chrono::system_clock::time_point(std::chrono::seconds(1234567890)),
        .prefix_ = ruvia::cookie_prefix::secure,
        .secure_ = ruvia::cookie_attribute_policy::emit,
    };
    auto secure_request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(secure_request, [&](ruvia::context& secure_context) -> ruvia::task<void> {
        secure_context.draft().set_cookie({.name_ = "sess", .value_ = "xyz", .attributes_ = secure});
        const auto secure_response = secure_context.text("ok");
        EXPECT_EQ(secure_response.header("Set-Cookie"),
            std::string_view("__Secure-sess=xyz; Path=/app; Domain=example.com; "
                             "Expires=Fri, 13 Feb 2009 23:31:30 GMT; Secure; SameSite=None"));
        co_return;
    });
}

TEST(Ruvia, context_signed_cookie_handles_large_values_in_request_storage) {
    const std::string value(2048, 'v');
    std::string cookie{};
    const auto request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context) -> ruvia::task<void> {
        bool signed_value = false;
        try {
            context.draft().set_signed_cookie({.name_ = "session", .value_ = value, .secret_ = "secret"});
            signed_value = true;
        } catch (const std::bad_alloc&) {
        }
        EXPECT_TRUE(signed_value);
        if (signed_value) {
            const auto response = context.text("ok");
            const auto header = response.header("Set-Cookie").value_or("");
            cookie.assign(header.substr(0, header.find(';')));
        }
        co_return;
    });
    EXPECT_TRUE(!cookie.empty());
    if (cookie.empty()) {
        return;
    }
    const auto read = ruvia::test_request::get("/").header("Cookie", cookie);
    (void)ruvia::test::with_context(read, [&](ruvia::context& context) -> ruvia::task<void> {
        std::optional<std::string_view> verified;
        try {
            verified = context.req().signed_cookie({.name_ = "session", .secret_ = "secret"});
        } catch (const std::bad_alloc&) {
        }
        EXPECT_TRUE(verified.has_value());
        if (verified) {
            EXPECT_EQ(*verified, std::string_view(value));
        }
        co_return;
    });
}

TEST(Ruvia, context_set_cookie_preserves_same_name_different_path) {
    auto request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        ruvia::cookie_options root;
        root.path_ = "/";
        ruvia::cookie_options admin;
        admin.path_ = "/admin";
        context_value.draft().set_cookie({.name_ = "session", .value_ = "root-old", .attributes_ = root});
        context_value.draft().set_cookie({.name_ = "session", .value_ = "admin", .attributes_ = admin});
        context_value.draft().set_cookie({.name_ = "session", .value_ = "root-new", .attributes_ = root});
        const auto response = context_value.text("ok");

        std::size_t count = 0;
        bool has_root_old = false;
        bool has_root_new = false;
        bool has_admin = false;
        for (const auto& header : response.headers()) {
            if (header.name() != std::string_view("Set-Cookie")) {
                continue;
            }
            ++count;
            has_root_old = has_root_old || header.value() == "session=root-old; Path=/";
            has_root_new = has_root_new || header.value() == "session=root-new; Path=/";
            has_admin = has_admin || header.value() == "session=admin; Path=/admin";
        }
        EXPECT_EQ(count, std::size_t{2});
        EXPECT_TRUE(!has_root_old);
        EXPECT_TRUE(has_root_new);
        EXPECT_TRUE(has_admin);
        co_return;
    });
}

TEST(Ruvia, context_signed_cookie_with_prefix_verifies_round_trip) {
    std::string set_cookie{};
    std::string bare{};
    auto write_request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(write_request, [&](ruvia::context& write_context) -> ruvia::task<void> {
        const ruvia::cookie_options options{
            .path_ = "/",
            .prefix_ = ruvia::cookie_prefix::host,
            .secure_ = ruvia::cookie_attribute_policy::emit,
        };
        write_context.draft().set_signed_cookie({.name_ = "session", .value_ = "user-1", .secret_ = "secret", .attributes_ = options});
        const auto write_response = write_context.text("ok");
        set_cookie = write_response.header("Set-Cookie").value_or(std::string_view{});
        co_return;
    });
    const std::string_view line(set_cookie);
    const auto pair = line.substr(0, line.find(';'));
    EXPECT_TRUE(pair.starts_with("__Host-session="));

    // Present the cookie exactly as a browser sends it back.
    const std::string cookie_field(pair);
    auto read_request = ruvia::test_request::get("/").header("Cookie", cookie_field);

    (void)ruvia::test::with_context(read_request, [&](ruvia::context& read_context) -> ruvia::task<void> {
        const auto verified = read_context.req().signed_cookie({.name_ = "__Host-session", .secret_ = "secret"});
        EXPECT_TRUE(verified.has_value());
        EXPECT_EQ(*verified, std::string_view("user-1"));

        co_return;
    });

    // An unprefixed signed cookie keeps verifying under its own name.
    auto bare_write_request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(bare_write_request, [&](ruvia::context& bare_write_context) -> ruvia::task<void> {
        bare_write_context.draft().set_signed_cookie({.name_ = "plain", .value_ = "user-2", .secret_ = "secret"});
        const auto bare_write_response = bare_write_context.text("ok");
        bare = bare_write_response.header("Set-Cookie").value_or(std::string_view{});
        co_return;
    });
    const std::string_view bare_line(bare);
    const std::string bare_field(bare_line.substr(0, bare_line.find(';')));
    auto bare_request = ruvia::test_request::get("/").header("Cookie", bare_field);

    (void)ruvia::test::with_context(bare_request, [&](ruvia::context& bare_context) -> ruvia::task<void> {
        const auto bare_verified = bare_context.req().signed_cookie({.name_ = "plain", .secret_ = "secret"});
        EXPECT_TRUE(bare_verified.has_value());
        EXPECT_EQ(*bare_verified, std::string_view("user-2"));
        co_return;
    });
}

TEST(Ruvia, context_signed_cookie_rejects_a_repeated_name) {
    std::string set_cookie{};
    auto write_request = ruvia::test_request::get("/");
    (void)ruvia::test::with_context(write_request, [&](ruvia::context& write_context) -> ruvia::task<void> {
        write_context.draft().set_signed_cookie({.name_ = "session", .value_ = "user-1", .secret_ = "secret"});
        const auto write_response = write_context.text("ok");
        set_cookie = write_response.header("Set-Cookie").value_or(std::string_view{});
        co_return;
    });
    const std::string_view line(set_cookie);
    const std::string pair(line.substr(0, line.find(';')));

    auto single_request = ruvia::test_request::get("/").header("Cookie", pair);
    (void)ruvia::test::with_context(single_request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_EQ(context_value.req().signed_cookie({.name_ = "session", .secret_ = "secret"}), std::optional<std::string_view>("user-1"));
        co_return;
    });

    // Even identical, individually valid occurrences are ambiguous about which
    // one the signature vouches for.
    auto same_field_request = ruvia::test_request::get("/").header("Cookie", pair + "; " + pair);
    (void)ruvia::test::with_context(same_field_request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_FALSE(context_value.req().signed_cookie({.name_ = "session", .secret_ = "secret"}).has_value());
        co_return;
    });

    // Last-wins lookup would have verified the trailing genuine occurrence.
    auto split_fields_request = ruvia::test_request::get("/").header("Cookie", "session=forged").header("Cookie", pair);
    (void)ruvia::test::with_context(split_fields_request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        EXPECT_FALSE(context_value.req().signed_cookie({.name_ = "session", .secret_ = "secret"}).has_value());
        co_return;
    });
}

TEST(Ruvia, context_delete_cookie_with_prefix_is_response_only) {
    auto request = ruvia::test_request::get("/").header("Cookie", "__Host-session=user-1");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const ruvia::cookie_options options{
            .path_ = "/",
            .prefix_ = ruvia::cookie_prefix::host,
            .secure_ = ruvia::cookie_attribute_policy::emit,
        };
        const auto previous = context_value.req().cookie("__Host-session");
        EXPECT_TRUE(previous.has_value());
        EXPECT_EQ(*previous, std::string_view("user-1"));
        context_value.draft().delete_cookie({.name_ = "session", .attributes_ = options});
        const auto response = context_value.text("deleted");
        const auto set_cookie = response.header("Set-Cookie");
        EXPECT_TRUE(set_cookie.has_value());
        EXPECT_TRUE(set_cookie.value_or(std::string_view{}).starts_with("__Host-session=;"));
        EXPECT_TRUE((set_cookie.value_or(std::string_view{}).find("; Expires=Thu, 01 Jan 1970 00:00:00 GMT") != std::string_view::npos));
        EXPECT_TRUE((set_cookie.value_or(std::string_view{}).find("Max-Age") == std::string_view::npos));
        co_return;
    });
}

RUVIA_MODEL(context_json_response, RUVIA_REQUIRED_FIELD(number, ruvia::int64), RUVIA_REQUIRED_FIELD(boolean, ruvia::bool_value), RUVIA_REQUIRED_FIELD(real, ruvia::double_value));

using ruvia::test::throws_on;

TEST(Ruvia, context_redirect_sets_verbatim_ascii_location_and_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "https://example.com/path?q=1"});
        EXPECT_EQ(response.status(), ruvia::http_status::found);
        EXPECT_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_accepts_only_redirect_statuses) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        for (const auto status : {ruvia::http_status::moved_permanently, ruvia::http_status::found, ruvia::http_status::see_other, ruvia::http_status::temporary_redirect, ruvia::http_status::permanent_redirect}) {
            EXPECT_EQ(context.redirect({.location_ = "/next", .status_ = status}).status(), status);
        }

        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::ok});
        }));
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_modified});
        }));
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_found});
        }));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_non_ascii_location) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // A UTF-8 'é' (0xC3 0xA9) is percent-encoded while the URI structure
        // (scheme, host, path separators) is preserved.
        const auto response = context.redirect({.location_ = std::string_view("https://example.com/caf\xC3\xA9"), .status_ = ruvia::http_status::temporary_redirect});
        EXPECT_EQ(response.status(), ruvia::http_status::temporary_redirect);
        EXPECT_EQ(response.header("Location"), std::string_view("https://example.com/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_invalid_ascii_uri_bytes) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/a b/100%off?q=\"x y\"\\z"});
        EXPECT_EQ(response.header("Location"), std::string_view("/a%20b/100%25off?q=%22x%20y%22%5Cz"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_existing_percent_escapes_when_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // The location mixes already-encoded escapes ("%20", "%2F") with a raw UTF-8
        // 'é' (0xC3 0xA9) that triggers the whole-string encoding pass. The 'é' must
        // become %C3%A9, but the existing escapes must survive intact -- not be
        // double-encoded to "%2520"/"%252F", which would corrupt the target.
        const auto response = context.redirect({.location_ = std::string_view("https://example.com/a%20b/caf\xC3\xA9?x=%2F")});
        EXPECT_EQ(response.header("Location"), std::string_view("https://example.com/a%20b/caf%C3%A9?x=%2F"));

        // A lone or malformed '%' (not followed by two hex digits) is not a valid
        // escape, so it IS percent-encoded to %25 -- the trailing 'é' forces the pass.
        const auto malformed = context.redirect({.location_ = std::string_view("https://example.com/100%off/caf\xC3\xA9")});
        EXPECT_EQ(malformed.header("Location"), std::string_view("https://example.com/100%25off/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_ipv6_literal_brackets_when_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = std::string_view("https://[2001:db8::1]/caf\xC3\xA9")});
        EXPECT_EQ(response.header("Location"), std::string_view("https://[2001:db8::1]/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_square_brackets_outside_ip_literal) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/items[0]?filter=[x]#section[1]"});
        EXPECT_EQ(response.header("Location"), std::string_view("/items%5B0%5D?filter=%5Bx%5D#section%5B1%5D"));

        const auto absolute = context.redirect({.location_ = std::string_view("https://[2001:db8::1]/items[0]?filter=[x]")});
        EXPECT_EQ(absolute.header("Location"), std::string_view("https://[2001:db8::1]/items%5B0%5D?filter=%5Bx%5D"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_fragment_separator_and_escapes_fragment_hashes) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        constexpr std::pair<std::string_view, std::string_view> cases[]{
            {"/page#part#nested", "/page#part%23nested"},
            {"/page?x=1#part##nested", "/page?x=1#part%23%23nested"},
            {"https://[::1]/x%20y#part#nested", "https://[::1]/x%20y#part%23nested"},
            {"/目标#片#段", "/%E7%9B%AE%E6%A0%87#%E7%89%87%23%E6%AE%B5"},
            {"#part/nested?x=1", "#part/nested?x=1"},
            {"/page#part%23nested", "/page#part%23nested"},
            {"/page%23part?x=%23#part", "/page%23part?x=%23#part"},
            {"/page#", "/page#"},
        };
        for (const auto& [location, expected] : cases) {
            const auto response = context.redirect({.location_ = location});
            EXPECT_EQ(response.header("Location").value_or(""), expected);
        }
        co_return;
    });
}

TEST(Ruvia, context_redirect_escapes_colons_that_would_read_as_a_scheme) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        constexpr std::pair<std::string_view, std::string_view> cases[]{
            {"1foo:bar", "1foo%3Abar"},
            {"a b:c/d:e", "a%20b%3Ac/d:e"},
            {"page:1?q=a:b#x:y", "page:1?q=a:b#x:y"},
            {"mailto:user@example.com", "mailto:user@example.com"},
            {"./a:b", "./a:b"},
            {"/a:b", "/a:b"},
            {"?a:b", "?a:b"},
        };
        for (const auto& [location, expected] : cases) {
            const auto response = context.redirect({.location_ = location});
            EXPECT_EQ(response.header("Location").value_or(""), expected);
        }
        co_return;
    });
}

TEST(Ruvia, context_redirect_escapes_at_signs_inside_userinfo) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        constexpr std::pair<std::string_view, std::string_view> cases[]{
            {"http://user@name@host.example/p@q", "http://user%40name@host.example/p@q"},
            {"//a@b@[::1]/", "//a%40b@[::1]/"},
            {"http://user@host.example/", "http://user@host.example/"},
        };
        for (const auto& [location, expected] : cases) {
            const auto response = context.redirect({.location_ = location});
            EXPECT_EQ(response.header("Location").value_or(""), expected);
        }
        co_return;
    });
}

TEST(Ruvia, context_redirect_rejects_crlf_header_injection) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // A CRLF in the location must not split the response: header-value
        // validation rejects it (the location is ASCII, so it takes the verbatim
        // path straight into the validated header setter).
        bool threw = false;
        try {
            (void)context.redirect({.location_ = std::string_view("https://example.com/\r\nX-Injected: y")});
        } catch (const std::exception&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        co_return;
    });
}

TEST(Ruvia, context_redirect_rejects_crlf_even_when_location_needs_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = std::string_view("https://example.com/caf\xC3\xA9\r\nX-Injected: y")});
        }));
        co_return;
    });
}

TEST(Ruvia, context_body_sets_body_and_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::created);
        const auto response = context.body("hello world");
        EXPECT_EQ(response.status(), ruvia::http_status::created);
        EXPECT_EQ(response.body_bytes(), std::string_view("hello world"));
        co_return;
    });
}

TEST(Ruvia, context_dynamic_body_owns_input_and_preserves_lvalue) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        std::string source_value("dynamic body");

        const auto response = context.body(std::string_view(source_value));
        source_value[0] = 'X';

        EXPECT_EQ(response.body_bytes(), std::string_view("dynamic body"));
        EXPECT_EQ(source_value, std::string_view("Xynamic body"));
        co_return;
    });
}

TEST(Ruvia, context_static_text_builders_borrow_static_storage) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        static constexpr std::string_view page = "<b>html</b>";
        constexpr ruvia::static_text body_text("bo\0dy");
        const auto body_response = context.body(body_text);
        const auto text_response = context.text(ruvia::static_text("text"));
        const auto html_response = context.html(ruvia::static_text(page));

        EXPECT_TRUE(body_response.body_bytes() == std::string_view("bo\0dy", 5));
        EXPECT_TRUE(body_response.body_bytes().data() == body_text.view().data());
        EXPECT_TRUE(text_response.body_bytes() == std::string_view("text"));
        EXPECT_EQ(text_response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_TRUE(html_response.body_bytes() == page);
        EXPECT_TRUE(html_response.body_bytes().data() == page.data());
        EXPECT_EQ(html_response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
        co_return;
    });
}

TEST(Ruvia, context_character_array_builders_copy_until_terminator) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        char buffer[64] = {};
        const auto written = std::snprintf(buffer, sizeof(buffer), "count=%d", 7);
        EXPECT_TRUE(written > 0);
        const auto text_response = context.text(buffer);
        const auto html_response = context.html(buffer);
        std::string owned_string = "abc";
        const auto body_response = context.body(owned_string);
        owned_string[0] = 'Z';
        buffer[0] = 'X';

        EXPECT_EQ(text_response.body_bytes(), std::string_view("count=7"));
        EXPECT_TRUE(text_response.body_bytes().data() != buffer);
        EXPECT_EQ(html_response.body_bytes(), std::string_view("count=7"));
        EXPECT_EQ(body_response.body_bytes(), std::string_view("abc"));
        EXPECT_TRUE(body_response.body_bytes().data() != owned_string.data());

        const auto literal_response = context.text("literal");
        EXPECT_EQ(literal_response.body_bytes(), std::string_view("literal"));
        co_return;
    });
}

TEST(Ruvia, context_rejects_informational_and_non_http_final_statuses) {
    {
        (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
            bool threw = false;
            try {
                context.draft().status(ruvia::http_status::early_hints);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            EXPECT_TRUE(threw);
            co_return;
        });
    }
    {
        bool threw = false;
        try {
            (void)ruvia::http_status_code::from_value(600);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
    }
}

TEST(Ruvia, context_error_normalizes_non_error_status_before_response_state) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("X-Trace", "1");

        const auto ok = context.error({.status_ = ruvia::http_status::ok, .code_ = "bad", .message_ = "not an error"});
        EXPECT_EQ(ok.status(), ruvia::http_status::internal_server_error);
        EXPECT_TRUE(!ok.header("X-Trace").has_value());

        const auto redirect = context.error({.status_ = ruvia::http_status::temporary_redirect, .code_ = "bad", .message_ = "not an error"});
        EXPECT_EQ(redirect.status(), ruvia::http_status::internal_server_error);
        co_return;
    });
}

TEST(Ruvia, context_response_metadata_uses_http_response_validation) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        bool threw = false;
        try {
            context.draft().header("Connection", "close,");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        EXPECT_TRUE(threw);
        const auto response = context.body("unchanged");
        EXPECT_TRUE(!response.header("Connection").has_value());
        co_return;
    });
}

TEST(Ruvia, context_body_applies_context_headers) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::ok);
        context.draft().header("Content-Type", "text/plain");
        context.draft().header("X-Custom", "v");
        const auto response = context.body("data");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.body_bytes(), std::string_view("data"));
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain"));
        EXPECT_EQ(response.header("X-Custom"), std::string_view("v"));
        co_return;
    });
}

TEST(Ruvia, context_text_responses_send_nosniff_unless_the_application_set_it) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto borrowed = context.text("plain");
        EXPECT_EQ(borrowed.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_EQ(borrowed.header("X-Content-Type-Options"), std::string_view("nosniff"));
        const auto owned = context.text(std::string("plain"));
        EXPECT_EQ(owned.header("X-Content-Type-Options"), std::string_view("nosniff"));
        context.draft().header("X-Content-Type-Options", "application-value");
        const auto configured = context.text("plain");
        EXPECT_EQ(configured.header("X-Content-Type-Options"), std::string_view("application-value"));
        co_return;
    });
}

TEST(Ruvia, context_response_header_transactions_preserve_owned_body_storage) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("X-Added", "context");
        context.draft().header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        context.draft().header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        ruvia::http_response response;
        response.header("Content-Type", "application/octet-stream");
        response.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        response.body(std::string(4096, 'x'));
        context.respond(std::move(response));
        EXPECT_EQ(context.response()->body_bytes().size(), std::size_t{4096});
        EXPECT_TRUE(!context.response()->header("X-Added").has_value());
        EXPECT_EQ(context.response()->headers().size(), std::size_t{2});
        co_return;
    });
}

TEST(Ruvia, context_response_header_transactions_preserve_cookie_policy_and_spilled_headers) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("Set-Cookie", "session=new; Path=/");
        context.draft().header("Cache-Control", "no-store");
        ruvia::http_response response;
        response.header("Set-Cookie", "session=old; Path=/");
        response.header("Set-Cookie", "other=kept; Path=/", {.mode_ = ruvia::http_response_header_mode::append});
        response.header("Content-Type", "application/octet-stream");
        response.header("Cache-Control", "private");
        for (int i = 0; i < 12; ++i) {
            response.header(std::move(std::string("X-Field-").append(std::to_string(i))), "value");
        }
        response.body(std::string(4096, 'x'));
        context.respond(std::move(response));
        const auto& result_value = *context.response();
        EXPECT_EQ(result_value.header("Content-Type"), std::string_view("application/octet-stream"));
        EXPECT_EQ(result_value.header("Cache-Control"), std::string_view("private"));
        EXPECT_EQ(result_value.header("Set-Cookie"), std::string_view("session=old; Path=/"));
        std::size_t cookies = 0;
        for (const auto& header : result_value.headers()) {
            if (header.name() == "Set-Cookie") {
                ++cookies;
            }
        }
        EXPECT_EQ(cookies, std::size_t{2});
        for (int i = 0; i < 12; ++i) {
            EXPECT_EQ(result_value.header(std::move(std::string("X-Field-").append(std::to_string(i)))), std::string_view("value"));
        }
        co_return;
    });
}

TEST(Ruvia, context_draft_is_consumed_and_final_edits_do_not_cross_replacement) {
    const auto delivered = ruvia::test::with_context(ruvia::test_request::get("/"), [](ruvia::context& context) -> ruvia::task<void> {
        bool missing_rejected = false;
        try {
            context.edit_response().header("X-Invalid", "value");
        } catch (const std::logic_error&) {
            missing_rejected = true;
        }
        EXPECT_TRUE(missing_rejected);
        context.draft().status(ruvia::http_status::partial_content);
        context.draft().header("Content-Range", "bytes 0-2/10");
        context.draft().set_cookie({.name_ = "draft", .value_ = "first"});
        context.respond(context.body("abc"));
        context.edit_response().header("ETag", "\"first\"");
        context.draft().header("X-Replacement", "fresh");
        context.respond(context.text("second"));
        EXPECT_TRUE(!context.response()->header("ETag").has_value());
        EXPECT_TRUE(!context.response()->header("Content-Range").has_value());
        co_return;
    });
    EXPECT_EQ(delivered.status(), ruvia::http_status::ok);
    EXPECT_EQ(delivered.body(), std::string_view("second"));
    EXPECT_EQ(delivered.header("X-Replacement"), std::string_view("fresh"));
    EXPECT_TRUE(!delivered.header("Set-Cookie").has_value());
}

TEST(Ruvia, context_policy_is_explicit_and_independent_of_response_selection_time) {
    for (const bool after : {false, true}) {
        const auto delivered = ruvia::test::with_context(ruvia::test_request::get("/"), [after](ruvia::context& context) -> ruvia::task<void> {
            if (!after) {
                context.policy().header("X-Policy", "retained");
                context.policy().set_cookie({.name_ = "session", .value_ = "policy"});
            }
            context.respond(context.text("first"));
            if (after) {
                context.policy().header("X-Policy", "retained");
                context.policy().set_cookie({.name_ = "session", .value_ = "policy"});
            }
            context.edit_response().header("X-Discarded", "first-only");
            context.respond(context.error({.status_ = ruvia::http_status::conflict}));
            EXPECT_TRUE(!context.response()->header("X-Policy").has_value());
            co_return;
        });
        EXPECT_EQ(delivered.status(), ruvia::http_status::conflict);
        EXPECT_EQ(delivered.header("X-Policy"), std::string_view("retained"));
        EXPECT_TRUE(!delivered.header("X-Discarded").has_value());
        EXPECT_TRUE(delivered.header("Set-Cookie").value_or("").starts_with("session=policy"));
        std::size_t cookies = 0;
        for (const auto& header : delivered.headers()) {
            if (header.name() == "Set-Cookie") {
                ++cookies;
            }
        }
        EXPECT_EQ(cookies, std::size_t{1});
    }
}

TEST(Ruvia, context_metadata_preserves_repeated_set_cookie_headers) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("Set-Cookie", "a=1", {.mode_ = ruvia::http_response_header_mode::append});
        context.draft().header("Set-Cookie", "b=2", {.mode_ = ruvia::http_response_header_mode::append});
        const auto response = context.body("data");

        std::size_t set_cookie_count = 0;
        for (const auto& header : response.headers()) {
            if (header.name() == std::string_view("Set-Cookie")) {
                ++set_cookie_count;
            }
        }
        EXPECT_EQ(set_cookie_count, std::size_t{2});
        co_return;
    });
}

TEST(Ruvia, context_body_null_gives_empty_body_with_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::no_content);
        const auto response = context.body();
        EXPECT_EQ(response.status(), ruvia::http_status::no_content);
        EXPECT_TRUE(response.body_bytes().empty());
        co_return;
    });
}

TEST(Ruvia, context_body_byte_span_copies_into_response_storage) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const std::byte bytes_value[] = {
            std::byte{0x00},
            std::byte{0x41},
            std::byte{0xff},
        };
        const auto response = context.body(std::span<const std::byte>(bytes_value));
        const auto body = response.body_bytes();

        EXPECT_EQ(body.size(), std::size(bytes_value));
        EXPECT_EQ(body[0], '\0');
        EXPECT_EQ(body[1], 'A');
        EXPECT_EQ(static_cast<unsigned char>(body[2]), 0xff);
        co_return;
    });
}

TEST(Ruvia, context_text_sets_plain_content_type) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.text("hello");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_EQ(response.body_bytes(), std::string_view("hello"));
        co_return;
    });
}

TEST(Ruvia, context_html_sets_html_content_type) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.html("<h1>hi</h1>");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
        EXPECT_EQ(response.body_bytes(), std::string_view("<h1>hi</h1>"));
        co_return;
    });
}

TEST(Ruvia, context_json_serializes_response_model_with_json_content_type) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context_json_response model;
        model.set<"number">(42);
        model.set<"boolean">(true);
        model.set<"real">(3.5);
        const auto response = context.json(model);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(), std::string_view(R"({"number":42,"boolean":true,"real":3.5})"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_rejects_backslash_inside_authority) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // Browsers end the host at '\'; "%5C" would send them to evil.example.
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = "http://good.example\\@evil.example/"});
        }));
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect({.location_ = "//good.example\\@evil.example/"});
        }));
        // Outside the authority the backslash is encoded, which only narrows it.
        const auto relative = context.redirect({.location_ = "/\\evil.example"});
        EXPECT_EQ(relative.header("Location"), std::string_view("/%5Cevil.example"));
        const auto in_path = context.redirect({.location_ = "https://good.example/a\\b"});
        EXPECT_EQ(in_path.header("Location"), std::string_view("https://good.example/a%5Cb"));
        co_return;
    });
}

TEST(Ruvia, health_responses_use_context_response_state) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        context_value.draft().header("X-Trace", "health");
        context_value.draft().set_cookie({.name_ = "probe", .value_ = "ok"});

        const auto response = ruvia::make_health_response(context_value);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.header("X-Trace"), std::string_view("health"));
        EXPECT_TRUE(response.header("Set-Cookie").value_or(std::string_view{}).starts_with("probe=ok;"));
        EXPECT_EQ(response.body_bytes(), std::string_view("{\"status\":\"ok\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_defaults_to_ready) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        const auto response = ruvia::make_readiness_response(context_value);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(), std::string_view("{\"status\":\"ready\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_keeps_explicit_failure_status) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        context_value.draft().status(ruvia::http_status::created);

        const auto response = ruvia::make_readiness_response(context_value,
            {
                .state_ = ruvia::readiness_state::unavailable,
                .unavailable_reason_ = "database is unavailable",
            });
        EXPECT_EQ(response.status(), ruvia::http_status::service_unavailable);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(), std::string_view("{\"status\":\"not_ready\",\"reason\":\"database is unavailable\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_rejects_invalid_state) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        bool rejected = false;
        try {
            (void)ruvia::make_readiness_response(context_value, {.state_ = static_cast<ruvia::readiness_state>(std::uint8_t{0xFF})});
        } catch (const std::invalid_argument& exception) {
            rejected = std::string_view(exception.what()) == "readiness state is invalid";
        }
        EXPECT_TRUE(rejected);
        co_return;
    });
}

namespace {

using ruvia::default_error_code;
using ruvia::http_error;

}  // namespace

TEST(Ruvia, default_error_code_mapping) {
    EXPECT_EQ(default_error_code(ruvia::http_status::bad_request), std::string_view("bad_request"));
    EXPECT_EQ(default_error_code(ruvia::http_status::not_found), std::string_view("not_found"));
    EXPECT_EQ(default_error_code(ruvia::http_status::method_not_allowed), std::string_view("method_not_allowed"));
    EXPECT_EQ(default_error_code(ruvia::http_status::content_too_large), std::string_view("content_too_large"));
}

TEST(Ruvia, http_error_info_round_trips) {
    const http_error error({.status_ = ruvia::http_status::unprocessable_content, .code_ = "unprocessable", .message_ = "bad fields"});
    const auto info = error.info();
    EXPECT_EQ(info.status(), ruvia::http_status::unprocessable_content);
    EXPECT_EQ(info.code(), std::string_view("unprocessable"));
    EXPECT_EQ(info.message(), std::string_view("bad fields"));
}

TEST(Ruvia, exception_text_describes_any_exception) {
    EXPECT_TRUE(ruvia::exception_text(nullptr).empty());
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(std::runtime_error("plain failure"))), std::string("plain failure"));
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(42)), std::string("non-standard exception"));

    const std::system_error generic(std::make_error_code(std::errc::timed_out), "upstream");
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(generic)), std::string(generic.what()));

    // A system-category message is the OS text (UTF-8 on Windows, where what()
    // uses the ANSI code page); the context before it is kept.
    const std::system_error system(std::error_code(1, std::system_category()), "bind");
    const auto text = ruvia::exception_text(std::make_exception_ptr(system));
    EXPECT_TRUE(text.starts_with("bind: "));
    EXPECT_TRUE(text.size() > std::string_view("bind: ").size());
}

TEST(Ruvia, http_error_preserves_all_validation_issues_beyond_64) {
    ruvia::validator validator_value;
    constexpr std::size_t issue_count = 80;
    for (std::size_t index = 0; index < issue_count; ++index) {
        validator_value.add(std::move(std::string("field_").append(std::to_string(index))), "invalid", std::move(std::string("msg_").append(std::to_string(index))));
    }
    EXPECT_EQ(validator_value.issues().size(), issue_count);

    const http_error error({.status_ = ruvia::http_status::bad_request, .code_ = "validation_failed", .message_ = "request validation failed", .validation_issues_ = validator_value.issues()});
    EXPECT_EQ(error.validation_issues().size(), issue_count);
    EXPECT_EQ(error.info().validation_issues().size(), issue_count);
    if (error.validation_issues().size() == issue_count) {
        EXPECT_EQ(error.validation_issues().front().field(), std::string_view("field_0"));
        EXPECT_EQ(error.validation_issues().back().field(), std::string_view("field_79"));
    }
}

namespace final_response_commit_test {

class invalid_final_response_routes final {
    RUVIA_ROUTES_BEGIN(invalid_final_response_routes)
    RUVIA_GET("/server-upgrade-required-without-upgrade", upgrade_required);
    RUVIA_ROUTES_END

    // 426 must name the required protocol in Upgrade (RFC 9110 15.5.22).
    ruvia::task<ruvia::http_response> upgrade_required(ruvia::context& context_value) {
        context_value.draft().status(ruvia::http_status::upgrade_required);
        co_return context_value.text("upgrade");
    }
};

}  // namespace final_response_commit_test

TEST(Ruvia, server_answers_an_uncommittable_route_response_with_a_closing_error) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::test::http_server_fixture server(io);
    asio::ip::tcp::socket client(io);
    client.connect(server.endpoint());
    std::exception_ptr failure;
    std::string response{};
    auto run = [&]() -> ruvia::task<void> {
        asio::steady_timer watchdog(io, std::chrono::seconds(5));
        watchdog.async_wait([&](std::error_code error) {
            if (!error) {
                ruvia::close_socket(client);
            }
        });
        try {
            // Keep-alive request: only the server's close verdict ends the read.
            const std::string request{"GET /server-upgrade-required-without-upgrade HTTP/1.1\r\nHost: localhost\r\n\r\n"};
            const auto sent = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(client, asio::buffer(request), std::move(handler));
            });
            if (sent.error_code()) {
                throw std::system_error(sent.error_code());
            }
            std::array<char, 1024> buffer;
            for (;;) {
                const auto received = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                    client.async_read_some(asio::buffer(buffer), std::move(handler));
                });
                if (received.error_code() == asio::error::eof) {
                    break;
                }
                if (received.error_code()) {
                    throw std::system_error(received.error_code());
                }
                response.append(buffer.data(), received.result());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        static_cast<void>(watchdog.cancel());
        ruvia::close_socket(client);
        attachment.stop();
    };
    auto done = attachment.loop().start(run());
    attachment.run();
    std::move(done).get();
    server.finish();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_TRUE(response.starts_with("HTTP/1.1 500 "));
    EXPECT_TRUE(response.find("\r\nConnection: close\r\n") != std::string::npos);
}

namespace ruvia::test {

class test_scoped_capability final {
public:
    test_scoped_capability(ruvia::operation_scope& scope, int& expired_count) noexcept :
        expired_count_(&expired_count),
        registration_(scope, this, &test_scoped_capability::expire) {}

    test_scoped_capability(const test_scoped_capability& other) noexcept :
        expired_count_(other.expired_count_),
        registration_(other.registration_, this) {}

    test_scoped_capability(test_scoped_capability&& other) noexcept :
        expired_count_(std::exchange(other.expired_count_, nullptr)),
        registration_(std::move(other.registration_), this) {}

    void use() const {
        registration_.require_active();
    }

private:
    static void expire(void* target) noexcept {
        auto& capability = *static_cast<test_scoped_capability*>(target);
        ++*capability.expired_count_;
    }

    int* expired_count_;
    ruvia::scoped_capability_registration registration_;
};

struct cold_frame_probe final {
    bool* destroyed_;

    explicit cold_frame_probe(bool& destroyed) noexcept :
        destroyed_(&destroyed) {}

    cold_frame_probe(cold_frame_probe&& other) noexcept :
        destroyed_(std::exchange(other.destroyed_, nullptr)) {}

    ~cold_frame_probe() {
        if (destroyed_ != nullptr) {
            *destroyed_ = true;
        }
    }
};

inline ruvia::task<void> cold_frame_task(cold_frame_probe probe) {
    (void)probe;
    co_return;
}

}  // namespace ruvia::test

using namespace ruvia::test;

// Scoped operations release cold frames and shared registrations exactly once.

TEST(Ruvia, scoped_operation_destruction_destroys_cold_frame) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    {
        auto operation = ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
        EXPECT_TRUE(!destroyed);
        (void)operation;
    }
    EXPECT_TRUE(destroyed);
}

TEST(Ruvia, scoped_capability_move_relinks_and_parent_close_expires_destination) {
    ruvia::operation_scope scope;
    int expired_count = 0;
    test_scoped_capability first(scope, expired_count);
    test_scoped_capability moved(std::move(first));
    moved.use();
    scope.close();
    EXPECT_EQ(expired_count, 1);
    bool rejected = false;
    try {
        moved.use();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, scoped_capability_copy_relinks_and_early_destruction_unlinks) {
    int expired_count = 0;
    ruvia::operation_scope scope;
    test_scoped_capability source(scope, expired_count);
    {
        test_scoped_capability destroyed_early(source);
        destroyed_early.use();
    }
    test_scoped_capability surviving_copy(source);
    scope.close();
    EXPECT_EQ(expired_count, 2);

    bool source_rejected = false;
    try {
        source.use();
    } catch (const std::logic_error&) {
        source_rejected = true;
    }
    EXPECT_TRUE(source_rejected);
}

TEST(Ruvia, scoped_operation_parent_close_destroys_cold_frame_immediately) {
    ruvia::operation_scope scope;
    bool destroyed = false;
    auto operation = ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
    EXPECT_TRUE(!destroyed);
    scope.close();
    EXPECT_TRUE(destroyed);
    (void)operation;
}
