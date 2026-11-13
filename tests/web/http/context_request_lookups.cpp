#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_header.h"

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

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"

#include "test_exceptions.h"
#include "worker_task_fixture.h"
#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db.h"
#endif
#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis.h"
#endif

#include <atomic>
#include <bit>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <semaphore>

#include <asio/co_spawn.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/web/http_client.h"
#include "ruvia/web/http_server.h"

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
    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        chosen = context_value.req().negotiate(ruvia::context_request::negotiable_field::encoding, supported);
        co_return;
    });
    return chosen;
}

}  // namespace

TEST(Ruvia, context_request_priority_combines_repeated_field_values_before_parsing) {
    auto request = make_request("/",
        {http_header_view{"Priority", "extra=\"alpha"}, http_header_view{"X-Other", "ignored"},
            http_header_view{"priority", "beta\", u=1, i"}});

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
    auto request = make_request("/",
        {http_header_view{"Cookie", "x=first; ignored; = unnamed; q=\"a;b\""},
            http_header_view{"X-Other", "unrelated=value"},
            http_header_view{"cOoKiE", "X=upper; x = last ; empty=;"}});
    const auto response = ruvia::test::with_context(request,
        [](ruvia::context& context) -> ruvia::task<void> {
            EXPECT_EQ(context.req().cookie("x").value_or("missing"), "last");
            EXPECT_EQ(context.req().cookie("X").value_or("missing"), "upper");
            const auto& fields = context.req().cookie_fields();
            const http_header_view expected[] = {
                {"x", "first"}, {"", "unnamed"}, {"q", "\"a"},
                {"X", "upper"}, {"x", "last"}, {"empty", ""}};
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
    auto request = make_request("/",
        {http_header_view{"Cookie", value}, http_header_view{"Cookie", "c0=latest; tail=end"}});
    const auto response = ruvia::test::with_context(request,
        [](ruvia::context& context) -> ruvia::task<void> {
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
        const auto response = ruvia::test::with_context(request,
            [](ruvia::context& context) -> ruvia::task<void> {
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
            accepted = context_value.req()
                           .negotiate(ruvia::context_request::negotiable_field::media_type, {media_type})
                           .has_value();
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
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("application/json"));

        // A first-acceptable-offer check would have taken html; negotiation
        // ranks the offers by the client's weights instead.
        EXPECT_TRUE(context_value.req()
                .negotiate(ruvia::context_request::negotiable_field::media_type, {"text/html"})
                .has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_reports_no_acceptable_representation) {
    auto request = make_request("/", {http_header_view{"Accept", "image/png"}});

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"text/html", "application/json"};
        // nullopt is the 406 signal, which is why this cannot fall back to front().
        EXPECT_TRUE(!context_value.req()
                .negotiate(ruvia::context_request::negotiable_field::media_type, supported)
                .has_value());
        co_return;
    });
}

TEST(Ruvia, context_request_negotiate_without_the_field_takes_server_preference) {
    auto request = make_request("/");

    (void)ruvia::test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const std::string_view supported[] = {"application/json", "text/html"};
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::media_type, supported);
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
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::language, supported);
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
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::encoding, supported);
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
        const auto chosen =
            context_value.req().negotiate(ruvia::context_request::negotiable_field::language, supported);
        EXPECT_TRUE(chosen.has_value());
        EXPECT_EQ(*chosen, std::string_view("ja"));
        co_return;
    });
}

