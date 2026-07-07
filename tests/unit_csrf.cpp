#include "test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include "CsrfInternal.h"
#include "http/ContextInternal.h"
#include "HttpRequestInternal.h"
#include "router/RouteTable.h"
#include "runtime/AsioAwait.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/Csrf.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpMethod;
using ruvia::HttpRequest;
using ruvia::Next;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::csrfTokensEqual;
using ruvia::detail::generateCsrfToken;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::NextAccess;
using ruvia::detail::RequestKnownHeader;

bool isLowerHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

struct CsrfOutcome final {
    bool nextInvoked{false};
    bool hasResponse{false};
    bool reseeded{false};
    std::uint16_t status{0};
};

// Runs CsrfProtection::handle over a synthesized request and reports whether the
// chain continued (next called) or was short-circuited with a response.
CsrfOutcome runCsrf(HttpMethod method, bool withCookie, std::string_view cookieToken,
                    bool withHeader, std::string_view headerToken) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setMethod(request, method);
    // The header views point into these strings, so they must outlive the context
    // use below -- keep them at function scope, not inside the if-blocks.
    std::string cookie = "XSRF-TOKEN=";
    cookie.append(cookieToken.data(), cookieToken.size());
    if (withCookie) {
        HttpRequestAccess::addHeader(
            request, HttpHeaderView{"Cookie", cookie},
            HttpRequestAccess::knownHeaderSlot(RequestKnownHeader::kCookie));
    }
    if (withHeader) {
        HttpRequestAccess::addHeader(request, HttpHeaderView{"X-XSRF-TOKEN", headerToken});
    }
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(memory, request);

    Next::State::Control control;
    Next::State state{};
    state.context = &context;
    state.control = &control;
    Next next = NextAccess::make(state, [](Next::State s) -> ruvia::Task<void> {
        if (s.control != nullptr) {
            s.control->invoked = true;
        }
        co_return;
    });

    ruvia::CsrfProtection csrf;
    asio::io_context io;
    asio::co_spawn(io, ruvia::detail::taskAsAwaitable(csrf.handle(context, next)), asio::detached);
    io.run();

    CsrfOutcome out;
    out.nextInvoked = control.invoked;
    out.reseeded = ContextAccess::hasPendingSetCookie(context, "XSRF-TOKEN=");
    out.hasResponse = ContextAccess::hasResponse(context);
    if (out.hasResponse) {
        out.status = ContextAccess::takeResponse(context).status();
    }
    return out;
}

}  // namespace

RUVIA_TEST(csrf_token_is_48_lowercase_hex_chars) {
    std::array<char, 64> buffer{};
    const auto token = generateCsrfToken(buffer);
    RUVIA_CHECK_EQ(token.size(), std::size_t{48});  // 24 random bytes -> 48 hex chars
    for (const char c : token) {
        RUVIA_CHECK(isLowerHex(c));
    }
}

RUVIA_TEST(csrf_token_requires_a_large_enough_buffer) {
    std::array<char, 47> tooSmall{};
    RUVIA_CHECK(generateCsrfToken(tooSmall).empty());  // one byte short -> empty
    std::array<char, 48> exact{};
    RUVIA_CHECK_EQ(generateCsrfToken(exact).size(), std::size_t{48});  // exact fit works
}

RUVIA_TEST(csrf_token_is_unpredictable) {
    std::array<char, 64> a{};
    std::array<char, 64> b{};
    const std::string first(generateCsrfToken(a));
    const std::string second(generateCsrfToken(b));
    RUVIA_CHECK_EQ(first.size(), std::size_t{48});
    RUVIA_CHECK_EQ(second.size(), std::size_t{48});
    // 192 bits of CSPRNG entropy: a repeat is astronomically unlikely.
    RUVIA_CHECK(first != second);
}

RUVIA_TEST(csrf_tokens_equal_is_length_checked_and_exact) {
    RUVIA_CHECK(csrfTokensEqual("abc123", "abc123"));
    RUVIA_CHECK(!csrfTokensEqual("abc123", "abc124"));   // last byte differs
    RUVIA_CHECK(!csrfTokensEqual("Xbc123", "abc123"));   // first byte differs
    // A length mismatch is never equal (and must not read past the shorter view).
    RUVIA_CHECK(!csrfTokensEqual("abc", "abc123"));
    RUVIA_CHECK(!csrfTokensEqual("abc123", "abc"));
    // Two empty tokens are equal (degenerate), but empty never matches non-empty.
    RUVIA_CHECK(csrfTokensEqual("", ""));
    RUVIA_CHECK(!csrfTokensEqual("", "a"));
    // The compare accumulates all byte diffs (no early-out): a difference in the
    // middle is still detected regardless of position.
    RUVIA_CHECK(!csrfTokensEqual("aaaaaaaa", "aaaXaaaa"));
}

