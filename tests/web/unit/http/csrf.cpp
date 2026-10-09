#include "ruvia/web/csrf.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "http/secure_token.h"
#include "router/route_table.h"
#include "server/trusted_proxies.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using ruvia::context;
using ruvia::http_header_view;
using ruvia::http_known_method;
using ruvia::http_request;
using ruvia::next;
using ruvia::request_memory;
using ruvia::worker_memory;
using ruvia::detail::context_access;
using ruvia::detail::csrf_tokens_equal;
using ruvia::detail::generate_secure_token;
using ruvia::detail::next_access;
using ruvia::detail::trusted_proxy_set;

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
    bool next_invoked_{false};
    bool has_response_{false};
    bool reseeded_{false};
    std::uint16_t status_{0};
    std::string set_cookie_;
};

[[nodiscard]] bool set_cookie_has_secure(std::string_view set_cookie) noexcept {
    return set_cookie.find("; Secure") != std::string_view::npos;
}

trusted_proxy_set trusted_proxy_set_of(std::initializer_list<std::string_view> cidrs) {
    trusted_proxy_set set;
    for (const auto cidr : cidrs) {
        if (const auto block = ruvia::detail::parse_trusted_proxy_block(cidr); block.index() == 0) {
            set.add(std::get<0>(block));
        }
    }
    return set;
}

// Runs csrf_protection::handle over a synthesized request and reports whether the
// chain continued (next called) or was short-circuited with a response.
csrf_outcome run_csrf(ruvia::csrf_protection& csrf, std::string_view cookie_name,
    std::string_view header_name, http_known_method method, bool with_cookie,
    std::string_view cookie_token, bool with_header, std::string_view header_token,
    ruvia::detail::context_services services = ruvia::test::test_context_services(),
    std::optional<http_header_view> extra_header = {}) {
    worker_memory worker;
    request_memory memory(worker);
    // Header views borrow these strings; keep every backing string alive through
    // middleware execution and request destruction.
    std::string cookie(cookie_name);
    cookie.push_back('=');
    cookie.append(cookie_token.data(), cookie_token.size());
    std::array<http_header_view, 3> headers{};
    std::size_t header_count = 0;
    if (with_cookie) {
        headers[header_count++] = http_header_view{"Cookie", cookie};
    }
    if (with_header) {
        headers[header_count++] = http_header_view{header_name, header_token};
    }
    if (extra_header.has_value()) {
        headers[header_count++] = *extra_header;
    }
    auto [request, parse_error] = ruvia::make_parsed_http_request(
        ruvia::known_http_method_token(method), "/",
        std::span<const http_header_view>{headers.data(), header_count}, {}, memory.resource());
    if (parse_error.has_value()) {
        throw std::logic_error("test request construction failed");
    }
    auto context_value = context_access::make(memory, request, services);

    ruvia::detail::next_state::control_type control;
    ruvia::detail::next_state state_value{};
    state_value.context_ = &context_value;
    state_value.control_ = &control;
    next next_value =
        next_access::make(state_value, [](ruvia::detail::next_state) -> ruvia::task<void> { co_return; });

    asio::io_context& io = ruvia::test::new_test_io_context();
    asio::co_spawn(io, ruvia::as_awaitable(csrf.handle(context_value, next_value)), asio::detached);
    io.run();

    csrf_outcome out;
    out.next_invoked_ = control.phase() == ruvia::detail::next_state::control_type::phase_type::invoked;
    std::string cookie_prefix(cookie_name);
    cookie_prefix.push_back('=');
    const auto set_cookie = context_access::pending_set_cookie_value(context_value, cookie_prefix);
    out.reseeded_ = !set_cookie.empty();
    out.set_cookie_.assign(set_cookie.data(), set_cookie.size());
    out.has_response_ = context_access::has_response(context_value);
    if (out.has_response_) {
        out.status_ = context_access::take_response(context_value).status().value();
    }
    return out;
}

csrf_outcome run_csrf(http_known_method method, bool with_cookie, std::string_view cookie_token,
    bool with_header, std::string_view header_token) {
    ruvia::csrf_protection csrf;
    return run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", method, with_cookie, cookie_token, with_header,
        header_token);
}

}  // namespace

