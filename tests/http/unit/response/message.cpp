#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_response.h"

#include "response/http_response_header_access.h"
#include "test_harness.h"

namespace {

using ruvia::http_response;

class counting_memory_resource final : public std::pmr::memory_resource {
public:
    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::size_t allocations_{0};
};

class failing_allocation_resource final : public std::pmr::memory_resource {
public:
    void fail_allocation_after_successful_allocations(std::size_t count) noexcept {
        fail_after_ = count;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_allocations_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (fail_after_.has_value()) {
            if (*fail_after_ == 0) {
                fail_after_.reset();
                throw std::bad_alloc();
            }
            --*fail_after_;
        }
        ++live_allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        --live_allocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::optional<std::size_t> fail_after_;
    std::size_t live_allocations_{0};
};

http_response make_response() {
    return http_response({.resource_ = std::pmr::new_delete_resource()});
}

template <typename fn_type>
bool throws_invalid(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(response_header_clone_shares_static_storage_and_isolates_owned_values) {
    counting_memory_resource resource;
    http_response empty({.resource_ = &resource});
    const auto before_empty_clone = resource.allocations();
    auto empty_clone = ruvia::detail::http_response_header_state_access::clone_headers_for_transaction(empty, 0);
    const auto empty_clone_allocations = resource.allocations() - before_empty_clone;
    RUVIA_CHECK(empty_clone.headers().empty());

    http_response source_value({.resource_ = &resource});
    source_value.header_stable_view("Content-Type", "application/json");
    source_value.header("X-Owned", "original");
    const auto* static_value = source_value.header("Content-Type")->data();
    const auto before_clone = resource.allocations();
    auto clone = ruvia::detail::http_response_header_state_access::clone_headers_for_transaction(source_value, 0);
    // Only the dynamic field needs byte storage, independent of any Debug
    // container bookkeeping already measured by the empty clone.
    RUVIA_CHECK_EQ(resource.allocations() - before_clone, empty_clone_allocations + 1);
    RUVIA_CHECK_EQ(clone.header("Content-Type")->data(), static_value);
    RUVIA_CHECK(clone.header("X-Owned")->data() != source_value.header("X-Owned")->data());
    source_value.header("X-Owned", "changed!");
    source_value.remove_header("Content-Type");
    RUVIA_CHECK_EQ(clone.header("X-Owned"), std::string_view("original"));
    RUVIA_CHECK_EQ(clone.header("Content-Type"), std::string_view("application/json"));
    clone.header("Content-Type", "text/plain");
    RUVIA_CHECK_EQ(clone.header("Content-Type"), std::string_view("text/plain"));
}

RUVIA_TEST(response_header_append_preserves_multiplicity_across_table_spill) {
    http_response response;
    response.header("X-Repeated", "first");
    for (int i = 0; i < 7; ++i) {
        response.header("X-Filler-" + std::to_string(i), "value");
    }
    response.header("x-repeated", "second", {.mode_ = ruvia::http_response_header_mode::append});
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{9});
    std::size_t marked = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::detail::http_ascii_equals_ignore_case(header.name(), "X-Repeated")) {
            RUVIA_CHECK(ruvia::detail::response_header_append(header));
            ++marked;
        }
    }
    RUVIA_CHECK_EQ(marked, std::size_t{2});
    response.header("X-Repeated", "replacement");
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{8});
    RUVIA_CHECK_EQ(response.header("X-Repeated"), std::string_view("replacement"));
}

RUVIA_TEST(response_status_is_version_neutral_code_only) {
    auto response = make_response();
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    response.status(ruvia::http_status::not_found);
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::not_found);
    response.status(ruvia::http_status_code::from_value(599));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status_code::from_value(599));
    response.status(ruvia::http_status_code::from_value(299));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status_code::from_value(299));
}

RUVIA_TEST(response_header_distinguishes_missing_from_present_empty) {
    auto response = make_response();
    RUVIA_CHECK(!response.header("X-Empty").has_value());

    response.header("X-Empty", "");
    const auto present_empty = response.header("x-empty");
    RUVIA_CHECK(present_empty.has_value());
    RUVIA_CHECK(present_empty.value_or("missing").empty());
}

