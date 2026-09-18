#include <concepts>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Model.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"

#include "context_services_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

RUVIA_RESPONSE_MODEL(ContextJsonResponse, RUVIA_REQUIRED_FIELD(number, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(boolean, ruvia::Bool), RUVIA_REQUIRED_FIELD(real, ruvia::Double));

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
using ruvia::testing::throwsOn;

class HeaderFailureResource final : public std::pmr::memory_resource {
public:
    std::optional<std::size_t> remainingHeaderAllocations{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        // Header bytes use alignment 1. Leave Debug iterator proxies alone:
        // MSVC may allocate those inside noexcept container construction.
        if (alignment == 1 && remainingHeaderAllocations) {
            if (*remainingHeaderAllocations == 0) {
                throw std::bad_alloc();
            }
            --*remainingHeaderAllocations;
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

// The Context holds the request by reference, so keep it in the test's scope
// (this macro-free setup avoids a returning helper that would dangle).
#define RUVIA_MAKE_CONTEXT(worker, memory, request, context)    \
    WorkerMemory worker;                                        \
    RequestMemory memory(worker);                               \
    HttpRequest request = HttpRequestAccess::make();            \
    HttpRequestAccess::reset(request);                          \
    HttpRequestAccess::setResource(request, memory.resource()); \
    auto context = ContextAccess::make(memory, request, ruvia::test::testContextServices())

}  // namespace

RUVIA_TEST(context_connection_info_is_adapter_owned) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setResource(request, memory.resource());
    HttpRequestAccess::setTarget(request, "/secure");
    HttpRequestAccess::setPath(request, "/secure");
    RUVIA_CHECK(HttpRequestAccess::addHeader(request, HttpHeaderView{"Host", "example.test"},
        HttpRequestAccess::knownHeaderSlot(ruvia::detail::RequestKnownHeader::kHost)));

    const auto services =
        ruvia::test::testContextServices().withTlsTransport("203.0.113.7", "/CN=client");
    auto context = ContextAccess::make(memory, request, services);
    const auto info = ruvia::getConnInfo(context);

    RUVIA_CHECK_EQ(info.remote().address(), std::string_view("203.0.113.7"));
    RUVIA_CHECK(info.plain() == nullptr);
    RUVIA_CHECK(info.tls() != nullptr);
    RUVIA_CHECK_EQ(info.tls()->clientCertificateSubject(), std::string_view("/CN=client"));
}

RUVIA_TEST(context_redirect_sets_verbatim_ascii_location_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect({.location = "https://example.com/path?q=1"});
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kFound);
    RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
}

RUVIA_TEST(context_redirect_accepts_only_redirect_statuses) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    for (const auto status : {ruvia::http_status::kMovedPermanently, ruvia::http_status::kFound,
             ruvia::http_status::kSeeOther, ruvia::http_status::kTemporaryRedirect,
             ruvia::http_status::kPermanentRedirect}) {
        RUVIA_CHECK_EQ(context.redirect({.location = "/next", .status = status}).status(), status);
    }

    RUVIA_CHECK(throwsOn(
        [&] { (void)context.redirect({.location = "/next", .status = ruvia::http_status::kOk}); }));
    RUVIA_CHECK(throwsOn([&] {
        (void)context.redirect({.location = "/next", .status = ruvia::http_status::kNotModified});
    }));
    RUVIA_CHECK(throwsOn([&] {
        (void)context.redirect({.location = "/next", .status = ruvia::http_status::kNotFound});
    }));
}

RUVIA_TEST(context_redirect_percent_encodes_non_ascii_location) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // A UTF-8 'é' (0xC3 0xA9) is percent-encoded while the URI structure
    // (scheme, host, path separators) is preserved.
    const auto response =
        context.redirect({.location = std::string_view("https://example.com/caf\xC3\xA9"),
            .status = ruvia::http_status::kTemporaryRedirect});
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kTemporaryRedirect);
    RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_percent_encodes_invalid_ascii_uri_bytes) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect({.location = "/a b/100%off?q=\"x y\"\\z"});
    RUVIA_CHECK_EQ(
        response.header("Location"), std::string_view("/a%20b/100%25off?q=%22x%20y%22%5Cz"));
}

