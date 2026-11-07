#include <gtest/gtest.h>

#include <cstdio>
#include <exception>
#include <iterator>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_response.h"
#include "ruvia/web/model.h"

#include "context_request_fixture.h"
#include "test_exceptions.h"

RUVIA_MODEL(context_json_response, RUVIA_REQUIRED_FIELD(number, ruvia::int64),
    RUVIA_REQUIRED_FIELD(boolean, ruvia::bool_value), RUVIA_REQUIRED_FIELD(real, ruvia::double_value));

using ruvia::testing::throws_on;

TEST(Ruvia, context_redirect_sets_verbatim_ascii_location_and_status) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "https://example.com/path?q=1"});
        EXPECT_EQ(response.status(), ruvia::http_status::found);
        EXPECT_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_accepts_only_redirect_statuses) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/a b/100%off?q=\"x y\"\\z"});
        EXPECT_EQ(
            response.header("Location"), std::string_view("/a%20b/100%25off?q=%22x%20y%22%5Cz"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_preserves_existing_percent_escapes_when_encoding) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response =
            context.redirect({.location_ = std::string_view("https://[2001:db8::1]/caf\xC3\xA9")});
        EXPECT_EQ(
            response.header("Location"), std::string_view("https://[2001:db8::1]/caf%C3%A9"));
        co_return;
    });
}

TEST(Ruvia, context_redirect_percent_encodes_square_brackets_outside_ip_literal) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        EXPECT_TRUE(throws_on([&] {
            (void)context.redirect(
                {.location_ = std::string_view("https://example.com/caf\xC3\xA9\r\nX-Injected: y")});
        }));
        co_return;
    });
}

TEST(Ruvia, context_body_sets_body_and_status) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::created);
        const auto response = context.body("hello world");
        EXPECT_EQ(response.status(), ruvia::http_status::created);
        EXPECT_EQ(response.body_bytes(), std::string_view("hello world"));
        co_return;
    });
}

TEST(Ruvia, context_dynamic_body_owns_input_and_preserves_lvalue) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        std::string source_value("dynamic body");

        const auto response = context.body(std::string_view(source_value));
        source_value[0] = 'X';

        EXPECT_EQ(response.body_bytes(), std::string_view("dynamic body"));
        EXPECT_EQ(source_value, std::string_view("Xynamic body"));
        co_return;
    });
}

TEST(Ruvia, context_static_text_builders_borrow_static_storage) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
        (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto borrowed = context.text("plain");
        EXPECT_EQ(borrowed.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_EQ(borrowed.header("X-Content-Type-Options"), std::string_view("nosniff"));
        const auto owned = context.text(std::pmr::string("plain", context.arena()));
        EXPECT_EQ(owned.header("X-Content-Type-Options"), std::string_view("nosniff"));
        context.draft().header("X-Content-Type-Options", "application-value");
        const auto configured = context.text("plain");
        EXPECT_EQ(configured.header("X-Content-Type-Options"), std::string_view("application-value"));
        co_return;
    });
}

TEST(Ruvia, context_response_header_transactions_preserve_owned_body_storage) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().header("Set-Cookie", "session=new; Path=/");
        context.draft().header("Cache-Control", "no-store");
        ruvia::http_response response;
        response.header("Set-Cookie", "session=old; Path=/");
        response.header("Set-Cookie", "other=kept; Path=/", {.mode_ = ruvia::http_response_header_mode::append});
        response.header("Content-Type", "application/octet-stream");
        response.header("Cache-Control", "private");
        for (int i = 0; i < 12; ++i) {
            response.header("X-Field-" + std::to_string(i), "value");
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
            EXPECT_EQ(result_value.header("X-Field-" + std::to_string(i)), std::string_view("value"));
        }
        co_return;
    });
}

TEST(Ruvia, context_draft_is_consumed_and_final_edits_do_not_cross_replacement) {
    const auto delivered = context_request_test::with_context(ruvia::test_request::get("/"),
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
        const auto delivered = context_request_test::with_context(ruvia::test_request::get("/"),
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.draft().status(ruvia::http_status::no_content);
        const auto response = context.body();
        EXPECT_EQ(response.status(), ruvia::http_status::no_content);
        EXPECT_TRUE(response.body_bytes().empty());
        co_return;
    });
}

TEST(Ruvia, context_body_byte_span_copies_into_response_storage) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.text("hello");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        EXPECT_EQ(response.body_bytes(), std::string_view("hello"));
        co_return;
    });
}

TEST(Ruvia, context_html_sets_html_content_type) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.html("<h1>hi</h1>");
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
        EXPECT_EQ(response.body_bytes(), std::string_view("<h1>hi</h1>"));
        co_return;
    });
}

TEST(Ruvia, context_json_serializes_response_model_with_json_content_type) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context_json_response model({.resource_ = context.arena()});
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
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
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
