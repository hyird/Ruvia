#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"

#include "ruvia/core/memory/MemoryPool.h"

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
    ruvia::detail::Http1ServerRequestParser parser;
    const auto parsed = parser.parseMessage("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
    return parsed.request;
}

void exerciseTypedResponsePhases(ruvia::RequestMemory& memory) {
    ruvia::detail::ContextResponseState state(memory.resource());
    check(state.pending() != nullptr);
    check(state.provisional() == nullptr);
    check(state.final() == nullptr);

    state.materializeProvisional().status(202);
    check(state.pending() == nullptr);
    check(state.provisional() != nullptr);
    state.finalize(ruvia::HttpResponse(memory.resource()));
    check(state.provisional() == nullptr);
    check(state.final() != nullptr);
    (void)state.take();
    check(state.pending() != nullptr);
}

// Append is a wire operation, not set membership: equal fields retain their
// multiplicity even after provisional storage has been materialized.
void exerciseAppendHeaderMultiplicity(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    (void)ruvia::detail::ContextAccess::responseStorage(context);
    check(context.response() == nullptr);
    context.header("X-Trace", "abc", ruvia::Context::HeaderOptions{.append = true});
    context.header("X-Trace", "abc", ruvia::Context::HeaderOptions{.append = true});
    auto response = context.text("hi");
    check(countHeaders(response, "X-Trace") == 2);
    ruvia::detail::ContextAccess::setResponse(context, std::move(response));
    auto finalResponse = ruvia::detail::ContextAccess::takeResponse(context);
    check(countHeaders(finalResponse, "X-Trace") == 2);
}

void exerciseSetCookieMultiplicity(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    (void)ruvia::detail::ContextAccess::responseStorage(context);
    context.setCookie("session", "id", ruvia::CookieOptions{});
    context.setCookie("session", "id", ruvia::CookieOptions{});
    auto response = context.text("hi");
    check(countHeaders(response, "Set-Cookie") == 2);
    ruvia::detail::ContextAccess::setResponse(context, std::move(response));
    auto finalResponse = ruvia::detail::ContextAccess::takeResponse(context);
    check(countHeaders(finalResponse, "Set-Cookie") == 2);
}

void exercisePendingStateMergesIntoRawResponse(
    ruvia::RequestMemory& memory,
    const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(404);
    context.header("X-Pending", "yes");
    ruvia::detail::ContextAccess::setResponse(
        context,
        ruvia::HttpResponse(context.resource()));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    // A raw response owns its status, while pending headers still decorate it.
    check(response.status() == 200);
    check(response.header("X-Pending") == "yes");
}

void exerciseProvisionalStateMergesIntoAssignedResponse(
    ruvia::RequestMemory& memory,
    const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    auto& provisional = ruvia::detail::ContextAccess::responseStorage(context);
    provisional.header("X-Middleware", "yes");
    context.respond(ruvia::HttpResponse(context.resource()));
    const auto* response = context.response();
    check(response != nullptr && response->header("X-Middleware") == "yes");
}

void exerciseActiveStorageCanFinalizeInPlace(
    ruvia::RequestMemory& memory,
    const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    auto& provisional = ruvia::detail::ContextAccess::responseStorage(context);
    provisional.header("X-In-Place", "yes");
    ruvia::detail::ContextAccess::setResponse(context, std::move(provisional));
    const auto* response = context.response();
    check(response != nullptr && response->header("X-In-Place") == "yes");
}

void exerciseContextStatusOnReturn(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(404);
    ruvia::detail::ContextAccess::setResponse(context, context.text("ok"));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == 404);
}

void exerciseContextStatusOnAssign(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(500);
    context.respond(context.text("failed"));
    check(context.response() != nullptr && context.response()->status() == 500);
    (void)ruvia::detail::ContextAccess::takeResponse(context);
    check(context.response() == nullptr);
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
    ruvia::detail::ContextAccess::setResponse(
        context,
        context.text("not found"));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == 404);
}

void exerciseRawResponseStatusIsNotReinterpreted(
    ruvia::RequestMemory& memory,
    const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(404);
    ruvia::detail::ContextAccess::setResponse(
        context,
        ruvia::HttpResponse(context.resource()));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == 200);
}

}  // namespace

int main() {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest();

    exerciseTypedResponsePhases(memory);
    exerciseAppendHeaderMultiplicity(memory, request);
    exerciseSetCookieMultiplicity(memory, request);
    exercisePendingStateMergesIntoRawResponse(memory, request);
    exerciseProvisionalStateMergesIntoAssignedResponse(memory, request);
    exerciseActiveStorageCanFinalizeInPlace(memory, request);
    exerciseContextStatusOnReturn(memory, request);
    exerciseContextStatusOnAssign(memory, request);
    exerciseRedirectLocationWins(memory, request);
    exerciseContextStatusAppliesAsDefault(memory, request);
    exerciseRawResponseStatusIsNotReinterpreted(memory, request);

    return failures;
}