RUVIA_TEST(context_redirect_preserves_existing_percent_escapes_when_encoding) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // The location mixes already-encoded escapes ("%20", "%2F") with a raw UTF-8
    // 'é' (0xC3 0xA9) that triggers the whole-string encoding pass. The 'é' must
    // become %C3%A9, but the existing escapes must survive intact -- not be
    // double-encoded to "%2520"/"%252F", which would corrupt the target.
    const auto response = context.redirect(
        {.location = std::string_view("https://example.com/a%20b/caf\xC3\xA9?x=%2F")});
    RUVIA_CHECK_EQ(
        response.header("Location"), std::string_view("https://example.com/a%20b/caf%C3%A9?x=%2F"));

    // A lone or malformed '%' (not followed by two hex digits) is not a valid
    // escape, so it IS percent-encoded to %25 -- the trailing 'é' forces the pass.
    const auto malformed =
        context.redirect({.location = std::string_view("https://example.com/100%off/caf\xC3\xA9")});
    RUVIA_CHECK_EQ(
        malformed.header("Location"), std::string_view("https://example.com/100%25off/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_preserves_ipv6_literal_brackets_when_encoding) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response =
        context.redirect({.location = std::string_view("https://[2001:db8::1]/caf\xC3\xA9")});
    RUVIA_CHECK_EQ(
        response.header("Location"), std::string_view("https://[2001:db8::1]/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_percent_encodes_square_brackets_outside_ip_literal) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect({.location = "/items[0]?filter=[x]#section[1]"});
    RUVIA_CHECK_EQ(response.header("Location"),
        std::string_view("/items%5B0%5D?filter=%5Bx%5D#section%5B1%5D"));

    const auto absolute = context.redirect(
        {.location = std::string_view("https://[2001:db8::1]/items[0]?filter=[x]")});
    RUVIA_CHECK_EQ(absolute.header("Location"),
        std::string_view("https://[2001:db8::1]/items%5B0%5D?filter=%5Bx%5D"));
}

RUVIA_TEST(context_redirect_rejects_crlf_header_injection) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // A CRLF in the location must not split the response: header-value
    // validation rejects it (the location is ASCII, so it takes the verbatim
    // path straight into the validated header setter).
    bool threw = false;
    try {
        (void)context.redirect(
            {.location = std::string_view("https://example.com/\r\nX-Injected: y")});
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(context_redirect_rejects_crlf_even_when_location_needs_encoding) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    RUVIA_CHECK(throwsOn([&] {
        (void)context.redirect(
            {.location = std::string_view("https://example.com/caf\xC3\xA9\r\nX-Injected: y")});
    }));
}

RUVIA_TEST(context_body_sets_body_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    context.status(ruvia::http_status::kCreated);
    const auto response = context.body("hello world");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kCreated);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("hello world"));
}

RUVIA_TEST(context_dynamic_body_owns_input_and_preserves_lvalue) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    std::pmr::string source("dynamic body", memory.resource());

    const auto response = context.body(source);
    source[0] = 'X';

    RUVIA_CHECK(responseBody(response).ownedBytes() != nullptr);
    RUVIA_CHECK(responseBody(response).borrowedBytes() == nullptr);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("dynamic body"));
    RUVIA_CHECK_EQ(source, std::string_view("Xynamic body"));
}

RUVIA_TEST(context_literal_builders_keep_static_storage) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    const auto bodyResponse = context.body("body");
    const auto textResponse = context.text("text");
    const auto htmlResponse = context.html("<b>html</b>");

    RUVIA_CHECK(responseBody(bodyResponse).staticBytes() != nullptr);
    RUVIA_CHECK(responseBody(textResponse).staticBytes() != nullptr);
    RUVIA_CHECK(responseBody(htmlResponse).staticBytes() != nullptr);
}

