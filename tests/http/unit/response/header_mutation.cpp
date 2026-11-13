#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_response.h"

namespace {

}  // namespace

TEST(Ruvia, response_commit_headers_leaves_staged_response_independent) {
    ruvia::http_response response;
    auto staged = response.headers().clone();
    staged.header("Content-Type", "text/plain");
    response.commit_headers(std::move(staged));
    EXPECT_EQ(response.header("Content-Type").value_or(""), "text/plain");

    // The moved-from staging block must not resolve headers into committed storage.
    EXPECT_TRUE(!staged.header("Content-Type").has_value());
    staged.header("Content-Type", "application/json");
    EXPECT_EQ(staged.header("Content-Type").value_or(""), "application/json");
    EXPECT_EQ(response.header("Content-Type").value_or(""), "text/plain");
    EXPECT_EQ(response.headers().size(), std::size_t{1});
}

TEST(Ruvia, response_vary_token_ignores_surrounding_whitespace) {
    ruvia::http_response fresh;
    fresh.add_vary_token(" Origin ");
    fresh.add_vary_token("Origin");
    fresh.add_vary_token("  ");
    EXPECT_EQ(fresh.headers().size(), std::size_t{1});
    EXPECT_EQ(fresh.header("Vary").value_or(""), "Origin");

    ruvia::http_response existing;
    existing.header("Vary", "Origin");
    existing.add_vary_token(" Origin");
    existing.add_vary_token("\t");
    existing.add_vary_token(" Accept\t");
    EXPECT_EQ(existing.headers().size(), std::size_t{1});
    EXPECT_EQ(existing.header("Vary").value_or(""), "Origin, Accept");
}

TEST(Ruvia, response_header_transfer_appends_disjoint_fields_with_their_rules) {
    const ruvia::http_response_header_options append{.mode_ = ruvia::http_response_header_mode::append};
    for (const auto mode : {ruvia::http_response_header_transfer::merge, ruvia::http_response_header_transfer::assign,
             ruvia::http_response_header_transfer::apply}) {
        ruvia::http_response source_value;
        source_value.header("X-Source", "one");
        source_value.header("Vary", "Origin", append);
        source_value.header("Vary", "Accept", append);
        source_value.header("Set-Cookie", "a=1");
        source_value.header("Set-Cookie", "b=2", append);
        source_value.header("Content-Type", "text/csv");

        ruvia::http_response destination;
        destination.header("X-Existing", "kept");
        destination.transfer_headers_from(source_value, mode);

        // Assignment keeps the destination's (absent) Content-Type.
        const bool assign = mode == ruvia::http_response_header_transfer::assign;
        EXPECT_EQ(destination.headers().size(), std::size_t{assign ? 6U : 7U});
        EXPECT_EQ(destination.header("X-Existing").value_or(""), "kept");
        EXPECT_EQ(destination.header("x-source").value_or(""), "one");
        EXPECT_EQ(destination.header("Vary").value_or(""), "Origin");
        EXPECT_EQ(destination.headers().begin()[3].value(), "Accept");
        EXPECT_EQ(destination.header("Set-Cookie").value_or(""), "a=1");
        EXPECT_EQ(destination.headers().begin()[5].value(), "b=2");
        EXPECT_EQ(destination.header("Content-Type").value_or(""), assign ? "" : "text/csv");

        // Transferred fields keep their rules: cookies still upsert by name, and
        // a replacement collapses the appended field.
        destination.header("Set-Cookie", "a=3", append);
        EXPECT_EQ(destination.headers().size(), std::size_t{assign ? 6U : 7U});
        EXPECT_EQ(destination.header("Set-Cookie").value_or(""), "a=3");
        destination.header("Vary", "Cookie");
        EXPECT_EQ(destination.headers().size(), std::size_t{assign ? 5U : 6U});
        EXPECT_EQ(destination.header("Vary").value_or(""), "Cookie");
        EXPECT_EQ(source_value.headers().size(), std::size_t{6});
    }

    // A field the destination already holds resolves by mode.
    ruvia::http_response source_value;
    source_value.header("Content-Type", "text/csv");
    source_value.header("X-Added", "yes");
    ruvia::http_response merged;
    merged.content_type(ruvia::http_content_type::application_json);
    merged.transfer_headers_from(source_value, ruvia::http_response_header_transfer::merge);
    EXPECT_EQ(merged.header("Content-Type").value_or(""), "application/json");
    EXPECT_EQ(merged.header("X-Added").value_or(""), "yes");
    ruvia::http_response applied;
    applied.content_type(ruvia::http_content_type::application_json);
    applied.transfer_headers_from(source_value, ruvia::http_response_header_transfer::apply);
    EXPECT_EQ(applied.header("Content-Type").value_or(""), "text/csv");
    EXPECT_EQ(applied.headers().size(), std::size_t{2});
}

TEST(Ruvia, response_header_transfer_rejects_unknown_modes_without_mutation) {
    for (const std::string_view source_name : {"X-Added", "X-Existing", ""}) {
        SCOPED_TRACE(source_name);
        ruvia::http_response source_value;
        if (!source_name.empty()) {
            source_value.header(source_name, "new");
        }
        ruvia::http_response destination;
        destination.header("X-Existing", "old");

        EXPECT_THROW(
            destination.transfer_headers_from(source_value, static_cast<ruvia::http_response_header_transfer>(255)),
            std::invalid_argument);
        EXPECT_EQ(destination.headers().size(), std::size_t{1});
        EXPECT_EQ(destination.header("X-Existing").value_or(""), "old");
        EXPECT_FALSE(destination.header("X-Added").has_value());
    }
}

TEST(Ruvia, response_header_block_iterates_without_const_qualification) {
    ruvia::http_response response;
    response.header("X-First", "1");
    response.header("X-Second", "2");
    auto staged = response.headers().clone();
    std::size_t visited = 0;
    for (const auto& field : staged) {
        EXPECT_TRUE(!field.name().empty());
        ++visited;
    }
    EXPECT_EQ(visited, std::size_t{2});
    EXPECT_TRUE(std::distance(staged.begin(), staged.end()) == 2);
}

TEST(Ruvia, response_header_stable_view_and_allow_methods_handle_self_aliasing) {
    ruvia::http_response response;
    response.header("X-Owned", "X-CopyValue");
    const auto existing = response.header("X-Owned").value_or("");
    EXPECT_EQ(existing, "X-CopyValue");

    // A contiguous slice inside an existing owned header must be copied before
    // any table growth or replacement could release the source descriptor.
    response.header_stable_view(existing.substr(0, 6), existing.substr(6));
    response.header("X-Owned", "replaced-with-longer-owned-header-value");
    EXPECT_EQ(response.header("X-Copy").value_or(""), "Value");

    response.header("Allow", "GET, WEBDAX");
    const auto current_allow = response.header("Allow").value_or("");
    EXPECT_EQ(current_allow, "GET, WEBDAX");
    const std::array<std::string_view, 1> borrowed_ext{current_allow.substr(5)};
    response.allow_methods(ruvia::http_known_method_bit(ruvia::http_known_method::get), borrowed_ext);
    EXPECT_EQ(response.header("Allow").value_or(""), "GET, WEBDAX");
}