RUVIA_TEST(response_header_spill_failure_releases_unpublished_header) {
    failing_allocation_resource resource;
    {
        http_response response({.resource_ = &resource});
        constexpr const char* existing_names[] = {
            "X-Ruvia-Existing-0",
            "X-Ruvia-Existing-1",
            "X-Ruvia-Existing-2",
            "X-Ruvia-Existing-3",
            "X-Ruvia-Existing-4",
            "X-Ruvia-Existing-5",
            "X-Ruvia-Existing-6",
            "X-Ruvia-Existing-7",
        };
        for (const auto* name : existing_names) {
            response.header(name, "value");
        }
        const auto live_before_failure = resource.live_allocations();

        // The ninth header first owns its bytes, then asks the header table to
        // spill. Reject that table allocation and verify the descriptor's
        // already-owned bytes are not stranded by the failed append.
        // Let the new descriptor allocate its owned bytes, then reject the
        // following heap-table allocation.
        resource.fail_allocation_after_successful_allocations(1);
        bool failed = false;
        try {
            response.header("X-Ruvia-New", "value");
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.live_allocations(), live_before_failure);
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{8});

        // The response remains usable after the failed publication. A retry
        // must publish exactly one new header, not duplicate an inline entry or
        // retain a dangling ownership copy from the aborted spill.
        response.header("X-Ruvia-New", "value");
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{9});
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(response_header_rejects_unrepresentable_storage_before_scanning) {
    auto response = make_response();
    constexpr auto max_descriptor_size =
        static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());

    // The view deliberately points at one byte but advertises the maximum
    // representable name length. The setter must reject the aggregate before
    // header-name grammar validation touches the view or allocates storage.
    const std::string_view oversized_name("x", max_descriptor_size);
    bool rejected = false;
    try {
        response.header(oversized_name, "v");
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(response.headers().empty());

    // The same guard applies to a value and to the append path, not only the
    // first replacement insertion.
    const std::string_view oversized_value("x", max_descriptor_size);
    rejected = false;
    try {
        response.header("X-Large", oversized_value,
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(response.headers().empty());
}

RUVIA_TEST(response_move_assignment_transfers_one_resource_domain) {
    counting_memory_resource source_resource;
    counting_memory_resource target_resource;
    http_response source_value({.resource_ = &source_resource});
    http_response target({.resource_ = &target_resource});

    source_value.body(std::string(4096, 's'));
    target.body(std::string(64, 't'));
    const auto target_allocations_before_assignment = target_resource.allocations();

    target = std::move(source_value);

    RUVIA_CHECK_EQ(target_resource.allocations(), target_allocations_before_assignment);

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
    RUVIA_CHECK_EQ(target_resource.allocations(), target_allocations_before_assignment);
    RUVIA_CHECK(source_resource.allocations() > 0);
}

RUVIA_TEST(response_moved_from_known_header_index_is_cleared) {
    http_response source_value({.resource_ = std::pmr::new_delete_resource()});
    source_value.header("X-Prefix", "keeps known header off slot zero");
    source_value.header("Content-Type", "text/plain");

    http_response moved(std::move(source_value));
    RUVIA_CHECK_EQ(moved.header("Content-Type"), std::string_view("text/plain"));

    // The moved-from object no longer owns any headers. Its known-header index
    // must be cleared with the header table; otherwise an indexed lookup can
    // read a stale inline descriptor that was memcpy-moved into `moved`.
    RUVIA_CHECK(!source_value.header("Content-Type").has_value());
}

RUVIA_TEST(response_status_code_range_validated) {
    auto response = make_response();
    RUVIA_CHECK(throws_invalid([&] { response.status(ruvia::http_status::continue_value); }));
    RUVIA_CHECK(throws_invalid([&] { response.status(ruvia::http_status_code::from_value(199)); }));
    RUVIA_CHECK(
        !throws_invalid([&] { response.status(ruvia::http_status::ok); }));  // lower boundary
    RUVIA_CHECK(!throws_invalid(
        [&] { response.status(ruvia::http_status_code::from_value(599)); }));  // upper boundary
}

RUVIA_TEST(response_switching_protocols_requires_a_dedicated_driver) {
    auto response = make_response();
    RUVIA_CHECK(throws_invalid([&] { response.status(ruvia::http_status::switching_protocols); }));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
}

RUVIA_TEST(interim_response_head_owns_the_non_switching_1xx_status_space) {
    const ruvia::http_header_view headers[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::http_interim_response_head early_hints(ruvia::http_status::early_hints, headers);
    RUVIA_CHECK_EQ(early_hints.status(), ruvia::http_status::early_hints);
    RUVIA_CHECK_EQ(early_hints.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(early_hints.headers()[0].name(), std::string_view("Link"));

    for (const ruvia::http_status_code status : {ruvia::http_status::switching_protocols,
             ruvia::http_status::ok, ruvia::http_status_code::from_value(599)}) {
        RUVIA_CHECK(throws_invalid([&] { (void)ruvia::http_interim_response_head(status); }));
    }
}

RUVIA_TEST(response_header_replace_append_and_remove) {
    auto response = make_response();

    // A plain set replaces: the latest value wins as a single header.
    response.header("X-Test", "first");
    response.header("X-Test", "second");
    RUVIA_CHECK_EQ(response.header("X-Test"), std::string_view("second"));

    // append emits an additional header line (needed for multi-valued fields
    // like Set-Cookie).
    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    std::size_t set_cookie_count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++set_cookie_count;
        }
    }
    RUVIA_CHECK_EQ(set_cookie_count, std::size_t{2});

    // Passing nullopt removes the header entirely.
    response.remove_header("X-Test");
    RUVIA_CHECK(!response.header("X-Test").has_value());
}

RUVIA_TEST(response_header_rejects_invalid_write_mode) {
    auto response = make_response();
    RUVIA_CHECK(throws_invalid([&] {
        response.header("X-Test", "value",
            http_response::header_options_type{.mode_ = static_cast<ruvia::http_response_header_mode>(255)});
    }));
    RUVIA_CHECK(response.headers().empty());
}

RUVIA_TEST(response_set_cookie_append_replaces_same_wire_name) {
    auto response = make_response();
    response.header("Set-Cookie", "session=old; Path=/");
    response.header("Set-Cookie", "theme=dark; Path=/",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=new; Path=/",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "Session=upper; Path=/",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});

    std::size_t count = 0;
    bool has_old = false;
    bool has_new = false;
    bool has_theme = false;
    bool has_upper = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie")) {
            continue;
        }
        ++count;
        has_old = has_old || header.value() == "session=old; Path=/";
        has_new = has_new || header.value() == "session=new; Path=/";
        has_theme = has_theme || header.value() == "theme=dark; Path=/";
        has_upper = has_upper || header.value() == "Session=upper; Path=/";
    }
    RUVIA_CHECK_EQ(count, std::size_t{3});
    RUVIA_CHECK(!has_old);
    RUVIA_CHECK(has_new);
    RUVIA_CHECK(has_theme);
    RUVIA_CHECK(has_upper);  // cookie-name is case-sensitive
}

RUVIA_TEST(response_set_cookie_append_preserves_same_name_different_scope) {
    auto response = make_response();
    response.header("Set-Cookie", "session=root-old; Path=/");
    response.header("Set-Cookie", "session=admin; Path=/admin",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=domain-old; Path=/; Domain=.Example.COM",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=root-new; Path=/",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=domain-new; Domain=example.com; Path=/",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});

    std::size_t count = 0;
    bool has_root_old = false;
    bool has_root_new = false;
    bool has_admin = false;
    bool has_domain_old = false;
    bool has_domain_new = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie")) {
            continue;
        }
        ++count;
        has_root_old = has_root_old || header.value() == "session=root-old; Path=/";
        has_root_new = has_root_new || header.value() == "session=root-new; Path=/";
        has_admin = has_admin || header.value() == "session=admin; Path=/admin";
        has_domain_old =
            has_domain_old || header.value() == "session=domain-old; Path=/; Domain=.Example.COM";
        has_domain_new =
            has_domain_new || header.value() == "session=domain-new; Domain=example.com; Path=/";
    }
    RUVIA_CHECK_EQ(count, std::size_t{3});
    RUVIA_CHECK(!has_root_old);
    RUVIA_CHECK(has_root_new);
    RUVIA_CHECK(has_admin);
    RUVIA_CHECK(!has_domain_old);
    RUVIA_CHECK(has_domain_new);
}