TEST(Ruvia, context_request_query_decodes_names_and_values_in_wire_order) {
    auto request = make_request(
        "/?%61=one+two&%62=three%20four&%61=five+six&plain=borrowed");

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
    const auto response = ruvia::test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
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
    const auto response = ruvia::test::with_context(
        ruvia::test_request::get("/capabilities"),
        [&](ruvia::context& context_value) -> ruvia::task<void> {
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
        write_context.draft().set_signed_cookie(
            {.name_ = "session", .value_ = "user-1", .secret_ = "secret", .attributes_ = options});
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
        const auto verified =
            read_context.req().signed_cookie({.name_ = "__Host-session", .secret_ = "secret"});
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

RUVIA_MODEL(context_json_response, RUVIA_REQUIRED_FIELD(number, ruvia::int64),
    RUVIA_REQUIRED_FIELD(boolean, ruvia::bool_value), RUVIA_REQUIRED_FIELD(real, ruvia::double_value));

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
        for (const auto status : {ruvia::http_status::moved_permanently, ruvia::http_status::found,
                 ruvia::http_status::see_other, ruvia::http_status::temporary_redirect,
                 ruvia::http_status::permanent_redirect}) {
            EXPECT_EQ(context.redirect({.location_ = "/next", .status_ = status}).status(), status);
        }

        EXPECT_TRUE(throws_on(
            [&] { (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::ok}); }));
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
        const auto response =
            context.redirect({.location_ = std::string_view("https://example.com/caf\xC3\xA9"),
                .status_ = ruvia::http_status::temporary_redirect});
        EXPECT_EQ(response.status(), ruvia::http_status::temporary_redirect);
        EXPECT_EQ(response.header("Location"), std::string_view("https://example.com/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_invalid_ascii_uri_bytes) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/a b/100%off?q=\"x y\"\\z"});
        EXPECT_EQ(
            response.header("Location"), std::string_view("/a%20b/100%25off?q=%22x%20y%22%5Cz"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_existing_percent_escapes_when_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // The location mixes already-encoded escapes ("%20", "%2F") with a raw UTF-8
        // 'é' (0xC3 0xA9) that triggers the whole-string encoding pass. The 'é' must
        // become %C3%A9, but the existing escapes must survive intact -- not be
        // double-encoded to "%2520"/"%252F", which would corrupt the target.
        const auto response = context.redirect(
            {.location_ = std::string_view("https://example.com/a%20b/caf\xC3\xA9?x=%2F")});
        EXPECT_EQ(
            response.header("Location"), std::string_view("https://example.com/a%20b/caf%C3%A9?x=%2F"));

        // A lone or malformed '%' (not followed by two hex digits) is not a valid
        // escape, so it IS percent-encoded to %25 -- the trailing 'é' forces the pass.
        const auto malformed =
            context.redirect({.location_ = std::string_view("https://example.com/100%off/caf\xC3\xA9")});
        EXPECT_EQ(
            malformed.header("Location"), std::string_view("https://example.com/100%25off/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_ipv6_literal_brackets_when_encoding) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response =
            context.redirect({.location_ = std::string_view("https://[2001:db8::1]/caf\xC3\xA9")});
        EXPECT_EQ(
            response.header("Location"), std::string_view("https://[2001:db8::1]/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_square_brackets_outside_ip_literal) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/items[0]?filter=[x]#section[1]"});
        EXPECT_EQ(response.header("Location"),
            std::string_view("/items%5B0%5D?filter=%5Bx%5D#section%5B1%5D"));

        const auto absolute = context.redirect(
            {.location_ = std::string_view("https://[2001:db8::1]/items[0]?filter=[x]")});
        EXPECT_EQ(absolute.header("Location"),
            std::string_view("https://[2001:db8::1]/items%5B0%5D?filter=%5Bx%5D"));
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
            (void)context.redirect(
                {.location_ = std::string_view("https://example.com/\r\nX-Injected: y")});
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
            (void)context.redirect(
                {.location_ = std::string_view("https://example.com/caf\xC3\xA9\r\nX-Injected: y")});
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

        const auto ok = context.error(
            {.status_ = ruvia::http_status::ok, .code_ = "bad", .message_ = "not an error"});
        EXPECT_EQ(ok.status(), ruvia::http_status::internal_server_error);
        EXPECT_TRUE(!ok.header("X-Trace").has_value());

        const auto redirect = context.error({.status_ = ruvia::http_status::temporary_redirect,
            .code_ = "bad",
            .message_ = "not an error"});
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
    const auto delivered = ruvia::test::with_context(ruvia::test_request::get("/"),
        [](ruvia::context& context) -> ruvia::task<void> {
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
        const auto delivered = ruvia::test::with_context(ruvia::test_request::get("/"),
            [after](ruvia::context& context) -> ruvia::task<void> {
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
        EXPECT_EQ(response.body_bytes(),
            std::string_view(R"({"number":42,"boolean":true,"real":3.5})"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_rejects_backslash_inside_authority) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // Browsers end the host at '\'; "%5C" would send them to evil.example.
        EXPECT_TRUE(throws_on([&] { (void)context.redirect({.location_ = "http://good.example\\@evil.example/"}); }));
        EXPECT_TRUE(throws_on([&] { (void)context.redirect({.location_ = "//good.example\\@evil.example/"}); }));
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
        EXPECT_TRUE(
            response.header("Set-Cookie").value_or(std::string_view{}).starts_with("probe=ok;"));
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

        const auto response =
            ruvia::make_readiness_response(context_value, {
                                                              .state_ = ruvia::readiness_state::unavailable,
                                                              .unavailable_reason_ = "database is unavailable",
                                                          });
        EXPECT_EQ(response.status(), ruvia::http_status::service_unavailable);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
        EXPECT_EQ(response.body_bytes(),
            std::string_view("{\"status\":\"not_ready\",\"reason\":\"database is unavailable\"}"));
        co_return;
    });
}

TEST(Ruvia, readiness_response_rejects_invalid_state) {
    (void)ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context_value) -> ruvia::task<void> {
        bool rejected = false;
        try {
            (void)ruvia::make_readiness_response(
                context_value, {.state_ = static_cast<ruvia::readiness_state>(std::uint8_t{0xFF})});
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
    EXPECT_EQ(
        default_error_code(ruvia::http_status::bad_request), std::string_view("bad_request"));
    EXPECT_EQ(default_error_code(ruvia::http_status::not_found), std::string_view("not_found"));
    EXPECT_EQ(default_error_code(ruvia::http_status::method_not_allowed),
        std::string_view("method_not_allowed"));
    EXPECT_EQ(default_error_code(ruvia::http_status::content_too_large),
        std::string_view("content_too_large"));
}

TEST(Ruvia, http_error_info_round_trips) {
    const http_error error({.status_ = ruvia::http_status::unprocessable_content,
        .code_ = "unprocessable",
        .message_ = "bad fields"});
    const auto info = error.info();
    EXPECT_EQ(info.status(), ruvia::http_status::unprocessable_content);
    EXPECT_EQ(info.code(), std::string_view("unprocessable"));
    EXPECT_EQ(info.message(), std::string_view("bad fields"));
}

TEST(Ruvia, exception_text_describes_any_exception) {
    EXPECT_TRUE(ruvia::exception_text(nullptr).empty());
    EXPECT_EQ(ruvia::exception_text(std::make_exception_ptr(std::runtime_error("plain failure"))),
        std::string("plain failure"));
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

    const http_error error({.status_ = ruvia::http_status::bad_request,
        .code_ = "validation_failed",
        .message_ = "request validation failed",
        .validation_issues_ = validator_value.issues()});
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
    test_scoped_capability(ruvia::operation_scope& scope, int& expired_count) noexcept
        : expired_count_(&expired_count),
          registration_(scope, this, &test_scoped_capability::expire) {}

    test_scoped_capability(const test_scoped_capability& other) noexcept
        : expired_count_(other.expired_count_),
          registration_(other.registration_, this) {}

    test_scoped_capability(test_scoped_capability&& other) noexcept
        : expired_count_(std::exchange(other.expired_count_, nullptr)),
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

    explicit cold_frame_probe(bool& destroyed) noexcept
        : destroyed_(&destroyed) {}

    cold_frame_probe(cold_frame_probe&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

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
        auto operation =
            ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
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
    auto operation =
        ruvia::make_scoped_operation(scope, cold_frame_task(cold_frame_probe(destroyed)));
    EXPECT_TRUE(!destroyed);
    scope.close();
    EXPECT_TRUE(destroyed);
    (void)operation;
}

namespace security_header_test {

using ruvia::default_security_header_policy;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::xss_protection_header_policy;

[[nodiscard]] ruvia::dispatch_response security_response(
    const security_headers_config& options = {}, std::string_view existing_frame_options = {}) {
    auto callback = [&](ruvia::context& ctx) -> ruvia::task<void> {
        if (!existing_frame_options.empty()) {
            ctx.draft().header("X-Frame-Options", existing_frame_options);
        }
        ctx.respond(ctx.text("ok"));
        co_return;
    };
    ruvia::test_app app;
    app.use<ruvia::security_headers_middleware>(options);
    app.use<ruvia::test::context_callback<decltype(callback)>>(&callback);
    return app.request(ruvia::test_request::get("/"));
}

}  // namespace security_header_test

using ruvia::default_security_header_policy;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::xss_protection_header_policy;
using security_header_test::security_response;

TEST(Ruvia, security_headers_default_set) {
    const auto response = security_response();

    EXPECT_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
    EXPECT_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
    EXPECT_TRUE(!response.header("Strict-Transport-Security").has_value());
    // Modern guidance disables the legacy XSS auditor rather than enabling it.
    EXPECT_EQ(response.header("X-XSS-Protection"), std::string_view("0"));
    // Secure-by-default policy values ship out of the box.
    EXPECT_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    EXPECT_EQ(
        response.header("Referrer-Policy"), std::string_view("strict-origin-when-cross-origin"));
    EXPECT_EQ(response.header("Permissions-Policy"),
        std::string_view("geolocation=(), microphone=(), camera=()"));
}

TEST(Ruvia, security_headers_xss_protection_header_policy_is_explicit) {
    const security_headers_config options{
        .xss_protection_header_ = xss_protection_header_policy::omit,
    };
    const auto response = security_response(options);
    EXPECT_TRUE(!response.header("X-XSS-Protection").has_value());
}

TEST(Ruvia, security_headers_reject_invalid_xss_protection_header_policy) {
    const security_headers_config options{
        .xss_protection_header_ = std::bit_cast<xss_protection_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, security_headers_reject_invalid_default_header_policy) {
    const security_headers_config options{
        .frame_options_header_ = std::bit_cast<default_security_header_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, security_headers_empty_policy_is_not_emitted) {
    // An explicitly-cleared policy string produces no header (rather than an
    // empty-valued one).
    security_headers_config options;
    options.content_security_policy_ = "";
    options.referrer_policy_ = "";
    const auto response = security_response(options);
    EXPECT_TRUE(!response.header("Content-Security-Policy").has_value());
    EXPECT_TRUE(!response.header("Referrer-Policy").has_value());
}

TEST(Ruvia, security_headers_disabled_options_omit_headers) {
    security_headers_config options;
    options.frame_options_header_ = default_security_header_policy::omit;
    options.strict_transport_security_header_ = default_security_header_policy::omit;
    const auto response = security_response(options);

    EXPECT_TRUE(!response.header("X-Frame-Options").has_value());
    EXPECT_TRUE(!response.header("Strict-Transport-Security").has_value());
    EXPECT_EQ(response.header("X-Content-Type-Options"), std::string_view("nosniff"));
}

TEST(Ruvia, security_headers_emit_configured_policies) {
    security_headers_config options;
    options.content_security_policy_ = "default-src 'self'";
    options.referrer_policy_ = "no-referrer";
    const auto response = security_response(options);

    EXPECT_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    EXPECT_EQ(response.header("Referrer-Policy"), std::string_view("no-referrer"));
}

TEST(Ruvia, security_headers_apply_custom_headers) {
    const std::vector<ruvia::security_header> custom_value({
        {"X-Custom-Security", "value-1"},
        {"X-Report-To", "endpoint"},
    });
    security_headers_config options;
    options.custom_headers_ = custom_value;
    const auto response = security_response(options);

    EXPECT_EQ(response.header("X-Custom-Security"), std::string_view("value-1"));
    EXPECT_EQ(response.header("X-Report-To"), std::string_view("endpoint"));
    // Built-in defaults are still applied alongside custom headers.
    EXPECT_EQ(response.header("X-Frame-Options"), std::string_view("DENY"));
}

TEST(Ruvia, security_headers_reject_invalid_custom_header_at_construction) {
    bool invalid_name_rejected = false;
    try {
        static_cast<void>(ruvia::security_headers_middleware(security_headers_config{
            .custom_headers_ = {{"bad name", "value"}},
        }));
    } catch (const std::invalid_argument&) {
        invalid_name_rejected = true;
    }
    EXPECT_TRUE(invalid_name_rejected);

    bool invalid_value_rejected = false;
    try {
        static_cast<void>(ruvia::security_headers_middleware(security_headers_config{
            .custom_headers_ = {{"X-Security", "bad\r\nvalue"}},
        }));
    } catch (const std::invalid_argument&) {
        invalid_value_rejected = true;
    }
    EXPECT_TRUE(invalid_value_rejected);
}

TEST(Ruvia, security_headers_respect_existing_header_conflict_policy) {
    // The default preserves a header a handler already set -- the middleware
    // only supplies defaults.
    const auto keep = security_response({}, "SAMEORIGIN");
    EXPECT_EQ(keep.header("X-Frame-Options"), std::string_view("SAMEORIGIN"));

    const auto replace = security_response(
        security_headers_config{
            .existing_headers_ = security_header_conflict_policy::replace_existing,
        },
        "SAMEORIGIN");
    EXPECT_EQ(replace.header("X-Frame-Options"), std::string_view("DENY"));
}

TEST(Ruvia, security_headers_reject_invalid_conflict_policy) {
    const security_headers_config options{
        .existing_headers_ = std::bit_cast<security_header_conflict_policy>(std::uint8_t{0xFF}),
    };

    bool rejected = false;
    try {
        static_cast<void>(security_response(options));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

namespace csrf_test {

using ruvia::http_known_method;

bool is_lower_hex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

template <typename fn_type>
bool throws_invalid(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

struct csrf_outcome final {
    bool accepted_{false};
    bool reseeded_{false};
    std::uint16_t status_{0};
    std::string set_cookie_{};
};

[[nodiscard]] bool set_cookie_has_secure(std::string_view set_cookie) noexcept {
    return set_cookie.find("; Secure") != std::string_view::npos;
}

class csrf_test_middleware final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    explicit csrf_test_middleware(ruvia::csrf_protection* middleware_value)
        : middleware_(middleware_value) {}

    ruvia::task<void> invoke(ruvia::context& ctx, ruvia::next& next_step) {
        co_await middleware_->invoke(ctx, next_step);
    }

private:
    ruvia::csrf_protection* middleware_;
};

class accepted_response final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& ctx, ruvia::next&) {
        ctx.respond(ctx.text("accepted"));
        co_return;
    }
};

csrf_outcome run_csrf(ruvia::csrf_protection& csrf, std::string_view cookie_name,
    std::string_view header_name, http_known_method method, bool with_cookie,
    std::string_view cookie_token, bool with_header, std::string_view header_token) {
    auto request = ruvia::test_request::method(ruvia::known_http_method_token(method), "/");
    if (with_cookie) {
        request.cookie(cookie_name, cookie_token);
    }
    if (with_header) {
        request.header(header_name, header_token);
    }
    ruvia::test_app app;
    app.use<csrf_test_middleware>(&csrf);
    app.use<accepted_response>();
    const auto response = app.request(request);
    const auto cookie = response.header("Set-Cookie");
    return csrf_outcome{
        .accepted_ = response.body() == "accepted",
        .reseeded_ = cookie.has_value(),
        .status_ = response.status().value(),
        .set_cookie_ = cookie ? std::string(*cookie) : std::string{},
    };
}

csrf_outcome run_csrf(http_known_method method, bool with_cookie, std::string_view cookie_token,
    bool with_header, std::string_view header_token) {
    ruvia::csrf_protection csrf;
    return run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", method, with_cookie, cookie_token, with_header,
        header_token);
}

[[nodiscard]] std::string issued_token() {
    const auto outcome = run_csrf(http_known_method::get, false, {}, false, {});
    const auto start = outcome.set_cookie_.find('=') + 1;
    const auto end = outcome.set_cookie_.find(';', start);
    return std::string(std::string_view(outcome.set_cookie_).substr(start, end - start));
}

}  // namespace csrf_test

using csrf_test::is_lower_hex;
using csrf_test::issued_token;
using csrf_test::run_csrf;
using csrf_test::set_cookie_has_secure;
using csrf_test::throws_invalid;
using ruvia::http_known_method;

TEST(Ruvia, csrf_token_is_48_lowercase_hex_chars) {
    const auto token = issued_token();
    EXPECT_EQ(token.size(), std::size_t{48});
    for (const char c : token) {
        EXPECT_TRUE(is_lower_hex(c));
    }
}

TEST(Ruvia, csrf_custom_names_validate_at_construction) {
    EXPECT_TRUE(throws_invalid(
        [] { (void)ruvia::csrf_protection({.cookie_name_ = "", .header_name_ = "X-XSRF-TOKEN"}); }));
    EXPECT_TRUE(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "bad;name", .header_name_ = "X-XSRF-TOKEN"});
    }));
    EXPECT_TRUE(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "XSRF-TOKEN", .header_name_ = "Bad Header"});
    }));
}

TEST(Ruvia, csrf_rejects_safelisted_and_ambient_token_fields) {
    for (const std::string_view name : {"Accept", "aCcEpT-LaNgUaGe", "Content-Language",
             "CONTENT-TYPE", "Range", "Origin", "Host", "Content-Length", "Cookie",
             "uSeR-aGeNt", "Sec-Fetch-Site", "sEc-Ch-Ua", "Proxy-Authorization",
             "Access-Control-Request-Headers", "X-Forwarded-For", "Forwarded",
             "X-Real-IP", "Save-Data", "DPR", "X-Requested-With"}) {
        EXPECT_TRUE(throws_invalid([name] {
            (void)ruvia::csrf_protection({.header_name_ = std::string(name)});
        }));
    }
}

TEST(Ruvia, csrf_custom_token_fields_preserve_issue_and_validation_round_trip) {
    for (const std::string_view name : {"X-App-Csrf", "Csrf-Token", "Security-Token"}) {
        ruvia::csrf_protection csrf({.cookie_name_ = "APP-XSRF", .header_name_ = std::string(name)});
        const auto issued = run_csrf(csrf, "APP-XSRF", name, http_known_method::get, false, {}, false, {});
        EXPECT_TRUE(issued.accepted_);
        EXPECT_TRUE(issued.reseeded_);
        const auto start = issued.set_cookie_.find('=') + 1;
        const auto token = std::string_view(issued.set_cookie_).substr(start, issued.set_cookie_.find(';', start) - start);
        const auto accepted = run_csrf(csrf, "APP-XSRF", name, http_known_method::post, true, token, true, token);
        EXPECT_TRUE(accepted.accepted_);
        const auto missing = run_csrf(csrf, "APP-XSRF", name, http_known_method::post, true, token, false, {});
        EXPECT_EQ(missing.status_, std::uint16_t{403});
        const auto mismatched = run_csrf(csrf, "APP-XSRF", name, http_known_method::post, true, token, true, "different");
        EXPECT_EQ(mismatched.status_, std::uint16_t{403});
    }
}

TEST(Ruvia, csrf_middleware_owns_custom_names_after_source_destruction) {
    std::unique_ptr<ruvia::csrf_protection> csrf;
    const std::string cookie_name(80, 'c');
    const std::string header_name(80, 'h');
    {
        ruvia::csrf_protection_config config{
            .cookie_name_ = cookie_name,
            .header_name_ = header_name,
        };
        csrf = std::make_unique<ruvia::csrf_protection>(config);
    }

    const auto safe =
        run_csrf(*csrf, cookie_name, header_name, http_known_method::get, false, {}, false, {});
    EXPECT_TRUE(safe.accepted_);
    EXPECT_TRUE(safe.reseeded_);

    const auto unsafe = run_csrf(*csrf, cookie_name, header_name, http_known_method::post, true,
        "abcdef123456", true, "abcdef123456");
    EXPECT_TRUE(unsafe.accepted_);
    EXPECT_EQ(unsafe.status_, std::uint16_t{200});
}

TEST(Ruvia, csrf_token_is_unpredictable) {
    const auto first = issued_token();
    const auto second = issued_token();
    EXPECT_EQ(first.size(), std::size_t{48});
    EXPECT_EQ(second.size(), std::size_t{48});
    // 192 bits of CSPRNG entropy: a repeat is astronomically unlikely.
    EXPECT_TRUE(first != second);
}

TEST(Ruvia, csrf_unsafe_method_requires_matching_double_submit) {
    const auto ok = run_csrf(http_known_method::post, true, "abcdef123456", true, "abcdef123456");
    EXPECT_TRUE(ok.accepted_);
    EXPECT_EQ(ok.status_, std::uint16_t{200});

    const auto mismatch =
        run_csrf(http_known_method::post, true, "abcdef123456", true, "DIFFERENTtoken");
    EXPECT_TRUE(!mismatch.accepted_);
    EXPECT_EQ(mismatch.status_, std::uint16_t{403});

    const auto no_header = run_csrf(http_known_method::post, true, "abcdef123456", false, {});
    EXPECT_TRUE(!no_header.accepted_);
    EXPECT_EQ(no_header.status_, std::uint16_t{403});

    const auto no_cookie = run_csrf(http_known_method::post, false, {}, true, "abcdef123456");
    EXPECT_TRUE(!no_cookie.accepted_);
    EXPECT_EQ(no_cookie.status_, std::uint16_t{403});
}

TEST(Ruvia, csrf_safe_method_skips_validation) {
    const auto get = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    EXPECT_TRUE(get.accepted_);
    EXPECT_EQ(get.status_, std::uint16_t{200});

    const auto head = run_csrf(http_known_method::head, true, "one", true, "two");
    EXPECT_EQ(head.status_, std::uint16_t{200});
}

TEST(Ruvia, csrf_safe_method_reseeds_absent_or_empty_cookie) {
    const auto absent = run_csrf(http_known_method::get, false, {}, false, {});
    EXPECT_TRUE(absent.accepted_);
    EXPECT_TRUE(absent.reseeded_);

    const auto empty = run_csrf(http_known_method::get, true, "", false, {});
    EXPECT_TRUE(empty.accepted_);
    EXPECT_TRUE(empty.reseeded_);

    const auto present = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    EXPECT_TRUE(present.accepted_);
    EXPECT_TRUE(!present.reseeded_);
}

TEST(Ruvia, csrf_reseed_cookie_omits_secure_on_plaintext) {
    ruvia::csrf_protection csrf;
    const auto issued = run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", http_known_method::get, false, {},
        false, {});
    EXPECT_TRUE(issued.reseeded_);
    EXPECT_TRUE(!set_cookie_has_secure(issued.set_cookie_));
}

TEST(Ruvia, csrf_rejection_reaches_the_error_handler) {
    ruvia::csrf_protection csrf;
    ruvia::test_app app;
    std::string seen_code{};
    app.on_error([seen = &seen_code](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        *seen = std::string(error.code());
        c.draft().status(error.status());
        co_return c.text("handled");
    });
    app.use<csrf_test::csrf_test_middleware>(&csrf);
    app.use<csrf_test::accepted_response>();
    auto request = ruvia::test_request::method("POST", "/");
    request.cookie("XSRF-TOKEN", "abcdef123456");
    request.header("X-XSRF-TOKEN", "DIFFERENTtoken");
    const auto response = app.request(request);
    EXPECT_EQ(response.status(), ruvia::http_status::forbidden);
    EXPECT_EQ(response.body(), std::string_view("handled"));
    EXPECT_EQ(seen_code, std::string("csrf_token_mismatch"));
}

// The per-worker rate-limit table: each key's sliding-window estimate, the
// refusal headers derived from it, slot reclamation, and the overflow policy
// for keys a saturated table cannot place.
//
// Every request pins its arrival at an exact offset from one origin, so the
// window arithmetic includes sub-millisecond arrivals without reading a clock.

namespace {

class rate_limit_table_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/rate-limit-table")
    RUVIA_ROUTES_BEGIN(rate_limit_table_controller)
    RUVIA_GET("/ok", ok);
    RUVIA_GET("/route-limited", ok, ruvia::rate_limit<2, 1000>);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ok(ruvia::context& context_value) {
        co_return context_value.text("ok");
    }
};

constexpr std::array<std::string_view, 4> held_addresses{"192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4"};

constexpr std::chrono::steady_clock::time_point window_origin{std::chrono::hours(1)};

[[nodiscard]] ruvia::dispatch_response request_at(ruvia::test_app& app, std::string_view address,
    std::chrono::steady_clock::duration offset, std::string_view target = "/rate-limit-table/ok") {
    return app.request(ruvia::test_request::get(target).remote(address).arrival(window_origin + offset));
}

[[nodiscard]] ruvia::http_status_code request_from(
    ruvia::test_app& app, std::string_view address, std::chrono::steady_clock::duration offset = {}) {
    return request_at(app, address, offset).status();
}

[[nodiscard]] std::optional<std::uint64_t> header_number(
    const ruvia::dispatch_response& response, std::string_view name) {
    const auto text = response.header(name);
    std::uint64_t value = 0;
    if (!text.has_value() || text->empty()) {
        return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size()) {
        return std::nullopt;
    }
    return value;
}

void configure_saturating_limit(ruvia::test_app& app, ruvia::rate_limit_overflow_policy overflow_policy) {
    app.rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = 2, .window_ = std::chrono::minutes(10), .overflow_policy_ = overflow_policy},
        .capacity_per_worker_ = held_addresses.size()});
}

