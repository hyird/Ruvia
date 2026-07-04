#include "test_harness.h"

#include <cstdint>
#include <exception>
#include <string_view>

#include "http/ContextInternal.h"
#include "http/HttpRequestInternal.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using ruvia::Context;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::HttpRequestAccess;

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