RUVIA_TEST(response_plain_set_collapses_prior_appended_fields) {
    auto response = make_response();
    response.header("Link", "</a>",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("link", "</b>",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("LINK", "</final>");

    std::size_t link_count = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::detail::http_ascii_equals_ignore_case(header.name(), "Link")) {
            ++link_count;
            RUVIA_CHECK_EQ(header.value(), std::string_view("</final>"));
            RUVIA_CHECK(!ruvia::detail::response_header_append(header));
        }
    }
    RUVIA_CHECK_EQ(link_count, std::size_t{1});

    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "c=3");
    std::size_t cookie_count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++cookie_count;
            RUVIA_CHECK_EQ(header.value(), std::string_view("c=3"));
        }
    }
    RUVIA_CHECK_EQ(cookie_count, std::size_t{1});
}

RUVIA_TEST(response_appended_header_carries_append_flag) {
    auto response = make_response();

    // Appending a non-Set-Cookie multi-valued field (here Link) must mark every
    // entry with the append flag. The flag is what a later merge of this response
    // -- a context response slot folded into a factory response via
    // mergeResponseSlotHeaders -- consults to keep all values; without it the
    // merge sees a non-append entry, treats the field as single-valued, and
    // drops every line but the first. append_header_validated previously left the
    // flag unset (only the context header list marked it), so a slot-carried
    // Link/Vary/WWW-Authenticate
    // collapsed on merge.
    response.header("Link", "</a>; rel=preload",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Link", "</b>; rel=preload",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});

    std::size_t link_count = 0;
    std::size_t append_marked = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Link")) {
            ++link_count;
            if (ruvia::detail::response_header_append(header)) {
                ++append_marked;
            }
        }
    }
    RUVIA_CHECK_EQ(link_count, std::size_t{2});
    RUVIA_CHECK_EQ(append_marked, std::size_t{2});
}

