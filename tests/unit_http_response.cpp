#include "test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/HttpResponseHeaderAccess.h"

namespace {

using ruvia::HttpResponse;

template <typename T>
concept HasCustomReasonPhraseSetter = requires(T& response) {
    response.status(std::uint16_t{200}, std::string_view{});
};

static_assert(!HasCustomReasonPhraseSetter<HttpResponse>);
static_assert(!std::is_copy_constructible_v<HttpResponse>);
static_assert(!std::is_copy_assignable_v<HttpResponse>);
static_assert(std::is_nothrow_move_constructible_v<HttpResponse>);
static_assert(std::is_nothrow_move_assignable_v<HttpResponse>);

template <typename T>
concept ExposesAnyRvalueResponseView =
    requires(T&& value) { std::move(value).headers(); } ||
    requires(T&& value) { std::move(value).header(std::string_view{}); } ||
    requires(T&& value) { std::move(value).begin(); } ||
    requires(T&& value) { std::move(value).end(); } ||
    requires(T&& value) { std::move(value).cbegin(); } ||
    requires(T&& value) { std::move(value).cend(); };

static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponse>);
static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponseHeaders>);
static_assert(std::same_as<
    decltype(std::declval<const HttpResponse&>().header(std::string_view{})),
    std::optional<std::string_view>>);

class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(
        void* pointer,
        std::size_t bytes,
        std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(
        const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
};

static_assert(!std::is_constructible_v<
    ruvia::HttpInterimResponseHead::HeaderInit,
    std::array<ruvia::HttpHeaderView, 1>&&>);
static_assert(!std::is_constructible_v<
    ruvia::HttpInterimResponseHead::HeaderInit,
    const std::vector<ruvia::HttpHeaderView>&>);
static_assert(!std::is_constructible_v<
    ruvia::HttpInterimResponseHead::HeaderInit,
    std::initializer_list<ruvia::HttpHeaderView>>);

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

RUVIA_TEST(response_status_is_version_neutral_code_only) {
    auto response = makeResponse();
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
    response.status(404);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{404});
    response.status(599);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{599});
    response.status(299);
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{299});
}

RUVIA_TEST(response_header_distinguishes_missing_from_present_empty) {
    auto response = makeResponse();
    RUVIA_CHECK(!response.header("X-Empty").has_value());

    response.header("X-Empty", "");
    const auto presentEmpty = response.header("x-empty");
    RUVIA_CHECK(presentEmpty.has_value());
    RUVIA_CHECK(presentEmpty.value_or("missing").empty());
}

RUVIA_TEST(response_move_assignment_transfers_one_resource_domain) {
    CountingMemoryResource sourceResource;
    CountingMemoryResource targetResource;
    HttpResponse source(&sourceResource);
    HttpResponse target(&targetResource);

    source.body(std::string(4096, 's'));
    target.body(std::string(64, 't'));
    const auto targetAllocationsBeforeAssignment = targetResource.allocations();

    target = std::move(source);

    RUVIA_CHECK_EQ(
        targetResource.allocations(),
        targetAllocationsBeforeAssignment);

    // The inline header table spills on the ninth field. Its PMR vector must have
    // moved to the same source resource as the body and owned header bytes.
    target.header("X-Ruvia-0", "0");
    target.header("X-Ruvia-1", "1");
    target.header("X-Ruvia-2", "2");
    target.header("X-Ruvia-3", "3");
    target.header("X-Ruvia-4", "4");
    target.header("X-Ruvia-5", "5");
    target.header("X-Ruvia-6", "6");
    target.header("X-Ruvia-7", "7");
    target.header("X-Ruvia-8", "8");

    RUVIA_CHECK_EQ(target.headers().size(), std::size_t{9});
    RUVIA_CHECK_EQ(
        targetResource.allocations(),
        targetAllocationsBeforeAssignment);
    RUVIA_CHECK(sourceResource.allocations() > 0);
}

RUVIA_TEST(response_status_code_range_validated) {
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(99); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(100); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(199); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(600); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(999); }));
    RUVIA_CHECK(!throwsInvalid([&] { response.status(200); }));  // lower boundary
    RUVIA_CHECK(!throwsInvalid([&] { response.status(599); }));  // upper boundary
}

RUVIA_TEST(response_switching_protocols_requires_a_dedicated_driver) {
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(101); }));
    RUVIA_CHECK_EQ(response.status(), std::uint16_t{200});
}

RUVIA_TEST(interim_response_head_owns_the_non_switching_1xx_status_space) {
    const ruvia::HttpHeaderView headers[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::HttpInterimResponseHead earlyHints(103, headers);
    RUVIA_CHECK_EQ(earlyHints.status(), std::uint16_t{103});
    RUVIA_CHECK_EQ(earlyHints.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(earlyHints.headers()[0].name(), std::string_view("Link"));

    for (const std::uint16_t status : {
             std::uint16_t{99},
             std::uint16_t{101},
             std::uint16_t{200},
             std::uint16_t{600}}) {
        RUVIA_CHECK(throwsInvalid([&] {
            (void)ruvia::HttpInterimResponseHead(status);
        }));
    }
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
    RUVIA_CHECK(!response.header("X-Test").has_value());
}

RUVIA_TEST(response_set_cookie_append_replaces_same_wire_name) {
    auto response = makeResponse();
    response.header("Set-Cookie", "session=old; Path=/");
    response.header(
        "Set-Cookie", "theme=dark; Path=/", HttpResponse::HeaderOptions{true});
    response.header(
        "Set-Cookie", "session=new; Path=/", HttpResponse::HeaderOptions{true});
    response.header(
        "Set-Cookie", "Session=upper; Path=/", HttpResponse::HeaderOptions{true});

    std::size_t count = 0;
    bool hasOld = false;
    bool hasNew = false;
    bool hasTheme = false;
    bool hasUpper = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie")) {
            continue;
        }
        ++count;
        hasOld = hasOld || header.value() == "session=old; Path=/";
        hasNew = hasNew || header.value() == "session=new; Path=/";
        hasTheme = hasTheme || header.value() == "theme=dark; Path=/";
        hasUpper = hasUpper || header.value() == "Session=upper; Path=/";
    }
    RUVIA_CHECK_EQ(count, std::size_t{3});
    RUVIA_CHECK(!hasOld);
    RUVIA_CHECK(hasNew);
    RUVIA_CHECK(hasTheme);
    RUVIA_CHECK(hasUpper);  // cookie-name is case-sensitive
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
    RUVIA_CHECK(!response.header("Content-Type").has_value());  // gone, not a stale index hit

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

RUVIA_TEST(response_header_rejects_invalid_connection_control_lists) {
    auto response = makeResponse();
    for (const auto value : {"close,", ", Upgrade", "close;invalid", ""}) {
        RUVIA_CHECK(throwsInvalid([&] {
            response.header("Connection", value);
        }));
    }
    for (const auto value : {"websocket/", ", websocket", ""}) {
        RUVIA_CHECK(throwsInvalid([&] {
            response.header("Upgrade", value);
        }));
    }

    RUVIA_CHECK(!throwsInvalid([&] {
        response.header("Connection", "keep-alive, Upgrade");
        response.header("Upgrade", "custom/1, websocket");
    }));
}
