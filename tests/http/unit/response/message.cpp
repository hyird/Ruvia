#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_response.h"

#include "failing_memory_resource.h"

namespace {

using ruvia::http_response;
using ruvia::http_response_header_options;

http_response make_response() {
    return http_response(std::pmr::new_delete_resource());
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

TEST(Ruvia, response_header_append_preserves_multiplicity_across_table_spill) {
    http_response response;
    response.header("X-Repeated", "first");
    for (int i = 0; i < 7; ++i) {
        response.header("X-Filler-" + std::to_string(i), "value");
    }
    response.header("x-repeated", "second", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(response.headers().size(), std::size_t{9});
    std::size_t marked = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::http_ascii_equals_ignore_case(header.name(), "X-Repeated")) {
            ++marked;
        }
    }
    EXPECT_EQ(marked, std::size_t{2});
    response.header("X-Repeated", "replacement");
    EXPECT_EQ(response.headers().size(), std::size_t{8});
    EXPECT_EQ(response.header("X-Repeated"), std::string_view("replacement"));
}

TEST(Ruvia, response_status_is_version_neutral_code_only) {
    auto response = make_response();
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    response.status(ruvia::http_status::not_found);
    EXPECT_EQ(response.status(), ruvia::http_status::not_found);
    response.status(ruvia::http_status_code::from_value(599));
    EXPECT_EQ(response.status(), ruvia::http_status_code::from_value(599));
    response.status(ruvia::http_status_code::from_value(299));
    EXPECT_EQ(response.status(), ruvia::http_status_code::from_value(299));
}

TEST(Ruvia, response_header_distinguishes_missing_from_present_empty) {
    auto response = make_response();
    EXPECT_TRUE(!response.header("X-Empty").has_value());

    response.header("X-Empty", "");
    const auto present_empty = response.header("x-empty");
    EXPECT_TRUE(present_empty.has_value());
    EXPECT_TRUE(present_empty.value_or("missing").empty());
}

TEST(Ruvia, response_move_assignment_transfers_body) {
    std::pmr::monotonic_buffer_resource source_resource;
    std::pmr::monotonic_buffer_resource target_resource;
    http_response source_value(&source_resource);
    http_response target(&target_resource);

    source_value.body(std::string(4096, 's'));
    target.body(std::string(64, 't'));

    target = std::move(source_value);

    EXPECT_EQ(target.body_bytes(), std::string(4096, 's'));
}

TEST(Ruvia, response_status_code_range_validated) {
    auto response = make_response();
    EXPECT_TRUE(throws_invalid([&] { response.status(ruvia::http_status::continue_value); }));
    EXPECT_TRUE(throws_invalid([&] { response.status(ruvia::http_status_code::from_value(199)); }));
    EXPECT_TRUE(!throws_invalid([&] { response.status(ruvia::http_status::ok); }));  // lower boundary
    EXPECT_TRUE(!throws_invalid(
        [&] { response.status(ruvia::http_status_code::from_value(599)); }));  // upper boundary
}

TEST(Ruvia, response_switching_protocols_requires_a_dedicated_driver) {
    auto response = make_response();
    EXPECT_TRUE(throws_invalid([&] { response.status(ruvia::http_status::switching_protocols); }));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

TEST(Ruvia, interim_response_head_owns_the_non_switching_1xx_status_space) {
    const ruvia::http_header_view headers[] = {
        {"Link", "</style.css>; rel=preload"},
    };
    const ruvia::http_interim_response_head early_hints(ruvia::http_status::early_hints, headers);
    EXPECT_EQ(early_hints.status(), ruvia::http_status::early_hints);
    EXPECT_EQ(early_hints.headers().size(), std::size_t{1});
    EXPECT_EQ(early_hints.headers()[0].name(), std::string_view("Link"));

    for (const ruvia::http_status_code status : {ruvia::http_status::switching_protocols,
             ruvia::http_status::ok, ruvia::http_status_code::from_value(599)}) {
        EXPECT_TRUE(throws_invalid([&] { (void)ruvia::http_interim_response_head(status); }));
    }
}

TEST(Ruvia, response_header_replace_append_and_remove) {
    auto response = make_response();

    // A plain set replaces: the latest value wins as a single header.
    response.header("X-Test", "first");
    response.header("X-Test", "second");
    EXPECT_EQ(response.header("X-Test"), std::string_view("second"));

    // append emits an additional header line (needed for multi-valued fields
    // like Set-Cookie).
    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    std::size_t set_cookie_count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++set_cookie_count;
        }
    }
    EXPECT_EQ(set_cookie_count, std::size_t{2});

    // Passing nullopt removes the header entirely.
    response.remove_header("X-Test");
    EXPECT_TRUE(!response.header("X-Test").has_value());
}

TEST(Ruvia, response_header_rejects_invalid_write_mode) {
    auto response = make_response();
    EXPECT_TRUE(throws_invalid([&] {
        response.header("X-Test", "value",
            http_response_header_options{.mode_ = static_cast<ruvia::http_response_header_mode>(255)});
    }));
    EXPECT_TRUE(response.headers().empty());
}

