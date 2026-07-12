#include "test_harness.h"

#include <cstdint>
#include <exception>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::ContextServices;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::responseBody;

// The Context holds the request by reference, so keep it in the test's scope
// (this macro-free setup avoids a returning helper that would dangle).
#define RUVIA_MAKE_CONTEXT(worker, memory, request, context)              \
    WorkerMemory worker;                                                  \
    RequestMemory memory(worker);                                         \
    HttpRequest request = HttpRequestAccess::make();                      \
    HttpRequestAccess::reset(request);                                    \
    HttpRequestAccess::setResource(request, memory.resource());           \
    auto context = ContextAccess::make(memory, request)

}  // namespace

RUVIA_TEST(context_connection_info_is_adapter_owned) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setResource(request, memory.resource());
    HttpRequestAccess::setTarget(request, "/secure");
    HttpRequestAccess::setPath(request, "/secure");
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Host", "example.test"},
        HttpRequestAccess::knownHeaderSlot(ruvia::detail::RequestKnownHeader::kHost)));

    const auto services = ContextServices{}.withTlsTransport(
        "203.0.113.7",
        "/CN=client");
    auto context = ContextAccess::make(memory, request, services);
    const auto info = ruvia::getConnInfo(context);

    RUVIA_CHECK_EQ(info.remote().address(), std::string_view("203.0.113.7"));
    RUVIA_CHECK(info.plain() == nullptr);
    RUVIA_CHECK(info.tls() != nullptr);
    RUVIA_CHECK_EQ(
        info.tls()->clientCertificateSubject(),
        std::string_view("/CN=client"));
    RUVIA_CHECK(context.req().url() == std::string_view("https://example.test/secure"));
}

RUVIA_TEST(context_redirect_sets_verbatim_ascii_location_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect("https://example.com/path?q=1", 302);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{302});
    RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
}

