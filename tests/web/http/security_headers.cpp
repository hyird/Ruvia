#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <asio/co_spawn.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "app.h"
#include "blocking_pool.h"
#include "context.h"
#include "controller.h"
#include "event_loop.h"
#include "http_client.h"
#include "http_header.h"
#include "http_response_server.h"
#include "http_server.h"
#include "http_server_fixture.h"
#include "middleware.h"
#include "task.h"
#include "task_scope.h"
#include "worker_task_fixture.h"
namespace security_header_test {

using ruvia::default_security_header_policy;
using ruvia::security_header_conflict_policy;
using ruvia::security_headers_config;
using ruvia::xss_protection_header_policy;

[[nodiscard]] ruvia::dispatch_response security_response(const security_headers_config& options = {}, std::string_view existing_frame_options = {}) {
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
    EXPECT_EQ(response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
    EXPECT_EQ(response.header("Referrer-Policy"), std::string_view("strict-origin-when-cross-origin"));
    EXPECT_EQ(response.header("Permissions-Policy"), std::string_view("geolocation=(), microphone=(), camera=()"));
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

    EXPECT_EQ(response.header("Content-Security-Policy"), std::string_view("default-src 'self'"));
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

    explicit csrf_test_middleware(ruvia::csrf_protection* middleware_value) :
        middleware_(middleware_value) {}

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

csrf_outcome run_csrf(ruvia::csrf_protection& csrf, std::string_view cookie_name, std::string_view header_name, http_known_method method, bool with_cookie, std::string_view cookie_token, bool with_header, std::string_view header_token) {
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

csrf_outcome run_csrf(http_known_method method, bool with_cookie, std::string_view cookie_token, bool with_header, std::string_view header_token) {
    ruvia::csrf_protection csrf;
    return run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", method, with_cookie, cookie_token, with_header, header_token);
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
    EXPECT_TRUE(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "", .header_name_ = "X-XSRF-TOKEN"});
    }));
    EXPECT_TRUE(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "bad;name", .header_name_ = "X-XSRF-TOKEN"});
    }));
    EXPECT_TRUE(throws_invalid([] {
        (void)ruvia::csrf_protection({.cookie_name_ = "XSRF-TOKEN", .header_name_ = "Bad Header"});
    }));
}