TEST(Ruvia, response_set_cookie_append_replaces_same_wire_name) {
    auto response = make_response();
    response.header("Set-Cookie", "session=old; Path=/");
    response.header("Set-Cookie", "theme=dark; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=new; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "Session=upper; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});

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
    EXPECT_EQ(count, std::size_t{3});
    EXPECT_TRUE(!has_old);
    EXPECT_TRUE(has_new);
    EXPECT_TRUE(has_theme);
    EXPECT_TRUE(has_upper);  // cookie-name is case-sensitive
}

TEST(Ruvia, response_set_cookie_append_preserves_same_name_different_scope) {
    auto response = make_response();
    response.header("Set-Cookie", "session=root-old; Path=/");
    response.header("Set-Cookie", "session=admin; Path=/admin",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=domain-old; Path=/; Domain=.Example.COM",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=root-new; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=domain-new; Domain=example.com; Path=/",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});

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
    EXPECT_EQ(count, std::size_t{3});
    EXPECT_TRUE(!has_root_old);
    EXPECT_TRUE(has_root_new);
    EXPECT_TRUE(has_admin);
    EXPECT_TRUE(!has_domain_old);
    EXPECT_TRUE(has_domain_new);
}

TEST(Ruvia, response_plain_set_collapses_prior_appended_fields) {
    auto response = make_response();
    response.header("Link", "</a>",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("link", "</b>",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("LINK", "</final>");

    std::size_t link_count = 0;
    for (const auto& header : response.headers()) {
        if (ruvia::http_ascii_equals_ignore_case(header.name(), "Link")) {
            ++link_count;
            EXPECT_EQ(header.value(), std::string_view("</final>"));
        }
    }
    EXPECT_EQ(link_count, std::size_t{1});

    response.header("Set-Cookie", "a=1");
    response.header("Set-Cookie", "b=2",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "c=3");
    std::size_t cookie_count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Set-Cookie")) {
            ++cookie_count;
            EXPECT_EQ(header.value(), std::string_view("c=3"));
        }
    }
    EXPECT_EQ(cookie_count, std::size_t{1});
}

TEST(Ruvia, response_header_append_failure_preserves_existing_header_for_retry) {
    failing_memory_resource resource;
    http_response response(&resource);
    response.header("Link", "</a>; rel=preload");

    resource.fail_after(0);
    bool failed = false;
    try {
        response.header("Link", "</b>; rel=preload",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    } catch (const std::bad_alloc&) {
        failed = true;
    }

    EXPECT_TRUE(failed);
    EXPECT_EQ(response.headers().size(), std::size_t{1});
    response.header("Link", "</b>; rel=preload",
        http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(response.headers().size(), std::size_t{2});
}

TEST(Ruvia, response_header_removal_preserves_unrelated_fields) {
    for (const int filler_count : {0, 8}) {
        http_response response;
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
        response.remove_header("content-length");
        response.remove_header("Set-Cookie");
        response.remove_header("X-Missing");
        EXPECT_EQ(response.headers().size(), fields.size());
        std::size_t index = 0;
        for (const auto& header : response.headers()) {
            EXPECT_EQ(header.name(), fields[index].first);
            EXPECT_EQ(header.value(), fields[index].second);
            ++index;
        }
        EXPECT_EQ(response.header("content-type"), std::string_view("text/plain"));
        EXPECT_EQ(response.header("vary"), std::string_view("Accept-Encoding"));
        EXPECT_TRUE(!response.header("Content-Length").has_value());

        // Removing an appended field removes every occurrence.
        response.header("Content-Length", "");
        EXPECT_TRUE(response.header("Content-Length").has_value());
        response.remove_header("CONTENT-LENGTH");
        EXPECT_TRUE(!response.header("Content-Length").has_value());
        response.remove_header("vary");
        EXPECT_TRUE(!response.header("Vary").has_value());
        EXPECT_EQ(response.headers().size(), fields.size() - 2);
        EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain"));
        response.remove_header("Vary");
        EXPECT_EQ(response.headers().size(), fields.size() - 2);
    }
}

TEST(Ruvia, response_header_remove_known_header_rebuilds_index) {
    auto response = make_response();
    // Content-Type is a KNOWN header tracked by a bit in the response's header index
    // (unlike the custom X-Test above, which is unindexed). Removing it must rebuild
    // that index, or the indexed lookup fast path could still resolve to the removed
    // slot -- returning a stale value.
    response.header("Content-Type", "text/plain");
    EXPECT_EQ(response.header("Content-Type"), std::string_view("text/plain"));

    response.remove_header("Content-Type");
    EXPECT_TRUE(!response.header("Content-Type").has_value());  // gone, not a stale index hit

    // Re-adding after removal replaces cleanly and leaves exactly one header line --
    // no duplicate resurrected from a stale index entry.
    response.header("Content-Type", "application/json");
    EXPECT_EQ(response.header("Content-Type"), std::string_view("application/json"));
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == std::string_view("Content-Type")) {
            ++count;
        }
    }
    EXPECT_EQ(count, std::size_t{1});
}