void configure_window_limit(ruvia::test_app& app, std::size_t max_requests, std::chrono::milliseconds window,
    std::size_t capacity = ruvia::default_rate_limit_capacity_per_worker) {
    app.rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = max_requests, .window_ = window}, .capacity_per_worker_ = capacity});
}

}  // namespace

TEST(Ruvia, rate_limit_saturated_table_denies_new_keys_and_keeps_counting_held_keys) {
    ruvia::test_app app;
    configure_saturating_limit(app, ruvia::rate_limit_overflow_policy::deny);
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
    }
    // Every slot holds a live entry, so a fifth address cannot be placed.
    EXPECT_TRUE(request_from(app, "192.0.2.5") == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, "192.0.2.6") == ruvia::http_status::too_many_requests);
    // Held keys are still found and counted against their own budget.
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::too_many_requests);
    }
}

TEST(Ruvia, rate_limit_saturated_table_admits_new_keys_under_allow_overflow) {
    ruvia::test_app app;
    configure_saturating_limit(app, ruvia::rate_limit_overflow_policy::allow);
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
    }
    // An unplaced key is admitted without a counter, so it is never limited.
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(request_from(app, "192.0.2.5") == ruvia::http_status::ok);
    }
    EXPECT_TRUE(request_from(app, held_addresses.front()) == ruvia::http_status::ok);
    EXPECT_TRUE(request_from(app, held_addresses.front()) == ruvia::http_status::too_many_requests);
}