RUVIA_TEST(context_redirect_percent_encodes_non_ascii_location) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // A UTF-8 'é' (0xC3 0xA9) is percent-encoded while the URI structure
    // (scheme, host, path separators) is preserved.
    const auto response = context.redirect(
        std::string_view("https://example.com/caf\xC3\xA9"), 307);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{307});
    RUVIA_CHECK_EQ(response.header("Location"),
                   std::string_view("https://example.com/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_preserves_existing_percent_escapes_when_encoding) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // The location mixes already-encoded escapes ("%20", "%2F") with a raw UTF-8
    // 'é' (0xC3 0xA9) that triggers the whole-string encoding pass. The 'é' must
    // become %C3%A9, but the existing escapes must survive intact -- not be
    // double-encoded to "%2520"/"%252F", which would corrupt the target.
    const auto response = context.redirect(
        std::string_view("https://example.com/a%20b/caf\xC3\xA9?x=%2F"), 302);
    RUVIA_CHECK_EQ(response.header("Location"),
                   std::string_view("https://example.com/a%20b/caf%C3%A9?x=%2F"));

    // A lone or malformed '%' (not followed by two hex digits) is not a valid
    // escape, so it IS percent-encoded to %25 -- the trailing 'é' forces the pass.
    const auto malformed = context.redirect(
        std::string_view("https://example.com/100%off/caf\xC3\xA9"), 302);
    RUVIA_CHECK_EQ(malformed.header("Location"),
                   std::string_view("https://example.com/100%25off/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_rejects_crlf_header_injection) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // A CRLF in the location must not split the response: header-value
    // validation rejects it (the location is ASCII, so it takes the verbatim
    // path straight into the validated header setter).
    bool threw = false;
    try {
        (void)context.redirect(std::string_view("https://example.com/\r\nX-Injected: y"), 302);
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(context_body_sets_body_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.body("hello world", Context::ResponseInit{.status = 201});
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{201});
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("hello world"));
}

RUVIA_TEST(context_rejects_informational_and_non_http_final_statuses) {
    {
        RUVIA_MAKE_CONTEXT(worker, memory, request, context);
        bool threw = false;
        try {
            context.status(103);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    {
        RUVIA_MAKE_CONTEXT(worker, memory, request, context);
        bool threw = false;
        try {
            (void)context.body("not final", Context::ResponseInit{.status = 103});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    {
        RUVIA_MAKE_CONTEXT(worker, memory, request, context);
        bool threw = false;
        try {
            context.status(600);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
}

RUVIA_TEST(context_body_applies_init_headers) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const HttpHeaderView headers[] = {{"Content-Type", "text/plain"}, {"X-Custom", "v"}};
    const auto response = context.body("data", Context::ResponseInit{.status = 200, .headers = headers});
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("data"));
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(response.header("X-Custom"), std::string_view("v"));
}

RUVIA_TEST(context_response_init_preserves_repeated_set_cookie_headers) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const HttpHeaderView headers[] = {{"Set-Cookie", "a=1"}, {"Set-Cookie", "b=2"}};
    const auto response = context.body("data", Context::ResponseInit{.headers = headers});

    std::size_t setCookieCount = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++setCookieCount;
        }
    }
    RUVIA_CHECK_EQ(setCookieCount, std::size_t{2});
}

RUVIA_TEST(context_body_null_gives_empty_body_with_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.body(nullptr, std::uint16_t{204});
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{204});
    RUVIA_CHECK(responseBody(response).bytes().empty());
}

RUVIA_TEST(context_body_byte_span_copies_into_response_storage) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const std::byte bytes[] = {
        std::byte{0x00},
        std::byte{0x41},
        std::byte{0xff},
    };
    const auto response = context.body(std::span<const std::byte>(bytes));
    const auto body = responseBody(response).bytes();

    RUVIA_CHECK_EQ(body.size(), std::size(bytes));
    RUVIA_CHECK_EQ(body[0], '\0');
    RUVIA_CHECK_EQ(body[1], 'A');
    RUVIA_CHECK_EQ(static_cast<unsigned char>(body[2]), 0xff);
    RUVIA_CHECK(body.data() != reinterpret_cast<const char*>(bytes));
}

RUVIA_TEST(context_text_sets_plain_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.text("hello", 200);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("hello"));
}

RUVIA_TEST(context_html_sets_html_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.html("<h1>hi</h1>", 200);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("<h1>hi</h1>"));
}

RUVIA_TEST(context_param_lookup_handles_unencoded_and_missing) {
    // A route-parameter lookup returns an unencoded value verbatim via the
    // zero-alloc fast path, and yields nullopt (not a false match or a crash)
    // for a name that was never captured. The encoded-decode path is covered
    // separately; this pins the two other branches of routeParam().
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    const std::string_view names[] = {"slug", "id"};
    const std::string_view values[] = {"hello", "42"};
    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());
    auto context = ContextAccess::make(
        memory, request, "/p/:slug/:id", names, values, std::size(names),
        ruvia::HttpKnownMethod::kGet, 0, 0);

    const auto slug = context.req().param("slug");
    RUVIA_CHECK(slug.has_value());
    RUVIA_CHECK_EQ(*slug, std::string_view("hello"));
    const auto id = context.req().param("id");
    RUVIA_CHECK(id.has_value());
    RUVIA_CHECK_EQ(*id, std::string_view("42"));
    // An unknown parameter name is a clean miss.
    RUVIA_CHECK(!context.req().param("missing").has_value());
    // Neither single lookup materializes the full parameter table.
    RUVIA_CHECK(!ContextAccess::routeParamsMaterialized(context));
}

RUVIA_TEST(context_json_serializes_scalars_with_json_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    const auto number = context.json(42, std::uint16_t{200});
    RUVIA_CHECK_EQ(number.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(number.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(responseBody(number).bytes(), std::string_view("42"));

    const auto boolean = context.json(true, std::uint16_t{200});
    RUVIA_CHECK_EQ(responseBody(boolean).bytes(), std::string_view("true"));

    const auto real = context.json(3.5, std::uint16_t{200});
    RUVIA_CHECK_EQ(responseBody(real).bytes(), std::string_view("3.5"));
}