RUVIA_TEST(csrf_unsafe_method_requires_matching_double_submit) {
    // A state-changing method with a cookie and header that match continues the chain.
    const auto ok = runCsrf(HttpMethod::kPost, true, "abcdef123456", true, "abcdef123456");
    RUVIA_CHECK(ok.nextInvoked);
    RUVIA_CHECK(!ok.hasResponse);

    // A mismatched header is rejected with 403 and the chain is NOT continued.
    const auto mismatch = runCsrf(HttpMethod::kPost, true, "abcdef123456", true, "DIFFERENTtoken");
    RUVIA_CHECK(!mismatch.nextInvoked);
    RUVIA_CHECK(mismatch.hasResponse);
    RUVIA_CHECK_EQ(mismatch.status, std::uint16_t{403});
}

RUVIA_TEST(csrf_unsafe_method_rejects_empty_or_missing_tokens) {
    // THE critical guard: csrfTokensEqual("","") is true (degenerate), so without the
    // explicit empty check an empty cookie AND empty header would falsely validate.
    // handle() must reject a both-empty double-submit with 403.
    const auto bothEmpty = runCsrf(HttpMethod::kPost, true, "", true, "");
    RUVIA_CHECK(!bothEmpty.nextInvoked);
    RUVIA_CHECK(bothEmpty.hasResponse);
    RUVIA_CHECK_EQ(bothEmpty.status, std::uint16_t{403});

    // A cookie with no matching request header is rejected.
    const auto noHeader = runCsrf(HttpMethod::kPost, true, "abcdef123456", false, {});
    RUVIA_CHECK(!noHeader.nextInvoked);
    RUVIA_CHECK_EQ(noHeader.status, std::uint16_t{403});

    // A header with no cookie is rejected.
    const auto noCookie = runCsrf(HttpMethod::kPost, false, {}, true, "abcdef123456");
    RUVIA_CHECK(!noCookie.nextInvoked);
    RUVIA_CHECK_EQ(noCookie.status, std::uint16_t{403});
}

RUVIA_TEST(csrf_safe_method_skips_validation) {
    // A safe method never enforces the double-submit: the chain continues even with
    // no tokens at all. (An existing cookie avoids the token-issuing branch.)
    const auto get = runCsrf(HttpMethod::kGet, true, "abcdef123456", false, {});
    RUVIA_CHECK(get.nextInvoked);
    RUVIA_CHECK(!get.hasResponse);

    // Even a mismatch is irrelevant for a safe method.
    const auto head = runCsrf(HttpMethod::kHead, true, "one", true, "two");
    RUVIA_CHECK(head.nextInvoked);
}

RUVIA_TEST(csrf_safe_method_reseeds_absent_or_empty_cookie) {
    // A safe method with NO cookie issues a fresh token so the client can later
    // send the double-submit pair.
    const auto absent = runCsrf(HttpMethod::kGet, false, {}, false, {});
    RUVIA_CHECK(absent.nextInvoked);
    RUVIA_CHECK(absent.reseeded);

    // A safe method with a present-but-EMPTY cookie must also reseed. Otherwise
    // the empty "XSRF-TOKEN=" is never repaired: the unsafe path rejects an empty
    // cookie with 403, so without this the client is permanently wedged. Issue
    // and validation must treat an empty cookie identically.
    const auto empty = runCsrf(HttpMethod::kGet, true, "", false, {});
    RUVIA_CHECK(empty.nextInvoked);
    RUVIA_CHECK(empty.reseeded);

    // A valid existing token must NOT be overwritten -- reseeding would rotate a
    // token the client is mid-flight with.
    const auto present = runCsrf(HttpMethod::kGet, true, "abcdef123456", false, {});
    RUVIA_CHECK(present.nextInvoked);
    RUVIA_CHECK(!present.reseeded);
}