TEST(Ruvia, rate_limit_budget_spent_before_a_window_boundary_does_not_refill_after_it) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 10, milliseconds(1000));
    constexpr std::string_view client = "198.51.100.1";
    // The first request opens the key's window at 0; the other nine go in its
    // last millisecond.
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    for (int i = 1; i < 10; ++i) {
        EXPECT_TRUE(request_from(app, client, milliseconds(999)) == ruvia::http_status::ok);
    }
    EXPECT_TRUE(request_from(app, client, milliseconds(999)) == ruvia::http_status::too_many_requests);

    // At the boundary a fixed window would grant ten fresh requests; here the
    // previous ten still count in full, so the estimate sits at the limit.
    const auto at_boundary = request_at(app, client, milliseconds(1000));
    EXPECT_TRUE(at_boundary.status() == ruvia::http_status::too_many_requests);
    // 10 * 999 / 1000 < 10 one millisecond later; draining 10 takes the rest
    // of this window. Both round up to one second.
    EXPECT_TRUE(header_number(at_boundary, "Retry-After") == std::optional<std::uint64_t>(1));
    EXPECT_TRUE(header_number(at_boundary, "X-RateLimit-Reset") == std::optional<std::uint64_t>(1));

    int admitted = 0;
    for (int i = 0; i < 10; ++i) {
        admitted += request_from(app, client, milliseconds(1001)) == ruvia::http_status::ok ? 1 : 0;
    }
    EXPECT_EQ(admitted, 1);

    // With one admitted, 10 * remaining / 1000 + 1 < 10 needs remaining
    // below 900 ms: exactly 100 ms after 1001, which Retry-After rounds up.
    const auto refused = request_at(app, client, milliseconds(1001));
    EXPECT_TRUE(refused.status() == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(1));
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Remaining") == std::optional<std::uint64_t>(0));
    // The current request fades out across the next window: 999 + 1000 ms.
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(2));
    EXPECT_TRUE(request_from(app, client, milliseconds(1100)) == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, client, milliseconds(1101)) == ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_admits_steady_traffic_paced_at_the_limit) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 4, milliseconds(400));
    // Gaps longer than window / limit keep the estimate below the limit at
    // every arrival, across three window boundaries.
    for (int i = 0; i < 13; ++i) {
        EXPECT_TRUE(request_from(app, "198.51.100.2", milliseconds(101 * i)) == ruvia::http_status::ok);
    }

    // Gaps of exactly window / limit put the estimate exactly at the limit
    // whenever an arrival coincides with a window start; one millisecond
    // later the previous window has faded enough.
    constexpr std::string_view aligned = "198.51.100.3";
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(request_from(app, aligned, milliseconds(100 * i)) == ruvia::http_status::ok);
    }
    EXPECT_TRUE(request_from(app, aligned, milliseconds(400)) == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, aligned, milliseconds(401)) == ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_refusal_headers_follow_the_estimate) {
    ruvia::test_app app;
    configure_window_limit(app, 2, std::chrono::minutes(10));
    constexpr std::string_view client = "198.51.100.4";
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    const auto refused = request_at(app, client, {});
    EXPECT_TRUE(refused.status() == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Limit") == std::optional<std::uint64_t>(2));
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Remaining") == std::optional<std::uint64_t>(0));
    // The spent window carries its full count into the next one, which admits
    // once its first millisecond has passed: 600'001 ms, rounded up.
    EXPECT_TRUE(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(601));
    // The two requests fade out across the next window: zero after 1'200'000 ms.
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(1200));
}

