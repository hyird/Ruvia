#include "ruvia/web/detail/http/context/ContextAccess.h"
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

[[nodiscard]] bool hasHeaderValue(ruvia::HttpResponse& response, std::string_view name, std::string_view value) {
    for (const auto& header : response.headers()) {
        if (header.name() == name && header.value() == value) {
            return true;
        }
    }
    return false;
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

    state.materializeProvisional().status(ruvia::http_status::kAccepted);
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

void exerciseSetCookieReplacement(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    (void)ruvia::detail::ContextAccess::responseStorage(context);
    context.setCookie("session", "first", ruvia::CookieOptions{});
    context.setCookie("theme", "dark", ruvia::CookieOptions{});
    context.setCookie("session", "second", ruvia::CookieOptions{});
    auto response = context.text("hi");
    check(countHeaders(response, "Set-Cookie") == 2);
    check(hasHeaderValue(response, "Set-Cookie", "session=second; Path=/"));
    check(hasHeaderValue(response, "Set-Cookie", "theme=dark; Path=/"));
    ruvia::detail::ContextAccess::setResponse(context, std::move(response));
    auto finalResponse = ruvia::detail::ContextAccess::takeResponse(context);
    check(countHeaders(finalResponse, "Set-Cookie") == 2);
    check(hasHeaderValue(finalResponse, "Set-Cookie", "session=second; Path=/"));
    check(hasHeaderValue(finalResponse, "Set-Cookie", "theme=dark; Path=/"));
}

void exercisePendingCookieReplacesRawResponseCookie(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.setCookie("session", "typed", ruvia::CookieOptions{});

    ruvia::HttpResponse raw(memory.resource());
    raw.header("Set-Cookie", "session=raw; Path=/");
    raw.header("Set-Cookie", "theme=light; Path=/", ruvia::HttpResponse::HeaderOptions{.append = true});
    ruvia::detail::ContextAccess::setResponse(context, std::move(raw));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);

    check(countHeaders(response, "Set-Cookie") == 2);
    check(hasHeaderValue(response, "Set-Cookie", "session=typed; Path=/"));
    check(hasHeaderValue(response, "Set-Cookie", "theme=light; Path=/"));
}

void exerciseCookieReplacementUsesWireName(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.setCookie("session", "bare", ruvia::CookieOptions{});

    ruvia::CookieOptions host;
    host.prefix = ruvia::CookiePrefix::kHost;
    host.secure = true;
    context.setCookie("session", "host-first", host);
    context.setCookie("session", "host-second", host);
    auto response = context.text("hi");

    check(countHeaders(response, "Set-Cookie") == 2);
    check(hasHeaderValue(response, "Set-Cookie", "session=bare; Path=/"));
    check(hasHeaderValue(response, "Set-Cookie", "__Host-session=host-second; Path=/; Secure"));
}

void exercisePendingStateMergesIntoRawResponse(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(ruvia::http_status::kNotFound);
    context.header("X-Pending", "yes");
    ruvia::detail::ContextAccess::setResponse(context, ruvia::HttpResponse(context.resource()));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    // A raw response owns its status, while pending headers still decorate it.
    check(response.status() == ruvia::http_status::kOk);
    check(response.header("X-Pending") == "yes");
}

void exerciseProvisionalStateMergesIntoAssignedResponse(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    auto& provisional = ruvia::detail::ContextAccess::responseStorage(context);
    provisional.header("X-Middleware", "yes");
    context.respond(ruvia::HttpResponse(context.resource()));
    const auto* response = context.response();
    check(response != nullptr && response->header("X-Middleware") == "yes");
}

void exerciseActiveStorageCanFinalizeInPlace(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    auto& provisional = ruvia::detail::ContextAccess::responseStorage(context);
    provisional.header("X-In-Place", "yes");
    ruvia::detail::ContextAccess::setResponse(context, std::move(provisional));
    const auto* response = context.response();
    check(response != nullptr && response->header("X-In-Place") == "yes");
}

void exerciseContextStatusOnReturn(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(ruvia::http_status::kNotFound);
    ruvia::detail::ContextAccess::setResponse(context, context.text("ok"));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == ruvia::http_status::kNotFound);
}

void exerciseContextStatusOnAssign(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(ruvia::http_status::kInternalServerError);
    context.respond(context.text("failed"));
    check(context.response() != nullptr && context.response()->status() == ruvia::http_status::kInternalServerError);
    (void)ruvia::detail::ContextAccess::takeResponse(context);
    check(context.response() == nullptr);
}

// The redirect target wins over a prepared context Location header.
void exerciseRedirectLocationWins(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.header("Location", "/wrong");
    auto response = context.redirect("/right", ruvia::http_status::kFound);
    check(countHeaders(response, "Location") == 1);
    check(response.header("Location") == "/right");
}

void exerciseContextStatusAppliesAsDefault(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(ruvia::http_status::kNotFound);
    ruvia::detail::ContextAccess::setResponse(context, context.text("not found"));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == ruvia::http_status::kNotFound);
}

void exerciseRawResponseStatusIsNotReinterpreted(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context = ruvia::detail::ContextAccess::make(memory, request);
    context.status(ruvia::http_status::kNotFound);
    ruvia::detail::ContextAccess::setResponse(context, ruvia::HttpResponse(context.resource()));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == ruvia::http_status::kOk);
}

}  // namespace

int main() {
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = makeRequest();

    exerciseTypedResponsePhases(memory);
    exerciseAppendHeaderMultiplicity(memory, request);
    exerciseSetCookieReplacement(memory, request);
    exercisePendingCookieReplacesRawResponseCookie(memory, request);
    exerciseCookieReplacementUsesWireName(memory, request);
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
