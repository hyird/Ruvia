#include <exception>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_response.h"
#include "ruvia/web/model.h"

#include "context_request_fixture.h"

RUVIA_MODEL(context_json_response, RUVIA_REQUIRED_FIELD(number, ruvia::int64),
    RUVIA_REQUIRED_FIELD(boolean, ruvia::bool_value), RUVIA_REQUIRED_FIELD(real, ruvia::double_value));

using ruvia::testing::throws_on;

RUVIA_TEST(context_redirect_sets_verbatim_ascii_location_and_status) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "https://example.com/path?q=1"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::found);
        RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
        co_return;
    });
}

RUVIA_TEST(context_redirect_accepts_only_redirect_statuses) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        for (const auto status : {ruvia::http_status::moved_permanently, ruvia::http_status::found,
                 ruvia::http_status::see_other, ruvia::http_status::temporary_redirect,
                 ruvia::http_status::permanent_redirect}) {
            RUVIA_CHECK_EQ(context.redirect({.location_ = "/next", .status_ = status}).status(), status);
        }

        RUVIA_CHECK(throws_on(
            [&] { (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::ok}); }));
        RUVIA_CHECK(throws_on([&] {
            (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_modified});
        }));
        RUVIA_CHECK(throws_on([&] {
            (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_found});
        }));
        co_return;
    });
}

RUVIA_TEST(context_redirect_percent_encodes_non_ascii_location) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // A UTF-8 'é' (0xC3 0xA9) is percent-encoded while the URI structure
        // (scheme, host, path separators) is preserved.
        const auto response =
            context.redirect({.location_ = std::string_view("https://example.com/caf\xC3\xA9"),
                .status_ = ruvia::http_status::temporary_redirect});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::temporary_redirect);
        RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/caf%C3%A9"));
        co_return;
    });
}

RUVIA_TEST(context_redirect_percent_encodes_invalid_ascii_uri_bytes) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/a b/100%off?q=\"x y\"\\z"});
        RUVIA_CHECK_EQ(
            response.header("Location"), std::string_view("/a%20b/100%25off?q=%22x%20y%22%5Cz"));
        co_return;
    });
}

RUVIA_TEST(context_redirect_preserves_existing_percent_escapes_when_encoding) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        // The location mixes already-encoded escapes ("%20", "%2F") with a raw UTF-8
        // 'é' (0xC3 0xA9) that triggers the whole-string encoding pass. The 'é' must
        // become %C3%A9, but the existing escapes must survive intact -- not be
        // double-encoded to "%2520"/"%252F", which would corrupt the target.
        const auto response = context.redirect(
            {.location_ = std::string_view("https://example.com/a%20b/caf\xC3\xA9?x=%2F")});
        RUVIA_CHECK_EQ(
            response.header("Location"), std::string_view("https://example.com/a%20b/caf%C3%A9?x=%2F"));

        // A lone or malformed '%' (not followed by two hex digits) is not a valid
        // escape, so it IS percent-encoded to %25 -- the trailing 'é' forces the pass.
        const auto malformed =
            context.redirect({.location_ = std::string_view("https://example.com/100%off/caf\xC3\xA9")});
        RUVIA_CHECK_EQ(
            malformed.header("Location"), std::string_view("https://example.com/100%25off/caf%C3%A9"));
        co_return;
    });
}

RUVIA_TEST(context_redirect_preserves_ipv6_literal_brackets_when_encoding) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response =
            context.redirect({.location_ = std::string_view("https://[2001:db8::1]/caf\xC3\xA9")});
        RUVIA_CHECK_EQ(
            response.header("Location"), std::string_view("https://[2001:db8::1]/caf%C3%A9"));
        co_return;
    });
}