RUVIA_TEST(csrf_token_is_48_lowercase_hex_chars) {
    std::array<char, 64> buffer{};
    const auto result_value = generate_secure_token(buffer);
    RUVIA_CHECK(result_value.ready() != nullptr);
    const auto token = result_value.ready()->value();
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
    RUVIA_CHECK(safe.next_invoked_);
    RUVIA_CHECK(safe.reseeded_);

    const auto unsafe = run_csrf(*csrf, cookie_name, header_name, http_known_method::post, true,
        "abcdef123456", true, "abcdef123456");
    RUVIA_CHECK(unsafe.next_invoked_);
    RUVIA_CHECK(!unsafe.has_response_);
}

RUVIA_TEST(csrf_token_requires_a_large_enough_buffer) {
    std::array<char, 47> too_small{};
    const auto too_small_result = generate_secure_token(too_small);
    RUVIA_CHECK(too_small_result.failure() != nullptr);
    std::array<char, 48> exact{};
    const auto exact_result = generate_secure_token(exact);
    RUVIA_CHECK(exact_result.ready() != nullptr);
    RUVIA_CHECK_EQ(exact_result.ready()->value().size(), std::size_t{48});
}

RUVIA_TEST(csrf_token_is_unpredictable) {
    std::array<char, 64> a{};
    std::array<char, 64> b{};
    const auto first_result = generate_secure_token(a);
    const auto second_result = generate_secure_token(b);
    RUVIA_CHECK(first_result.ready() != nullptr);
    RUVIA_CHECK(second_result.ready() != nullptr);
    const std::string first(first_result.ready()->value());
    const std::string second(second_result.ready()->value());
    RUVIA_CHECK_EQ(first.size(), std::size_t{48});
    RUVIA_CHECK_EQ(second.size(), std::size_t{48});
    // 192 bits of CSPRNG entropy: a repeat is astronomically unlikely.
    RUVIA_CHECK(first != second);
}

RUVIA_TEST(csrf_tokens_equal_is_length_checked_and_exact) {
    RUVIA_CHECK(csrf_tokens_equal("abc123", "abc123"));
    RUVIA_CHECK(!csrf_tokens_equal("abc123", "abc124"));  // last byte differs
    RUVIA_CHECK(!csrf_tokens_equal("Xbc123", "abc123"));  // first byte differs
    // A length mismatch is never equal (and must not read past the shorter view).
    RUVIA_CHECK(!csrf_tokens_equal("abc", "abc123"));
    RUVIA_CHECK(!csrf_tokens_equal("abc123", "abc"));
    // Two empty tokens are equal (degenerate), but empty never matches non-empty.
    RUVIA_CHECK(csrf_tokens_equal("", ""));
    RUVIA_CHECK(!csrf_tokens_equal("", "a"));
    // The compare accumulates all byte diffs (no early-out): a difference in the
    // middle is still detected regardless of position.
    RUVIA_CHECK(!csrf_tokens_equal("aaaaaaaa", "aaaXaaaa"));
}

RUVIA_TEST(csrf_unsafe_method_requires_matching_double_submit) {
    // A state-changing method with a cookie and header that match continues the chain.
    const auto ok = run_csrf(http_known_method::post, true, "abcdef123456", true, "abcdef123456");
    RUVIA_CHECK(ok.next_invoked_);
    RUVIA_CHECK(!ok.has_response_);

    // A mismatched header is rejected with 403 and the chain is NOT continued.
    const auto mismatch =
        run_csrf(http_known_method::post, true, "abcdef123456", true, "DIFFERENTtoken");
    RUVIA_CHECK(!mismatch.next_invoked_);
    RUVIA_CHECK(mismatch.has_response_);
    RUVIA_CHECK_EQ(mismatch.status_, std::uint16_t{403});
}