RUVIA_TEST(response_header_append_failure_does_not_mark_existing_header) {
    failing_allocation_resource resource;
    http_response response({.resource_ = &resource});
    response.header("Link", "</a>; rel=preload");

    resource.fail_allocation_after_successful_allocations(0);
    bool failed = false;
    try {
        response.header("Link", "</b>; rel=preload",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    } catch (const std::bad_alloc&) {
        failed = true;
    }

    RUVIA_CHECK(failed);
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
    RUVIA_CHECK(!ruvia::detail::response_header_append(*response.headers().begin()));

    // The failed publication must be retryable, and a successful retry must
    // mark both the retained and newly appended descriptors.
    response.header("Link", "</b>; rel=preload",
        http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
    for (const auto& header : response.headers()) {
        RUVIA_CHECK(ruvia::detail::response_header_append(header));
    }
}

RUVIA_TEST(response_header_removal_preserves_unrelated_fields) {
    for (const int filler_count : {0, 8}) {
        counting_memory_resource resource;
        http_response response({.resource_ = &resource});
        response.header("Content-Type", "text/plain");
        response.header("Vary", "Accept-Encoding");
        response.header("Vary", "Origin", {.mode_ = ruvia::http_response_header_mode::append});
        for (int i = 0; i < filler_count; ++i) {
            response.header("X-Filler-" + std::to_string(i), "value");
        }
        std::vector<std::pair<std::string_view, std::string_view>> fields;
        for (const auto& header : response.headers()) {
            fields.emplace_back(header.name(), header.value());
        }
        const auto* const table_value = response.headers().begin();
        const auto allocations = resource.allocations();
        response.remove_header("content-length");
        response.remove_header("Set-Cookie");
        response.remove_header("X-Missing");
        RUVIA_CHECK_EQ(resource.allocations(), allocations);
        RUVIA_CHECK(response.headers().begin() == table_value);
        RUVIA_CHECK_EQ(response.headers().size(), fields.size());
        std::size_t index = 0;
        for (const auto& header : response.headers()) {
            RUVIA_CHECK_EQ(header.name(), fields[index].first);
            RUVIA_CHECK_EQ(header.value(), fields[index].second);
            ++index;
        }
        RUVIA_CHECK_EQ(response.header("content-type"), std::string_view("text/plain"));
        RUVIA_CHECK_EQ(response.header("vary"), std::string_view("Accept-Encoding"));
        RUVIA_CHECK(!response.header("Content-Length").has_value());

        // A present empty known field is still removed, and removing an
        // appended known field removes every occurrence without stale indexes.
        response.header("Content-Length", "");
        RUVIA_CHECK(response.header("Content-Length").has_value());
        response.remove_header("CONTENT-LENGTH");
        RUVIA_CHECK(!response.header("Content-Length").has_value());
        response.remove_header("vary");
        RUVIA_CHECK(!response.header("Vary").has_value());
        RUVIA_CHECK_EQ(response.headers().size(), fields.size() - 2);
        RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));
        response.remove_header("Vary");
        RUVIA_CHECK_EQ(response.headers().size(), fields.size() - 2);
    }
}

