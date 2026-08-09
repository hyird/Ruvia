#include "test_harness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/response/HttpResponseHeaderAccess.h"

namespace {

using ruvia::HttpResponse;

template <typename T>
concept HasCustomReasonPhraseSetter = requires(T& response) { response.status(std::uint16_t{200}, std::string_view{}); };

static_assert(!HasCustomReasonPhraseSetter<HttpResponse>);
static_assert(!std::is_copy_constructible_v<HttpResponse>);
static_assert(!std::is_copy_assignable_v<HttpResponse>);
static_assert(std::is_nothrow_move_constructible_v<HttpResponse>);
static_assert(std::is_nothrow_move_assignable_v<HttpResponse>);

template <typename T>
concept ExposesAnyRvalueResponseView = requires(T&& value) { std::move(value).headers(); } || requires(T&& value) { std::move(value).header(std::string_view{}); } || requires(T&& value) { std::move(value).begin(); } || requires(T&& value) { std::move(value).end(); } || requires(T&& value) { std::move(value).cbegin(); } || requires(T&& value) { std::move(value).cend(); };

static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponse>);
static_assert(!ExposesAnyRvalueResponseView<ruvia::HttpResponseHeaders>);
static_assert(std::same_as<decltype(std::declval<const HttpResponse&>().header(std::string_view{})), std::optional<std::string_view>>);

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

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
};

class FailingAllocationResource final : public std::pmr::memory_resource {
public:
    void failAllocationAfterSuccessfulAllocations(std::size_t count) noexcept {
        failAfter_ = count;
    }

    [[nodiscard]] std::size_t liveAllocations() const noexcept {
        return liveAllocations_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (failAfter_.has_value()) {
            if (*failAfter_ == 0) {
                failAfter_.reset();
                throw std::bad_alloc();
            }
            --*failAfter_;
        }
        ++liveAllocations_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --liveAllocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::optional<std::size_t> failAfter_;
    std::size_t liveAllocations_{0};
};

static_assert(!std::is_constructible_v<ruvia::HttpInterimResponseHead::HeaderInit, std::array<ruvia::HttpHeaderView, 1>&&>);
static_assert(!std::is_constructible_v<ruvia::HttpInterimResponseHead::HeaderInit, const std::vector<ruvia::HttpHeaderView>&>);
static_assert(!std::is_constructible_v<ruvia::HttpInterimResponseHead::HeaderInit, std::initializer_list<ruvia::HttpHeaderView>>);

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
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    response.status(ruvia::http_status::kNotFound);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kNotFound);
    response.status(ruvia::HttpStatusCode::fromValue(599));
    RUVIA_CHECK_EQ(response.status(), ruvia::HttpStatusCode::fromValue(599));
    response.status(ruvia::HttpStatusCode::fromValue(299));
    RUVIA_CHECK_EQ(response.status(), ruvia::HttpStatusCode::fromValue(299));
}

RUVIA_TEST(response_header_distinguishes_missing_from_present_empty) {
    auto response = makeResponse();
    RUVIA_CHECK(!response.header("X-Empty").has_value());

    response.header("X-Empty", "");
    const auto presentEmpty = response.header("x-empty");
    RUVIA_CHECK(presentEmpty.has_value());
    RUVIA_CHECK(presentEmpty.value_or("missing").empty());
}

