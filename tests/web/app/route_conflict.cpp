#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

#include "app.h"
#include "context.h"
#include "controller.h"

// Each invalid or conflicting route registration makes every application in its
// executable fail startup, so each case compiles into its own test executable
// via RUVIA_ROUTE_CONFLICT_CASE while sharing a single source unit.
#if !defined(RUVIA_ROUTE_CONFLICT_CASE) || RUVIA_ROUTE_CONFLICT_CASE == 1
namespace {

class route_conflict_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_conflict_controller)
    RUVIA_GET("/conflict/:id/files", first);
    RUVIA_GET("/conflict/:name/files", second);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> first(ruvia::context& c) {
        co_return c.text("first");
    }

    ruvia::task<ruvia::http_response> second(ruvia::context& c) {
        co_return c.text("second");
    }
};

}  // namespace

TEST(Ruvia, route_conflict_rejects_equal_precedence_dynamic_routes_at_startup) {
    ruvia::test_app app;
    std::string message{};
    try {
        (void)app.request(ruvia::test_request::get("/conflict/1/files"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    EXPECT_TRUE(message.find("GET /conflict/:id/files") != std::string::npos);
    EXPECT_TRUE(message.find("GET /conflict/:name/files") != std::string::npos);
}
#elif RUVIA_ROUTE_CONFLICT_CASE == 2
namespace {

class route_encoded_duplicate_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_encoded_duplicate_controller)
    RUVIA_GET("/dup/admin", first);
    RUVIA_GET("/dup/%61dmin", second);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> first(ruvia::context& c) {
        co_return c.text("first");
    }

    ruvia::task<ruvia::http_response> second(ruvia::context& c) {
        co_return c.text("second");
    }
};

}  // namespace

TEST(Ruvia, route_encoded_duplicate_rejects_equivalent_spellings_at_startup) {
    ruvia::test_app app;
    bool rejected = false;
    try {
        (void)app.request(ruvia::test_request::get("/dup/admin"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}
#elif RUVIA_ROUTE_CONFLICT_CASE == 3
namespace {

class route_protocol_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_protocol_controller)
    RUVIA_CONNECT_PROTOCOL("bad protocol", "/foo/:host/:port", tunnel);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> tunnel(ruvia::context&) {
        co_return;
    }
};

}  // namespace

TEST(Ruvia, route_protocol_rejects_invalid_connect_protocol_at_startup) {
    ruvia::test_app app;
    bool rejected = false;
    try {
        (void)app.request(ruvia::test_request::get("/foo/example.test/53"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}
#elif RUVIA_ROUTE_CONFLICT_CASE == 4
namespace {

class route_dot_segment_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_dot_segment_controller)
    RUVIA_GET("/a/../b", unreachable);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> unreachable(ruvia::context& c) {
        co_return c.text("unreachable");
    }
};

}  // namespace

TEST(Ruvia, route_dot_segment_pattern_is_rejected_at_startup) {
    ruvia::test_app app;
    std::string message{};
    try {
        (void)app.request(ruvia::test_request::get("/b"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    EXPECT_TRUE(message.find("dot segment") != std::string::npos);
}
#endif