TEST(Ruvia, rate_limit_refusal_headers_do_not_saturate_for_the_longest_window) {
    ruvia::test_app app;
    configure_window_limit(app, 1, std::chrono::milliseconds::max());
    constexpr std::string_view client = "198.51.100.5";
    EXPECT_TRUE(request_from(app, client) == ruvia::http_status::ok);
    const auto refused = request_at(app, client, {});
    EXPECT_TRUE(refused.status() == ruvia::http_status::too_many_requests);
    // Retry: the whole window plus one millisecond, 2^63 ms. Reset: two whole
    // windows, 2^64 - 2 ms, beyond any signed millisecond count. Both are
    // rounded up to seconds without clamping.
    EXPECT_TRUE(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(9'223'372'036'854'776ULL));
    EXPECT_TRUE(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(18'446'744'073'709'552ULL));
}

TEST(Ruvia, rate_limit_reset_preserves_fractional_window_origin) {
    using namespace std::chrono;
    ruvia::test_app app;
    configure_window_limit(app, 1, milliseconds(1000));
    constexpr std::string_view client = "198.51.100.7";
    EXPECT_EQ(request_from(app, client, microseconds(900)), ruvia::http_status::ok);
    const auto before_roll = request_at(app, client, milliseconds(1000));
    EXPECT_EQ(before_roll.status(), ruvia::http_status::too_many_requests);
    EXPECT_EQ(header_number(before_roll, "X-RateLimit-Reset"), std::optional<std::uint64_t>(2));
    const auto at_roll = request_at(app, client, milliseconds(1000) + microseconds(900));
    EXPECT_EQ(at_roll.status(), ruvia::http_status::too_many_requests);
    EXPECT_EQ(header_number(at_roll, "X-RateLimit-Reset"), std::optional<std::uint64_t>(1));
    EXPECT_EQ(request_from(app, client, milliseconds(1001) + microseconds(900)), ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_reclamation_preserves_fractional_window_origin) {
    using namespace std::chrono;
    ruvia::test_app app;
    configure_window_limit(app, 1, milliseconds(1000), 1);
    EXPECT_EQ(request_from(app, held_addresses[0], microseconds(900)), ruvia::http_status::ok);
    EXPECT_EQ(request_from(app, held_addresses[1], milliseconds(2000)), ruvia::http_status::too_many_requests);
    EXPECT_EQ(request_from(app, held_addresses[1], milliseconds(2000) + microseconds(900)), ruvia::http_status::ok);
}

TEST(Ruvia, rate_limit_table_reclaims_a_slot_only_after_two_windows) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 2, milliseconds(1000), held_addresses.size());
    for (const auto address : held_addresses) {
        EXPECT_TRUE(request_from(app, address) == ruvia::http_status::ok);
    }
    constexpr std::string_view newcomer = "192.0.2.5";
    EXPECT_TRUE(request_from(app, newcomer) == ruvia::http_status::too_many_requests);
    // After one window each held count has only become its key's previous
    // count, which the estimate still reads.
    EXPECT_TRUE(request_from(app, newcomer, milliseconds(1999)) == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_from(app, newcomer, milliseconds(2000)) == ruvia::http_status::ok);
}

TEST(Ruvia, route_rate_limit_counts_with_the_sliding_window_estimate) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    constexpr std::string_view client = "198.51.100.6";
    constexpr std::string_view target = "/rate-limit-table/route-limited";
    EXPECT_TRUE(request_at(app, client, {}, target).status() == ruvia::http_status::ok);
    EXPECT_TRUE(request_at(app, client, {}, target).status() == ruvia::http_status::ok);
    EXPECT_TRUE(request_at(app, client, {}, target).status() == ruvia::http_status::too_many_requests);
    // rate_limit<2, 1000>: 2 * 1000 / 1000 is still the limit at the
    // boundary, 2 * 999 / 1000 is below it a millisecond later.
    EXPECT_TRUE(request_at(app, client, milliseconds(1000), target).status() == ruvia::http_status::too_many_requests);
    EXPECT_TRUE(request_at(app, client, milliseconds(1001), target).status() == ruvia::http_status::ok);
}

namespace happy_eyeballs_tests {

using namespace std::chrono_literals;
using tcp = asio::ip::tcp;

[[nodiscard]] std::optional<bool> localhost_first_is_v6() {
    asio::io_context io;
    tcp::resolver resolver(io);
    std::error_code error;
    const auto addresses = resolver.resolve("localhost", "80", error);
    if (error || addresses.empty()) {
        return std::nullopt;
    }
    bool has_v4 = false;
    bool has_v6 = false;
    for (const auto& entry : addresses) {
        has_v4 = has_v4 || entry.endpoint().address().is_v4();
        has_v6 = has_v6 || entry.endpoint().address().is_v6();
    }
    if (!has_v4 || !has_v6) {
        return std::nullopt;
    }
    return addresses.begin()->endpoint().address().is_v6();
}

[[nodiscard]] tcp::endpoint loopback_endpoint(bool v6, std::uint16_t port) {
    return v6 ? tcp::endpoint(asio::ip::address_v6::loopback(), port)
              : tcp::endpoint(asio::ip::address_v4::loopback(), port);
}

void open_acceptor(tcp::acceptor& acceptor, bool v6) {
    acceptor.open(v6 ? tcp::v6() : tcp::v4());
    if (v6) {
        acceptor.set_option(asio::ip::v6_only(true));
    }
}

// The first localhost family is bound but does not listen. The other family
// answers HTTP, so success requires the client to launch the later family
// while the first attempt remains unresolved or refused.
class opposite_family_peer final {
public:
    opposite_family_peer(asio::io_context& io, const ruvia::worker_ref& worker, bool first_is_v6)
        : io_(io),
          listening_(io),
          unavailable_(io),
          socket_(io),
          done_(worker) {
        const bool listening_is_v6 = !first_is_v6;
        std::vector<tcp::acceptor> skipped{};
        for (std::size_t attempt = 0;; ++attempt) {
            open_acceptor(listening_, listening_is_v6);
            listening_.bind(loopback_endpoint(listening_is_v6, 0));
            listening_.listen();

            open_acceptor(unavailable_, first_is_v6);
            std::error_code ec;
            unavailable_.bind(loopback_endpoint(first_is_v6, port()), ec);
            if (!ec) {
                break;
            }
            std::error_code ignored;
            unavailable_.close(ignored);
            if (ec != asio::error::address_in_use || attempt == 64) {
                throw std::system_error(ec, "bind");
            }
            skipped.push_back(std::move(listening_));
            listening_ = tcp::acceptor(io);
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return listening_.local_endpoint().port();
    }

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = std::move(failure);
            }
            complete_ = true;
            done_.notify();
        });
    }

    void stop() noexcept {
        stopping_ = true;
        std::error_code ignored;
        listening_.close(ignored);
        unavailable_.close(ignored);
        socket_.close(ignored);
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    [[nodiscard]] asio::awaitable<void> serve() {
        co_await listening_.async_accept(socket_, asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket_, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        co_await asio::async_write(socket_, asio::buffer(response), asio::use_awaitable);
    }

    asio::io_context& io_;
    tcp::acceptor listening_;
    tcp::acceptor unavailable_;
    tcp::socket socket_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool stopping_{false};
    bool complete_{false};
};

[[nodiscard]] ruvia::task<void> exercise(ruvia::event_loop_attachment& attachment,
    opposite_family_peer& peer, std::string_view host,
    std::chrono::milliseconds connect_timeout) {
    const std::string authority = host.find(':') == std::string_view::npos ? std::string(host) : "[" + std::string(host) + "]";
    const std::string base_url = std::string("http://") + authority + std::move(std::string(":").append(std::to_string(peer.port())));
    ruvia::http_client client(attachment.loop(),
        {.base_url_ = base_url,
            .connect_timeout_ = connect_timeout,
            .request_timeout_ = 3s,
            .versions_ = ruvia::http_client_versions{.http2_ = false}});
    std::exception_ptr failure;
    try {
        auto response = co_await client.client().get("/happy-eyeballs").send();
        const auto body = co_await response.stream().read_all();
        const std::string_view body_text(reinterpret_cast<const char*>(body.bytes().data()), body.size());
        EXPECT_TRUE(body_text == "ok");
    } catch (...) {
        failure = std::current_exception();
        peer.stop();
    }
    try {
        co_await client.shutdown();
        co_await peer.wait();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    attachment.stop();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace happy_eyeballs_tests

#if defined(_WIN32)
TEST(Ruvia, http_client_tries_next_address_family_when_first_is_unavailable) {
#else
TEST(Ruvia, http_client_fast_address_failure_immediately_starts_the_next_family) {
#endif
    using namespace happy_eyeballs_tests;
    const auto first_is_v6 = localhost_first_is_v6();
    if (!first_is_v6) {
        // A single-family host has no cross-family race to exercise.
        return;
    }
    auto& io = ruvia::test::new_test_io_context();
    // Looks "localhost" up on the pool the loop borrows; declared first, it
    // outlives the attachment.
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    auto loop = attachment.loop();
    auto worker = loop.worker();
    opposite_family_peer peer(io, worker, *first_is_v6);
    peer.start();
    // Windows can leave a connect to a bound, non-listening socket pending
    // instead of refusing it immediately. Allow the 250 ms stagger there;
    // the shorter deadline elsewhere still checks immediate failure handling.
#if defined(_WIN32)
    constexpr auto connect_timeout = 2s;
#else
    constexpr auto connect_timeout = 150ms;
#endif
    auto root = loop.start(exercise(attachment, peer, "localhost", connect_timeout));
    attachment.run();
    std::move(root).get();
    lookups.join();
}

TEST(Ruvia, http_client_connects_to_an_ipv6_literal) {
    using namespace happy_eyeballs_tests;
    if (!localhost_first_is_v6()) {
        return;
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    auto loop = attachment.loop();
    auto worker = loop.worker();
    opposite_family_peer peer(io, worker, false);
    peer.start();
    auto root = loop.start(exercise(attachment, peer, "::1", 3s));
    attachment.run();
    std::move(root).get();
}

namespace resolve_cancellation_tests {

using namespace std::chrono_literals;

enum class stop_kind { timeout,
    cancellation,
    client_close,
    worker_stop };

struct held_queries final {
    std::binary_semaphore release_{0};
    std::atomic<unsigned> finished_{0};
    std::atomic<bool> released_{false};
    void release() {
        if (!released_.exchange(true)) {
            release_.release();
        }
    }
};

ruvia::task<void> hold_executor(ruvia::worker_ref worker, std::shared_ptr<held_queries> held) {
    auto result = co_await ruvia::run_blocking(std::move(worker), {}, [held] {
        held->release_.acquire();
        ++held->finished_;
    });
    if (result.status() != ruvia::blocking_status::worker_stopping) {
        std::move(result).take_value();
    }
}

ruvia::task<void> send_query(ruvia::http_client& client, ruvia::stop_token stop,
    std::optional<std::error_code>& code) {
    try {
        auto response = co_await client.client().with_options({.stop_token_ = std::move(stop)}).get("/resolve").send();
        (void)co_await response.stream().read_all();
    } catch (const ruvia::http_client_error& error) {
        code = error.code();
    }
}

struct observation final {
    std::optional<std::error_code> code_;
    bool retired_before_release_{false};
};

ruvia::task<void> exercise(ruvia::event_loop loop, const ruvia::blocking_pool& lookups, stop_kind kind,
    std::shared_ptr<held_queries> held, observation& observed, std::promise<void>& ready,
    std::promise<void>& retired) {
    auto worker = loop.worker();
    ruvia::task_scope occupied(worker);
    // 事件循环借用的单线程池被一个未释放的任务占住，名称查询确定性地排队等待，
    // 不依赖外部 DNS 服务器、网络超时或对系统解析器的进程级修改。
    occupied.spawn(hold_executor(worker, held));
    while (lookups.stats().running_ != 1) {
        (void)co_await ruvia::sleep_for(worker, 1ms);
    }
    ruvia::http_client client(loop, {.base_url_ = "http://localhost:1", .connect_timeout_ = kind == stop_kind::timeout ? 30ms : 3s, .request_timeout_ = 3s, .versions_ = ruvia::http_client_versions{.http2_ = false}});
    ruvia::stop_source stop;
    ruvia::task_scope operations(worker);
    std::exception_ptr failure;
    try {
        operations.spawn(send_query(client, stop.token(), observed.code_));
        if (kind != stop_kind::timeout) {
            (void)co_await ruvia::sleep_for(worker, 10ms);
            if (kind == stop_kind::cancellation) {
                stop.request_stop();
            } else if (kind == stop_kind::client_close) {
                client.close();
            } else {
                ready.set_value();
            }
        }
        co_await operations.join();
    } catch (...) {
        failure = std::current_exception();
    }
    co_await client.shutdown();
    observed.retired_before_release_ = held->finished_ == 0;
    retired.set_value();
    if (kind != stop_kind::worker_stop) {
        held->release();
    }
    co_await occupied.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

[[nodiscard]] ruvia::http_client_config dns_http_config() {
    return {.base_url_ = "http://localhost:1"};
}

}  // namespace resolve_cancellation_tests

TEST(Ruvia, clients_need_a_loop_with_a_blocking_pool_to_look_up_a_dns_host) {
    using namespace resolve_cancellation_tests;
    bool http_rejected = false;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        try {
            ruvia::http_client client(loops.loop(0), dns_http_config());
        } catch (const std::invalid_argument&) {
            http_rejected = true;
        }
        loops.join();
    }
    EXPECT_TRUE(http_rejected);

    // The unified HTTP/WebSocket origin is accepted once the loop borrows a pool.
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &lookups});
    {
        ruvia::http_client http(loops.loop(0), dns_http_config());
    }
    loops.join();
    lookups.join();
}

TEST(Ruvia, host_lookup_timeout_cancel_and_shutdown_retire_before_system_work_finishes) {
    using namespace resolve_cancellation_tests;
    for (const auto kind : {stop_kind::timeout, stop_kind::cancellation, stop_kind::client_close, stop_kind::worker_stop}) {
        auto held = std::make_shared<held_queries>();
        observation observed;
        std::promise<void> ready;
        auto ready_result = ready.get_future();
        std::promise<void> retired;
        auto retired_result = retired.get_future();
        // 循环借用查询池，池须声明在循环之前并比它活得久。
        ruvia::blocking_pool lookups({.thread_count_ = 1});
        ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 64, .blocking_pool_ = &lookups});
        auto root = pool.loop(0).start(exercise(pool.loop(0), lookups, kind, held, observed, ready, retired));
        pool.start();
        if (kind == stop_kind::worker_stop) {
            const bool started = ready_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(started);
            pool.stop();
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(ended);
            if (!started || !ended) {
                held->release();
            }
            pool.join();
            held->release();
        } else {
            const bool ended = retired_result.wait_for(2s) == std::future_status::ready;
            EXPECT_TRUE(ended);
            if (!ended) {
                held->release();
            }
            root.wait();
            pool.join();
        }
        lookups.join();
        std::move(root).get();
        EXPECT_TRUE(observed.retired_before_release_);
        const auto expected = kind == stop_kind::timeout        ? std::make_error_code(std::errc::timed_out)
                              : kind == stop_kind::cancellation ? std::make_error_code(std::errc::operation_canceled)
                                                                : ruvia::make_error_code(ruvia::http_client_errc::closing);
        EXPECT_TRUE(observed.code_ == expected);
    }
}

namespace {

ruvia::task<std::string> read_to_end(asio::ip::tcp::socket& stream) {
    std::array<char, 4096> bytes_value{};
    std::string received{};
    for (;;) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            stream.async_read_some(asio::buffer(bytes_value), std::move(handler));
        });
        if (read.error_code() == asio::error::eof) {
            co_return received;
        }
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        received.append(bytes_value.data(), read.result());
    }
}