RUVIA_TEST(response_header_spill_failure_releases_unpublished_header) {
    FailingAllocationResource resource;
    {
        HttpResponse response(&resource);
        constexpr const char* existingNames[] = {
            "X-Ruvia-Existing-0",
            "X-Ruvia-Existing-1",
            "X-Ruvia-Existing-2",
            "X-Ruvia-Existing-3",
            "X-Ruvia-Existing-4",
            "X-Ruvia-Existing-5",
            "X-Ruvia-Existing-6",
            "X-Ruvia-Existing-7",
        };
        for (const auto* name : existingNames) {
            response.header(name, "value");
        }
        const auto liveBeforeFailure = resource.liveAllocations();

        // The ninth header first owns its bytes, then asks the header table to
        // spill. Reject that table allocation and verify the descriptor's
        // already-owned bytes are not stranded by the failed append.
        // Let the new descriptor allocate its owned bytes, then reject the
        // following heap-table allocation.
        resource.failAllocationAfterSuccessfulAllocations(1);
        bool failed = false;
        try {
            response.header("X-Ruvia-New", "value");
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.liveAllocations(), liveBeforeFailure);
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{8});

        // The response remains usable after the failed publication. A retry
        // must publish exactly one new header, not duplicate an inline entry or
        // retain a dangling ownership copy from the aborted spill.
        response.header("X-Ruvia-New", "value");
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{9});
    }
    RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(response_header_rejects_unrepresentable_storage_before_scanning) {
    auto response = makeResponse();
    constexpr auto maxDescriptorSize = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());

    // The view deliberately points at one byte but advertises the maximum
    // representable name length. The setter must reject the aggregate before
    // header-name grammar validation touches the view or allocates storage.
    const std::string_view oversizedName("x", maxDescriptorSize);
    bool rejected = false;
    try {
        response.header(oversizedName, "v");
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(response.headers().empty());

    // The same guard applies to a value and to the append path, not only the
    // first replacement insertion.
    const std::string_view oversizedValue("x", maxDescriptorSize);
    rejected = false;
    try {
        response.header("X-Large", oversizedValue, HttpResponse::HeaderOptions{true});
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(response.headers().empty());
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

    RUVIA_CHECK_EQ(targetResource.allocations(), targetAllocationsBeforeAssignment);

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
    RUVIA_CHECK_EQ(targetResource.allocations(), targetAllocationsBeforeAssignment);
    RUVIA_CHECK(sourceResource.allocations() > 0);
}

RUVIA_TEST(response_moved_from_known_header_index_is_cleared) {
    HttpResponse source(std::pmr::new_delete_resource());
    source.header("X-Prefix", "keeps known header off slot zero");
    source.header("Content-Type", "text/plain");

    HttpResponse moved(std::move(source));
    RUVIA_CHECK_EQ(moved.header("Content-Type"), std::string_view("text/plain"));

    // The moved-from object no longer owns any headers. Its known-header index
    // must be cleared with the header table; otherwise an indexed lookup can
    // read a stale inline descriptor that was memcpy-moved into `moved`.
    RUVIA_CHECK(!source.header("Content-Type").has_value());
}

RUVIA_TEST(response_status_code_range_validated) {
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(ruvia::http_status::kContinue); }));
    RUVIA_CHECK(throwsInvalid([&] { response.status(ruvia::HttpStatusCode::fromValue(199)); }));
    RUVIA_CHECK(!throwsInvalid([&] { response.status(ruvia::http_status::kOk); }));                // lower boundary
    RUVIA_CHECK(!throwsInvalid([&] { response.status(ruvia::HttpStatusCode::fromValue(599)); }));  // upper boundary
}

RUVIA_TEST(response_switching_protocols_requires_a_dedicated_driver) {
    auto response = makeResponse();
    RUVIA_CHECK(throwsInvalid([&] { response.status(ruvia::http_status::kSwitchingProtocols); }));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
}

