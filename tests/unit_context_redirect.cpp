#include "test_harness.h"

#include <cstdint>
#include <exception>
#include <string_view>

#include "http/ContextInternal.h"
#include "http/HttpRequestInternal.h"
#include "http/HttpResponseBodyAccess.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/ContextModel.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::responseBodyBytes;

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

RUVIA_TEST(context_redirect_sets_verbatim_ascii_location_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect("https://example.com/path?q=1", 302, "Found");
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{302});
    RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
}

RUVIA_TEST(context_redirect_percent_encodes_non_ascii_location) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // A UTF-8 'é' (0xC3 0xA9) is percent-encoded while the URI structure
    // (scheme, host, path separators) is preserved.
    const auto response = context.redirect(
        std::string_view("https://example.com/caf\xC3\xA9"), 307, "Temporary Redirect");
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{307});
    RUVIA_CHECK_EQ(response.header("Location"),
                   std::string_view("https://example.com/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_rejects_crlf_header_injection) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // A CRLF in the location must not split the response: header-value
    // validation rejects it (the location is ASCII, so it takes the verbatim
    // path straight into the validated header setter).
    bool threw = false;
    try {
        (void)context.redirect(std::string_view("https://example.com/\r\nX-Injected: y"), 302, "Found");
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(context_body_sets_body_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.body("hello world", Context::ResponseInit{.status = 201});
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{201});
    RUVIA_CHECK_EQ(responseBodyBytes(response), std::string_view("hello world"));
}

RUVIA_TEST(context_body_applies_init_headers) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const HttpHeaderView headers[] = {{"Content-Type", "text/plain"}, {"X-Custom", "v"}};
    const auto response = context.body("data", Context::ResponseInit{.status = 200, .headers = headers});
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(responseBodyBytes(response), std::string_view("data"));
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(response.header("X-Custom"), std::string_view("v"));
}

RUVIA_TEST(context_body_null_gives_empty_body_with_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.body(nullptr, std::uint16_t{204}, "No Content");
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{204});
    RUVIA_CHECK(responseBodyBytes(response).empty());
}

RUVIA_TEST(context_text_sets_plain_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.text("hello", 200, "OK");
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
    RUVIA_CHECK_EQ(responseBodyBytes(response), std::string_view("hello"));
}

RUVIA_TEST(context_html_sets_html_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.html("<h1>hi</h1>", 200, "OK");
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
    RUVIA_CHECK_EQ(responseBodyBytes(response), std::string_view("<h1>hi</h1>"));
}

RUVIA_TEST(context_json_serializes_scalars_with_json_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    const auto number = context.json(42, std::uint16_t{200});
    RUVIA_CHECK_EQ(number.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(number.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(responseBodyBytes(number), std::string_view("42"));

    const auto boolean = context.json(true, std::uint16_t{200});
    RUVIA_CHECK_EQ(responseBodyBytes(boolean), std::string_view("true"));

    const auto real = context.json(3.5, std::uint16_t{200});
    RUVIA_CHECK_EQ(responseBodyBytes(real), std::string_view("3.5"));
}