RUVIA_TEST(context_rejects_informational_and_non_http_final_statuses) {
    {
        RUVIA_MAKE_CONTEXT(worker, memory, request, context);
        bool threw = false;
        try {
            context.status(ruvia::http_status::kEarlyHints);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    {
        bool threw = false;
        try {
            (void)ruvia::HttpStatusCode::fromValue(600);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
}

RUVIA_TEST(context_error_normalizes_non_error_status_before_response_state) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    context.header("X-Trace", "1");

    const auto ok = context.error(
        {.status = ruvia::http_status::kOk, .code = "bad", .message = "not an error"});
    RUVIA_CHECK_EQ(ok.status(), ruvia::http_status::kInternalServerError);
    RUVIA_CHECK_EQ(ok.header("X-Trace"), std::string_view("1"));

    const auto redirect = context.error({.status = ruvia::http_status::kTemporaryRedirect,
        .code = "bad",
        .message = "not an error"});
    RUVIA_CHECK_EQ(redirect.status(), ruvia::http_status::kInternalServerError);
}

RUVIA_TEST(context_response_metadata_uses_http_response_validation) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    bool threw = false;
    try {
        context.header("Connection", "close,");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    const auto response = context.body("unchanged");
    RUVIA_CHECK(!response.header("Connection").has_value());
}

RUVIA_TEST(context_body_applies_context_headers) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    context.status(ruvia::http_status::kOk);
    context.header("Content-Type", "text/plain");
    context.header("X-Custom", "v");
    const auto response = context.body("data");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("data"));
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(response.header("X-Custom"), std::string_view("v"));
}

RUVIA_TEST(context_response_merge_keeps_complete_headers_without_allocating) {
    std::size_t moveAllocations = 0;
    for (const bool withContextHeaders : {false, true}) {
        ruvia::test::CountingMemoryResource resource;
        {
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            if (withContextHeaders) {
                context.header("Cache-Control", "no-store");
                context.header("X-Custom", "context");
                context.header("X-Tag", "same", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
                context.header("X-Tag", "same", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
            }
            ruvia::HttpResponse response({.resource = &resource});
            response.header("Cache-Control", "private");
            response.header("X-Custom", "response");
            response.header("X-Tag", "same", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
            response.header("X-Tag", "same", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
            response.body(std::string(4096, 'x'));
            const auto* bodyData = responseBody(response).bytes().data();
            const auto allocations = resource.allocationCount();

            ContextAccess::setResponse(context, std::move(response));

            // Debug standard libraries may allocate iterator bookkeeping on move.
            // Reconciliation must add nothing to the plain finalization cost.
            const auto finalizationAllocations = resource.allocationCount() - allocations;
            if (withContextHeaders) {
                RUVIA_CHECK_EQ(finalizationAllocations, moveAllocations);
            } else {
                moveAllocations = finalizationAllocations;
            }
            RUVIA_CHECK_EQ(responseBody(*context.response()).bytes().data(), bodyData);
            RUVIA_CHECK_EQ(context.response()->header("Cache-Control"), std::string_view("private"));
            RUVIA_CHECK_EQ(context.response()->header("X-Custom"), std::string_view("response"));
            RUVIA_CHECK_EQ(context.response()->headers().size(), std::size_t{4});
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
}

RUVIA_TEST(context_response_header_transactions_preserve_owned_body_storage) {
    for (const bool assigned : {false, true}) {
        ruvia::test::CountingMemoryResource resource;
        {
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            context.header("X-Added", "context");
            context.header("X-Tag", "same", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
            context.header("X-Tag", "same", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
            ruvia::HttpResponse response({.resource = &resource});
            response.header("Content-Type", "application/octet-stream");
            response.header("X-Tag", "same", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
            response.body(std::string(4096, 'x'));
            const auto* bodyData = responseBody(response).bytes().data();
            if (assigned) {
                context.respond(std::move(response));
            } else {
                ContextAccess::setResponse(context, std::move(response));
            }
            RUVIA_CHECK_EQ(responseBody(*context.response()).bytes().data(), bodyData);
            RUVIA_CHECK_EQ(responseBody(*context.response()).bytes().size(), std::size_t{4096});
            RUVIA_CHECK_EQ(context.response()->header("X-Added"), std::string_view("context"));
            RUVIA_CHECK_EQ(context.response()->headers().size(), std::size_t{4});
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
}

RUVIA_TEST(context_response_header_transaction_failure_preserves_both_inputs) {
    for (const bool assigned : {false, true}) {
        for (const std::size_t successfulHeaders : {std::size_t{0}, std::size_t{1}}) {
            HeaderFailureResource resource;
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            context.header("X-Added", "context");
            ruvia::HttpResponse response({.resource = &resource});
            response.header("X-Original", "response");
            response.body(std::string(4096, 'x'));
            const auto* bodyData = responseBody(response).bytes().data();
            resource.remainingHeaderAllocations = successfulHeaders;
            bool allocationFailed = false;
            try {
                if (assigned) {
                    context.respond(std::move(response));
                } else {
                    ContextAccess::setResponse(context, std::move(response));
                }
            } catch (const std::bad_alloc&) {
                allocationFailed = true;
            }
            RUVIA_CHECK(allocationFailed);
            RUVIA_CHECK_EQ(responseBody(response).bytes().data(), bodyData);
            RUVIA_CHECK_EQ(response.header("X-Original"), std::string_view("response"));
            RUVIA_CHECK(!response.header("X-Added"));
            resource.remainingHeaderAllocations.reset();
            ContextAccess::setResponse(context, std::move(response));
            RUVIA_CHECK_EQ(context.response()->header("X-Added"), std::string_view("context"));
        }
    }
}

RUVIA_TEST(context_response_header_transactions_preserve_cookie_policy_and_spilled_headers) {
    for (const bool assigned : {false, true}) {
        ruvia::test::CountingMemoryResource resource;
        {
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            context.header("Set-Cookie", "session=new; Path=/");
            context.header("Cache-Control", "no-store");
            ruvia::HttpResponse response({.resource = &resource});
            response.header("Set-Cookie", "session=old; Path=/");
            response.header("Set-Cookie", "other=kept; Path=/", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
            response.header("Content-Type", "application/octet-stream");
            response.header("Cache-Control", "private");
            for (int i = 0; i < 12; ++i) {
                response.header("X-Field-" + std::to_string(i), "value");
            }
            response.body(std::string(4096, 'x'));
            const auto* bodyData = responseBody(response).bytes().data();
            if (assigned) {
                context.respond(std::move(response));
            } else {
                ContextAccess::setResponse(context, std::move(response));
            }
            const auto& result = *context.response();
            RUVIA_CHECK_EQ(responseBody(result).bytes().data(), bodyData);
            RUVIA_CHECK_EQ(result.header("Content-Type"), std::string_view("application/octet-stream"));
            RUVIA_CHECK_EQ(result.header("Cache-Control"), std::string_view(assigned ? "no-store" : "private"));
            RUVIA_CHECK_EQ(result.header("Set-Cookie"), std::string_view("session=new; Path=/"));
            std::size_t cookies = 0;
            for (const auto& header : result.headers()) {
                if (header.name() == "Set-Cookie") {
                    ++cookies;
                }
            }
            RUVIA_CHECK_EQ(cookies, assigned ? std::size_t{1} : std::size_t{2});
            for (int i = 0; i < 12; ++i) {
                RUVIA_CHECK_EQ(result.header("X-Field-" + std::to_string(i)), std::string_view("value"));
            }
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
}

RUVIA_TEST(context_metadata_preserves_repeated_set_cookie_headers) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    context.header("Set-Cookie", "a=1", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    context.header("Set-Cookie", "b=2", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    const auto response = context.body("data");

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
    context.status(ruvia::http_status::kNoContent);
    const auto response = context.body(nullptr);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kNoContent);
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
    const auto response = context.text("hello");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
    RUVIA_CHECK_EQ(responseBody(response).bytes(), std::string_view("hello"));
}

RUVIA_TEST(context_html_sets_html_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.html("<h1>hi</h1>");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
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
    auto context = ContextAccess::make(memory, request, "/p/:slug/:id", names, values,
        std::size(names), 0, ruvia::test::testContextServices());

    const auto slug = context.req().param("slug");
    RUVIA_CHECK(slug.has_value());
    RUVIA_CHECK_EQ(*slug, std::string_view("hello"));
    const auto id = context.req().param("id");
    RUVIA_CHECK(id.has_value());
    RUVIA_CHECK_EQ(*id, std::string_view("42"));
    // An unknown parameter name is a clean miss.
    RUVIA_CHECK(!context.req().param("missing").has_value());
    // Every lookup shares the one typed parameter cache used by field binding.
    RUVIA_CHECK(ContextAccess::routeParamsMaterialized(context));
}

RUVIA_TEST(context_json_serializes_response_model_with_json_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    ContextJsonResponse model({.resource = context.arena()});
    model.set<"number">(42);
    model.set<"boolean">(true);
    model.set<"real">(3.5);
    const auto response = context.json(model);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(responseBody(response).bytes(),
        std::string_view(R"({"number":42,"boolean":true,"real":3.5})"));
}
