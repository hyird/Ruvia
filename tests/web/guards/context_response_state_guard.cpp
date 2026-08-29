#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"

#include "ruvia/core/memory/MemoryPool.h"
#include "context_services_fixture.h"

#include <cstddef>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void check(bool condition) {
    if (!condition) {
        ++failures;
    }
}

class FailingResponseResource final : public std::pmr::memory_resource {
public:
    void failAfterSuccessfulAllocations(std::size_t count) noexcept {
        failAfter_ = count;
    }

    void allowAllocations() noexcept {
        failAfter_.reset();
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (failAfter_.has_value()) {
            if (*failAfter_ == 0) {
                throw std::bad_alloc();
            }
            --*failAfter_;
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::optional<std::size_t> failAfter_;
};

[[nodiscard]] std::size_t countHeaders(ruvia::HttpResponse& response, std::string_view name) {
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == name) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] bool hasHeaderValue(
    ruvia::HttpResponse& response, std::string_view name, std::string_view value) {
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
    state.finalize(ruvia::HttpResponse({.resource = memory.resource()}));
    check(state.provisional() == nullptr);
    check(state.final() != nullptr);
    (void)state.take();
    check(state.pending() != nullptr);
}

// Append is a wire operation, not set membership: equal fields retain their
// multiplicity even after provisional storage has been materialized.
void exerciseAppendHeaderMultiplicity(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    (void)ruvia::detail::ContextAccess::responseStorage(context);
    check(context.response() == nullptr);
    context.header("X-Trace", "abc",
        ruvia::Context::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
    context.header("X-Trace", "abc",
        ruvia::Context::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
    auto response = context.text("hi");
    check(countHeaders(response, "X-Trace") == 2);
    ruvia::detail::ContextAccess::setResponse(context, std::move(response));
    auto finalResponse = ruvia::detail::ContextAccess::takeResponse(context);
    check(countHeaders(finalResponse, "X-Trace") == 2);
}

void exerciseSetCookieReplacement(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    (void)ruvia::detail::ContextAccess::responseStorage(context);
    context.setCookie({.name = "session", .value = "first"});
    context.setCookie({.name = "theme", .value = "dark"});
    context.setCookie({.name = "session", .value = "second"});
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

void exercisePendingCookieReplacesRawResponseCookie(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.setCookie({.name = "session", .value = "typed"});

    ruvia::HttpResponse raw({.resource = memory.resource()});
    raw.header("Set-Cookie", "session=raw; Path=/");
    raw.header("Set-Cookie", "theme=light; Path=/",
        ruvia::HttpResponse::HeaderOptions{.mode = ruvia::HttpResponseHeaderMode::kAppend});
    ruvia::detail::ContextAccess::setResponse(context, std::move(raw));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);

    check(countHeaders(response, "Set-Cookie") == 2);
    check(hasHeaderValue(response, "Set-Cookie", "session=typed; Path=/"));
    check(hasHeaderValue(response, "Set-Cookie", "theme=light; Path=/"));
}

void exerciseCookieReplacementUsesWireName(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.setCookie({.name = "session", .value = "bare"});

    const ruvia::CookieOptions host{
        .prefix = ruvia::CookiePrefix::kHost,
        .secure = ruvia::CookieAttributePolicy::kEmit,
    };
    context.setCookie({.name = "session", .value = "host-first", .attributes = host});
    context.setCookie({.name = "session", .value = "host-second", .attributes = host});
    auto response = context.text("hi");

    check(countHeaders(response, "Set-Cookie") == 2);
    check(hasHeaderValue(response, "Set-Cookie", "session=bare; Path=/"));
    check(hasHeaderValue(response, "Set-Cookie", "__Host-session=host-second; Path=/; Secure"));
}

void exercisePendingStateMergesIntoRawResponse(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.status(ruvia::http_status::kNotFound);
    context.header("X-Pending", "yes");
    ruvia::detail::ContextAccess::setResponse(
        context, ruvia::HttpResponse({.resource = context.resource()}));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    // A raw response owns its status, while pending headers still decorate it.
    check(response.status() == ruvia::http_status::kOk);
    check(response.header("X-Pending") == "yes");
}

void exerciseProvisionalStateMergesIntoAssignedResponse(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    auto& provisional = ruvia::detail::ContextAccess::responseStorage(context);
    provisional.header("X-Middleware", "yes");
    context.respond(ruvia::HttpResponse({.resource = context.resource()}));
    const auto* response = context.response();
    check(response != nullptr && response->header("X-Middleware") == "yes");
}

void exerciseActiveStorageCanFinalizeInPlace(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    auto& provisional = ruvia::detail::ContextAccess::responseStorage(context);
    provisional.header("X-In-Place", "yes");
    ruvia::detail::ContextAccess::setResponse(context, std::move(provisional));
    const auto* response = context.response();
    check(response != nullptr && response->header("X-In-Place") == "yes");
}

void exerciseContextStatusOnReturn(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.status(ruvia::http_status::kNotFound);
    ruvia::detail::ContextAccess::setResponse(context, context.text("ok"));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == ruvia::http_status::kNotFound);
}

void exerciseContextStatusOnAssign(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.status(ruvia::http_status::kInternalServerError);
    context.respond(context.text("failed"));
    check(context.response() != nullptr &&
          context.response()->status() == ruvia::http_status::kInternalServerError);
    (void)ruvia::detail::ContextAccess::takeResponse(context);
    check(context.response() == nullptr);
}

// The redirect target wins over a prepared context Location header.
void exerciseRedirectLocationWins(ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.header("Location", "/wrong");
    auto response = context.redirect({.location = "/right"});
    check(countHeaders(response, "Location") == 1);
    check(response.header("Location") == "/right");
}

void exerciseContextStatusAppliesAsDefault(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.status(ruvia::http_status::kNotFound);
    ruvia::detail::ContextAccess::setResponse(context, context.text("not found"));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == ruvia::http_status::kNotFound);
}

void exerciseRawResponseStatusIsNotReinterpreted(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    auto context =
        ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
    context.status(ruvia::http_status::kNotFound);
    ruvia::detail::ContextAccess::setResponse(
        context, ruvia::HttpResponse({.resource = context.resource()}));
    auto response = ruvia::detail::ContextAccess::takeResponse(context);
    check(response.status() == ruvia::http_status::kOk);
}

void exerciseResponseMergeRollsBackOnAllocationFailure(
    ruvia::RequestMemory& memory, const ruvia::HttpRequest& request) {
    const auto exercise = [&](bool assigned) {
        auto context =
            ruvia::detail::ContextAccess::make(memory, request, ruvia::test::testContextServices());
        context.header("X-Pending-A", "a");
        context.header("X-Pending-B", "b");

        FailingResponseResource resource;
        const std::string rawBody(128, 'r');
        ruvia::HttpResponse raw({.resource = &resource});
        raw.status(ruvia::http_status::kAccepted);
        raw.body(rawBody);
        raw.header("X-Raw", "raw");

        // Cloning the raw header and body consumes two allocations. The first
        // pending header can then be published, but the second allocation
        // fails. The raw response must still be exactly retryable afterward.
        resource.failAfterSuccessfulAllocations(3);
        bool failed = false;
        try {
            if (assigned) {
                context.respond(std::move(raw));
            } else {
                ruvia::detail::ContextAccess::setResponse(context, std::move(raw));
            }
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        check(failed);
        check(context.response() == nullptr);
        check(raw.status() == ruvia::http_status::kAccepted);
        check(ruvia::detail::responseBody(raw).bytes() == rawBody);
        check(countHeaders(raw, "X-Raw") == 1);
        check(!hasHeaderValue(raw, "X-Pending-A", "a"));
        check(!hasHeaderValue(raw, "X-Pending-B", "b"));

        resource.allowAllocations();
        if (assigned) {
            context.respond(std::move(raw));
        } else {
            ruvia::detail::ContextAccess::setResponse(context, std::move(raw));
        }
        auto response = ruvia::detail::ContextAccess::takeResponse(context);
        check(response.status() == ruvia::http_status::kAccepted);
        check(ruvia::detail::responseBody(response).bytes() == rawBody);
        check(hasHeaderValue(response, "X-Raw", "raw"));
        check(hasHeaderValue(response, "X-Pending-A", "a"));
        check(hasHeaderValue(response, "X-Pending-B", "b"));
    };

    exercise(false);
    exercise(true);
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
    exerciseResponseMergeRollsBackOnAllocationFailure(memory, request);

    return failures;
}