ruvia::task<void> serve_client_connect(asio::ip::tcp::socket& stream, unsigned mode, std::string& received_value) {
    if (mode == 3) {
        received_value = co_await read_to_end(stream);
        co_return;
    }
    std::string head{};
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) { stream.async_read_some(asio::buffer(&byte, 1), std::move(h)); });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        head.push_back(byte);
    }
    ruvia::http1_server_request_parse_state parsed;
    parsed.parse_head(head);
    if (!parsed.head_ready() || parsed.request().method() != "CONNECT" || parsed.request().target() != "target.test:443") {
        throw std::runtime_error("invalid CONNECT authority-form");
    }
    if (parsed.request().header("content-length") || parsed.request().header("transfer-encoding")) {
        throw std::runtime_error("CONNECT head has HTTP content framing");
    }
    const auto send = [&](std::string_view bytes_value, bool end = false) -> ruvia::task<void> {
        const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(stream, asio::buffer(bytes_value), std::move(handler));
        });
        if (written.error_code()) {
            throw std::system_error(written.error_code());
        }
        if (end) {
            std::error_code error;
            stream.shutdown(asio::ip::tcp::socket::shutdown_send, error);
            if (error) {
                throw std::system_error(error);
            }
        }
    };
    if (mode == 2) {
        co_await send("HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\nConnection: close\r\nProxy-Authenticate: Basic realm=proxy\r\n\r\ndenied", true);
        co_return;
    }
    co_await send("HTTP/1.1 200 Connection Established\r\nX-Tunnel: owned-metadata\r\n\r\n");
    if (mode == 1) {
        co_await send(std::string(100003, 's'), true);
    }
    received_value = co_await read_to_end(stream);
    if (mode == 0) {
        co_await send(std::string(100003, 's'), true);
    }
}