TEST(Ruvia, csrf_rejects_safelisted_and_ambient_token_fields) {
    for (const std::string_view name : {"Accept", "aCcEpT-LaNgUaGe", "Content-Language", "CONTENT-TYPE", "Range", "Origin", "Host", "Content-Length", "Cookie", "uSeR-aGeNt", "Sec-Fetch-Site", "sEc-Ch-Ua", "Proxy-Authorization", "Access-Control-Request-Headers", "X-Forwarded-For", "Forwarded", "X-Real-IP", "Save-Data", "DPR", "X-Requested-With"}) {
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

    const auto safe = run_csrf(*csrf, cookie_name, header_name, http_known_method::get, false, {}, false, {});
    EXPECT_TRUE(safe.accepted_);
    EXPECT_TRUE(safe.reseeded_);

    const auto unsafe = run_csrf(*csrf, cookie_name, header_name, http_known_method::post, true, "abcdef123456", true, "abcdef123456");
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

    const auto mismatch = run_csrf(http_known_method::post, true, "abcdef123456", true, "DIFFERENTtoken");
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
    const auto issued = run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", http_known_method::get, false, {}, false, {});
    EXPECT_TRUE(issued.reseeded_);
    EXPECT_TRUE(!set_cookie_has_secure(issued.set_cookie_));
}

TEST(Ruvia, csrf_rejection_reaches_the_error_handler) {
    ruvia::csrf_protection csrf;
    ruvia::test_app app;
    std::string seen_code{};
    app.on_error([seen = &seen_code](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
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

[[nodiscard]] ruvia::dispatch_response request_at(ruvia::test_app& app, std::string_view address, std::chrono::steady_clock::duration offset, std::string_view target = "/rate-limit-table/ok") {
    return app.request(ruvia::test_request::get(target).remote(address).arrival(window_origin + offset));
}

[[nodiscard]] ruvia::http_status_code request_from(ruvia::test_app& app, std::string_view address, std::chrono::steady_clock::duration offset = {}) {
    return request_at(app, address, offset).status();
}

[[nodiscard]] std::optional<std::uint64_t> header_number(const ruvia::dispatch_response& response, std::string_view name) {
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
    app.rate_limit(ruvia::rate_limit_config{.rule_ = {.max_requests_ = 2, .window_ = std::chrono::minutes(10), .overflow_policy_ = overflow_policy}, .capacity_per_worker_ = held_addresses.size()});
}

void configure_window_limit(ruvia::test_app& app, std::size_t max_requests, std::chrono::milliseconds window, std::size_t capacity = ruvia::default_rate_limit_capacity_per_worker) {
    app.rate_limit(ruvia::rate_limit_config{.rule_ = {.max_requests_ = max_requests, .window_ = window}, .capacity_per_worker_ = capacity});
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
    return v6 ? tcp::endpoint(asio::ip::address_v6::loopback(), port) : tcp::endpoint(asio::ip::address_v4::loopback(), port);
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
    opposite_family_peer(asio::io_context& io, const ruvia::worker_ref& worker, bool first_is_v6) :
        io_(io),
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
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
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

[[nodiscard]] ruvia::task<void> exercise(ruvia::event_loop_attachment& attachment, opposite_family_peer& peer, std::string_view host, std::chrono::milliseconds connect_timeout) {
    const std::string authority = host.find(':') == std::string_view::npos ? std::string(host) : "[" + std::string(host) + "]";
    const std::string base_url = std::string("http://") + authority + std::move(std::string(":").append(std::to_string(peer.port())));
    ruvia::http_client client(attachment.loop(), {.base_url_ = base_url, .connect_timeout_ = connect_timeout, .request_timeout_ = 3s, .versions_ = ruvia::http_client_versions{.http2_ = false}});
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

ruvia::task<void> send_query(ruvia::http_client& client, ruvia::stop_token stop, std::optional<std::error_code>& code) {
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

ruvia::task<void> exercise(ruvia::event_loop loop, const ruvia::blocking_pool& lookups, stop_kind kind, std::shared_ptr<held_queries> held, observation& observed, std::promise<void>& ready, std::promise<void>& retired) {
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
        const auto expected = kind == stop_kind::timeout ? std::make_error_code(std::errc::timed_out) : (kind == stop_kind::cancellation ? std::make_error_code(std::errc::operation_canceled) : ruvia::make_error_code(ruvia::http_client_errc::closing));
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
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) {
            stream.async_read_some(asio::buffer(&byte, 1), std::move(h));
        });
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
            auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) {
                acceptor.async_accept(std::move(h));
            });
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
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto h) {
            stream.async_read_some(asio::buffer(&byte, 1), std::move(h));
        });
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
                auto accepted_socket = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto h) {
                    acceptor.async_accept(std::move(h));
                });
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
                    if (!parsed.head_ready() || parsed.request().method() != "CONNECT" || parsed.request().target() != "target.test:443") {
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
    static_cast<void>(ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
        try {
            found = (co_await ctx.static_file(root, {.relative_path_ = path})).status() == ruvia::http_status::ok;
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
    static_cast<void>(ruvia::test::with_context(ruvia::test_request::get("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
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
    options.file_types_ = ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
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
    options.file_types_ = ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
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
    static_cast<void>(ruvia::test::with_context(ruvia::test_request::head("/"), [&](ruvia::context& ctx) -> ruvia::task<void> {
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
    ruvia::static_root root(dir, {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}, .resident_max_bytes_ = 32, .resident_budget_bytes_ = 1024});

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
    ruvia::static_root root(dir, {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}, .resident_max_bytes_ = 32, .resident_budget_bytes_ = 9});
    EXPECT_TRUE(!serve_body(root, "nested/page.txt").file_backed_);
    EXPECT_TRUE(serve_body(root, "small.txt").file_backed_);
}