TEST(Ruvia, response_header_append_rejects_body_framing_headers) {
    auto response = make_response();

    EXPECT_TRUE(throws_invalid([&] {
        response.header("Content-Length", "5",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Transfer-Encoding", "chunked",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Location", "/next",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    EXPECT_TRUE(!throws_invalid([&] { response.header("Content-Length", "5"); }));
    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Set-Cookie", "a=1",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

TEST(Ruvia, response_header_append_rejects_single_value_headers) {
    auto response = make_response();

    EXPECT_TRUE(throws_invalid([&] {
        response.header("Content-Type", "text/plain",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("ETag", "\"abc\"",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Access-Control-Allow-Origin", "https://example.com",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("Server", "ruvia",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Vary", "Origin",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Cache-Control", "no-store",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Set-Cookie", "a=1",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

TEST(Ruvia, response_header_rejects_invalid_content_type_syntax) {
    auto response = make_response();

    for (const std::string_view invalid :
        {"not a media type", "text/", "*/plain", "text/plain; charset"}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Content-Type", invalid); }));
    }

    EXPECT_TRUE(!throws_invalid(
        [&] { response.header("Content-Type", "application/json; charset=utf-8"); }));
    EXPECT_EQ(response.header("Content-Type").value_or(std::string_view{}), std::string_view("application/json; charset=utf-8"));
}

TEST(Ruvia, response_header_rejects_invalid_content_encoding_syntax) {
    auto response = make_response();

    for (const std::string_view invalid :
        {"gzip;level=9", "bad coding", "gzip/deflate", "", ",gzip", "gzip,", "gzip,,br"}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Content-Encoding", invalid); }));
    }

    for (const std::string_view valid : {"deflate", "gzip, br"}) {
        EXPECT_TRUE(!throws_invalid([&] { response.header("Content-Encoding", valid); }));
    }
}

TEST(Ruvia, response_header_rejects_invalid_trailer_field_names) {
    auto response = make_response();

    for (const std::string_view invalid : {"Content-Length", "X-Checksum, bad field", ","}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Trailer", invalid); }));
        EXPECT_TRUE(throws_invalid([&] {
            response.header("Trailer", invalid,
                http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
        }));
    }

    EXPECT_TRUE(!throws_invalid([&] { response.header("Trailer", "ETag, X-Checksum"); }));
    EXPECT_EQ(response.header("Trailer"), std::string_view("ETag, X-Checksum"));

    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Trailer", "Server-Timing",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));
}

TEST(Ruvia, response_header_rejects_name_and_value_injection) {
    auto response = make_response();
    // The developer-facing header setter is the header-injection chokepoint: a CR or
    // LF in the value (the classic response-splitting vector) is rejected rather than
    // written into the response head.
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\r\nInjected: x", 14)); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\nb", 3)); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\rb", 3)); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("X-Foo", std::string_view("a\0b", 3)); }));  // NUL

    // The name is validated too: a CR/LF or a non-token byte (space) is rejected.
    EXPECT_TRUE(throws_invalid([&] { response.header(std::string_view("Bad\r\nName", 9), "v"); }));
    EXPECT_TRUE(throws_invalid([&] { response.header("Bad Name", "v"); }));

    // The append path funnels through the same validation, not just replace.
    EXPECT_TRUE(throws_invalid([&] {
        response.header("X-Foo", std::string_view("a\r\nb", 4),
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    // A clean header is accepted.
    EXPECT_TRUE(!throws_invalid([&] { response.header("X-Clean", "ok"); }));
    EXPECT_EQ(response.header("X-Clean"), std::string_view("ok"));
}

TEST(Ruvia, response_header_rejects_invalid_connection_control_lists) {
    auto response = make_response();
    for (const auto value : {"close,", ", Upgrade", "close;invalid", ""}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Connection", value); }));
    }
    for (const auto value : {"Content-Length", "Date", "Trailer", "Authorization", "Cookie", "Range", "Forwarded",
             "X-Forwarded-For", "X-Forwarded-Proto", "close, X-Forwarded-Host"}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Connection", value); }));
    }
    for (const auto value : {"websocket/", ", websocket", ""}) {
        EXPECT_TRUE(throws_invalid([&] { response.header("Upgrade", value); }));
    }
    EXPECT_TRUE(throws_invalid([&] { response.header("TE", "trailers"); }));
    EXPECT_TRUE(throws_invalid([&] {
        response.header("TE", "trailers",
            http_response_header_options{.mode_ = ruvia::http_response_header_mode::append});
    }));

    EXPECT_TRUE(!throws_invalid([&] {
        response.header("Connection", "keep-alive, Upgrade");
        response.header("Upgrade", "custom/1, websocket");
    }));
}
