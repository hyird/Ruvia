#include "ruvia/web/csrf.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_known_method.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/testing.h"

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
    std::string set_cookie_;
};

[[nodiscard]] bool set_cookie_has_secure(std::string_view set_cookie) noexcept {
    return set_cookie.find("; Secure") != std::string_view::npos;
}

class csrf_test_middleware final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    explicit csrf_test_middleware(ruvia::csrf_protection* middleware_value)
        : middleware_(middleware_value) {}

    ruvia::task<void> handle(ruvia::context& ctx, ruvia::next& next_step) {
        co_await middleware_->handle(ctx, next_step);
    }

private:
    ruvia::csrf_protection* middleware_;
};

class accepted_response final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> handle(ruvia::context& ctx, ruvia::next&) {
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
    return outcome.set_cookie_.substr(start, end - start);
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
        const auto token = issued.set_cookie_.substr(start, issued.set_cookie_.find(';', start) - start);
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
    std::string seen_code;
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