void exercise_client_connect(unsigned mode) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    std::string received{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::task_scope tasks(worker_value);
        const auto serve = [&]() -> ruvia::task<void> {
            auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
            if (accepted.error_code()) {
                throw std::system_error(accepted.error_code());
            }
            auto socket = std::move(accepted.result());
            co_await serve_client_connect(socket, mode, received);
        };
        tasks.spawn(serve());
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .request_timeout_ = std::chrono::seconds(5), .max_response_bytes_ = 16384, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            if (mode == 3) {
                bool unavailable = false;
                try {
                    auto result_value = co_await client.client().connect("target.test:443").protocol("test-datagram").target("/datagrams").open();
                } catch (const ruvia::http_client_error& error) {
                    unavailable = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_unavailable);
                }
                EXPECT_TRUE(unavailable);
            } else {
                auto result_value = co_await client.client().connect("target.test:443").open();
                if (mode == 2) {
                    EXPECT_TRUE(!result_value.tunnel() && result_value.response());
                    if (!result_value.response()) {
                        throw std::runtime_error("missing CONNECT rejection");
                    }
                    EXPECT_TRUE(result_value.response()->status().value() == 407);
                    std::string text{};
                    while (auto bytes = co_await result_value.response()->stream().text()) {
                        text.append(*bytes);
                    }
                    EXPECT_TRUE(text == "denied");
                } else {
                    EXPECT_TRUE(result_value.tunnel() && !result_value.response());
                    if (!result_value.tunnel()) {
                        throw std::runtime_error("CONNECT rejected");
                    }
                    auto tunnel = std::move(*result_value.tunnel());
                    EXPECT_TRUE(tunnel.header("x-tunnel") == "owned-metadata");
                    const auto read_greeting = [&]() -> ruvia::task<void> {
                        std::string greeting{};
                        while (auto bytes = co_await tunnel.read()) {
                            greeting.append(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                        }
                        EXPECT_TRUE(greeting == std::string(100003, 's'));
                    };
                    if (mode == 1) {
                        co_await read_greeting();
                    }
                    for (unsigned part = 0; part != 8; ++part) {
                        std::string payload_value(16384, 't');
                        auto write = tunnel.write(std::string_view(payload_value));
                        payload_value.assign("mutated");
                        co_await std::move(write);
                    }
                    co_await tunnel.finish();
                    co_await tunnel.finish();
                    if (mode == 0) {
                        co_await read_greeting();
                    }
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        try {
            co_await tasks.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (mode == 3) {
        EXPECT_TRUE(received.empty());
    } else if (mode != 2) {
        EXPECT_TRUE(received == std::string(8 * 16384, 't'));
    }
}

}  // namespace

TEST(Ruvia, http1_client_tunnel_and_rejections_preserve_ownership_and_tcp_half_close) {
    for (unsigned mode = 0; mode != 4; ++mode) {
        exercise_client_connect(mode);
    }
}

namespace {

ruvia::task<std::string> read_head(asio::ip::tcp::socket& stream) {
    std::string head{};
    char byte{};
    while (!head.ends_with("\r\n\r\n")) {
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) { stream.async_read_some(asio::buffer(&byte, 1), std::move(h)); });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        head.push_back(byte);
    }
    co_return head;
}

ruvia::task<void> write_all(asio::ip::tcp::socket& stream, std::string_view bytes_value) {
    const auto written = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_write(stream, asio::buffer(bytes_value), std::move(handler));
    });
    if (written.error_code()) {
        throw std::system_error(written.error_code());
    }
}

}  // namespace