RUVIA_TEST(context_redirect_percent_encodes_square_brackets_outside_ip_literal) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.redirect({.location_ = "/items[0]?filter=[x]#section[1]"});
        RUVIA_CHECK_EQ(response.header("Location"),
            std::string_view("/items%5B0%5D?filter=%5Bx%5D#section%5B1%5D"));

        const auto absolute = context.redirect(
            {.location_ = std::string_view("https://[2001:db8::1]/items[0]?filter=[x]")});
        RUVIA_CHECK_EQ(absolute.header("Location"),
            std::string_view("https://[2001:db8::1]/items%5B0%5D?filter=%5Bx%5D"));
        co_return;
    });
}

RUVIA_TEST(context_redirect_preserves_fragment_separator_and_escapes_fragment_hashes) {
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
            RUVIA_CHECK_EQ(response.header("Location").value_or(""), expected);
        }
        co_return;
    });
}

RUVIA_TEST(context_redirect_rejects_crlf_header_injection) {
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
        RUVIA_CHECK(threw);
        co_return;
    });
}

RUVIA_TEST(context_redirect_rejects_crlf_even_when_location_needs_encoding) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        RUVIA_CHECK(throws_on([&] {
            (void)context.redirect(
                {.location_ = std::string_view("https://example.com/caf\xC3\xA9\r\nX-Injected: y")});
        }));
        co_return;
    });
}

RUVIA_TEST(context_body_sets_body_and_status) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.status(ruvia::http_status::created);
        const auto response = context.body("hello world");
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::created);
        RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("hello world"));
        co_return;
    });
}

RUVIA_TEST(context_dynamic_body_owns_input_and_preserves_lvalue) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        std::string source_value("dynamic body");

        const auto response = context.body(std::string_view(source_value));
        source_value[0] = 'X';

        RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("dynamic body"));
        RUVIA_CHECK_EQ(source_value, std::string_view("Xynamic body"));
        co_return;
    });
}

RUVIA_TEST(context_literal_builders_keep_static_storage) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto body_response = context.body("body");
        const auto text_response = context.text("text");
        const auto html_response = context.html("<b>html</b>");

        RUVIA_CHECK(body_response.body_bytes() == std::string_view("body"));
        RUVIA_CHECK(text_response.body_bytes() == std::string_view("text"));
        RUVIA_CHECK(html_response.body_bytes() == std::string_view("<b>html</b>"));
        co_return;
    });
}

RUVIA_TEST(context_rejects_informational_and_non_http_final_statuses) {
    {
        (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
            bool threw = false;
            try {
                context.status(ruvia::http_status::early_hints);
            } catch (const std::invalid_argument&) {
                threw = true;
            }
            RUVIA_CHECK(threw);
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
        RUVIA_CHECK(threw);
    }
}

RUVIA_TEST(context_error_normalizes_non_error_status_before_response_state) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.header("X-Trace", "1");

        const auto ok = context.error(
            {.status_ = ruvia::http_status::ok, .code_ = "bad", .message_ = "not an error"});
        RUVIA_CHECK_EQ(ok.status(), ruvia::http_status::internal_server_error);
        RUVIA_CHECK_EQ(ok.header("X-Trace"), std::string_view("1"));

        const auto redirect = context.error({.status_ = ruvia::http_status::temporary_redirect,
            .code_ = "bad",
            .message_ = "not an error"});
        RUVIA_CHECK_EQ(redirect.status(), ruvia::http_status::internal_server_error);
        co_return;
    });
}

RUVIA_TEST(context_response_metadata_uses_http_response_validation) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        bool threw = false;
        try {
            context.header("Connection", "close,");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        const auto response = context.body("unchanged");
        RUVIA_CHECK(!response.header("Connection").has_value());
        co_return;
    });
}

RUVIA_TEST(context_body_applies_context_headers) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.status(ruvia::http_status::ok);
        context.header("Content-Type", "text/plain");
        context.header("X-Custom", "v");
        const auto response = context.body("data");
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("data"));
        RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));
        RUVIA_CHECK_EQ(response.header("X-Custom"), std::string_view("v"));
        co_return;
    });
}

