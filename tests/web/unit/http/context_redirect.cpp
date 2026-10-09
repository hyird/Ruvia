#include <concepts>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/context.h"
#include "ruvia/web/model.h"

#include "context/context_access.h"
#include "context_services_fixture.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

RUVIA_MODEL(context_json_response, RUVIA_REQUIRED_FIELD(number, ruvia::int64),
    RUVIA_REQUIRED_FIELD(boolean, ruvia::bool_value), RUVIA_REQUIRED_FIELD(real, ruvia::double_value));

namespace {

using ruvia::context;
using ruvia::http_header_view;
using ruvia::http_request;
using ruvia::request_memory;
using ruvia::worker_memory;
using ruvia::detail::context_access;
using ruvia::detail::context_services;
using ruvia::testing::throws_on;

class header_failure_resource final : public std::pmr::memory_resource {
public:
    std::optional<std::size_t> remaining_header_allocations_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // Header bytes use alignment 1. Leave Debug iterator proxies alone:
        // MSVC may allocate those inside noexcept container construction.
        if (alignment == 1 && remaining_header_allocations_) {
            if (*remaining_header_allocations_ == 0) {
                throw std::bad_alloc();
            }
            --*remaining_header_allocations_;
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

// The context holds the request by reference, so keep it in the test's scope
// (this macro-free setup avoids a returning helper that would dangle).
#define RUVIA_MAKE_CONTEXT(worker, memory, request, context) \
    worker_memory worker;                                    \
    request_memory memory(worker);                           \
    auto parsed = ruvia::make_parsed_http_request(           \
        "GET", "/", {}, {}, memory.resource());              \
    http_request request = std::move(parsed.first);          \
    auto context = context_access::make(memory, request, ruvia::test::test_context_services())

}  // namespace

RUVIA_TEST(context_connection_info_is_adapter_owned) {
    worker_memory worker;
    request_memory memory(worker);
    const http_header_view headers[]{{"Host", "example.test"}};
    auto parsed_value = ruvia::make_parsed_http_request(
        "GET", "/secure", headers, {}, memory.resource());
    RUVIA_CHECK(!parsed_value.second.has_value());
    http_request request = std::move(parsed_value.first);

    const auto services =
        ruvia::test::test_context_services().with_tls_transport("203.0.113.7", "/CN=client");
    auto context_value = context_access::make(memory, request, services);
    const auto info = context_value.conn();

    RUVIA_CHECK_EQ(info.remote().address(), std::string_view("203.0.113.7"));
    RUVIA_CHECK(info.plain() == nullptr);
    RUVIA_CHECK(info.tls() != nullptr);
    RUVIA_CHECK_EQ(info.tls()->client_certificate_subject(), std::string_view("/CN=client"));
}

RUVIA_TEST(context_redirect_sets_verbatim_ascii_location_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect({.location_ = "https://example.com/path?q=1"});
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::found);
    RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/path?q=1"));
}

RUVIA_TEST(context_redirect_accepts_only_redirect_statuses) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    for (const auto status : {ruvia::http_status::moved_permanently, ruvia::http_status::found,
             ruvia::http_status::see_other, ruvia::http_status::temporary_redirect,
             ruvia::http_status::permanent_redirect}) {
        RUVIA_CHECK_EQ(context.redirect({.location_ = "/next", .status_ = status}).status(), status);
    }

    RUVIA_CHECK(throws_on(
        [&] { (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::ok}); }));
    RUVIA_CHECK(throws_on([&] {
        (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_modified});
    }));
    RUVIA_CHECK(throws_on([&] {
        (void)context.redirect({.location_ = "/next", .status_ = ruvia::http_status::not_found});
    }));
}