// A proxy that rejects CONNECT without Connection: close may follow the
// rejection with bytes of unknown ownership. The client delivers the rejection
// but never reuses that connection: the next request opens a new one.
TEST(Ruvia, http1_client_rejected_keep_alive_connect_retires_the_proxy_connection) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    unsigned accepted = 0;
    std::string after_rejection{};
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        const auto& worker_value = attachment.loop().worker();
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
        ruvia::task_scope tasks(worker_value);
        const auto serve = [&]() -> ruvia::task<void> {
            for (;;) {
                auto accepted_socket = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) { acceptor.async_accept(std::move(h)); });
                if (accepted_socket.error_code() == asio::error::operation_aborted) {
                    co_return;
                }
                if (accepted_socket.error_code()) {
                    throw std::system_error(accepted_socket.error_code());
                }
                auto socket = std::move(accepted_socket.result());
                ++accepted;
                const auto head = co_await read_head(socket);
                ruvia::http1_server_request_parse_state parsed;
                parsed.parse_head(head);
                if (accepted == 1) {
                    if (!parsed.head_ready() || parsed.request().method() != "CONNECT" ||
                        parsed.request().target() != "target.test:443") {
                        throw std::runtime_error("expected CONNECT on the first connection");
                    }
                    co_await write_all(socket, "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\nProxy-Authenticate: Basic realm=proxy\r\n\r\ndenied");
                    // Keep-alive: the client, not the proxy, must end this connection.
                    after_rejection = co_await read_to_end(socket);
                    continue;
                }
                if (!parsed.head_ready() || parsed.request().method() != "GET" || parsed.request().target() != "/after") {
                    throw std::runtime_error("expected the next request on a new connection");
                }
                co_await write_all(socket, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
            }
        };
        tasks.spawn(serve());
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .request_timeout_ = std::chrono::seconds(5)});
        try {
            {
                auto result_value = co_await client.client().connect("target.test:443").open();
                EXPECT_TRUE(!result_value.tunnel() && result_value.response());
                if (!result_value.response()) {
                    throw std::runtime_error("missing CONNECT rejection");
                }
                EXPECT_TRUE(result_value.response()->status().value() == 407);
                std::string text{};
                while (auto bytes = co_await result_value.response()->stream().text()) {
                    text.append(*bytes);
                }
                EXPECT_TRUE(text == "denied");
            }
            auto response = co_await client.client().get("/after").send();
            EXPECT_TRUE(response.status().value() == 200);
            std::string text{};
            while (auto bytes = co_await response.stream().text()) {
                text.append(*bytes);
            }
            EXPECT_TRUE(text == "ok");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        try {
            co_await tasks.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    EXPECT_EQ(accepted, 2U);
    EXPECT_TRUE(after_rejection.empty());
}

namespace static_files_test {

namespace fs = std::filesystem;

// A throwaway document root mixing a normal asset with hidden files and a
// hidden directory (the .git checkout an operator might accidentally deploy).
void populate_dotfile_root(const fs::path& dir) {
    fs::create_directories(dir / ".git");
    const auto write = [](const fs::path& path, std::string_view body) {
        std::ofstream file(path);
        file << body;
    };
    write(dir / "app.js", "console.log(1)");
    write(dir / ".env", "SECRET=1");
    write(dir / ".htpasswd", "user:hash");
    write(dir / ".backup.json", "{}");
    write(dir / ".git" / "config", "[core]");
}

[[nodiscard]] bool served(const ruvia::static_root& root, std::string_view path) {
    bool found = false;
    static_cast<void>(ruvia::test::with_context(
        ruvia::test_request::get("/"),
        [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                found =
                    (co_await ctx.static_file(root, {.relative_path_ = path})).status() == ruvia::http_status::ok;
            } catch (const ruvia::http_error&) {
                found = false;
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    return found;
}

struct served_body final {
    ruvia::http_status_code status_{ruvia::http_status::internal_server_error};
    bool file_backed_{false};
    std::string bytes_{};
};

// Serves `path` and reports whether the response names a file for the
// transport or carries its bytes in memory.
[[nodiscard]] served_body serve_body(const ruvia::static_root& root, std::string_view path) {
    served_body result;
    static_cast<void>(ruvia::test::with_context(
        ruvia::test_request::get("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                const auto response = co_await ctx.static_file(root, {.relative_path_ = path});
                result.status_ = response.status();
                result.file_backed_ = response.file_body().has_value();
                result.bytes_.assign(response.body_bytes());
            } catch (const ruvia::http_error& error) {
                result.status_ = error.info().status();
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    return result;
}

void populate_resident_root(const fs::path& dir) {
    fs::create_directories(dir / "nested");
    std::ofstream(dir / "small.txt", std::ios::binary) << "small-v1";
    std::ofstream(dir / "nested" / "page.txt", std::ios::binary) << "nested-v1";
    std::ofstream(dir / "large.txt", std::ios::binary) << std::string(64, 'L');
}

}  // namespace static_files_test

using static_files_test::populate_dotfile_root;
using static_files_test::populate_resident_root;
using static_files_test::serve_body;
using static_files_test::served;
namespace fs = std::filesystem;

TEST(Ruvia, static_root_hides_dotfiles_even_under_all_policy) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_dotfiles");
    const auto& dir = directory.path();
    populate_dotfile_root(dir);
    ruvia::static_root_options options;
    // The hidden-path default-deny must still keep secrets out of responses.
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    ruvia::static_root root(dir, std::move(options));

    EXPECT_TRUE(served(root, "app.js"));
    EXPECT_TRUE(!served(root, ".env"));
    EXPECT_TRUE(!served(root, ".htpasswd"));
    EXPECT_TRUE(!served(root, ".backup.json"));
    EXPECT_TRUE(!served(root, ".git/config"));
}

TEST(Ruvia, static_root_serves_dotfiles_when_opted_in) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_dotfiles");
    const auto& dir = directory.path();
    populate_dotfile_root(dir);
    ruvia::static_root_options options;
    options.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    options.dotfiles_ = ruvia::static_dotfile_policy::serve;
    ruvia::static_root root(dir, std::move(options));

    EXPECT_TRUE(served(root, ".env"));
    EXPECT_TRUE(served(root, ".git/config"));
    EXPECT_TRUE(served(root, "app.js"));
}

TEST(Ruvia, static_root_without_resident_opt_in_stays_file_backed_and_fails_closed) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_resident");
    const auto& dir = directory.path();
    populate_resident_root(dir);
    ruvia::static_root root(dir, {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}});

    const auto before = serve_body(root, "nested/page.txt");
    EXPECT_EQ(before.status_, ruvia::http_status::ok);
    EXPECT_TRUE(before.file_backed_);

    std::ofstream(dir / "nested" / "page.txt", std::ios::binary | std::ios::trunc) << "nested-v2-longer";
    // A HEAD has no body for the transport to check, so the identity check
    // runs while framing.
    bool head_failed = false;
    static_cast<void>(ruvia::test::with_context(
        ruvia::test_request::head("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
            try {
                static_cast<void>(co_await ctx.static_file(root, {.relative_path_ = "nested/page.txt"}));
            } catch (const ruvia::http_error& error) {
                head_failed = error.info().status() == ruvia::http_status::internal_server_error;
            }
            ctx.respond(ctx.text("ok"));
            co_return;
        }));
    EXPECT_TRUE(head_failed);
}

TEST(Ruvia, static_root_resident_opt_in_serves_indexed_bytes_from_memory) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_resident");
    const auto& dir = directory.path();
    populate_resident_root(dir);
    ruvia::static_root root(dir,
        {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 32,
            .resident_budget_bytes_ = 1024});

    // Rewrite and delete after indexing: resident files keep serving the
    // indexed bytes; the file over resident_max_bytes_ stays on disk.
    std::ofstream(dir / "small.txt", std::ios::binary | std::ios::trunc) << "small-v2-longer";
    fs::remove(dir / "nested" / "page.txt");

    const auto small = serve_body(root, "small.txt");
    EXPECT_EQ(small.status_, ruvia::http_status::ok);
    EXPECT_TRUE(!small.file_backed_);
    EXPECT_EQ(small.bytes_, std::string("small-v1"));

    const auto nested = serve_body(root, "nested/page.txt");
    EXPECT_EQ(nested.status_, ruvia::http_status::ok);
    EXPECT_TRUE(!nested.file_backed_);
    EXPECT_EQ(nested.bytes_, std::string("nested-v1"));

    const auto large = serve_body(root, "large.txt");
    EXPECT_EQ(large.status_, ruvia::http_status::ok);
    EXPECT_TRUE(large.file_backed_);
}

TEST(Ruvia, static_root_resident_budget_bounds_the_kept_bytes) {
    const ruvia::test::scoped_temp_directory directory("ruvia_static_resident");
    const auto& dir = directory.path();
    populate_resident_root(dir);
    // Index order is large.txt, nested/page.txt, small.txt; only the first
    // eligible files that fit the budget stay resident.
    ruvia::static_root root(dir,
        {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 32,
            .resident_budget_bytes_ = 9});
    EXPECT_TRUE(!serve_body(root, "nested/page.txt").file_backed_);
    EXPECT_TRUE(serve_body(root, "small.txt").file_backed_);
}
