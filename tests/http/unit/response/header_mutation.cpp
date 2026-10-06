#include <cstddef>
#include <new>
#include <string_view>

#include "ruvia/http/HttpResponse.h"

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

void populate_headers(ruvia::HttpResponse& response, const field_names& name) {
    const ruvia::HttpResponse::HeaderOptions append{.mode = ruvia::HttpResponseHeaderMode::kAppend};
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
        for (std::size_t source = 0; source < 3; ++source) {
            failing_memory_resource resource;
            {
                ruvia::HttpResponse response({.resource = &resource});
                populate_headers(response, name);
                const auto live = resource.live_allocations();
                const auto& field = response.headers().begin()[source * 2 + 1];
                const auto borrowed_name = field.name();
                const auto borrowed_value = field.value();
                response.header(borrowed_name, borrowed_value);

                RUVIA_CHECK_EQ(response.headers().size(), std::size_t{4});
                RUVIA_CHECK_EQ(response.headers().begin()[0].name(), "X-Before");
                RUVIA_CHECK_EQ(response.headers().begin()[1].value(), values[source]);
                RUVIA_CHECK_EQ(response.headers().begin()[2].name(), "Content-Type");
                RUVIA_CHECK_EQ(response.headers().begin()[3].name(), "X-After");
                RUVIA_CHECK_EQ(response.header(name.primary_).value_or(""), values[source]);
                RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/plain");
                RUVIA_CHECK_EQ(resource.live_allocations(), live - 2);
            }
            RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        }
    }
}

RUVIA_TEST(response_header_removal_accepts_borrowed_names_without_allocating) {
    for (const auto& name : names) {
        for (std::size_t source = 0; source < 3; ++source) {
            failing_memory_resource resource;
            {
                ruvia::HttpResponse response({.resource = &resource});
                populate_headers(response, name);
                const auto live = resource.live_allocations();
                const auto borrowed_name = response.headers().begin()[source * 2 + 1].name();
                resource.fail_after(0);
                response.removeHeader(borrowed_name);

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
            ruvia::HttpResponse response({.resource = &resource});
            const auto owner_allocations = resource.live_allocations();
            response.header(name.primary_, "only");
            const auto borrowed_name = response.headers().begin()->name();
            resource.fail_after(0);
            response.removeHeader(borrowed_name);
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
            ruvia::HttpResponse response({.resource = &resource});
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