RUVIA_TEST(context_redirect_percent_encodes_non_ascii_location) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    // A UTF-8 'é' (0xC3 0xA9) is percent-encoded while the URI structure
    // (scheme, host, path separators) is preserved.
    const auto response =
        context.redirect({.location_ = std::string_view("https://example.com/caf\xC3\xA9"),
            .status_ = ruvia::http_status::temporary_redirect});
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::temporary_redirect);
    RUVIA_CHECK_EQ(response.header("Location"), std::string_view("https://example.com/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_percent_encodes_invalid_ascii_uri_bytes) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect({.location_ = "/a b/100%off?q=\"x y\"\\z"});
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
        {.location_ = std::string_view("https://example.com/a%20b/caf\xC3\xA9?x=%2F")});
    RUVIA_CHECK_EQ(
        response.header("Location"), std::string_view("https://example.com/a%20b/caf%C3%A9?x=%2F"));

    // A lone or malformed '%' (not followed by two hex digits) is not a valid
    // escape, so it IS percent-encoded to %25 -- the trailing 'é' forces the pass.
    const auto malformed =
        context.redirect({.location_ = std::string_view("https://example.com/100%off/caf\xC3\xA9")});
    RUVIA_CHECK_EQ(
        malformed.header("Location"), std::string_view("https://example.com/100%25off/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_preserves_ipv6_literal_brackets_when_encoding) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response =
        context.redirect({.location_ = std::string_view("https://[2001:db8::1]/caf\xC3\xA9")});
    RUVIA_CHECK_EQ(
        response.header("Location"), std::string_view("https://[2001:db8::1]/caf%C3%A9"));
}

RUVIA_TEST(context_redirect_percent_encodes_square_brackets_outside_ip_literal) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.redirect({.location_ = "/items[0]?filter=[x]#section[1]"});
    RUVIA_CHECK_EQ(response.header("Location"),
        std::string_view("/items%5B0%5D?filter=%5Bx%5D#section%5B1%5D"));

    const auto absolute = context.redirect(
        {.location_ = std::string_view("https://[2001:db8::1]/items[0]?filter=[x]")});
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
            {.location_ = std::string_view("https://example.com/\r\nX-Injected: y")});
    } catch (const std::exception&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(context_redirect_rejects_crlf_even_when_location_needs_encoding) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    RUVIA_CHECK(throws_on([&] {
        (void)context.redirect(
            {.location_ = std::string_view("https://example.com/caf\xC3\xA9\r\nX-Injected: y")});
    }));
}

RUVIA_TEST(context_body_sets_body_and_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    context.status(ruvia::http_status::created);
    const auto response = context.body("hello world");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::created);
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("hello world"));
}

RUVIA_TEST(context_dynamic_body_owns_input_and_preserves_lvalue) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    std::pmr::string source_value("dynamic body", memory.resource());

    const auto response = context.body(source_value);
    source_value[0] = 'X';

    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("dynamic body"));
    RUVIA_CHECK_EQ(source_value, std::string_view("Xynamic body"));
}

RUVIA_TEST(context_literal_builders_keep_static_storage) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    const auto body_response = context.body("body");
    const auto text_response = context.text("text");
    const auto html_response = context.html("<b>html</b>");

    RUVIA_CHECK(body_response.body_bytes() == std::string_view("body"));
    RUVIA_CHECK(text_response.body_bytes() == std::string_view("text"));
    RUVIA_CHECK(html_response.body_bytes() == std::string_view("<b>html</b>"));
}

RUVIA_TEST(context_rejects_informational_and_non_http_final_statuses) {
    {
        RUVIA_MAKE_CONTEXT(worker, memory, request, context);
        bool threw = false;
        try {
            context.status(ruvia::http_status::early_hints);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
    }
    {
        bool threw = false;
        try {
            (void)ruvia::http_status_code::from_value(600);
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
        {.status_ = ruvia::http_status::ok, .code_ = "bad", .message_ = "not an error"});
    RUVIA_CHECK_EQ(ok.status(), ruvia::http_status::internal_server_error);
    RUVIA_CHECK_EQ(ok.header("X-Trace"), std::string_view("1"));

    const auto redirect = context.error({.status_ = ruvia::http_status::temporary_redirect,
        .code_ = "bad",
        .message_ = "not an error"});
    RUVIA_CHECK_EQ(redirect.status(), ruvia::http_status::internal_server_error);
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
    context.status(ruvia::http_status::ok);
    context.header("Content-Type", "text/plain");
    context.header("X-Custom", "v");
    const auto response = context.body("data");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("data"));
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));
    RUVIA_CHECK_EQ(response.header("X-Custom"), std::string_view("v"));
}

