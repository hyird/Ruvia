#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

#include "context.h"
#include "controller.h"
#include "test_app.h"

// Each invalid or conflicting controller fails startup only for the test_app
// that names it, so every case shares one executable that starts no application.
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

class route_valid_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_valid_controller)
    RUVIA_GET("/valid", valid);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> valid(ruvia::context& c) {
        co_return c.text("valid");
    }
};

[[nodiscard]] std::string startup_error(ruvia::test_app& app, std::string_view target) {
    try {
        (void)app.request(ruvia::test_request::get(target));
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

}  // namespace

TEST(Ruvia, route_conflict_rejects_equal_precedence_dynamic_routes_at_startup) {
    ruvia::test_app app;
    app.controllers<route_conflict_controller>();
    const auto message = startup_error(app, "/conflict/1/files");
    EXPECT_TRUE(message.find("GET /conflict/:id/files") != std::string::npos);
    EXPECT_TRUE(message.find("GET /conflict/:name/files") != std::string::npos);
}

TEST(Ruvia, route_encoded_duplicate_rejects_equivalent_spellings_at_startup) {
    ruvia::test_app app;
    app.controllers<route_encoded_duplicate_controller>();
    EXPECT_FALSE(startup_error(app, "/dup/admin").empty());
}

TEST(Ruvia, route_protocol_rejects_invalid_connect_protocol_at_startup) {
    ruvia::test_app app;
    app.controllers<route_protocol_controller>();
    EXPECT_FALSE(startup_error(app, "/foo/example.test/53").empty());
}

TEST(Ruvia, route_dot_segment_pattern_is_rejected_at_startup) {
    ruvia::test_app app;
    app.controllers<route_dot_segment_controller>();
    EXPECT_TRUE(startup_error(app, "/b").find("dot segment") != std::string::npos);
}

TEST(Ruvia, route_controller_named_twice_is_a_duplicate_registration) {
    ruvia::test_app app;
    app.controllers<route_valid_controller, route_valid_controller>();
    EXPECT_TRUE(startup_error(app, "/valid").find("duplicate route registration") != std::string::npos);
}

TEST(Ruvia, route_test_apps_serve_only_their_named_controllers) {
    ruvia::test_app named;
    named.controllers<route_valid_controller>();
    ruvia::test_app unnamed;
    EXPECT_EQ(named.request(ruvia::test_request::get("/valid")).status(), ruvia::http_status::ok);
    EXPECT_EQ(unnamed.request(ruvia::test_request::get("/valid")).status(), ruvia::http_status::not_found);
}