RUVIA_TEST(interim_response_head_owns_the_non_switching_1xx_status_space) {
    const ruvia::HttpHeaderView headers[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::HttpInterimResponseHead earlyHints(ruvia::http_status::kEarlyHints, headers);
    RUVIA_CHECK_EQ(earlyHints.status(), ruvia::http_status::kEarlyHints);
    RUVIA_CHECK_EQ(earlyHints.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(earlyHints.headers()[0].name(), std::string_view("Link"));

    for (const ruvia::HttpStatusCode status : {ruvia::http_status::kSwitchingProtocols, ruvia::http_status::kOk, ruvia::HttpStatusCode::fromValue(599)}) {
        RUVIA_CHECK(throwsInvalid([&] { (void)ruvia::HttpInterimResponseHead(status); }));
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
    response.removeHeader("X-Test");
    RUVIA_CHECK(!response.header("X-Test").has_value());
}

RUVIA_TEST(response_set_cookie_append_replaces_same_wire_name) {
    auto response = makeResponse();
    response.header("Set-Cookie", "session=old; Path=/");
    response.header("Set-Cookie", "theme=dark; Path=/", HttpResponse::HeaderOptions{true});
    response.header("Set-Cookie", "session=new; Path=/", HttpResponse::HeaderOptions{true});
    response.header("Set-Cookie", "Session=upper; Path=/", HttpResponse::HeaderOptions{true});

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

RUVIA_TEST(response_set_cookie_append_preserves_same_name_different_scope) {
    auto response = makeResponse();
    response.header("Set-Cookie", "session=root-old; Path=/");
    response.header("Set-Cookie", "session=admin; Path=/admin", HttpResponse::HeaderOptions{true});
    response.header("Set-Cookie", "session=domain-old; Path=/; Domain=.Example.COM", HttpResponse::HeaderOptions{true});
    response.header("Set-Cookie", "session=root-new; Path=/", HttpResponse::HeaderOptions{true});
    response.header("Set-Cookie", "session=domain-new; Domain=example.com; Path=/", HttpResponse::HeaderOptions{true});

    std::size_t count = 0;
    bool hasRootOld = false;
    bool hasRootNew = false;
    bool hasAdmin = false;
    bool hasDomainOld = false;
    bool hasDomainNew = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie")) {
            continue;
        }
        ++count;
        hasRootOld = hasRootOld || header.value() == "session=root-old; Path=/";
        hasRootNew = hasRootNew || header.value() == "session=root-new; Path=/";
        hasAdmin = hasAdmin || header.value() == "session=admin; Path=/admin";
        hasDomainOld = hasDomainOld || header.value() == "session=domain-old; Path=/; Domain=.Example.COM";
        hasDomainNew = hasDomainNew || header.value() == "session=domain-new; Domain=example.com; Path=/";
    }
    RUVIA_CHECK_EQ(count, std::size_t{3});
    RUVIA_CHECK(!hasRootOld);
    RUVIA_CHECK(hasRootNew);
    RUVIA_CHECK(hasAdmin);
    RUVIA_CHECK(!hasDomainOld);
    RUVIA_CHECK(hasDomainNew);
}

RUVIA_TEST(response_plain_set_collapses_prior_appended_fields) {
    auto response = makeResponse();
    response.header("Link", "</a>", HttpResponse::HeaderOptions{true});
    response.header("link", "</b>", HttpResponse::HeaderOptions{true});
    response.header("LINK", "</final>");

    std::size_t linkCount = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::detail::httpAsciiEqualsIgnoreCase(header.name(), "Link")) {
            ++linkCount;
            RUVIA_CHECK_EQ(header.value(), std::string_view("</final>"));
            RUVIA_CHECK(!ruvia::detail::responseHeaderAppend(header));
        }
    }
    RUVIA_CHECK_EQ(linkCount, std::size_t{1});

    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2", HttpResponse::HeaderOptions{true});
    response.header("Set-Cookie", "c=3");
    std::size_t cookieCount = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++cookieCount;
            RUVIA_CHECK_EQ(header.value(), std::string_view("c=3"));
        }
    }
    RUVIA_CHECK_EQ(cookieCount, std::size_t{1});
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

RUVIA_TEST(response_header_append_failure_does_not_mark_existing_header) {
    FailingAllocationResource resource;
    HttpResponse response(&resource);
    response.header("Link", "</a>; rel=preload");

    resource.failAllocationAfterSuccessfulAllocations(0);
    bool failed = false;
    try {
        response.header("Link", "</b>; rel=preload", HttpResponse::HeaderOptions{true});
    } catch (const std::bad_alloc&) {
        failed = true;
    }

    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
    RUVIA_CHECK(!ruvia::detail::responseHeaderAppend(*response.headers().begin()));

    // The failed publication must be retryable, and a successful retry must
    // mark both the retained and newly appended descriptors.
    response.header("Link", "</b>; rel=preload", HttpResponse::HeaderOptions{true});
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
    for (const auto& header : response.headers()) {
        RUVIA_CHECK(ruvia::detail::responseHeaderAppend(header));
    }
}

RUVIA_TEST(response_header_remove_known_header_rebuilds_index) {
    auto response = makeResponse();
    // Content-Type is a KNOWN header tracked by a bit in the response's header index
    // (unlike the custom X-Test above, which is unindexed). Removing it must rebuild
    // that index, or the indexed lookup fast path could still resolve to the removed
    // slot -- returning a stale value.
    response.header("Content-Type", "text/plain");
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));

    response.removeHeader("Content-Type");
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

    RUVIA_CHECK(throwsInvalid([&] { response.header("Content-Length", "5", HttpResponse::HeaderOptions{true}); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("Transfer-Encoding", "chunked", HttpResponse::HeaderOptions{true}); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("Location", "/next", HttpResponse::HeaderOptions{true}); }));

    RUVIA_CHECK(!throwsInvalid([&] { response.header("Content-Length", "5"); }));
    RUVIA_CHECK(!throwsInvalid([&] { response.header("Set-Cookie", "a=1", HttpResponse::HeaderOptions{true}); }));
}

RUVIA_TEST(response_header_append_rejects_single_value_headers) {
    auto response = makeResponse();

    RUVIA_CHECK(throwsInvalid([&] { response.header("Content-Type", "text/plain", HttpResponse::HeaderOptions{true}); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("ETag", "\"abc\"", HttpResponse::HeaderOptions{true}); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("Access-Control-Allow-Origin", "https://example.com", HttpResponse::HeaderOptions{true}); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("Server", "ruvia", HttpResponse::HeaderOptions{true}); }));

    RUVIA_CHECK(!throwsInvalid([&] { response.header("Vary", "Origin", HttpResponse::HeaderOptions{true}); }));
    RUVIA_CHECK(!throwsInvalid([&] { response.header("Cache-Control", "no-store", HttpResponse::HeaderOptions{true}); }));
    RUVIA_CHECK(!throwsInvalid([&] { response.header("Set-Cookie", "a=1", HttpResponse::HeaderOptions{true}); }));
}

