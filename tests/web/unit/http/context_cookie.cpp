#include <chrono>
#include <string>
#include <string_view>

#include "context_request_fixture.h"

// Setting response cookies, including prefixes and signatures.

RUVIA_TEST(context_set_cookie_serializes_all_attributes) {
    auto request = ruvia::test_request::get("/");
    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const ruvia::cookie_options host{
            .path_ = "/",
            .same_site_ = ruvia::cookie_same_site::strict,
            .priority_ = ruvia::cookie_priority::high,
            .max_age_ = std::chrono::seconds(3600),
            .prefix_ = ruvia::cookie_prefix::host,
            .http_only_ = ruvia::cookie_attribute_policy::emit,
            .secure_ = ruvia::cookie_attribute_policy::emit,
            .partitioned_ = ruvia::cookie_attribute_policy::emit,
        };
        context_value.set_cookie({.name_ = "id", .value_ = "abc", .attributes_ = host});
        const auto host_response = context_value.text("ok");
        RUVIA_CHECK_EQ(host_response.header("Set-Cookie"),
            std::string_view("__Host-id=abc; Path=/; Max-Age=3600; HttpOnly; Secure; "
                             "SameSite=Strict; Priority=High; Partitioned"));

        co_return;
    });

    // Case B: __Secure- prefix carrying Domain and a fixed Expires (the well-known
    // instant 1234567890 = Fri 13 Feb 2009 23:31:30 UTC, formatted as a
    // locale-independent IMF-fixdate) plus SameSite=None. Covers the Domain and
    // Expires branches Case A omits.
    const ruvia::cookie_options secure{
        .path_ = "/app",
        .domain_ = "example.com",
        .same_site_ = ruvia::cookie_same_site::none,
        .expires_ = std::chrono::system_clock::time_point(std::chrono::seconds(1234567890)),
        .prefix_ = ruvia::cookie_prefix::secure,
        .secure_ = ruvia::cookie_attribute_policy::emit,
    };
    auto secure_request = ruvia::test_request::get("/");
    (void)context_request_test::with_context(secure_request, [&](ruvia::context& secure_context) -> ruvia::task<void> {
        secure_context.set_cookie({.name_ = "sess", .value_ = "xyz", .attributes_ = secure});
        const auto secure_response = secure_context.text("ok");
        RUVIA_CHECK_EQ(secure_response.header("Set-Cookie"),
            std::string_view("__Secure-sess=xyz; Path=/app; Domain=example.com; "
                             "Expires=Fri, 13 Feb 2009 23:31:30 GMT; Secure; SameSite=None"));
        co_return;
    });
}

RUVIA_TEST(context_set_cookie_preserves_same_name_different_path) {
    auto request = ruvia::test_request::get("/");
    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        ruvia::cookie_options root;
        root.path_ = "/";
        ruvia::cookie_options admin;
        admin.path_ = "/admin";
        context_value.set_cookie({.name_ = "session", .value_ = "root-old", .attributes_ = root});
        context_value.set_cookie({.name_ = "session", .value_ = "admin", .attributes_ = admin});
        context_value.set_cookie({.name_ = "session", .value_ = "root-new", .attributes_ = root});
        const auto response = context_value.text("ok");

        std::size_t count = 0;
        bool has_root_old = false;
        bool has_root_new = false;
        bool has_admin = false;
        for (const auto& header : response.headers()) {
            if (header.name() != std::string_view("Set-Cookie")) {
                continue;
            }
            ++count;
            has_root_old = has_root_old || header.value() == "session=root-old; Path=/";
            has_root_new = has_root_new || header.value() == "session=root-new; Path=/";
            has_admin = has_admin || header.value() == "session=admin; Path=/admin";
        }
        RUVIA_CHECK_EQ(count, std::size_t{2});
        RUVIA_CHECK(!has_root_old);
        RUVIA_CHECK(has_root_new);
        RUVIA_CHECK(has_admin);
        co_return;
    });
}

