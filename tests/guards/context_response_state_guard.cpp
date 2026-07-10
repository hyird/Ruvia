#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/HttpParserInternal.h"

#include "ruvia/memory/MemoryPool.h"

#include <string_view>

namespace {

int failures = 0;

void check(bool condition) {
    if (!condition) {
        ++failures;
    }
}

[[nodiscard]] std::size_t countHeaders(ruvia::HttpResponse& response, std::string_view name) {
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == name) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] ruvia::HttpRequest makeRequest() {
    ruvia::detail::HttpServerParser parser;
    const auto parsed = parser.parse("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    return parsed.request;
}

// A header appended while a response slot exists is stored in both the slot
// and the context header list; response builders must emit it exactly once.
void exerciseAppendHeaderNotDuplicated(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    (void)context.res();
    context.header("X-Trace", "abc", ruvia::Context::HeaderOptions{.append = true});
    auto response = context.text("hi");
    check(countHeaders(response, "X-Trace") == 1);
}

void exerciseSetCookieNotDuplicated(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    (void)context.res();
    context.setCookie("session", "id", ruvia::CookieOptions{});
    auto response = context.text("hi");
    check(countHeaders(response, "Set-Cookie") == 1);
}

// An explicit per-response status wins over the context default, matching the
// creation-time rule (statusCode == 0 means "use the context status").
void exerciseExplicitStatusWinsOnReturn(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(404);
    ruvia::detail::ContextAccess::setResponse(context, context.text("created", 201));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == 201);
}

void exerciseExplicitStatusWinsOnAssign(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(404);
    context.res(context.text("failed", 500));
    check(context.res().status() == 500);
}

// The redirect target wins over a prepared context Location header.
void exerciseRedirectLocationWins(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.header("Location", "/wrong");
    auto response = context.redirect("/right", 302);
    check(countHeaders(response, "Location") == 1);
    check(response.header("Location") == "/right");
}

void exerciseContextStatusAppliesAsDefault(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(404);
    ruvia::detail::ContextAccess::setResponse(context, ruvia::HttpResponse(context.resource()));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == 404);
}

}  // namespace

int main() {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest();

    exerciseAppendHeaderNotDuplicated(memory, request);
    exerciseSetCookieNotDuplicated(memory, request);
    exerciseExplicitStatusWinsOnReturn(memory, request);
    exerciseExplicitStatusWinsOnAssign(memory, request);
    exerciseRedirectLocationWins(memory, request);
    exerciseContextStatusAppliesAsDefault(memory, request);

    return failures;
}