RUVIA_TEST(context_response_merge_keeps_complete_headers_without_allocating) {
    std::size_t move_allocations = 0;
    for (const bool with_context_headers : {false, true}) {
        ruvia::test::counting_memory_resource resource;
        {
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            if (with_context_headers) {
                context.header("Cache-Control", "no-store");
                context.header("X-Custom", "context");
                context.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
                context.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
            }
            ruvia::http_response response({.resource_ = &resource});
            response.header("Cache-Control", "private");
            response.header("X-Custom", "response");
            response.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
            response.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
            response.body(std::string(4096, 'x'));
            const auto* body_data = response.body_bytes().data();
            const auto allocations = resource.allocation_count();

            context_access::set_response(context, std::move(response));

            // Debug standard libraries may allocate iterator bookkeeping on move.
            // Reconciliation must add nothing to the plain finalization cost.
            const auto finalization_allocations = resource.allocation_count() - allocations;
            if (with_context_headers) {
                RUVIA_CHECK_EQ(finalization_allocations, move_allocations);
            } else {
                move_allocations = finalization_allocations;
            }
            RUVIA_CHECK_EQ(context.response()->body_bytes().data(), body_data);
            RUVIA_CHECK_EQ(context.response()->header("Cache-Control"), std::string_view("private"));
            RUVIA_CHECK_EQ(context.response()->header("X-Custom"), std::string_view("response"));
            RUVIA_CHECK_EQ(context.response()->headers().size(), std::size_t{4});
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(context_response_header_transactions_preserve_owned_body_storage) {
    for (const bool assigned : {false, true}) {
        ruvia::test::counting_memory_resource resource;
        {
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            context.header("X-Added", "context");
            context.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
            context.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
            ruvia::http_response response({.resource_ = &resource});
            response.header("Content-Type", "application/octet-stream");
            response.header("X-Tag", "same", {.mode_ = ruvia::http_response_header_mode::append});
            response.body(std::string(4096, 'x'));
            const auto* body_data = response.body_bytes().data();
            if (assigned) {
                context.respond(std::move(response));
            } else {
                context_access::set_response(context, std::move(response));
            }
            RUVIA_CHECK_EQ(context.response()->body_bytes().data(), body_data);
            RUVIA_CHECK_EQ(context.response()->body_bytes().size(), std::size_t{4096});
            RUVIA_CHECK_EQ(context.response()->header("X-Added"), std::string_view("context"));
            RUVIA_CHECK_EQ(context.response()->headers().size(), std::size_t{4});
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(context_response_header_transaction_failure_preserves_both_inputs) {
    for (const bool assigned : {false, true}) {
        for (const std::size_t successful_headers : {std::size_t{0}, std::size_t{1}}) {
            header_failure_resource resource;
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            context.header("X-Added", "context");
            ruvia::http_response response({.resource_ = &resource});
            response.header("X-Original", "response");
            response.body(std::string(4096, 'x'));
            const auto* body_data = response.body_bytes().data();
            resource.remaining_header_allocations_ = successful_headers;
            bool allocation_failed = false;
            try {
                if (assigned) {
                    context.respond(std::move(response));
                } else {
                    context_access::set_response(context, std::move(response));
                }
            } catch (const std::bad_alloc&) {
                allocation_failed = true;
            }
            RUVIA_CHECK(allocation_failed);
            RUVIA_CHECK_EQ(response.body_bytes().data(), body_data);
            RUVIA_CHECK_EQ(response.header("X-Original"), std::string_view("response"));
            RUVIA_CHECK(!response.header("X-Added"));
            resource.remaining_header_allocations_.reset();
            context_access::set_response(context, std::move(response));
            RUVIA_CHECK_EQ(context.response()->header("X-Added"), std::string_view("context"));
        }
    }
}

RUVIA_TEST(context_response_header_transactions_preserve_cookie_policy_and_spilled_headers) {
    for (const bool assigned : {false, true}) {
        ruvia::test::counting_memory_resource resource;
        {
            RUVIA_MAKE_CONTEXT(worker, memory, request, context);
            context.header("Set-Cookie", "session=new; Path=/");
            context.header("Cache-Control", "no-store");
            ruvia::http_response response({.resource_ = &resource});
            response.header("Set-Cookie", "session=old; Path=/");
            response.header("Set-Cookie", "other=kept; Path=/", {.mode_ = ruvia::http_response_header_mode::append});
            response.header("Content-Type", "application/octet-stream");
            response.header("Cache-Control", "private");
            for (int i = 0; i < 12; ++i) {
                response.header("X-Field-" + std::to_string(i), "value");
            }
            response.body(std::string(4096, 'x'));
            const auto* body_data = response.body_bytes().data();
            if (assigned) {
                context.respond(std::move(response));
            } else {
                context_access::set_response(context, std::move(response));
            }
            const auto& result_value = *context.response();
            RUVIA_CHECK_EQ(result_value.body_bytes().data(), body_data);
            RUVIA_CHECK_EQ(result_value.header("Content-Type"), std::string_view("application/octet-stream"));
            RUVIA_CHECK_EQ(result_value.header("Cache-Control"), std::string_view(assigned ? "no-store" : "private"));
            RUVIA_CHECK_EQ(result_value.header("Set-Cookie"), std::string_view("session=new; Path=/"));
            std::size_t cookies = 0;
            for (const auto& header : result_value.headers()) {
                if (header.name() == "Set-Cookie") {
                    ++cookies;
                }
            }
            RUVIA_CHECK_EQ(cookies, assigned ? std::size_t{1} : std::size_t{2});
            for (int i = 0; i < 12; ++i) {
                RUVIA_CHECK_EQ(result_value.header("X-Field-" + std::to_string(i)), std::string_view("value"));
            }
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(context_metadata_preserves_repeated_set_cookie_headers) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    context.header("Set-Cookie", "a=1", {.mode_ = ruvia::http_response_header_mode::append});
    context.header("Set-Cookie", "b=2", {.mode_ = ruvia::http_response_header_mode::append});
    const auto response = context.body("data");

    std::size_t set_cookie_count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++set_cookie_count;
        }
    }
    RUVIA_CHECK_EQ(set_cookie_count, std::size_t{2});
}

RUVIA_TEST(context_body_null_gives_empty_body_with_status) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    context.status(ruvia::http_status::no_content);
    const auto response = context.body(nullptr);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::no_content);
    RUVIA_CHECK(response.body_bytes().empty());
}

RUVIA_TEST(context_body_byte_span_copies_into_response_storage) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const std::byte bytes_value[] = {
        std::byte{0x00},
        std::byte{0x41},
        std::byte{0xff},
    };
    const auto response = context.body(std::span<const std::byte>(bytes_value));
    const auto body = response.body_bytes();

    RUVIA_CHECK_EQ(body.size(), std::size(bytes_value));
    RUVIA_CHECK_EQ(body[0], '\0');
    RUVIA_CHECK_EQ(body[1], 'A');
    RUVIA_CHECK_EQ(static_cast<unsigned char>(body[2]), 0xff);
    RUVIA_CHECK(body.data() != reinterpret_cast<const char*>(bytes_value));
}

RUVIA_TEST(context_text_sets_plain_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.text("hello");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain; charset=UTF-8"));
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("hello"));
}

RUVIA_TEST(context_html_sets_html_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);
    const auto response = context.html("<h1>hi</h1>");
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/html; charset=UTF-8"));
    RUVIA_CHECK_EQ(response.body_bytes(), std::string_view("<h1>hi</h1>"));
}

RUVIA_TEST(context_param_lookup_handles_unencoded_and_missing) {
    // A route-parameter lookup returns an unencoded value verbatim via the
    // zero-alloc fast path, and yields nullopt (not a false match or a crash)
    // for a name that was never captured. The encoded-decode path is covered
    // separately; this pins the two other branches of route_param().
    worker_memory worker;
    request_memory memory(worker);
    auto parsed_value = ruvia::make_parsed_http_request("GET", "/p/hello/42", {}, {}, memory.resource());
    http_request request = std::move(parsed_value.first);
    const std::string_view names[] = {"slug", "id"};
    const std::string_view values[] = {"hello", "42"};
    auto context_value = context_access::make(memory, request, "/p/:slug/:id", names, values,
        std::size(names), 0, ruvia::test::test_context_services());

    const auto slug = context_value.req().param("slug");
    RUVIA_CHECK(slug.has_value());
    RUVIA_CHECK_EQ(*slug, std::string_view("hello"));
    const auto id = context_value.req().param("id");
    RUVIA_CHECK(id.has_value());
    RUVIA_CHECK_EQ(*id, std::string_view("42"));
    // An unknown parameter name is a clean miss.
    RUVIA_CHECK(!context_value.req().param("missing").has_value());
    // Every lookup shares the one typed parameter cache used by field binding.
    RUVIA_CHECK(context_access::route_params_materialized(context_value));
}

RUVIA_TEST(context_json_serializes_response_model_with_json_content_type) {
    RUVIA_MAKE_CONTEXT(worker, memory, request, context);

    context_json_response model({.resource_ = context.arena()});
    model.set<"number">(42);
    model.set<"boolean">(true);
    model.set<"real">(3.5);
    const auto response = context.json(model);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    RUVIA_CHECK_EQ(response.body_bytes(),
        std::string_view(R"({"number":42,"boolean":true,"real":3.5})"));
}