RUVIA_TEST(response_header_rejects_invalid_content_type_syntax) {
    auto response = makeResponse();

    for (const std::string_view invalid : {"not a media type", "text/", "*/plain", "text/plain; charset"}) {
        RUVIA_CHECK(throwsInvalid([&] { response.header("Content-Type", invalid); }));
    }

    RUVIA_CHECK(!throwsInvalid([&] { response.header("Content-Type", "application/json; charset=utf-8"); }));
    RUVIA_CHECK_EQ(response.header("Content-Type").value_or(std::string_view{}), std::string_view("application/json; charset=utf-8"));
}

RUVIA_TEST(response_header_rejects_invalid_content_encoding_syntax) {
    auto response = makeResponse();

    for (const std::string_view invalid : {"gzip;level=9", "bad coding", "gzip/deflate", "", ",gzip", "gzip,", "gzip,,br"}) {
        RUVIA_CHECK(throwsInvalid([&] { response.header("Content-Encoding", invalid); }));
    }

    for (const std::string_view valid : {"deflate", "gzip, br"}) {
        RUVIA_CHECK(!throwsInvalid([&] { response.header("Content-Encoding", valid); }));
    }
}

RUVIA_TEST(response_header_rejects_invalid_trailer_field_names) {
    auto response = makeResponse();

    for (const std::string_view invalid : {"Content-Length", "X-Checksum, bad field", ","}) {
        RUVIA_CHECK(throwsInvalid([&] { response.header("Trailer", invalid); }));
        RUVIA_CHECK(throwsInvalid([&] { response.header("Trailer", invalid, HttpResponse::HeaderOptions{true}); }));
    }

    RUVIA_CHECK(!throwsInvalid([&] { response.header("Trailer", "ETag, X-Checksum"); }));
    RUVIA_CHECK_EQ(response.header("Trailer"), std::string_view("ETag, X-Checksum"));

    RUVIA_CHECK(!throwsInvalid([&] { response.header("Trailer", "Server-Timing", HttpResponse::HeaderOptions{true}); }));
}

RUVIA_TEST(response_header_rejects_name_and_value_injection) {
    auto response = makeResponse();
    // The developer-facing header setter is the header-injection chokepoint: a CR or
    // LF in the value (the classic response-splitting vector) is rejected rather than
    // written into the response head.
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\r\nInjected: x", 14)); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\nb", 3)); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\rb", 3)); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\0b", 3)); }));  // NUL

    // The name is validated too: a CR/LF or a non-token byte (space) is rejected.
    RUVIA_CHECK(throwsInvalid([&] { response.header(std::string_view("Bad\r\nName", 9), "v"); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("Bad Name", "v"); }));

    // The append path funnels through the same validation, not just replace.
    RUVIA_CHECK(throwsInvalid([&] { response.header("X-Foo", std::string_view("a\r\nb", 4), HttpResponse::HeaderOptions{true}); }));

    // A clean header is accepted.
    RUVIA_CHECK(!throwsInvalid([&] { response.header("X-Clean", "ok"); }));
    RUVIA_CHECK_EQ(response.header("X-Clean"), std::string_view("ok"));
}

RUVIA_TEST(response_header_rejects_invalid_connection_control_lists) {
    auto response = makeResponse();
    for (const auto value : {"close,", ", Upgrade", "close;invalid", ""}) {
        RUVIA_CHECK(throwsInvalid([&] { response.header("Connection", value); }));
    }
    for (const auto value : {"Content-Length", "Date", "Trailer", "Authorization", "Cookie", "Range"}) {
        RUVIA_CHECK(throwsInvalid([&] { response.header("Connection", value); }));
    }
    for (const auto value : {"websocket/", ", websocket", ""}) {
        RUVIA_CHECK(throwsInvalid([&] { response.header("Upgrade", value); }));
    }
    RUVIA_CHECK(throwsInvalid([&] { response.header("TE", "trailers"); }));
    RUVIA_CHECK(throwsInvalid([&] { response.header("TE", "trailers", HttpResponse::HeaderOptions{true}); }));

    RUVIA_CHECK(!throwsInvalid([&] {
        response.header("Connection", "keep-alive, Upgrade");
        response.header("Upgrade", "custom/1, websocket");
    }));
}