RUVIA_TEST(response_header_remove_known_header_rebuilds_index) {
    auto response = make_response();
    // Content-Type is a KNOWN header tracked by a bit in the response's header index
    // (unlike the custom X-Test above, which is unindexed). Removing it must rebuild
    // that index, or the indexed lookup fast path could still resolve to the removed
    // slot -- returning a stale value.
    response.header("Content-Type", "text/plain");
    RUVIA_CHECK_EQ(response.header("Content-Type"), std::string_view("text/plain"));

    response.remove_header("Content-Type");
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
    auto response = make_response();

    RUVIA_CHECK(throws_invalid([&] {
        response.header("Content-Length", "5",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
    RUVIA_CHECK(throws_invalid([&] {
        response.header("Transfer-Encoding", "chunked",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
    RUVIA_CHECK(throws_invalid([&] {
        response.header("Location", "/next",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));

    RUVIA_CHECK(!throws_invalid([&] { response.header("Content-Length", "5"); }));
    RUVIA_CHECK(!throws_invalid([&] {
        response.header("Set-Cookie", "a=1",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

RUVIA_TEST(response_header_append_rejects_single_value_headers) {
    auto response = make_response();

    RUVIA_CHECK(throws_invalid([&] {
        response.header("Content-Type", "text/plain",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
    RUVIA_CHECK(throws_invalid([&] {
        response.header("ETag", "\"abc\"",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
    RUVIA_CHECK(throws_invalid([&] {
        response.header("Access-Control-Allow-Origin", "https://example.com",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
    RUVIA_CHECK(throws_invalid([&] {
        response.header("Server", "ruvia",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));

    RUVIA_CHECK(!throws_invalid([&] {
        response.header("Vary", "Origin",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
    RUVIA_CHECK(!throws_invalid([&] {
        response.header("Cache-Control", "no-store",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
    RUVIA_CHECK(!throws_invalid([&] {
        response.header("Set-Cookie", "a=1",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

RUVIA_TEST(response_header_rejects_invalid_content_type_syntax) {
    auto response = make_response();

    for (const std::string_view invalid :
        {"not a media type", "text/", "*/plain", "text/plain; charset"}) {
        RUVIA_CHECK(throws_invalid([&] { response.header("Content-Type", invalid); }));
    }

    RUVIA_CHECK(!throws_invalid(
        [&] { response.header("Content-Type", "application/json; charset=utf-8"); }));
    RUVIA_CHECK_EQ(response.header("Content-Type").value_or(std::string_view{}),
        std::string_view("application/json; charset=utf-8"));
}

RUVIA_TEST(response_header_rejects_invalid_content_encoding_syntax) {
    auto response = make_response();

    for (const std::string_view invalid :
        {"gzip;level=9", "bad coding", "gzip/deflate", "", ",gzip", "gzip,", "gzip,,br"}) {
        RUVIA_CHECK(throws_invalid([&] { response.header("Content-Encoding", invalid); }));
    }

    for (const std::string_view valid : {"deflate", "gzip, br"}) {
        RUVIA_CHECK(!throws_invalid([&] { response.header("Content-Encoding", valid); }));
    }
}

RUVIA_TEST(response_header_rejects_invalid_trailer_field_names) {
    auto response = make_response();

    for (const std::string_view invalid : {"Content-Length", "X-Checksum, bad field", ","}) {
        RUVIA_CHECK(throws_invalid([&] { response.header("Trailer", invalid); }));
        RUVIA_CHECK(throws_invalid([&] {
            response.header("Trailer", invalid,
                http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
        }));
    }

    RUVIA_CHECK(!throws_invalid([&] { response.header("Trailer", "ETag, X-Checksum"); }));
    RUVIA_CHECK_EQ(response.header("Trailer"), std::string_view("ETag, X-Checksum"));

    RUVIA_CHECK(!throws_invalid([&] {
        response.header("Trailer", "Server-Timing",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

RUVIA_TEST(response_header_rejects_name_and_value_injection) {
    auto response = make_response();
    // The developer-facing header setter is the header-injection chokepoint: a CR or
    // LF in the value (the classic response-splitting vector) is rejected rather than
    // written into the response head.
    RUVIA_CHECK(
        throws_invalid([&] { response.header("X-Foo", std::string_view("a\r\nInjected: x", 14)); }));
    RUVIA_CHECK(throws_invalid([&] { response.header("X-Foo", std::string_view("a\nb", 3)); }));
    RUVIA_CHECK(throws_invalid([&] { response.header("X-Foo", std::string_view("a\rb", 3)); }));
    RUVIA_CHECK(
        throws_invalid([&] { response.header("X-Foo", std::string_view("a\0b", 3)); }));  // NUL

    // The name is validated too: a CR/LF or a non-token byte (space) is rejected.
    RUVIA_CHECK(throws_invalid([&] { response.header(std::string_view("Bad\r\nName", 9), "v"); }));
    RUVIA_CHECK(throws_invalid([&] { response.header("Bad Name", "v"); }));

    // The append path funnels through the same validation, not just replace.
    RUVIA_CHECK(throws_invalid([&] {
        response.header("X-Foo", std::string_view("a\r\nb", 4),
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));

    // A clean header is accepted.
    RUVIA_CHECK(!throws_invalid([&] { response.header("X-Clean", "ok"); }));
    RUVIA_CHECK_EQ(response.header("X-Clean"), std::string_view("ok"));
}

RUVIA_TEST(response_header_rejects_invalid_connection_control_lists) {
    auto response = make_response();
    for (const auto value : {"close,", ", Upgrade", "close;invalid", ""}) {
        RUVIA_CHECK(throws_invalid([&] { response.header("Connection", value); }));
    }
    for (const auto value :
        {"Content-Length", "Date", "Trailer", "Authorization", "Cookie", "Range"}) {
        RUVIA_CHECK(throws_invalid([&] { response.header("Connection", value); }));
    }
    for (const auto value : {"websocket/", ", websocket", ""}) {
        RUVIA_CHECK(throws_invalid([&] { response.header("Upgrade", value); }));
    }
    RUVIA_CHECK(throws_invalid([&] { response.header("TE", "trailers"); }));
    RUVIA_CHECK(throws_invalid([&] {
        response.header("TE", "trailers",
            http_response::header_options_type{.mode_ = ruvia::http_response_header_mode::append});
    }));

    RUVIA_CHECK(!throws_invalid([&] {
        response.header("Connection", "keep-alive, Upgrade");
        response.header("Upgrade", "custom/1, websocket");
    }));
}
