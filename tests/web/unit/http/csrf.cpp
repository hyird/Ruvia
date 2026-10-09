#include "ruvia/web/csrf.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_known_method.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/testing.h"

#include "test_harness.h"

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

class csrf_test_middleware final : public ruvia::middleware {
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

class accepted_response final : public ruvia::middleware {
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

RUVIA_TEST(csrf_token_is_48_lowercase_hex_chars) {
    const auto token = issued_token();
    RUVIA_CHECK_EQ(token.size(), std::size_t{48});
    for (const char c : token) {
        RUVIA_CHECK(is_lower_hex(c));
    }
}

RUVIA_TEST(csrf_custom_names_validate_at_construction) {
    RUVIA_CHECK(!throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "APP-XSRF", .header_name_ = "X-APP-XSRF"});
    }));
    RUVIA_CHECK(throws_invalid(
        [] { (void)ruvia::csrf_protection({.cookie_name_ = "", .header_name_ = "X-XSRF-TOKEN"}); }));
    RUVIA_CHECK(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "bad;name", .header_name_ = "X-XSRF-TOKEN"});
    }));
    RUVIA_CHECK(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "XSRF-TOKEN", .header_name_ = "Bad Header"});
    }));
}

RUVIA_TEST(csrf_middleware_owns_custom_names_after_source_destruction) {
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
    RUVIA_CHECK(safe.accepted_);
    RUVIA_CHECK(safe.reseeded_);

    const auto unsafe = run_csrf(*csrf, cookie_name, header_name, http_known_method::post, true,
        "abcdef123456", true, "abcdef123456");
    RUVIA_CHECK(unsafe.accepted_);
    RUVIA_CHECK_EQ(unsafe.status_, std::uint16_t{200});
}

RUVIA_TEST(csrf_token_is_unpredictable) {
    const auto first = issued_token();
    const auto second = issued_token();
    RUVIA_CHECK_EQ(first.size(), std::size_t{48});
    RUVIA_CHECK_EQ(second.size(), std::size_t{48});
    // 192 bits of CSPRNG entropy: a repeat is astronomically unlikely.
    RUVIA_CHECK(first != second);
}

RUVIA_TEST(csrf_unsafe_method_requires_matching_double_submit) {
    const auto ok = run_csrf(http_known_method::post, true, "abcdef123456", true, "abcdef123456");
    RUVIA_CHECK(ok.accepted_);
    RUVIA_CHECK_EQ(ok.status_, std::uint16_t{200});

    const auto mismatch =
        run_csrf(http_known_method::post, true, "abcdef123456", true, "DIFFERENTtoken");
    RUVIA_CHECK(!mismatch.accepted_);
    RUVIA_CHECK_EQ(mismatch.status_, std::uint16_t{403});

    const auto no_header = run_csrf(http_known_method::post, true, "abcdef123456", false, {});
    RUVIA_CHECK(!no_header.accepted_);
    RUVIA_CHECK_EQ(no_header.status_, std::uint16_t{403});

    const auto no_cookie = run_csrf(http_known_method::post, false, {}, true, "abcdef123456");
    RUVIA_CHECK(!no_cookie.accepted_);
    RUVIA_CHECK_EQ(no_cookie.status_, std::uint16_t{403});
}

RUVIA_TEST(csrf_safe_method_skips_validation) {
    const auto get = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    RUVIA_CHECK(get.accepted_);
    RUVIA_CHECK_EQ(get.status_, std::uint16_t{200});

    const auto head = run_csrf(http_known_method::head, true, "one", true, "two");
    RUVIA_CHECK_EQ(head.status_, std::uint16_t{200});
}

RUVIA_TEST(csrf_safe_method_reseeds_absent_or_empty_cookie) {
    const auto absent = run_csrf(http_known_method::get, false, {}, false, {});
    RUVIA_CHECK(absent.accepted_);
    RUVIA_CHECK(absent.reseeded_);

    const auto empty = run_csrf(http_known_method::get, true, "", false, {});
    RUVIA_CHECK(empty.accepted_);
    RUVIA_CHECK(empty.reseeded_);

    const auto present = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    RUVIA_CHECK(present.accepted_);
    RUVIA_CHECK(!present.reseeded_);
}

RUVIA_TEST(csrf_reseed_cookie_omits_secure_on_plaintext) {
    ruvia::csrf_protection csrf;
    const auto issued = run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", http_known_method::get, false, {},
        false, {});
    RUVIA_CHECK(issued.reseeded_);
    RUVIA_CHECK(!set_cookie_has_secure(issued.set_cookie_));
}
