#include <cstddef>
#include <iterator>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_response.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

struct field_names final {
    std::string_view primary_;
    std::string_view alternate_;
};

constexpr field_names names[]{
    {"X-Trace-Field", "x-trace-field"}, {"Cache-Control", "cache-control"}};
constexpr std::string_view values[]{"first", "middle", "final"};

void populate_headers(ruvia::http_response& response, const field_names& name) {
    const ruvia::http_response_header_options append{.mode_ = ruvia::http_response_header_mode::append};
    response.header("X-Before", "before");
    response.header(name.primary_, values[0], append);
    response.header("Content-Type", "text/plain");
    response.header(name.alternate_, values[1], append);
    response.header("X-After", "after");
    response.header(name.primary_, values[2], append);
}

}  // namespace

RUVIA_TEST(response_header_replacement_accepts_borrowed_names_and_values) {
    for (const auto& name : names) {
        for (std::size_t source_value = 0; source_value < 3; ++source_value) {
            failing_memory_resource resource;
            {
                ruvia::http_response response(&resource);
                populate_headers(response, name);
                const auto live = resource.live_allocations();
                const auto& field = response.headers().begin()[source_value * 2 + 1];
                const auto borrowed_name = field.name();
                const auto borrowed_value = field.value();
                response.header(borrowed_name, borrowed_value);

                RUVIA_CHECK_EQ(response.headers().size(), std::size_t{4});
                RUVIA_CHECK_EQ(response.headers().begin()[0].name(), "X-Before");
                RUVIA_CHECK_EQ(response.headers().begin()[1].value(), values[source_value]);
                RUVIA_CHECK_EQ(response.headers().begin()[2].name(), "Content-Type");
                RUVIA_CHECK_EQ(response.headers().begin()[3].name(), "X-After");
                RUVIA_CHECK_EQ(response.header(name.primary_).value_or(""), values[source_value]);
                RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/plain");
                RUVIA_CHECK_EQ(resource.live_allocations(), live - 2);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        }
    }
}

RUVIA_TEST(response_header_removal_accepts_borrowed_names_without_allocating) {
    for (const auto& name : names) {
        for (std::size_t source_value = 0; source_value < 3; ++source_value) {
            failing_memory_resource resource;
            {
                ruvia::http_response response(&resource);
                populate_headers(response, name);
                const auto live = resource.live_allocations();
                const auto borrowed_name = response.headers().begin()[source_value * 2 + 1].name();
                resource.fail_after(0);
                response.remove_header(borrowed_name);

                RUVIA_CHECK_EQ(response.headers().size(), std::size_t{3});
                RUVIA_CHECK_EQ(response.headers().begin()[0].name(), "X-Before");
                RUVIA_CHECK_EQ(response.headers().begin()[1].name(), "Content-Type");
                RUVIA_CHECK_EQ(response.headers().begin()[2].name(), "X-After");
                RUVIA_CHECK(!response.header(name.primary_));
                RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/plain");
                RUVIA_CHECK_EQ(resource.live_allocations(), live - 3);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        }

        failing_memory_resource resource;
        {
            ruvia::http_response response(&resource);
            const auto owner_allocations = resource.live_allocations();
            response.header(name.primary_, "only");
            const auto borrowed_name = response.headers().begin()->name();
            resource.fail_after(0);
            response.remove_header(borrowed_name);
            RUVIA_CHECK(response.headers().empty());
            RUVIA_CHECK(!response.header(name.primary_));
            RUVIA_CHECK_EQ(resource.live_allocations(), owner_allocations);
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(response_header_replacement_failure_preserves_borrowed_inputs_for_retry) {
    for (const auto& name : names) {
        failing_memory_resource resource;
        {
            ruvia::http_response response(&resource);
            populate_headers(response, name);
            const auto live = resource.live_allocations();
            const auto borrowed_name = response.headers().begin()[1].name();
            const auto borrowed_value = response.headers().begin()[1].value();
            resource.fail_after(0);
            bool failed = false;
            try {
                response.header(borrowed_name, borrowed_value);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(resource.live_allocations(), live);
            RUVIA_CHECK_EQ(response.headers().size(), std::size_t{6});
            RUVIA_CHECK_EQ(borrowed_name, name.primary_);
            RUVIA_CHECK_EQ(borrowed_value, values[0]);
            RUVIA_CHECK_EQ(response.headers().begin()[3].value(), values[1]);
            RUVIA_CHECK_EQ(response.headers().begin()[5].value(), values[2]);
            resource.allow_allocations();
            response.header(borrowed_name, borrowed_value);
            RUVIA_CHECK_EQ(response.headers().size(), std::size_t{4});
            RUVIA_CHECK_EQ(response.header(name.primary_).value_or(""), values[0]);
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(response_commit_headers_leaves_staged_response_independent) {
    ruvia::http_response response;
    auto staged = response.headers().clone();
    staged.header("Content-Type", "text/plain");
    response.commit_headers(std::move(staged));
    RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/plain");

    // The moved-from staging block must not resolve headers into committed storage.
    RUVIA_CHECK(!staged.header("Content-Type").has_value());
    staged.header("Content-Type", "application/json");
    RUVIA_CHECK_EQ(staged.header("Content-Type").value_or(""), "application/json");
    RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/plain");
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
}

RUVIA_TEST(response_content_type_constants_are_valid_fields_set_without_allocation) {
    struct constant_case final {
        ruvia::http_content_type type_;
        std::string_view value_;
    };
    constexpr constant_case cases[]{
        {ruvia::http_content_type::text_plain_utf8, "text/plain; charset=UTF-8"},
        {ruvia::http_content_type::text_html_utf8, "text/html; charset=UTF-8"},
        {ruvia::http_content_type::application_json, "application/json"},
    };
    for (const auto& constant : cases) {
        // The fully validated setter accepts every constant the trusted one stores.
        ruvia::http_response validated;
        validated.header("Content-Type", constant.value_);
        RUVIA_CHECK_EQ(validated.header("Content-Type").value_or(""), constant.value_);

        failing_memory_resource resource;
        {
            ruvia::http_response response(&resource);
            response.header("X-Before", "before");
            response.header("content-type", "text/csv");
            const auto live = resource.live_allocations();
            response.content_type(constant.type_);
            RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
            RUVIA_CHECK_EQ(response.headers().begin()[1].name(), "Content-Type");
            RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), constant.value_);
            RUVIA_CHECK_EQ(resource.live_allocations(), live - 1);

            ruvia::http_response fresh(&resource);
            resource.fail_after(0);
            fresh.content_type(constant.type_);
            resource.allow_allocations();
            RUVIA_CHECK_EQ(fresh.header("Content-Type").value_or(""), constant.value_);
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }

    ruvia::http_response response;
    response.header("Content-Type", "text/csv");
    bool rejected = false;
    try {
        response.content_type(static_cast<ruvia::http_content_type>(0xff));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/csv");
}

RUVIA_TEST(response_vary_token_ignores_surrounding_whitespace) {
    ruvia::http_response fresh;
    fresh.add_vary_token(" Origin ");
    fresh.add_vary_token("Origin");
    fresh.add_vary_token("  ");
    RUVIA_CHECK_EQ(fresh.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(fresh.header("Vary").value_or(""), "Origin");

    ruvia::http_response existing;
    existing.header("Vary", "Origin");
    existing.add_vary_token(" Origin");
    existing.add_vary_token("\t");
    existing.add_vary_token(" Accept\t");
    RUVIA_CHECK_EQ(existing.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(existing.header("Vary").value_or(""), "Origin, Accept");
}

RUVIA_TEST(response_header_transfer_appends_disjoint_fields_with_their_rules) {
    auto* const resource = std::pmr::new_delete_resource();
    const ruvia::http_response_header_options append{.mode_ = ruvia::http_response_header_mode::append};
    for (const auto mode : {ruvia::http_response_header_transfer::merge, ruvia::http_response_header_transfer::assign,
             ruvia::http_response_header_transfer::apply}) {
        ruvia::http_response source_value(resource);
        source_value.header("X-Source", "one");
        source_value.header("Vary", "Origin", append);
        source_value.header("Vary", "Accept", append);
        source_value.header("Set-Cookie", "a=1");
        source_value.header("Set-Cookie", "b=2", append);
        source_value.header("Content-Type", "text/csv");

        ruvia::http_response destination(resource);
        destination.header("X-Existing", "kept");
        destination.transfer_headers_from(source_value, mode);

        // Assignment keeps the destination's (absent) Content-Type.
        const bool assign = mode == ruvia::http_response_header_transfer::assign;
        RUVIA_CHECK_EQ(destination.headers().size(), std::size_t{assign ? 6U : 7U});
        RUVIA_CHECK_EQ(destination.header("X-Existing").value_or(""), "kept");
        RUVIA_CHECK_EQ(destination.header("x-source").value_or(""), "one");
        RUVIA_CHECK_EQ(destination.header("Vary").value_or(""), "Origin");
        RUVIA_CHECK_EQ(destination.headers().begin()[3].value(), "Accept");
        RUVIA_CHECK_EQ(destination.header("Set-Cookie").value_or(""), "a=1");
        RUVIA_CHECK_EQ(destination.headers().begin()[5].value(), "b=2");
        RUVIA_CHECK_EQ(destination.header("Content-Type").value_or(""), assign ? "" : "text/csv");

        // Transferred fields keep their rules: cookies still upsert by name, and
        // a replacement collapses the appended field.
        destination.header("Set-Cookie", "a=3", append);
        RUVIA_CHECK_EQ(destination.headers().size(), std::size_t{assign ? 6U : 7U});
        RUVIA_CHECK_EQ(destination.header("Set-Cookie").value_or(""), "a=3");
        destination.header("Vary", "Cookie");
        RUVIA_CHECK_EQ(destination.headers().size(), std::size_t{assign ? 5U : 6U});
        RUVIA_CHECK_EQ(destination.header("Vary").value_or(""), "Cookie");
        RUVIA_CHECK_EQ(source_value.headers().size(), std::size_t{6});
    }

    // A field the destination already holds resolves by mode.
    ruvia::http_response source_value(resource);
    source_value.header("Content-Type", "text/csv");
    source_value.header("X-Added", "yes");
    ruvia::http_response merged(resource);
    merged.content_type(ruvia::http_content_type::application_json);
    merged.transfer_headers_from(source_value, ruvia::http_response_header_transfer::merge);
    RUVIA_CHECK_EQ(merged.header("Content-Type").value_or(""), "application/json");
    RUVIA_CHECK_EQ(merged.header("X-Added").value_or(""), "yes");
    ruvia::http_response applied(resource);
    applied.content_type(ruvia::http_content_type::application_json);
    applied.transfer_headers_from(source_value, ruvia::http_response_header_transfer::apply);
    RUVIA_CHECK_EQ(applied.header("Content-Type").value_or(""), "text/csv");
    RUVIA_CHECK_EQ(applied.headers().size(), std::size_t{2});
}

RUVIA_TEST(response_header_transfer_failure_leaves_destination_unchanged) {
    failing_memory_resource resource;
    {
        ruvia::http_response source_value(std::pmr::new_delete_resource());
        source_value.header("X-First", "1");
        source_value.header("X-Second", "2");

        ruvia::http_response destination(&resource);
        destination.header("X-Existing", "kept");
        const auto live = resource.live_allocations();
        resource.fail_after(1);
        bool failed = false;
        try {
            destination.transfer_headers_from(source_value, ruvia::http_response_header_transfer::apply);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(resource.live_allocations(), live);
        RUVIA_CHECK_EQ(destination.headers().size(), std::size_t{1});
        RUVIA_CHECK(!destination.header("X-First").has_value());

        resource.allow_allocations();
        destination.transfer_headers_from(source_value, ruvia::http_response_header_transfer::apply);
        RUVIA_CHECK_EQ(destination.headers().size(), std::size_t{3});
        RUVIA_CHECK_EQ(destination.header("X-First").value_or(""), "1");
        RUVIA_CHECK_EQ(destination.header("X-Second").value_or(""), "2");
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(response_header_block_iterates_without_const_qualification) {
    ruvia::http_response response(std::pmr::new_delete_resource());
    response.header("X-First", "1");
    response.header("X-Second", "2");
    auto staged = response.headers().clone();
    std::size_t visited = 0;
    for (const auto& field : staged) {
        RUVIA_CHECK(!field.name().empty());
        ++visited;
    }
    RUVIA_CHECK_EQ(visited, std::size_t{2});
    RUVIA_CHECK(std::distance(staged.begin(), staged.end()) == 2);
}