RUVIA_TEST(csrf_unsafe_method_rejects_empty_or_missing_tokens) {
    // THE critical guard: csrf_tokens_equal("","") is true (degenerate), so without the
    // explicit empty check an empty cookie AND empty header would falsely validate.
    // handle() must reject a both-empty double-submit with 403.
    const auto both_empty = run_csrf(http_known_method::post, true, "", true, "");
    RUVIA_CHECK(!both_empty.next_invoked_);
    RUVIA_CHECK(both_empty.has_response_);
    RUVIA_CHECK_EQ(both_empty.status_, std::uint16_t{403});

    // A cookie with no matching request header is rejected.
    const auto no_header = run_csrf(http_known_method::post, true, "abcdef123456", false, {});
    RUVIA_CHECK(!no_header.next_invoked_);
    RUVIA_CHECK_EQ(no_header.status_, std::uint16_t{403});

    // A header with no cookie is rejected.
    const auto no_cookie = run_csrf(http_known_method::post, false, {}, true, "abcdef123456");
    RUVIA_CHECK(!no_cookie.next_invoked_);
    RUVIA_CHECK_EQ(no_cookie.status_, std::uint16_t{403});
}

RUVIA_TEST(csrf_safe_method_skips_validation) {
    // A safe method never enforces the double-submit: the chain continues even with
    // no tokens at all. (An existing cookie avoids the token-issuing branch.)
    const auto get = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    RUVIA_CHECK(get.next_invoked_);
    RUVIA_CHECK(!get.has_response_);

    // Even a mismatch is irrelevant for a safe method.
    const auto head = run_csrf(http_known_method::head, true, "one", true, "two");
    RUVIA_CHECK(head.next_invoked_);
}

RUVIA_TEST(csrf_safe_method_reseeds_absent_or_empty_cookie) {
    // A safe method with NO cookie issues a fresh token so the client can later
    // send the double-submit pair.
    const auto absent = run_csrf(http_known_method::get, false, {}, false, {});
    RUVIA_CHECK(absent.next_invoked_);
    RUVIA_CHECK(absent.reseeded_);

    // A safe method with a present-but-EMPTY cookie must also reseed. Otherwise
    // the empty "XSRF-TOKEN=" is never repaired: the unsafe path rejects an empty
    // cookie with 403, so without this the client is permanently wedged. Issue
    // and validation must treat an empty cookie identically.
    const auto empty = run_csrf(http_known_method::get, true, "", false, {});
    RUVIA_CHECK(empty.next_invoked_);
    RUVIA_CHECK(empty.reseeded_);

    // A valid existing token must NOT be overwritten -- reseeding would rotate a
    // token the client is mid-flight with.
    const auto present = run_csrf(http_known_method::get, true, "abcdef123456", false, {});
    RUVIA_CHECK(present.next_invoked_);
    RUVIA_CHECK(!present.reseeded_);
}

RUVIA_TEST(csrf_reseed_cookie_omits_secure_on_plaintext) {
    ruvia::csrf_protection csrf;
    const auto issued = run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", http_known_method::get, false, {},
        false, {});
    RUVIA_CHECK(issued.next_invoked_);
    RUVIA_CHECK(issued.reseeded_);
    RUVIA_CHECK(!issued.set_cookie_.empty());
    RUVIA_CHECK(!set_cookie_has_secure(issued.set_cookie_));
}

RUVIA_TEST(csrf_reseed_cookie_sets_secure_on_tls_transport) {
    ruvia::csrf_protection csrf;
    const auto issued = run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", http_known_method::get, false, {},
        false, {}, ruvia::test::test_context_services().with_tls_transport("203.0.113.7"));
    RUVIA_CHECK(issued.next_invoked_);
    RUVIA_CHECK(issued.reseeded_);
    RUVIA_CHECK(set_cookie_has_secure(issued.set_cookie_));
}

RUVIA_TEST(csrf_reseed_cookie_sets_secure_behind_tls_terminating_proxy) {
    ruvia::csrf_protection csrf;
    const auto trusted = trusted_proxy_set_of({"10.0.0.0/8"});
    const auto issued = run_csrf(csrf, "XSRF-TOKEN", "X-XSRF-TOKEN", http_known_method::get, false, {},
        false, {},
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted),
        http_header_view{"Forwarded", "for=203.0.113.9;proto=https"});
    RUVIA_CHECK(issued.next_invoked_);
    RUVIA_CHECK(issued.reseeded_);
    RUVIA_CHECK(set_cookie_has_secure(issued.set_cookie_));
}