RUVIA_TEST(context_signed_cookie_with_prefix_verifies_round_trip) {
    std::string set_cookie;
    std::string bare;
    auto write_request = ruvia::test_request::get("/");
    (void)context_request_test::with_context(write_request, [&](ruvia::context& write_context) -> ruvia::task<void> {
        const ruvia::cookie_options options{
            .path_ = "/",
            .prefix_ = ruvia::cookie_prefix::host,
            .secure_ = ruvia::cookie_attribute_policy::emit,
        };
        write_context.set_signed_cookie(
            {.name_ = "session", .value_ = "user-1", .secret_ = "secret", .attributes_ = options});
        const auto write_response = write_context.text("ok");
        set_cookie = write_response.header("Set-Cookie").value_or(std::string_view{});
        co_return;
    });
    const std::string_view line(set_cookie);
    const auto pair = line.substr(0, line.find(';'));
    RUVIA_CHECK(pair.starts_with("__Host-session="));

    // Present the cookie exactly as a browser sends it back.
    const std::string cookie_field(pair);
    auto read_request = ruvia::test_request::get("/").header("Cookie", cookie_field);

    (void)context_request_test::with_context(read_request, [&](ruvia::context& read_context) -> ruvia::task<void> {
        const auto verified =
            read_context.req().signed_cookie({.name_ = "__Host-session", .secret_ = "secret"});
        RUVIA_CHECK(verified.has_value());
        RUVIA_CHECK_EQ(*verified, std::string_view("user-1"));

        co_return;
    });

    // An unprefixed signed cookie keeps verifying under its own name.
    auto bare_write_request = ruvia::test_request::get("/");
    (void)context_request_test::with_context(bare_write_request, [&](ruvia::context& bare_write_context) -> ruvia::task<void> {
        bare_write_context.set_signed_cookie({.name_ = "plain", .value_ = "user-2", .secret_ = "secret"});
        const auto bare_write_response = bare_write_context.text("ok");
        bare = bare_write_response.header("Set-Cookie").value_or(std::string_view{});
        co_return;
    });
    const std::string_view bare_line(bare);
    const std::string bare_field(bare_line.substr(0, bare_line.find(';')));
    auto bare_request = ruvia::test_request::get("/").header("Cookie", bare_field);

    (void)context_request_test::with_context(bare_request, [&](ruvia::context& bare_context) -> ruvia::task<void> {
        const auto bare_verified = bare_context.req().signed_cookie({.name_ = "plain", .secret_ = "secret"});
        RUVIA_CHECK(bare_verified.has_value());
        RUVIA_CHECK_EQ(*bare_verified, std::string_view("user-2"));
        co_return;
    });
}

RUVIA_TEST(context_delete_cookie_with_prefix_is_response_only) {
    auto request = ruvia::test_request::get("/").header("Cookie", "__Host-session=user-1");

    (void)context_request_test::with_context(request, [&](ruvia::context& context_value) -> ruvia::task<void> {
        const ruvia::cookie_options options{
            .path_ = "/",
            .prefix_ = ruvia::cookie_prefix::host,
            .secure_ = ruvia::cookie_attribute_policy::emit,
        };
        const auto previous = context_value.req().cookie("__Host-session");
        RUVIA_CHECK(previous.has_value());
        RUVIA_CHECK_EQ(*previous, std::string_view("user-1"));
        context_value.delete_cookie({.name_ = "session", .attributes_ = options});
        const auto response = context_value.text("deleted");
        const auto set_cookie = response.header("Set-Cookie");
        RUVIA_CHECK(set_cookie.has_value());
        RUVIA_CHECK(set_cookie.value_or(std::string_view{}).starts_with("__Host-session=;"));
        RUVIA_CHECK((set_cookie.value_or(std::string_view{}).find("Max-Age=0") != std::string_view::npos));
        co_return;
    });
}