RUVIA_TEST(context_response_header_transactions_preserve_owned_body_storage) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.header("X-Added", "context");
        context.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        context.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        ruvia::http_response response;
        response.header("Content-Type", "application/octet-stream");
        response.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
        response.body(std::string(4096, 'x'));
        context.respond(std::move(response));
        RUVIA_CHECK_EQ(context.response()->body_bytes().size(), std::size_t{4096});
        RUVIA_CHECK_EQ(context.response()->header("X-Added"), std::string_view("context"));
        RUVIA_CHECK_EQ(context.response()->headers().size(), std::size_t{4});
        co_return;
    });
}

RUVIA_TEST(context_response_header_transactions_preserve_cookie_policy_and_spilled_headers) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.header("Set-Cookie", "session=new; Path=/");
        context.header("Cache-Control", "no-store");
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
        RUVIA_CHECK_EQ(result_value.header("Content-Type"), std::string_view("application/octet-stream"));
        RUVIA_CHECK_EQ(result_value.header("Cache-Control"), std::string_view("no-store"));
        RUVIA_CHECK_EQ(result_value.header("Set-Cookie"), std::string_view("session=new; Path=/"));
        std::size_t cookies = 0;
        for (const auto& header : result_value.headers()) {
            if (header.name() == "Set-Cookie") {
                ++cookies;
            }
        }
        RUVIA_CHECK_EQ(cookies, std::size_t{1});
        for (int i = 0; i < 12; ++i) {
            RUVIA_CHECK_EQ(result_value.header("X-Field-" + std::to_string(i)), std::string_view("value"));
        }
        co_return;
    });
}

RUVIA_TEST(context_metadata_preserves_repeated_set_cookie_headers) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.header("Set-Cookie", "a=1", {.mode_ = ruvia::http_response_header_mode::append});
        context.header("Set-Cookie", "b=2", {.mode_ = ruvia::http_response_header_mode::append});
        const auto response = context.body("data");

        std::size_t set_cookie_count = 0;
        for (const auto& header : response.headers()) {
            if (header.name() == std::string_view("Set-Cookie")) {
                ++set_cookie_count;
            }
        }
        RUVIA_CHECK_EQ(set_cookie_count, std::size_t{2});
        co_return;
    });
}

RUVIA_TEST(context_body_null_gives_empty_body_with_status) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context.status(ruvia::http_status::no_content);
        const auto response = context.body(nullptr);
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::no_content);
        RUVIA_CHECK(response.body_bytes().empty());
        co_return;
    });
}

RUVIA_TEST(context_body_byte_span_copies_into_response_storage) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const std::byte bytes_value[] = {
            std::byte{0x00},
            std::byte{0x41},
            std::byte{0xff},
        };
        const auto response = context.body(std::span<const std::byte>(bytes_value));
        const auto body = response.body_bytes();

        RUVIA_CHECK_EQ(body.size(), std::size(bytes_value));
        RUVIA_CHECK_EQ(body[0], '\0');
        RUVIA_CHECK_EQ(body[1], 'A');
        RUVIA_CHECK_EQ(static_cast<unsigned char>(body[2]), 0xff);
        co_return;
    });
}

RUVIA_TEST(context_text_sets_plain_content_type) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.text("hello");
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
        RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("hello"));
        co_return;
    });
}

RUVIA_TEST(context_html_sets_html_content_type) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        const auto response = context.html("<h1>hi</h1>");
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
        RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("<h1>hi</h1>"));
        co_return;
    });
}

RUVIA_TEST(context_json_serializes_response_model_with_json_content_type) {
    (void)context_request_test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& context) -> ruvia::task<void> {
        context_json_response model({.resource_ = context.arena()});
        model.set<"number">(42);
        model.set<"boolean">(true);
        model.set<"real">(3.5);
        const auto response = context.json(model);
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
        RUVIA_CHECK_EQ(response.body_bytes(),
            std::string_view(R"({"number":42,"boolean":true,"real":3.5})"));
        co_return;
    });
}
