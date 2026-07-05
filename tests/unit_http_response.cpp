#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/HttpResponse.h"
#include "http/HttpResponseHeaderAccess.h"

namespace {

using ruvia::HttpResponse;

HttpResponse makeResponse() {
    return HttpResponse(std::pmr::new_delete_resource());
}

template <typename Fn>
bool throwsInvalid(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(response_default_status_text) {
    auto response = makeResponse();
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("OK"));
    response.status(404);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{404});
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Not Found"));
    // Unknown codes fall back by class.
    response.status(599);
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Internal Server Error"));
    response.status(299);
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Bad Request"));
}

RUVIA_TEST(response_custom_status_text_and_normalization) {
    auto response = makeResponse();
    response.status(404, "Totally Missing");
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Totally Missing"));
    // Explicitly passing the default phrase normalizes back to the default.
    response.status(404, "Not Found");
    RUVIA_CHECK_EQ(response.statusText(), std::string_view("Not Found"));
}

RUVIA_TEST(response_status_code_range_validated) {
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(99); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(1000); }));
    RUVIA_CHECK(!throwsInvalid([&] { response.status(100); }));  // lower boundary
    RUVIA_CHECK(!throwsInvalid([&] { response.status(999); }));  // upper boundary
}

RUVIA_TEST(response_status_text_rejects_injection) {
    // A custom reason phrase must not carry response-splitting bytes.
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(200, std::string_view("a\r\nInjected: x", 14)); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(200, std::string_view("a\nb", 3)); }));
}

RUVIA_TEST(response_header_replace_append_and_remove) {
    auto response = makeResponse();

    // A plain set replaces: the latest value wins as a single header.
    response.header("X-Test", "first");
    response.header("X-Test", "second");
    RUVIA_CHECK_EQ(response.header("X-Test"), std::string_view("second"));

    // append=true emits an additional header line (needed for multi-valued
    // fields like Set-Cookie).
    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2", HttpResponse::HeaderOptions{true});
    std::size_t setCookieCount = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++setCookieCount;
        }
    }
    RUVIA_CHECK_EQ(setCookieCount, std::size_t{2});

    // Passing nullopt removes the header entirely.
    response.header("X-Test", std::nullopt);
    RUVIA_CHECK(response.header("X-Test").empty());
}

RUVIA_TEST(response_appended_header_carries_append_flag) {
    auto response = makeResponse();

    // Appending a non-Set-Cookie multi-valued field (here Link) must mark every
    // entry with the append flag. The flag is what a later merge of this response
    // -- a Context response slot folded into a factory response via
    // mergeResponseSlotHeaders -- consults to keep all values; without it the merge
    // sees append=false, treats the field as single-valued, and drops every line
    // but the first. appendHeaderValidated previously left the flag unset (only the
    // Context header list marked it), so a slot-carried Link/Vary/WWW-Authenticate
    // collapsed on merge.
    response.header("Link", "</a>; rel=preload", HttpResponse::HeaderOptions{true});
    response.header("Link", "</b>; rel=preload", HttpResponse::HeaderOptions{true});

    std::size_t linkCount = 0;
    std::size_t appendMarked = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Link")) {
            ++linkCount;
            if (ruvia::detail::responseHeaderAppend(header)) {
                ++appendMarked;
            }
        }
    }
    RUVIA_CHECK_EQ(linkCount, std::size_t{2});
    RUVIA_CHECK_EQ(appendMarked, std::size_t{2});
}

RUVIA_TEST(response_header_remove_known_header_rebuilds_index) {
    auto response = makeResponse();
    // Content-Type is a KNOWN header tracked by a bit in the response's header index
    // (unlike the custom X-Test above, which is unindexed). Removing it must rebuild
    // that index, or the indexed lookup fast path could still resolve to the removed
    // slot -- returning a stale value.
    response.header("Content-Type", "text/plain");
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));

    response.header("Content-Type", std::nullopt);
    RUVIA_CHECK(response.header("Content-Type").empty());   // gone, not a stale index hit

    // Re-adding after removal replaces cleanly and leaves exactly one header line --
    // no duplicate resurrected from a stale index entry.
    response.header("Content-Type", "application/json");
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("application/json"));
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Content-Type")) {
            ++count;
        }
    }
    RUVIA_CHECK_EQ(count, std::size_t{1});
}

RUVIA_TEST(response_header_append_rejects_body_framing_headers) {
    auto response = makeResponse();

    RUVIA_CHECK(throwsInvalid([&] {
        response.header("Content-Length", "5", HttpResponse::HeaderOptions{true});
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        response.header("Transfer-Encoding", "chunked", HttpResponse::HeaderOptions{true});
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        response.header("Location", "/next", HttpResponse::HeaderOptions{true});
    }));

    RUVIA_CHECK(!throwsInvalid([&] {
        response.header("Content-Length", "5");
    }));
    RUVIA_CHECK(!throwsInvalid([&] {
        response.header("Set-Cookie", "a=1", HttpResponse::HeaderOptions{true});
    }));
}

RUVIA_TEST(response_header_append_rejects_single_value_headers) {
    auto response = makeResponse();

    RUVIA_CHECK(throwsInvalid([&] {
        response.header("Content-Type", "text/plain", HttpResponse::HeaderOptions{true});
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        response.header("ETag", "\"abc\"", HttpResponse::HeaderOptions{true});
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        response.header("Access-Control-Allow-Origin", "https://example.com", HttpResponse::HeaderOptions{true});
    }));
    RUVIA_CHECK(throwsInvalid([&] {
        response.header("Server", "ruvia", HttpResponse::HeaderOptions{true});
    }));

    RUVIA_CHECK(!throwsInvalid([&] {
        response.header("Vary", "Origin", HttpResponse::HeaderOptions{true});
    }));
    RUVIA_CHECK(!throwsInvalid([&] {
        response.header("Cache-Control", "no-store", HttpResponse::HeaderOptions{true});
    }));
    RUVIA_CHECK(!throwsInvalid([&] {
        response.header("Set-Cookie", "a=1", HttpResponse::HeaderOptions{true});
    }));
}

RUVIA_TEST(response_header_rejects_name_and_value_injection) {
    auto response = makeResponse();
    // The developer-facing header setter is the header-injection chokepoint: a CR or
    // LF in the value (the classic response-splitting vector) is rejected rather than
    // written into the response head.
    RUVIA_CHECK(throwsInvalid([&] {
        response.header("X-Foo", std::string_view("a\r\nInjected: x", 14));
    }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\nb", 3)); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\rb", 3)); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\0b", 3)); }));  // NUL

    // The name is validated too: a CR/LF or a non-token byte (space) is rejected.
    RUVIA_CHECK(throwsInvalid([&] { response.header(std::string_view("Bad\r\nName", 9), "v"); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("Bad Name", "v"); }));

    // The append path funnels through the same validation, not just replace.
    RUVIA_CHECK(throwsInvalid([&] {
        response.header("X-Foo", std::string_view("a\r\nb", 4), HttpResponse::HeaderOptions{true});
    }));

    // A clean header is accepted.
    RUVIA_CHECK(!throwsInvalid([&] { response.header("X-Clean", "ok"); }));
    RUVIA_CHECK_EQ(response.header("X-Clean"), std::string_view("ok"));
}
