#include <gtest/gtest.h>

// The public in-memory testing facade must dispatch through the production
// pipeline: controller macros, route params, query/cookie access, model
// bodies with their 415/400 split, global middleware, prefix and app-wide
// fallbacks, url_for, worker state, and the automatic HEAD fallback -- all
// without a socket. These tests use ONLY public headers, exactly like an
// application's own test suite would.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/model.h"

#include "worker_task_fixture.h"
RUVIA_MODEL(testing_facade_echo, RUVIA_OPTIONAL_FIELD(value, ruvia::string));

RUVIA_MODEL(testing_facade_report, RUVIA_REQUIRED_FIELD(path, ruvia::string),
    RUVIA_REQUIRED_FIELD(count, ruvia::uint64),
    RUVIA_REQUIRED_FIELD(tags, ruvia::array<ruvia::string>));

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <concepts>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <set>
#include <span>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/ocsp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "http_server_fixture.h"

namespace {

struct testing_facade_counter final {
    int count_{0};
};

std::filesystem::path& testing_facade_file_path() {
    static std::filesystem::path path;
    return path;
}

std::optional<ruvia::static_root>& testing_facade_static_root() {
    static std::optional<ruvia::static_root> root;
    return root;
}
struct testing_facade_startup_failure_state final {};

class testing_facade_startup_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class testing_facade_throwing_middleware final {
public:
    explicit testing_facade_throwing_middleware(int* attempts) {
        ++*attempts;
        throw testing_facade_startup_error("testing facade middleware startup failed");
    }

    ruvia::task<void> invoke(ruvia::context&, ruvia::next&) {
        co_return;
    }
};

class testing_facade_stamp final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Stamp", "on");
        c.edit_response().header("X-Middleware-Method", c.req().method());
    }
};

// Registered with constructor arguments rather than default constructed, so it
// is deliberately not default constructible: the descriptor must carry the
// registration arguments to every instance the router builds.
class testing_facade_configured_stamp final {
public:
    testing_facade_configured_stamp(std::string_view name, int level) noexcept
        : name_(name),
          level_(level) {}

    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Configured", name_);
        c.edit_response().header("X-Test-Level", level_ == 2 ? "two" : "other");
    }

private:
    std::string_view name_;
    int level_;
};

struct testing_facade_user final {
    std::string_view name_;
    int level_{0};
};

// The pattern request-scoped bindings exist for: a middleware computes a value,
// owns it in its own coroutine frame, and publishes it to everything downstream
// of its next() for exactly that scope.
class testing_facade_auth final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        const testing_facade_user user_value{
            .name_ = c.req().header("X-User").value_or("anonymous"), .level_ = 2};
        const auto binding = c.bind_request_state(user_value);
        co_await next_value();
    }
};

// Stamps a header so a response shows whether this middleware ran at all.
class testing_facade_scoped final {
public:
    explicit testing_facade_scoped(std::string_view tag) noexcept
        : tag_(tag) {}

    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Scope", tag_);
    }

private:
    std::string_view tag_;
};

// Declares itself meaningful on a request that matched no route, the way
// security_headers_middleware does.
class testing_facade_always final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Always", "on");
    }
};
class testing_facade_const_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) const {
        co_await next_value();
        c.edit_response().header("X-Const-Middleware", "yes");
    }
};

// Rejects requests under /guarded before reaching the answer.
class testing_facade_unmatched_reject final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        if (c.req().path().starts_with("/guarded")) {
            throw ruvia::http_error({.status_ = ruvia::http_status::unauthorized});
        }
        co_await next_value();
    }
};

// Each replaces a complete downstream response after next(): one by failing,
// one by responding.
class testing_facade_deny_after final {
public:
    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
        throw ruvia::http_error({.status_ = ruvia::http_status::forbidden});
    }
};

class testing_facade_replace_after final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.respond(c.text("denied"));
    }
};

class testing_facade_policy_stamp final {
public:
    explicit testing_facade_policy_stamp(bool after) noexcept
        : after_(after) {}
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        if (!after_) {
            c.policy().header("X-Policy", "retained");
        }
        co_await next_value();
        if (after_) {
            c.policy().header("X-Policy", "retained");
        }
    }

private:
    bool after_;
};

// Replaces a downstream response through context metadata assigned after next().
class testing_facade_forbid_after final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.draft().status(ruvia::http_status::forbidden);
        c.draft().header("X-Test-Denied", "after");
        c.respond(c.text("denied"));
    }
};

// Post-processes a downstream response after next(), then fails.
class testing_facade_stamp_then_fail final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-After", "1");
        throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
    }
};

int all_path_evaluations = 0;
int on_path_evaluations = 0;

std::string_view counted_all_path() {
    ++all_path_evaluations;
    return "/counted-all";
}

std::string_view counted_on_path() {
    ++on_path_evaluations;
    return "/counted-on";
}

class testing_facade_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/t")
    RUVIA_ROUTES_BEGIN(testing_facade_controller)
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/a/:id/", slash_test);
    RUVIA_ALL(counted_all_path(), hello);
    RUVIA_ON((::ruvia::http_known_method::put, ::ruvia::http_known_method::delete_value),
        (counted_on_path()), hello);
    RUVIA_GET("/users/:id", user);
    RUVIA_GET("/empty//:id", user);
    RUVIA_GET("/slash-link", slash_link);
    RUVIA_GET("/literal/", hello);
    RUVIA_GET("/tail/:id//", user);
    RUVIA_GET("/trailing-links", trailing_links);
    RUVIA_GET("/tree/*", tree);
    RUVIA_GET("/dot-links", dot_links);
    RUVIA_GET("/head/:id", head_metadata, testing_facade_stamp);
    RUVIA_GET("/greet", greet);
    RUVIA_GET("/link", link);
    RUVIA_GET("/count", count);
    RUVIA_POST("/echo", echo, ruvia::json_body<testing_facade_echo>);
    RUVIA_GET("/boom", boom);
    RUVIA_GET("/whoami", whoami, testing_facade_auth);
    RUVIA_GET("/whoami-unbound", whoami_unbound);
    RUVIA_GET("/report", report);
    RUVIA_GET("/large", large);
    RUVIA_GET("/worker", worker);
    RUVIA_GET("/partial", partial);
    RUVIA_GET("/subrequest", subrequest);
    RUVIA_GET("/sub-file", sub_file);
    RUVIA_GET("/sub-static", sub_static);
    RUVIA_GET("/sub-static/:name", sub_static_named);
    RUVIA_POST("/sub-stream", sub_stream, ruvia::stream_body);
    RUVIA_GET("/sub-checks", sub_checks);
    RUVIA_GET("/sub-refused-file", sub_refused_file);
    RUVIA_GET("/typed/:id/:active", typed_inputs);
    RUVIA_POST("/json-array", json_array);
    RUVIA_GET("/const-handler", const_handler, testing_facade_const_middleware);
    RUVIA_GET("/conn-info", conn_info_handler);
    RUVIA_CONNECT("example.com:443", connect_authority);
    RUVIA_METHOD("PROPFIND", "/files", propfind);
    RUVIA_METHOD("PURGE", "/files", purge);
    RUVIA_METHOD("PROPFIND", "/dav-only", dav_only);
#ifdef RUVIA_ENABLE_REDIS
    RUVIA_GET("/session-clear", clear_session);
#endif
    RUVIA_ROUTES_END
    ruvia::task<ruvia::http_response> slash_test(ruvia::context& c) {
        co_return c.text("ok");
    }

private:
    ruvia::task<ruvia::http_response> boom(ruvia::context&) {
        throw std::runtime_error("boom");
        co_return ruvia::http_response{};
    }

    ruvia::task<ruvia::http_response> hello(ruvia::context& c) {
        co_return c.text("hello");
    }

    // Relays a subrequest named by the query as status and body; X-Test-Relayed
    // tells a relayed answer from a failure escaping dispatch().
    ruvia::task<ruvia::http_response> subrequest(ruvia::context& c) {
        std::string method(c.req().query("method").value_or("GET"));
        std::string target(c.req().query("target").value_or("/"));
        auto pending = c.dispatch({
            .method_ = method,
            .target_ = target,
        });
        method.assign("PUT");
        target.assign("/mutated");
        const auto child = co_await std::move(pending);
        c.draft().status(child.status());
        c.draft().header("X-Test-Relayed", "1");
        co_return c.text(child.body());
    }

    // A representation-specific answer whose metadata describes only its own body.
    ruvia::task<ruvia::http_response> partial(ruvia::context& c) {
        c.draft().status(ruvia::http_status::partial_content);
        c.draft().header("Content-Range", "bytes 0-3/10");
        c.draft().header("ETag", "\"v1\"");
        c.draft().header("Cache-Control", "public, max-age=60");
        c.draft().header("Content-Type", "application/octet-stream");
        co_return c.body("abcd");
    }

    ruvia::task<ruvia::http_response> user(ruvia::context& c) {
        co_return c.body(c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> head_metadata(ruvia::context& c) {
        c.draft().header("X-Handler-Method", c.req().method());
        c.draft().header("X-Route-Param", c.req().param("id").value_or("?"));
        co_return c.text("payload");
    }

    ruvia::task<ruvia::http_response> greet(ruvia::context& c) {
        std::string reply;
        reply.append(c.req().query("name").value_or("nobody"));
        reply.push_back('/');
        reply.append(c.req().cookie("sid").value_or("no-sid"));
        co_return c.body(std::move(reply));
    }

    ruvia::task<ruvia::http_response> link(ruvia::context& c) {
        co_return c.body(c.url_for("/t/users/:id", {"9"}));
    }

    ruvia::task<ruvia::http_response> count(ruvia::context& c) {
        auto& counter = c.worker_state<testing_facade_counter>();
        ++counter.count_;
        std::string reply;
        reply.append(std::to_string(counter.count_));
        co_return c.body(std::move(reply));
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto& body = c.req().validated<testing_facade_echo>();
        const auto value = body.get<"value">().has_value() ? body.get<"value">()->view()
                                                           : std::string_view("missing");
        if (const auto independent = c.req().header("X-Test-Independent")) {
            std::string reply;
            reply.append(*independent);
            reply.push_back('/');
            reply.append(value);
            co_return c.body(std::move(reply));
        }
        co_return c.body(value);
    }

    ruvia::task<ruvia::http_response> propfind(ruvia::context& c) {
        co_return c.body(c.req().method());
    }

    ruvia::task<ruvia::http_response> purge(ruvia::context& c) {
        co_return c.body(std::string_view("purged"));
    }

    ruvia::task<ruvia::http_response> dav_only(ruvia::context& c) {
        co_return c.body(std::string_view("dav"));
    }

#ifdef RUVIA_ENABLE_REDIS
    ruvia::task<ruvia::http_response> clear_session(ruvia::context& c) {
        c.session().clear();
        co_return c.body("cleared");
    }
#endif

    ruvia::task<ruvia::http_response> whoami(ruvia::context& c) {
        const auto& user_value = c.request_state<testing_facade_user>();
        std::string reply;
        reply.append(user_value.name_);
        reply.push_back('/');
        reply.append(std::to_string(user_value.level_));
        co_return c.body(std::move(reply));
    }

    // Runtime-sized collections stay inside a statically declared response
    // schema; JSON output never bypasses the model boundary.
    ruvia::task<ruvia::http_response> report(ruvia::context& c) {
        const std::string_view tags[] = {"a\"quoted", "b"};
        testing_facade_report report;
        report.set<"path">(c.req().path());
        report.set<"count">(static_cast<std::uint64_t>(std::size(tags)));
        auto& report_tags = report.ensure<"tags">();
        report_tags.reserve(std::size(tags));
        for (const auto tag : tags) {
            report_tags.emplace_back(tag);
        }
        co_return c.json(report);
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& c) {
        std::string body;
        const auto fill = c.req().query("fill").value_or("x");
        body.assign(1024 * 1024, fill.empty() ? 'x' : fill.front());
        co_return c.body(std::move(body));
    }

    ruvia::task<ruvia::http_response> worker(ruvia::context& c) {
        std::string state_value;
        state_value.append(c.worker().valid() ? "valid" : "invalid");
        state_value.push_back('/');
        state_value.append(c.worker().is_current() ? "current" : "foreign");
        co_return c.body(std::move(state_value));
    }

    // No auth middleware on this route, so nothing is bound and the lookup
    // throws std::logic_error.
    ruvia::task<ruvia::http_response> whoami_unbound(ruvia::context& c) {
        try {
            (void)c.request_state<testing_facade_user>();
            co_return c.body("bound");
        } catch (const std::logic_error&) {
            co_return c.body("unbound");
        }
    }
    ruvia::task<ruvia::http_response> typed_inputs(ruvia::context& c) const {
        const auto id = c.req().param<std::uint32_t>("id").value_or(0);
        const auto active = c.req().param<bool>("active").value_or(false);
        const auto page = c.req().query<std::int64_t>("page").value_or(1);
        const auto verbose = c.req().query<bool>("verbose").value_or(false);
        const auto* optional_user = c.try_request_state<testing_facade_user>();
        const std::string_view span_args[] = {"12", "true"};
        std::string reply;
        reply.append(std::to_string(id));
        reply.push_back(':');
        reply.append(active ? "1" : "0");
        reply.push_back(':');
        reply.append(std::to_string(page));
        reply.push_back(':');
        reply.append(verbose ? "1" : "0");
        reply.push_back(':');
        reply.append(optional_user == nullptr ? "none" : optional_user->name_);
        reply.push_back(':');
        reply.append(c.url_for("/t/typed/:id/:active", std::span<const std::string_view>(span_args)));
        co_return c.text(std::move(reply));
    }

    ruvia::task<ruvia::http_response> json_array(ruvia::context& c) const {
        const auto raw = co_await c.req().text();
        auto items = ruvia::from_json<ruvia::array<ruvia::string>>(raw);
        items.emplace_back("tail");
        co_return c.json(items);
    }

    ruvia::task<ruvia::http_response> const_handler(ruvia::context& c) const {
        const auto* user_ptr = c.try_request_state<testing_facade_user>();
        co_return c.text(user_ptr == nullptr ? "const-ok" : user_ptr->name_);
    }

    ruvia::task<ruvia::http_response> conn_info_handler(ruvia::context& c) const {
        std::string reply;
        reply.append(c.conn().remote().address().to_string());
        reply.push_back(':');
        reply.append(std::to_string(c.conn().remote().port()));
        reply.push_back('/');
        reply.append(c.conn().client().address().to_string());
        reply.push_back('/');
        reply.append(c.conn().via_trusted_proxy() ? "proxy" : "direct");
        reply.push_back('/');
        reply.append(c.conn().plain() != nullptr ? "plain" : "tls");
        co_return c.text(std::move(reply));
    }

    ruvia::task<ruvia::http_response> slash_link(ruvia::context& c) {
        co_return c.body(c.url_for("/t/empty//:id", {"456"}));
    }

    ruvia::task<ruvia::http_response> trailing_links(ruvia::context& c) {
        std::string reply;
        reply.append(c.url_for("/t/literal/"));
        reply.push_back(' ');
        reply.append(c.url_for("/t/tail/:id//", {"789"}));
        co_return c.body(std::move(reply));
    }

    ruvia::task<ruvia::http_response> tree(ruvia::context& c) {
        co_return c.body(c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> dot_links(ruvia::context& c) {
        const auto rejects = [&c](std::string_view pattern, std::string_view value) {
            try {
                (void)c.url_for(pattern, {value});
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        static constexpr std::string_view cases[][2] = {{"/t/users/:id", "."}, {"/t/users/:id", ".."},
            {"/t/tree/*", "."}, {"/t/tree/*", ".."}, {"/t/tree/*", "./a"}, {"/t/tree/*", "a/../b"},
            {"/t/tree/*", "a/."}, {"/t/tree/*", "a\\b"}, {"example.com:443", "a"}};
        std::string reply;
        for (const auto& entry : cases) {
            reply.push_back(rejects(entry[0], entry[1]) ? 'x' : '-');
        }
        reply.push_back(' ');
        reply.append(c.url_for("/t/tree/*", {"a.b/..c/.d"}));
        // Below the root a leading '/' is an empty segment the matcher strips
        // again, so the value round-trips.
        reply.push_back(' ');
        reply.append(c.url_for("/t/tree/*", {"/a"}));
        co_return c.body(std::move(reply));
    }

    ruvia::task<void> connect_authority(ruvia::context&) {
        co_return;
    }

    ruvia::task<ruvia::http_response> sub_file(ruvia::context& c) {
        co_return co_await c.file({.path_ = testing_facade_file_path()});
    }

    ruvia::task<ruvia::http_response> sub_static(ruvia::context& c) {
        co_return co_await c.static_file(*testing_facade_static_root(), {.relative_path_ = "asset.txt"});
    }

    ruvia::task<ruvia::http_response> sub_static_named(ruvia::context& c) {
        co_return co_await c.static_file(
            *testing_facade_static_root(), {.relative_path_ = c.req().param("name").value_or("")});
    }

    ruvia::task<ruvia::http_response> sub_stream(ruvia::context& c) {
        co_return c.text("stream");
    }

    ruvia::task<ruvia::http_response> sub_checks(ruvia::context& c) {
        bool stream_rejected = false;
        try {
            (void)co_await c.dispatch({.method_ = "POST", .target_ = "/t/sub-stream", .body_ = "abc"});
        } catch (const std::logic_error&) {
            stream_rejected = true;
        }
        bool multipart_file_rejected = false;
        try {
            const ruvia::http_header_view range_headers[] = {{"Range", "bytes=0-1, 4-5"}};
            (void)co_await c.dispatch({
                .method_ = "GET",
                .target_ = "/t/sub-file",
                .headers_ = range_headers,
            });
        } catch (const std::logic_error&) {
            multipart_file_rejected = true;
        }
        // Connection-specific fields (RFC 9110 section 7.6.1) belong to the
        // protocol, like framing fields; a subrequest cannot supply them.
        bool connection_fields_rejected = true;
        for (const auto name : {"TE", "Keep-Alive", "Proxy-Connection", "Transfer-Encoding"}) {
            const ruvia::http_header_view connection_headers[] = {{name, "trailers"}};
            try {
                (void)co_await c.dispatch({
                    .method_ = "GET",
                    .target_ = "/t/hello",
                    .headers_ = connection_headers,
                });
                connection_fields_rejected = false;
            } catch (const std::invalid_argument&) {
            }
        }
        const ruvia::http_header_view encoding_headers[] = {{"Accept-Encoding", "identity;q=0"}};
        const auto not_acceptable = co_await c.dispatch({
            .method_ = "GET",
            .target_ = "/t/hello",
            .headers_ = encoding_headers,
        });
        std::string reply;
        reply.push_back(stream_rejected ? '1' : '0');
        reply.push_back(multipart_file_rejected ? '1' : '0');
        reply.push_back(not_acceptable.status() == ruvia::http_status::not_acceptable ? '1' : '0');
        reply.push_back(connection_fields_rejected ? '1' : '0');
        co_return c.body(std::move(reply));
    }

    // A refused coding turns the buffered /t/hello answer into a 406 from the
    // error handler, which this case's handler answers with a file: a shape
    // dispatch cannot deliver, whichever step produced it.
    ruvia::task<ruvia::http_response> sub_refused_file(ruvia::context& c) {
        const ruvia::http_header_view encoding_headers[] = {{"Accept-Encoding", "identity;q=0"}};
        try {
            const auto refused = co_await c.dispatch({
                .method_ = "GET",
                .target_ = "/t/hello",
                .headers_ = encoding_headers,
            });
            std::string reply("delivered:");
            reply.append(std::to_string(refused.body().size()));
            co_return c.body(std::move(reply));
        } catch (const std::logic_error&) {
            co_return c.body("rejected");
        }
    }
};

ruvia::task<ruvia::http_response> facade_not_found(ruvia::context& c) {
    c.draft().status(ruvia::http_status::not_found);
    co_return c.body("custom-miss");
}

ruvia::task<ruvia::http_response> api_scoped_miss(ruvia::context& c) {
    c.draft().status(ruvia::http_status::not_found);
    co_return c.body("api-miss");
}

ruvia::task<ruvia::http_response> facade_error(ruvia::context& c, ruvia::http_error_info error) {
    c.draft().status(error.status());
    co_return c.body("custom-error");
}

}  // namespace

TEST(Ruvia, testing_facade_multi_route_paths_are_evaluated_once_per_registration) {
    const auto all_before = all_path_evaluations;
    const auto on_before = on_path_evaluations;
    ruvia::test_app app;
    const auto all_response = app.request(ruvia::test_request::get("/t/counted-all"));
    EXPECT_EQ(all_response.body(), std::string_view("hello"));
    const auto on_response = app.request(ruvia::test_request::put("/t/counted-on"));
    EXPECT_EQ(on_response.body(), std::string_view("hello"));
    EXPECT_TRUE(all_path_evaluations > all_before);
    EXPECT_EQ(all_path_evaluations - all_before, on_path_evaluations - on_before);
}

TEST(Ruvia, testing_facade_dispatches_routes_params_query_and_cookies) {
    ruvia::test_app app;

    const auto hello = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_TRUE(hello.status() == ruvia::http_status::ok);
    EXPECT_EQ(hello.body(), std::string_view("hello"));

    const auto user_value = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_EQ(user_value.body(), std::string_view("42"));

    const auto greet =
        app.request(ruvia::test_request::get("/t/greet?name=ada").cookie("sid", "s-1"));
    EXPECT_EQ(greet.body(), std::string_view("ada/s-1"));

    const auto absolute = app.request(
        ruvia::test_request::get("http://example.test/t/hello").header("Host", "stale.example"));
    EXPECT_TRUE(absolute.status() == ruvia::http_status::ok);
    EXPECT_EQ(absolute.body(), std::string_view("hello"));

    const auto server_wide_options = app.request(
        ruvia::test_request::options("http://example.test").header("Host", "stale.example"));
    EXPECT_TRUE(server_wide_options.status() == ruvia::http_status::no_content);

    const auto link = app.request(ruvia::test_request::get("/t/link"));
    EXPECT_EQ(link.body(), std::string_view("/t/users/9"));

    const auto worker_value = app.request(ruvia::test_request::get("/t/worker"));
    EXPECT_EQ(worker_value.body(), std::string_view("valid/current"));

    // The automatic HEAD fallback answers with the GET status and no body.
    // Writer-synthesized framing headers (Content-Length, Date) are not part
    // of the in-memory dispatch product.
    const auto head = app.request(ruvia::test_request::head("/t/hello"));
    EXPECT_TRUE(head.status() == ruvia::http_status::ok);
    EXPECT_TRUE(head.body().empty());
}

TEST(Ruvia, testing_facade_head_fallback_preserves_request_method_and_route_params) {
    ruvia::test_app app;
    const auto get = app.request(ruvia::test_request::get("/t/head/42"));
    EXPECT_TRUE(get.status() == ruvia::http_status::ok);
    EXPECT_EQ(get.body(), std::string_view("payload"));
    EXPECT_EQ(get.header("X-Handler-Method").value_or(""), std::string_view("GET"));
    EXPECT_EQ(get.header("X-Middleware-Method").value_or(""), std::string_view("GET"));

    const auto head = app.request(ruvia::test_request::head("/t/head/42"));
    EXPECT_TRUE(head.status() == ruvia::http_status::ok);
    EXPECT_TRUE(head.body().empty());
    EXPECT_EQ(head.header("X-Handler-Method").value_or(""), std::string_view("HEAD"));
    EXPECT_EQ(head.header("X-Middleware-Method").value_or(""), std::string_view("HEAD"));
    EXPECT_EQ(head.header("X-Route-Param").value_or(""), std::string_view("42"));
    EXPECT_EQ(head.header("X-Test-Stamp").value_or(""), std::string_view("on"));
}

TEST(Ruvia, testing_facade_rejects_invalid_request_line_targets) {
    ruvia::test_app app;

    EXPECT_TRUE(app.request(ruvia::test_request::get("/bad path")).status() ==
                ruvia::http_status::bad_request);
    EXPECT_TRUE(
        app.request(ruvia::test_request::get("*")).status() == ruvia::http_status::bad_request);
    EXPECT_TRUE(app.request(ruvia::test_request::get("/bad%zz")).status() ==
                ruvia::http_status::bad_request);
    EXPECT_TRUE(app.request(ruvia::test_request::method("BAD(METHOD", "/")).status() ==
                ruvia::http_status::bad_request);
}

TEST(Ruvia, testing_facade_rejects_invalid_request_headers) {
    ruvia::test_app app;

    EXPECT_TRUE(
        app.request(ruvia::test_request::get("/t/hello").header("Bad Header", "x")).status() ==
        ruvia::http_status::bad_request);
    EXPECT_TRUE(
        app.request(
               ruvia::test_request::get("/t/hello").header("X-Bad", std::string_view("a\rb", 3)))
            .status() == ruvia::http_status::bad_request);
    const auto independent = app.request(ruvia::test_request::post("/t/echo")
            .json(R"({"value":"body"})")
            .header("Content-Length", "5")
            .header("Content-Length", "6")
            .header("Transfer-Encoding", "chunked")
            .header("X-Test-Independent", "header"));
    EXPECT_TRUE(independent.status() == ruvia::http_status::ok);
    EXPECT_EQ(independent.body(), std::string_view("header/body"));

    auto many = ruvia::test_request::get("/t/hello");
    for (std::size_t i = 0; i < 80; ++i) {
        many.header("X-Test", "v");
    }
    EXPECT_TRUE(app.request(many).status() == ruvia::http_status::ok);

    const std::string oversized_header_value(ruvia::max_http_header_bytes, 'x');
    EXPECT_TRUE(
        app.request(ruvia::test_request::get("/t/hello").header("X-Big", oversized_header_value))
            .status() == ruvia::http_status::request_header_fields_too_large);
}

TEST(Ruvia, testing_facade_runs_model_bodies_with_media_type_split) {
    ruvia::test_app app;

    const auto ok = app.request(ruvia::test_request::post("/t/echo").json(R"({"value":"hi"})"));
    EXPECT_TRUE(ok.status() == ruvia::http_status::ok);
    EXPECT_EQ(ok.body(), std::string_view("hi"));

    const auto wrong_type =
        app.request(ruvia::test_request::post("/t/echo").body(R"({"value":"hi"})", "text/plain"));
    EXPECT_TRUE(wrong_type.status() == ruvia::http_status::unsupported_media_type);

    const auto bad_body = app.request(ruvia::test_request::post("/t/echo").json("{not-json"));
    EXPECT_TRUE(bad_body.status() == ruvia::http_status::bad_request);
}

TEST(Ruvia, testing_facade_applies_app_level_configuration) {
    ruvia::test_app app;
    app.use<testing_facade_stamp>()
        .on_not_found(&facade_not_found)
        .on_not_found({.prefix_ = "/api", .handler_ = &api_scoped_miss});
    app.use_worker_state<testing_facade_counter>();

    // Global middleware wraps every matched route.
    const auto stamped = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(stamped.header("X-Test-Stamp").value_or(""), std::string_view("on"));

    // Worker state persists across requests on the same test_app.
    const auto first_count = app.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(first_count.body(), std::string_view("1"));
    const auto second_count = app.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(second_count.body(), std::string_view("2"));

    // Prefix fallback wins under its scope; the app-wide one covers the rest.
    const auto scoped_miss = app.request(ruvia::test_request::get("/api/missing"));
    EXPECT_EQ(scoped_miss.body(), std::string_view("api-miss"));
    const auto global_miss = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(global_miss.body(), std::string_view("custom-miss"));

    // 405 keeps flowing through the production fallback path too.
    const auto not_allowed = app.request(ruvia::test_request::delete_request("/t/hello"));
    EXPECT_TRUE(not_allowed.status() == ruvia::http_status::method_not_allowed);

    // The route table is sealed after the first request.
    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    EXPECT_TRUE(sealed);
}

TEST(Ruvia, testing_facade_retains_startup_failure_without_retrying) {
    ruvia::test_app app;
    int factory_calls = 0;
    app.use_worker_state<testing_facade_startup_failure_state>([&]()
                                                                   -> testing_facade_startup_failure_state {
        if (++factory_calls == 1) {
            throw std::runtime_error("testing facade startup failed");
        }
        return {};
    });

    for (int attempt_value = 0; attempt_value != 2; ++attempt_value) {
        bool threw = false;
        try {
            (void)app.request(ruvia::test_request::get("/t/hello"));
        } catch (const std::runtime_error& error) {
            threw = true;
            EXPECT_EQ(std::string_view(error.what()),
                std::string_view("testing facade startup failed"));
        }
        EXPECT_TRUE(threw);
    }
    EXPECT_EQ(factory_calls, 1);

    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    EXPECT_TRUE(sealed);
}

TEST(Ruvia, testing_facade_retains_pre_worker_startup_failure_without_retrying) {
    ruvia::test_app app;
    int constructor_calls = 0;
    app.use<testing_facade_throwing_middleware>(&constructor_calls);

    for (int attempt_value = 0; attempt_value != 2; ++attempt_value) {
        bool threw = false;
        try {
            (void)app.request(ruvia::test_request::get("/t/hello"));
        } catch (const testing_facade_startup_error& error) {
            threw = true;
            EXPECT_EQ(std::string_view(error.what()),
                std::string_view("testing facade middleware startup failed"));
        }
        EXPECT_TRUE(threw);
    }
    EXPECT_EQ(constructor_calls, 1);

    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    EXPECT_TRUE(sealed);
}

TEST(Ruvia, testing_facade_constructs_middleware_from_registration_arguments) {
    ruvia::test_app app;
    app.use<testing_facade_configured_stamp>("audit", 2);

    const auto first = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(first.header("X-Test-Configured").value_or(""), std::string_view("audit"));
    EXPECT_EQ(first.header("X-Test-Level").value_or(""), std::string_view("two"));

    // One instance serves every request, so the arguments must still be readable
    // after the first dispatch rather than having been consumed by it.
    const auto second = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(second.header("X-Test-Configured").value_or(""), std::string_view("audit"));

    // Separate registrations of the same type stay independent.
    ruvia::test_app other;
    other.use<testing_facade_configured_stamp>("other", 5);
    const auto distinct = other.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(distinct.header("X-Test-Configured").value_or(""), std::string_view("other"));
    EXPECT_EQ(distinct.header("X-Test-Level").value_or(""), std::string_view("other"));
}

TEST(Ruvia, testing_facade_runs_fallback_handlers_that_carry_state) {
    // A fallback handler used to be a plain function pointer, so anything it
    // needed had to be a global. It now accepts any callable, including one that
    // captures the collaborators the handler depends on.
    struct branding final {
        std::string label_{};
    };
    const branding branding_value{std::string("tenant-a")};

    ruvia::test_app app;
    app.on_not_found([branding_value](ruvia::context& c) -> ruvia::task<ruvia::http_response> {
        c.draft().status(ruvia::http_status::not_found);
        co_return c.text(std::string_view(branding_value.label_));
    });
    app.on_error([branding_value](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        c.draft().status(error.status());
        std::string body;
        body.append(branding_value.label_);
        body.append(":error");
        co_return c.text(std::move(body));
    });

    const auto missed = app.request(ruvia::test_request::get("/nowhere"));
    EXPECT_TRUE(missed.status() == ruvia::http_status::not_found);
    EXPECT_EQ(missed.body(), std::string_view("tenant-a"));

    // The captured state is still readable on a later request: the callable was
    // copied at registration, not borrowed from the caller's frame.
    const auto missed_again = app.request(ruvia::test_request::get("/nowhere/else"));
    EXPECT_EQ(missed_again.body(), std::string_view("tenant-a"));

    const auto failed = app.request(ruvia::test_request::get("/t/boom"));
    EXPECT_TRUE(failed.status() == ruvia::http_status::internal_server_error);
    EXPECT_EQ(failed.body(), std::string_view("tenant-a:error"));
}

TEST(Ruvia, testing_facade_error_handler_throwing_validation_issues_keeps_its_status) {
    // An http_error thrown inside on_error is answered with the default error
    // response for that error, issues included; it is not an error-handler
    // failure and must not be rewritten as a 500.
    ruvia::test_app app;
    app.on_error([](ruvia::context& c, ruvia::http_error_info) -> ruvia::task<ruvia::http_response> {
        ruvia::validator checks;
        std::optional<std::string> absent;
        checks.required(absent, "email", "email is required");
        checks.throw_if_invalid({.status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation"});
        co_return c.text("unreachable");
    });

    const auto failed = app.request(ruvia::test_request::get("/t/boom"));
    EXPECT_EQ(failed.status(), ruvia::http_status::unprocessable_content);
    EXPECT_TRUE(failed.body().find("\"invalid_payload\"") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("payload failed validation") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("\"email\"") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("email is required") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("error_handler_failed") == std::string_view::npos);
}

TEST(Ruvia, testing_facade_error_responses_are_not_renegotiated_into_406) {
    // An error response is not a representation of the target resource, so a
    // refused coding must neither turn the 500 into a 406 nor run the error
    // handler a second time. A successful response still becomes a 406.
    int calls = 0;
    ruvia::test_app app;
    app.on_error([counter = &calls](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        ++*counter;
        c.draft().status(error.status());
        co_return c.text("err");
    });

    // The test app configures no compression, so "gzip, identity;q=0" cannot be
    // satisfied either.
    for (const std::string_view refused : {std::string_view("identity;q=0, *;q=0"),
             std::string_view("gzip, identity;q=0")}) {
        calls = 0;
        const auto failed =
            app.request(ruvia::test_request::get("/t/boom").header("Accept-Encoding", refused));
        EXPECT_EQ(failed.status(), ruvia::http_status::internal_server_error);
        EXPECT_EQ(failed.body(), std::string_view("err"));
        EXPECT_EQ(calls, 1);

        calls = 0;
        const auto refused_ok =
            app.request(ruvia::test_request::get("/t/hello").header("Accept-Encoding", refused));
        EXPECT_EQ(refused_ok.status(), ruvia::http_status::not_acceptable);
        EXPECT_EQ(calls, 1);
    }
}

TEST(Ruvia, testing_facade_rejects_duplicate_normalized_fallback_prefixes) {
    ruvia::test_app not_found_app;
    not_found_app.on_not_found({.prefix_ = "/api", .handler_ = &api_scoped_miss});

    bool not_found_rejected = false;
    try {
        not_found_app.on_not_found({.prefix_ = "/api///", .handler_ = &api_scoped_miss});
    } catch (const std::invalid_argument& error) {
        not_found_rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    EXPECT_TRUE(not_found_rejected);

    ruvia::test_app error_app;
    error_app.on_error({.prefix_ = "/api/", .handler_ = &facade_error});

    bool error_rejected = false;
    try {
        error_app.on_error({.prefix_ = "/api", .handler_ = &facade_error});
    } catch (const std::invalid_argument& error) {
        error_rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    EXPECT_TRUE(error_rejected);

    bool malformed_rejected = false;
    try {
        error_app.on_not_found({.prefix_ = "api", .handler_ = &api_scoped_miss});
    } catch (const std::invalid_argument& error) {
        malformed_rejected = std::string_view(error.what()) == "fallback prefix must start with '/'";
    }
    EXPECT_TRUE(malformed_rejected);

    const auto rejects_malformed_prefixed_scope = [](std::string_view prefix) {
        ruvia::test_app app;
        try {
            app.on_not_found({.prefix_ = prefix, .handler_ = &api_scoped_miss});
        } catch (const std::invalid_argument& error) {
            return std::string_view(error.what()) ==
                   "fallback prefix must be an origin-form path without query";
        }
        return false;
    };
    EXPECT_TRUE(rejects_malformed_prefixed_scope("/api?debug=1"));
    EXPECT_TRUE(rejects_malformed_prefixed_scope("/bad path"));
    EXPECT_TRUE(rejects_malformed_prefixed_scope("/api#fragment"));
}

TEST(Ruvia, testing_facade_rejects_root_scopes_and_second_app_wide_fallbacks) {
    const auto rejected = [](auto&& registration) {
        try {
            registration();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    ruvia::test_app app;
    // The prefix-less form is the only app-wide spelling; "/" would shadow it.
    EXPECT_TRUE(rejected([&app] { app.on_not_found({.prefix_ = "/", .handler_ = &api_scoped_miss}); }));
    EXPECT_TRUE(rejected([&app] { app.on_error({.prefix_ = "//", .handler_ = &facade_error}); }));
    EXPECT_TRUE(rejected([&app] { app.use_at<testing_facade_scoped>({.prefix_ = "/"}, "root"); }));

    app.on_not_found(&facade_not_found);
    app.on_error(&facade_error);
    EXPECT_TRUE(rejected([&app] { app.on_not_found(&api_scoped_miss); }));
    EXPECT_TRUE(rejected([&app] { app.on_error(&facade_error); }));

    // Neither the rejected scopes nor the rejected replacement took effect.
    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    EXPECT_EQ(missing.body(), std::string_view("custom-miss"));
    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(matched.status(), ruvia::http_status::ok);
    EXPECT_TRUE(!matched.header("X-Test-Scope"));
}

TEST(Ruvia, testing_facade_isolates_instances) {
    // Two TestApps own separate controller instances and worker state.
    ruvia::test_app first;
    first.use_worker_state<testing_facade_counter>();
    ruvia::test_app second;
    second.use_worker_state<testing_facade_counter>();

    const auto first_count = first.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(first_count.body(), std::string_view("1"));
    const auto second_count = second.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(second_count.body(), std::string_view("1"));
}

TEST(Ruvia, testing_facade_middleware_publishes_request_state_to_handler) {
    ruvia::test_app app;

    const auto named = app.request(ruvia::test_request::get("/t/whoami").header("X-User", "ada"));
    EXPECT_EQ(named.status(), ruvia::http_status::ok);
    EXPECT_EQ(named.body(), std::string_view("ada/2"));

    // A second request gets its own binding, not the previous one's value --
    // this is request scope, not worker scope.
    const auto anonymous = app.request(ruvia::test_request::get("/t/whoami"));
    EXPECT_EQ(anonymous.body(), std::string_view("anonymous/2"));
}

TEST(Ruvia, testing_facade_request_state_is_absent_without_its_middleware) {
    ruvia::test_app app;
    const auto response =
        app.request(ruvia::test_request::get("/t/whoami-unbound").header("X-User", "ada"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.body(), std::string_view("unbound"));
}

TEST(Ruvia, testing_facade_builds_a_runtime_sized_response_model) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::get("/t/report"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.body(),
        std::string_view(R"({"path":"/t/report","count":2,"tags":["a\"quoted","b"]})"));
    const auto content_type_value = response.header("Content-Type");
    EXPECT_TRUE(content_type_value.has_value());
    EXPECT_EQ(*content_type_value, std::string_view("application/json"));
}

TEST(Ruvia, testing_facade_response_owns_large_bodies_across_requests) {
    ruvia::test_app app;
    const auto first = app.request(ruvia::test_request::get("/t/large?fill=a"));
    EXPECT_EQ(first.body().size(), std::size_t(1024 * 1024));
    EXPECT_EQ(first.body(), std::string(1024 * 1024, 'a'));

    const auto second = app.request(ruvia::test_request::get("/t/large?fill=b"));
    EXPECT_EQ(second.body().size(), std::size_t(1024 * 1024));
    EXPECT_EQ(first.body().size(), std::size_t(1024 * 1024));
    EXPECT_EQ(second.body(), std::string(1024 * 1024, 'b'));
    EXPECT_EQ(first.body(), std::string(1024 * 1024, 'a'));
}

TEST(Ruvia, testing_facade_path_scoped_middleware_runs_only_under_its_prefix) {
    ruvia::test_app app;
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/users"}, "users");

    // Under the scope, on both the exact prefix path shape and a deeper one.
    const auto scoped = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_EQ(scoped.status(), ruvia::http_status::ok);
    const auto scoped_header = scoped.header("X-Test-Scope");
    EXPECT_TRUE(scoped_header.has_value());
    EXPECT_EQ(*scoped_header, std::string_view("users"));

    // A sibling route outside the scope never receives the frame.
    const auto outside = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(outside.status(), ruvia::http_status::ok);
    EXPECT_TRUE(!outside.header("X-Test-Scope").has_value());
}

TEST(Ruvia, testing_facade_path_scope_matches_whole_segments_only) {
    ruvia::test_app app;
    // "/t/user" must not scope "/t/users/:id" -- that is a different segment,
    // not a deeper path.
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/user"}, "prefix-only");

    const auto response = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_TRUE(!response.header("X-Test-Scope").has_value());
}

TEST(Ruvia, testing_facade_path_scope_normalizes_a_trailing_slash) {
    ruvia::test_app app;
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/users/"}, "trailing");

    const auto response = app.request(ruvia::test_request::get("/t/users/42"));
    const auto header_value = response.header("X-Test-Scope");
    EXPECT_TRUE(header_value.has_value());
    EXPECT_EQ(*header_value, std::string_view("trailing"));
}

TEST(Ruvia, testing_facade_app_wide_middleware_still_runs_everywhere) {
    ruvia::test_app app;
    app.use<testing_facade_scoped>("global");

    // Bound to names: dispatch_response owns its headers, so header() is rightly
    // rvalue-deleted and a view into a temporary is not obtainable.
    const auto hello = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_TRUE(hello.header("X-Test-Scope").has_value());
    const auto user_value = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_TRUE(user_value.header("X-Test-Scope").has_value());
}

TEST(Ruvia, testing_facade_unmatched_middleware_wraps_the_404_terminal) {
    ruvia::test_app app;
    app.use<testing_facade_always>();
    app.use<testing_facade_stamp>();

    // A matched route runs both, as before.
    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(matched.status(), ruvia::http_status::ok);
    EXPECT_TRUE(matched.header("X-Test-Always").has_value());
    EXPECT_TRUE(matched.header("X-Test-Stamp").has_value());

    // An unmatched one runs only what declared itself for unmatched requests.
    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    const auto always = missing.header("X-Test-Always");
    EXPECT_TRUE(always.has_value());
    EXPECT_EQ(*always, std::string_view("on"));
    EXPECT_TRUE(!missing.header("X-Test-Stamp").has_value());
}

TEST(Ruvia, testing_facade_unmatched_middleware_also_wraps_405_and_501) {
    ruvia::test_app app;
    app.use<testing_facade_always>();

    // Known method, wrong verb for an existing path.
    const auto wrong_method = app.request(ruvia::test_request::post("/t/hello"));
    EXPECT_EQ(wrong_method.status(), ruvia::http_status::method_not_allowed);
    EXPECT_TRUE(wrong_method.header("X-Test-Always").has_value());
    // The Allow header the fallback sets must survive the chain.
    EXPECT_TRUE(wrong_method.header("Allow").has_value());

    // A token no route registered: the server does not know it, so 501.
    const auto unknown_method = app.request(ruvia::test_request::method("FROBNICATE", "/t/hello"));
    EXPECT_EQ(unknown_method.status(), ruvia::http_status::not_implemented);
    EXPECT_TRUE(unknown_method.header("X-Test-Always").has_value());
}

TEST(Ruvia, testing_facade_replacement_responses_do_not_inherit_the_discarded_one) {
    // An error raised after next() describes itself, not the 206 it replaces:
    // no range, validator or cache policy of the discarded representation.
    {
        ruvia::test_app app;
        app.use_at<testing_facade_deny_after>({.prefix_ = "/t/partial"});
        const auto denied = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
        EXPECT_EQ(denied.header("Content-Type").value_or(std::string_view{}),
            std::string_view("application/problem+json"));
        EXPECT_TRUE(!denied.header("Content-Range").has_value());
        EXPECT_TRUE(!denied.header("ETag").has_value());
        EXPECT_TRUE(!denied.header("Cache-Control").has_value());
    }
    // Likewise a response built and sent in its place.
    {
        ruvia::test_app app;
        app.use_at<testing_facade_replace_after>({.prefix_ = "/t/partial"});
        const auto replaced = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(replaced.status(), ruvia::http_status::ok);
        EXPECT_EQ(replaced.body(), std::string_view("denied"));
        EXPECT_EQ(replaced.header("Content-Type").value_or(std::string_view{}),
            std::string_view("text/plain; charset=UTF-8"));
        EXPECT_TRUE(!replaced.header("Content-Range").has_value());
        EXPECT_TRUE(!replaced.header("ETag").has_value());
        EXPECT_TRUE(!replaced.header("Cache-Control").has_value());
    }
}

TEST(Ruvia, testing_facade_explicit_policy_survives_replacement_before_and_after_next) {
    for (const bool after : {false, true}) {
        ruvia::test_app app;
        app.use<testing_facade_replace_after>();
        app.use<testing_facade_policy_stamp>(after);
        const auto response = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(response.body(), std::string_view("denied"));
        EXPECT_EQ(response.header("X-Policy"), std::string_view("retained"));
        EXPECT_TRUE(!response.header("Content-Range").has_value());
        EXPECT_TRUE(!response.header("ETag").has_value());
    }
}

TEST(Ruvia, testing_facade_metadata_set_after_a_final_response_is_kept) {
    // 后处理必须明确选择 edit_response；构造替换响应则写入独立 draft。
    {
        ruvia::test_app app;
        app.use_at<testing_facade_scoped>({.prefix_ = "/t/partial"}, "after");
        const auto stamped = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(stamped.status(), ruvia::http_status::partial_content);
        EXPECT_EQ(stamped.body(), std::string_view("abcd"));
        EXPECT_EQ(stamped.header("X-Test-Scope").value_or(std::string_view{}), std::string_view("after"));
        EXPECT_TRUE(stamped.header("Content-Range").has_value());
    }
    // A replacement built afterwards starts from that metadata, not from the
    // discarded response.
    {
        ruvia::test_app app;
        app.use_at<testing_facade_forbid_after>({.prefix_ = "/t/partial"});
        const auto denied = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
        EXPECT_EQ(denied.body(), std::string_view("denied"));
        EXPECT_EQ(denied.header("X-Test-Denied").value_or(std::string_view{}), std::string_view("after"));
        EXPECT_EQ(denied.header("Content-Type").value_or(std::string_view{}),
            std::string_view("text/plain; charset=UTF-8"));
        EXPECT_TRUE(!denied.header("Content-Range").has_value());
        EXPECT_TRUE(!denied.header("ETag").has_value());
        EXPECT_TRUE(!denied.header("Cache-Control").has_value());
    }
    // A custom error handler answering a failure raised after next() keeps the
    // status it assigns through the context.
    {
        ruvia::test_app app;
        app.on_error(&facade_error);
        app.use_at<testing_facade_deny_after>({.prefix_ = "/t/partial"});
        const auto denied = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
        EXPECT_EQ(denied.body(), std::string_view("custom-error"));
        EXPECT_TRUE(!denied.header("Content-Range").has_value());
        EXPECT_TRUE(!denied.header("ETag").has_value());
        EXPECT_TRUE(!denied.header("Cache-Control").has_value());
    }
    // 已选响应的后处理字段只属于该响应，不污染随后生成的错误表示。
    {
        ruvia::test_app app;
        app.on_error(&facade_error);
        app.use_at<testing_facade_stamp_then_fail>({.prefix_ = "/t/partial"});
        const auto failed = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(failed.status(), ruvia::http_status::conflict);
        EXPECT_EQ(failed.body(), std::string_view("custom-error"));
        EXPECT_TRUE(!failed.header("X-Test-After").has_value());
        EXPECT_TRUE(!failed.header("Content-Range").has_value());
        EXPECT_TRUE(!failed.header("ETag").has_value());
    }
}

TEST(Ruvia, testing_facade_failure_after_next_discards_selected_response_metadata) {
    // 默认错误响应与自定义错误处理器具有相同的明确归属规则。
    ruvia::test_app app;
    app.use_at<testing_facade_stamp_then_fail>({.prefix_ = "/t/partial"});
    const auto failed = app.request(ruvia::test_request::get("/t/partial"));
    EXPECT_EQ(failed.status(), ruvia::http_status::conflict);
    EXPECT_TRUE(!failed.header("X-Test-After").has_value());
    EXPECT_TRUE(!failed.header("Content-Range").has_value());
}

TEST(Ruvia, testing_facade_subrequest_answers_like_a_network_request) {
    ruvia::test_app app;
    app.use<testing_facade_unmatched_reject>();

    const auto missing = app.request(ruvia::test_request::get("/t/subrequest?target=/guarded/missing"));
    EXPECT_EQ(missing.status(), ruvia::http_status::unauthorized);
    EXPECT_TRUE(missing.header("X-Test-Relayed").has_value());
    const auto options =
        app.request(ruvia::test_request::get("/t/subrequest?method=OPTIONS&target=/guarded/missing"));
    EXPECT_EQ(options.status(), ruvia::http_status::unauthorized);
    EXPECT_TRUE(options.header("X-Test-Relayed").has_value());

    // RFC 9110 9.3.2: HEAD answers with the GET metadata but no body.
    const auto head = app.request(ruvia::test_request::get("/t/subrequest?method=HEAD&target=/t/hello"));
    EXPECT_EQ(head.status(), ruvia::http_status::ok);
    EXPECT_TRUE(head.body().empty());
    const auto get = app.request(ruvia::test_request::get("/t/subrequest?target=/t/hello"));
    EXPECT_EQ(get.body(), std::string_view("hello"));

    // The same dispatch entry preserves route parameters and error handling
    // when reached through the public subrequest API.
    for (const auto target : {"/t/users/42", "/t/boom", "/missing"}) {
        const auto direct = app.request(ruvia::test_request::get(target));
        const auto relayed = app.request(ruvia::test_request::get(std::string("/t/subrequest?target=") + target));
        EXPECT_EQ(relayed.status(), direct.status());
        EXPECT_TRUE(relayed.header("X-Test-Relayed").has_value());
        EXPECT_EQ(relayed.body(), direct.body());
    }
}

TEST(Ruvia, testing_facade_unmatched_middleware_wraps_automatic_options_answers) {
    ruvia::test_app app;
    app.use<testing_facade_always>();

    // The per-resource OPTIONS answer and the server-wide OPTIONS * one are
    // unmatched answers too, so the same response layer covers them.
    const auto resource = app.request(ruvia::test_request::options("/t/hello"));
    EXPECT_EQ(resource.status(), ruvia::http_status::no_content);
    EXPECT_TRUE(resource.header("Allow").has_value());
    EXPECT_TRUE(resource.header("X-Test-Always").has_value());

    const auto server = app.request(ruvia::test_request::options("*"));
    EXPECT_EQ(server.status(), ruvia::http_status::no_content);
    // Methods differ per resource, so the server-wide answer carries no Allow.
    EXPECT_TRUE(!server.header("Allow").has_value());
    EXPECT_TRUE(server.header("X-Test-Always").has_value());
}

TEST(Ruvia, testing_facade_unmatched_middleware_runs_with_a_custom_not_found_handler) {
    ruvia::test_app app;
    app.use<testing_facade_always>();
    app.on_not_found(&facade_not_found);

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    // The application's own fallback body still wins; the chain only wraps it.
    EXPECT_EQ(missing.body(), std::string_view("custom-miss"));
    EXPECT_TRUE(missing.header("X-Test-Always").has_value());
}

TEST(Ruvia, testing_facade_without_unmatched_middleware_the_404_path_is_unchanged) {
    ruvia::test_app app;
    app.use<testing_facade_stamp>();

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    EXPECT_TRUE(!missing.header("X-Test-Stamp").has_value());
}

TEST(Ruvia, testing_facade_security_headers_reach_unmatched_requests) {
    // The concrete gap this exists to close: a 404 is an attacker-reachable URL
    // and needs the same content policy a matched route gets.
    ruvia::test_app app;
    app.use<ruvia::security_headers_middleware>();

    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_TRUE(matched.header("Content-Security-Policy").has_value());

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    const auto policy = missing.header("Content-Security-Policy");
    EXPECT_TRUE(policy.has_value());
    EXPECT_EQ(*policy, std::string_view("default-src 'self'"));
    EXPECT_TRUE(missing.header("X-Content-Type-Options").has_value());
    EXPECT_TRUE(missing.header("X-Frame-Options").has_value());
}

TEST(Ruvia, testing_facade_security_headers_own_configured_policy) {
    ruvia::test_app app;
    {
        ruvia::security_headers_config config;
        config.frame_options_header_ = ruvia::default_security_header_policy::omit;
        config.xss_protection_header_ = ruvia::xss_protection_header_policy::omit;
        config.content_security_policy_ = "default-src 'none'";
        const std::string custom_value(80, 'v');
        config.custom_headers_ = {{"X-Custom-Security", custom_value}};
        app.use<ruvia::security_headers_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'none'"));
    EXPECT_TRUE(!response.header("X-Frame-Options").has_value());
    EXPECT_TRUE(!response.header("X-XSS-Protection").has_value());
    const std::string expected_custom_value(80, 'v');
    EXPECT_EQ(response.header("X-Custom-Security"), std::string_view(expected_custom_value));
}

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, testing_facade_session_middleware_owns_configured_cookie_policy) {
    ruvia::test_app app;
    {
        ruvia::session_config config;
        const std::string redis_alias(80, 'r');
        const std::string key_prefix(80, 'k');
        config.redis_alias_ = redis_alias;
        config.cookie_name_ = "ruvia_session";
        config.key_prefix_ = key_prefix;
        config.ttl_ = std::chrono::minutes(15);
        app.use<ruvia::session_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/t/session-clear"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    const auto cookie = response.header("Set-Cookie");
    EXPECT_TRUE(cookie.has_value());
    EXPECT_TRUE(cookie->starts_with("ruvia_session=;"));
}

// Session Cookie 是显式请求级响应策略；外层替换或错误不会丢失已经持久化的 Cookie，
// 也不依赖 Cookie 在 next() 前还是后发布。
TEST(Ruvia, testing_facade_session_cookie_survives_response_replacement_after_next) {
    for (const bool error_after : {false, true}) {
        ruvia::test_app app;
        if (error_after) {
            app.use<testing_facade_deny_after>();
        } else {
            app.use<testing_facade_replace_after>();
        }
        {
            ruvia::session_config config;
            config.cookie_name_ = "ruvia_session";
            config.ttl_ = std::chrono::minutes(15);
            app.use<ruvia::session_middleware>(config);
        }

        const auto response = app.request(ruvia::test_request::get("/t/session-clear"));
        EXPECT_EQ(response.status(), error_after ? ruvia::http_status::forbidden : ruvia::http_status::ok);
        if (!error_after) {
            EXPECT_EQ(response.body(), std::string_view("denied"));
        }
        const auto cookie = response.header("Set-Cookie");
        EXPECT_TRUE(cookie.has_value());
        if (cookie.has_value()) {
            EXPECT_TRUE(cookie->starts_with("ruvia_session=;"));
        }
    }
}
#endif

TEST(Ruvia, testing_facade_routes_an_extension_method_by_its_exact_token) {
    ruvia::test_app app;

    const auto propfind = app.request(ruvia::test_request::method("PROPFIND", "/t/files"));
    EXPECT_EQ(propfind.status(), ruvia::http_status::ok);
    // The handler sees the exact wire token, not a classification.
    EXPECT_EQ(propfind.body(), std::string_view("PROPFIND"));

    const auto purge = app.request(ruvia::test_request::method("PURGE", "/t/files"));
    EXPECT_EQ(purge.status(), ruvia::http_status::ok);
    EXPECT_EQ(purge.body(), std::string_view("purged"));
}

TEST(Ruvia, testing_facade_extension_method_tokens_are_case_sensitive) {
    ruvia::test_app app;
    // RFC 9110 9.1: the method token is case-sensitive, so "propfind" is a
    // different method from "PROPFIND" and no route registered it. 405 is
    // reserved for a method the origin server knows (15.5.6), so an
    // unregistered token is 501 no matter what the target path supports.
    const auto response = app.request(ruvia::test_request::method("propfind", "/t/files"));
    EXPECT_EQ(response.status(), ruvia::http_status::not_implemented);
}

TEST(Ruvia, testing_facade_extension_method_splits_404_from_501) {
    ruvia::test_app app;

    // PROPFIND is registered somewhere, so the server knows it. A path with no
    // route at all is then an ordinary 404 -- the method is not the problem.
    const auto missing_path = app.request(ruvia::test_request::method("PROPFIND", "/t/nothing-here"));
    EXPECT_EQ(missing_path.status(), ruvia::http_status::not_found);

    // A token no route registered is 501 even on a path that exists.
    const auto unknown_method = app.request(ruvia::test_request::method("FROBNICATE", "/t/files"));
    EXPECT_EQ(unknown_method.status(), ruvia::http_status::not_implemented);
}

TEST(Ruvia, testing_facade_extension_method_on_a_known_path_is_405_not_501) {
    ruvia::test_app app;
    // The resource exists under GET, so the method is the problem, not the URI.
    const auto response = app.request(ruvia::test_request::method("PROPFIND", "/t/hello"));
    EXPECT_EQ(response.status(), ruvia::http_status::method_not_allowed);
    const auto allow = response.header("Allow");
    EXPECT_TRUE(allow.has_value());
    EXPECT_TRUE(allow.has_value() && allow->find("GET") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_allow_header_names_extension_methods) {
    ruvia::test_app app;

    // A known method the path does not support: Allow must still name the
    // extension methods it does, or it is not the full supported set.
    const auto known = app.request(ruvia::test_request::post("/t/files"));
    EXPECT_EQ(known.status(), ruvia::http_status::method_not_allowed);
    const auto known_allow = known.header("Allow");
    EXPECT_TRUE(known_allow.has_value());
    EXPECT_TRUE(known_allow.has_value() && known_allow->find("PROPFIND") != std::string_view::npos);
    EXPECT_TRUE(known_allow.has_value() && known_allow->find("PURGE") != std::string_view::npos);

    // A path whose ONLY methods are extension ones must still answer 405 with a
    // populated Allow rather than 404 or an empty header.
    const auto dav_only = app.request(ruvia::test_request::method("PURGE", "/t/dav-only"));
    EXPECT_EQ(dav_only.status(), ruvia::http_status::method_not_allowed);
    const auto dav_allow = dav_only.header("Allow");
    EXPECT_TRUE(dav_allow.has_value());
    EXPECT_TRUE(dav_allow.has_value() && dav_allow->find("OPTIONS") != std::string_view::npos);
    EXPECT_TRUE(dav_allow.has_value() && dav_allow->find("PROPFIND") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_options_reports_extension_methods_too) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::options("/t/files"));
    EXPECT_EQ(response.status(), ruvia::http_status::no_content);
    const auto allow = response.header("Allow");
    EXPECT_TRUE(allow.has_value());
    EXPECT_TRUE(allow.has_value() && allow->find("PROPFIND") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_options_reports_extension_only_resource_methods_too) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::options("/t/dav-only"));
    EXPECT_EQ(response.status(), ruvia::http_status::no_content);
    const auto allow = response.header("Allow");
    EXPECT_TRUE(allow.has_value());
    EXPECT_TRUE(allow.has_value() && allow->find("OPTIONS") != std::string_view::npos);
    EXPECT_TRUE(allow.has_value() && allow->find("PROPFIND") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_router_slash) {
    ruvia::test_app app;
    auto res1 = app.request(ruvia::test_request::get("/t/a/123/"));
    EXPECT_EQ(res1.status(), ruvia::http_status::ok);

    // "/a/:id/" names paths ending in '/'; the bare spelling is another path.
    auto res2 = app.request(ruvia::test_request::get("/t/a/123"));
    EXPECT_EQ(res2.status(), ruvia::http_status::not_found);
}

TEST(Ruvia, testing_facade_dynamic_paths_preserve_empty_segments) {
    ruvia::test_app app;

    const auto repeated = app.request(ruvia::test_request::get("/t/empty//42"));
    EXPECT_EQ(repeated.status(), ruvia::http_status::ok);
    EXPECT_EQ(repeated.body(), "42");

    const auto missing_empty = app.request(ruvia::test_request::get("/t/empty/42"));
    EXPECT_EQ(missing_empty.status(), ruvia::http_status::not_found);

    const auto extra_empty = app.request(ruvia::test_request::get("/t/empty///42"));
    EXPECT_EQ(extra_empty.status(), ruvia::http_status::not_found);

    const auto empty_param = app.request(ruvia::test_request::get("/t/users//"));
    EXPECT_EQ(empty_param.status(), ruvia::http_status::not_found);
}

TEST(Ruvia, testing_facade_reverse_routes_preserve_empty_segments) {
    ruvia::test_app app;

    const auto link = app.request(ruvia::test_request::get("/t/slash-link"));
    EXPECT_EQ(link.status(), ruvia::http_status::ok);
    EXPECT_EQ(link.body(), "/t/empty//456");

    const auto response = app.request(ruvia::test_request::get(link.body()));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.body(), "456");
}

TEST(Ruvia, testing_facade_reverse_routes_preserve_trailing_segments) {
    ruvia::test_app app;

    const auto links = app.request(ruvia::test_request::get("/t/trailing-links"));
    EXPECT_EQ(links.status(), ruvia::http_status::ok);
    EXPECT_EQ(links.body(), "/t/literal/ /t/tail/789//");

    const auto separator = links.body().find(' ');
    const auto literal = app.request(ruvia::test_request::get(links.body().substr(0, separator)));
    EXPECT_EQ(literal.status(), ruvia::http_status::ok);
    EXPECT_EQ(literal.body(), "hello");

    const auto tail = app.request(ruvia::test_request::get(links.body().substr(separator + 1)));
    EXPECT_EQ(tail.status(), ruvia::http_status::ok);
    EXPECT_EQ(tail.body(), "789");
}

TEST(Ruvia, testing_facade_reverse_routes_reject_dot_segments) {
    // RFC 3986 5.2.4 resolvers rewrite "." and ".." segments to a different
    // resource, so url_for refuses to emit them; dots inside a segment stay.
    ruvia::test_app app;

    const auto links = app.request(ruvia::test_request::get("/t/dot-links"));
    EXPECT_EQ(links.status(), ruvia::http_status::ok);
    EXPECT_EQ(links.body(), "xxxxxxxxx /t/tree/a.b/..c/.d /t/tree//a");

    const auto tree = app.request(ruvia::test_request::get("/t/tree/a.b/..c/.d"));
    EXPECT_EQ(tree.status(), ruvia::http_status::ok);
    EXPECT_EQ(tree.body(), "a.b/..c/.d");

    const auto leading = app.request(ruvia::test_request::get("/t/tree//a"));
    EXPECT_EQ(leading.status(), ruvia::http_status::ok);
    EXPECT_EQ(leading.body(), "/a");
}

TEST(Ruvia, testing_facade_wildcard_capture_rejects_encoded_separators) {
    // Routing and path-scoped middleware split the wildcard tail on raw '/'
    // only, so a decoded "%2F" or "%5C" would forge a boundary they never saw.
    ruvia::test_app app;

    const auto spaced = app.request(ruvia::test_request::get("/t/tree/a%20b/c"));
    EXPECT_EQ(spaced.status(), ruvia::http_status::ok);
    EXPECT_EQ(spaced.body(), "a b/c");

    for (const auto* target : {"/t/tree/secret%2Fkey.txt", "/t/tree/x%2F..%2Fsecret", "/t/tree/a%5Cb"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(target)).status(), ruvia::http_status::bad_request);
    }

    // Decoded exactly once: a doubly encoded "%252E%252E" stays the literal
    // name "%2E%2E" that static_file looks up as is, never a ".." segment.
    const auto doubled = app.request(ruvia::test_request::get("/t/tree/public/%252E%252E/private"));
    EXPECT_EQ(doubled.status(), ruvia::http_status::ok);
    EXPECT_EQ(doubled.body(), "public/%2E%2E/private");

    // A single named segment has no boundary to forge and still decodes.
    const auto named = app.request(ruvia::test_request::get("/t/users/a%2Fb"));
    EXPECT_EQ(named.status(), ruvia::http_status::ok);
    EXPECT_EQ(named.body(), "a/b");
}

TEST(Ruvia, testing_facade_subrequest_rejects_non_buffered_routes_protocol_fields_and_enforces_406) {
    const ruvia::test::scoped_temp_directory directory("ruvia_testing_facade_subrequest_file");
    const auto& dir = directory.path();
    testing_facade_file_path() = dir / "sample.bin";
    {
        std::ofstream out(testing_facade_file_path(), std::ios::binary | std::ios::trunc);
        out << "0123456789abcdef";
    }
    ruvia::test_app app;
    const auto res = app.request(ruvia::test_request::get("/t/sub-checks"));
    EXPECT_EQ(res.status(), ruvia::http_status::ok);
    EXPECT_EQ(res.body(), "1111");
}

TEST(Ruvia, testing_facade_subrequest_rejects_a_file_response_recovered_from_406) {
    const ruvia::test::scoped_temp_directory directory("ruvia_testing_facade_subrequest_406_file");
    const auto& dir = directory.path();
    testing_facade_file_path() = dir / "large.bin";
    // Larger than the inline-read limit, so the error handler's answer stays
    // a file body instead of being read into the response.
    const std::string large_payload(20000, 'L');
    {
        std::ofstream out(testing_facade_file_path(), std::ios::binary | std::ios::trunc);
        out.write(large_payload.data(), static_cast<std::streamsize>(large_payload.size()));
    }
    ruvia::test_app app;
    app.on_error([](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        if (error.status() == ruvia::http_status::not_acceptable) {
            co_return co_await c.file({.path_ = testing_facade_file_path()});
        }
        c.draft().status(error.status());
        co_return c.text("err");
    });
    const auto res = app.request(ruvia::test_request::get("/t/sub-refused-file"));
    EXPECT_EQ(res.status(), ruvia::http_status::ok);
    EXPECT_EQ(res.body(), "rejected");
}

TEST(Ruvia, testing_facade_reads_file_bodies_and_negotiates_precompressed_static_files) {
    const ruvia::test::scoped_temp_directory directory("ruvia_testing_facade_files");
    const auto& dir = directory.path();
    testing_facade_file_path() = dir / "large.bin";
    const std::string large_payload(20000, 'L');
    {
        std::ofstream out(testing_facade_file_path(), std::ios::binary | std::ios::trunc);
        out.write(large_payload.data(), static_cast<std::streamsize>(large_payload.size()));
    }
    {
        std::ofstream out(dir / "asset.txt", std::ios::binary | std::ios::trunc);
        out << "plain-asset-body";
    }
    {
        std::ofstream out(dir / "asset.txt.gz", std::ios::binary | std::ios::trunc);
        out << "gz-sidecar-bytes";
    }
    ruvia::static_root_options static_opts;
    static_opts.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    testing_facade_static_root().emplace(dir, std::move(static_opts));

    ruvia::test_app app;
    const auto full_file = app.request(ruvia::test_request::get("/t/sub-file"));
    EXPECT_EQ(full_file.status(), ruvia::http_status::ok);
    EXPECT_EQ(full_file.body(), large_payload);

    const auto multi_range =
        app.request(ruvia::test_request::get("/t/sub-file").header("Range", "bytes=0-1, 4-5"));
    EXPECT_EQ(multi_range.status(), ruvia::http_status::partial_content);
    EXPECT_TRUE(multi_range.body().find("LL") != std::string_view::npos);

    const auto direct_static = app.request(ruvia::test_request::get("/t/sub-static"));
    EXPECT_EQ(direct_static.status(), ruvia::http_status::ok);
    EXPECT_EQ(direct_static.body(), "plain-asset-body");

    // A named parameter decodes "%2F" and "%5C" inside its one segment, so it
    // can carry a ".." no route or path scope saw; static_file rejects it
    // instead of resolving it back inside the root.
    const auto named_static = app.request(ruvia::test_request::get("/t/sub-static/asset.txt"));
    EXPECT_EQ(named_static.status(), ruvia::http_status::ok);
    EXPECT_EQ(named_static.body(), "plain-asset-body");
    for (const auto* target : {"/t/sub-static/sub%2F..%2Fasset.txt", "/t/sub-static/sub%5C..%5Casset.txt",
             "/t/sub-static/.%2Fasset.txt"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(target)).status(), ruvia::http_status::forbidden);
    }
    testing_facade_static_root().reset();
}

TEST(Ruvia, testing_facade_supports_typed_params_queries_const_handlers_json_arrays_and_remote_info) {
    ruvia::test_app app;
    app.trusted_proxies({.cidrs_ = {"10.0.0.0/8"}, .trust_x_forwarded_proto_ = true});

    const auto typed =
        app.request(ruvia::test_request::get("/t/typed/42/true?page=-5&verbose=1"));
    EXPECT_EQ(typed.status(), ruvia::http_status::ok);
    EXPECT_EQ(typed.body(), "42:1:-5:1:none:/t/typed/12/true");

    const auto bad_param = app.request(ruvia::test_request::get("/t/typed/not-an-int/true"));
    EXPECT_EQ(bad_param.status(), ruvia::http_status::bad_request);

    const auto bad_query = app.request(ruvia::test_request::get("/t/typed/42/true?page=abc"));
    EXPECT_EQ(bad_query.status(), ruvia::http_status::bad_request);

    const auto const_res = app.request(ruvia::test_request::get("/t/const-handler"));
    EXPECT_EQ(const_res.status(), ruvia::http_status::ok);
    EXPECT_EQ(const_res.body(), "const-ok");
    EXPECT_EQ(const_res.header("X-Const-Middleware").value_or(""), "yes");

    ruvia::array<ruvia::string> input_items;
    input_items.emplace_back("first");
    const auto array_res =
        app.request(ruvia::test_request::post("/t/json-array").json(input_items));
    EXPECT_EQ(array_res.status(), ruvia::http_status::ok);
    EXPECT_EQ(array_res.body(), R"(["first","tail"])");

    const auto direct_conn = app.request(ruvia::test_request::get("/t/conn-info"));
    EXPECT_EQ(direct_conn.body(), "127.0.0.1:0/127.0.0.1/direct/plain");

    const auto proxied_conn = app.request(ruvia::test_request::get("/t/conn-info")
            .remote("10.1.2.3", 12345)
            .header("X-Forwarded-For", "203.0.113.9"));
    EXPECT_EQ(proxied_conn.body(), "10.1.2.3:12345/203.0.113.9/proxy/plain");
}

namespace {

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

// In-memory PEM serves HTTPS listeners only: every client reads identity
// files, and is never handed PEM text it would silently ignore.
TEST(Ruvia, client_tls_rejects_in_memory_pem_identity) {
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().client({.alias_ = "tls-pem-identity",
            .base_url_ = "https://example.com",
            .tls_ = {.identity_ = ruvia::tls_identity_config{.certificate_chain_file_ = "client.pem",
                         .private_key_file_ = "client.key",
                         .certificate_chain_pem_ = "pem",
                         .private_key_pem_ = "pem"}}});
    }));
}

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, client_tls_rejects_unsafe_outgoing_record_limits) {
    const auto rejects = [](std::uint64_t limit) {
        ruvia::redis_config config;
        config.host_ = "example.com";
        config.tls_.max_outgoing_records_ = limit;
        return throws_invalid([&config] {
            ruvia::app().redis({.alias_ = "tls-record-limit", .config_ = std::move(config)});
        });
    };
    EXPECT_TRUE(rejects(0));
    EXPECT_TRUE(rejects((std::uint64_t{1} << 23) + 1));
}

// An engaged CA or identity path must name a file: an empty one is a mistake,
// never a fallback to the system trust store or to no client certificate.
TEST(Ruvia, client_tls_rejects_empty_file_paths) {
    const auto rejects = [](ruvia::client_tls_config tls) {
        ruvia::redis_config config;
        config.host_ = "example.com";
        config.tls_ = std::move(tls);
        return throws_invalid([&config] {
            ruvia::app().redis({.alias_ = "tls-empty-path", .config_ = std::move(config)});
        });
    };
    EXPECT_TRUE(rejects({.ca_file_ = std::filesystem::path{}}));
    EXPECT_TRUE(rejects({.identity_ = ruvia::tls_identity_config{.certificate_chain_file_ = "client.pem"}}));
    EXPECT_TRUE(rejects({.identity_ = ruvia::tls_identity_config{.private_key_file_ = "client.key"}}));
}
#endif

TEST(Ruvia, app_document_root_rejects_invalid_static_options_at_configuration) {
    ruvia::document_root_config config;
    config.root_ = "public";
    config.static_options_.cache_control_ = " private";

    EXPECT_TRUE(throws_invalid([&config] { ruvia::app().document_root(std::move(config)); }));
}

TEST(Ruvia, app_document_root_rejects_disabled_refresh) {
    ruvia::document_root_config disabled_refresh;
    disabled_refresh.root_ = "public";
    disabled_refresh.refresh_interval_ = std::chrono::milliseconds::zero();
    EXPECT_TRUE(throws_invalid(
        [&disabled_refresh] { ruvia::app().document_root(std::move(disabled_refresh)); }));
}

TEST(Ruvia, app_compression_rejects_invalid_thresholds_at_configuration) {
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().server({.compression_ = ruvia::compression_config{.min_bytes_ = 1024, .sync_bytes_ = 512}});
    }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().server(
            {.compression_ = ruvia::compression_config{.min_bytes_ = 1024, .sync_bytes_ = 2048, .max_bytes_ = 1024}});
    }));
}

namespace {

class testing_memory_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/testing-memory")
    RUVIA_ROUTES_BEGIN(testing_memory_controller)
    RUVIA_GET("/owned", owned);
    RUVIA_GET("/throw", throwing);
    RUVIA_GET("/cancel", cancelled);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> owned(ruvia::context&) {
        ruvia::http_response response;
        std::string body(256 * 1024, 'm');
        response.owned_body(std::move(body));
        co_return response;
    }

    ruvia::task<ruvia::http_response> throwing(ruvia::context&) {
        ruvia::http_response response;
        std::string body(256 * 1024, 'e');
        response.owned_body(std::move(body));
        throw std::runtime_error("testing memory failure");
        co_return response;
    }

    ruvia::task<ruvia::http_response> cancelled(ruvia::context& c) {
        ruvia::http_response response;
        std::string body(256 * 1024, 'c');
        response.owned_body(std::move(body));
        // Exercise explicit downstream cancellation without a request-level timer.
        ruvia::stop_source stop;
        stop.request_stop();
        const auto result_value = co_await ruvia::sleep_for(
            c.worker(), std::chrono::hours(1), stop.token());
        if (result_value != ruvia::timer_sleep_result::stop_requested) {
            throw std::logic_error("explicit cancellation did not stop the operation");
        }
        co_return response;
    }
};

}  // namespace

TEST(Ruvia, testing_facade_retains_owned_response_memory_across_app_lifetime) {
    std::optional<ruvia::dispatch_response> retained;
    {
        ruvia::test_app app;

        retained.emplace(app.request(ruvia::test_request::get("/testing-memory/owned")));
        EXPECT_EQ(retained->body().size(), std::size_t(256 * 1024));
        EXPECT_TRUE(retained->body().front() == 'm');

        const auto cancelled = app.request(ruvia::test_request::get("/testing-memory/cancel"));
        EXPECT_EQ(cancelled.body(), std::string(256 * 1024, 'c'));

        const auto repeated = app.request(ruvia::test_request::get("/testing-memory/owned"));
        EXPECT_EQ(repeated.body().size(), std::size_t(256 * 1024));
        EXPECT_TRUE(retained->body().front() == 'm');

        const auto failed = app.request(ruvia::test_request::get("/testing-memory/throw"));
        EXPECT_TRUE(failed.status() == ruvia::http_status::internal_server_error);
    }
    EXPECT_EQ(retained->body().size(), std::size_t(256 * 1024));
    EXPECT_TRUE(retained->body().front() == 'm');
}

namespace {

std::filesystem::path write_temp_env(const std::filesystem::path& directory, std::string_view name, std::string_view contents) {
    const auto path = directory / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}

}  // namespace

TEST(Ruvia, dotenv_typed_lookup_does_not_hide_invalid_values) {
    const ruvia::test::scoped_temp_directory directory("ruvia_dotenv_typed");
    const auto path = write_temp_env(directory.path(), "ruvia_dotenv_typed.env",
        "PORT=8080\nBAD_PORT=not-a-port\nENABLED=maybe\nBAD_DOUBLE=nan\nBAD_INF=inf\n");
    ruvia::env env;
    env.load_dotenv(path);

    EXPECT_TRUE(!env.get<std::uint16_t>("MISSING").has_value());
    EXPECT_EQ(env.get<std::uint16_t>("PORT").value_or(0), std::uint16_t{8080});
    EXPECT_TRUE(!env.empty());

    bool bad_port_threw = false;
    try {
        (void)env.get<std::uint16_t>("BAD_PORT");
    } catch (const std::invalid_argument& error) {
        bad_port_threw = (std::string_view(error.what()).find("BAD_PORT") != std::string_view::npos);
    }
    EXPECT_TRUE(bad_port_threw);

    bool bad_bool_threw = false;
    try {
        (void)env.get<bool>("ENABLED");
    } catch (const std::invalid_argument& error) {
        bad_bool_threw = (std::string_view(error.what()).find("ENABLED") != std::string_view::npos);
    }
    EXPECT_TRUE(bad_bool_threw);

    bool bad_double_threw = false;
    try {
        (void)env.get<double>("BAD_DOUBLE");
    } catch (const std::invalid_argument& error) {
        bad_double_threw =
            (std::string_view(error.what()).find("BAD_DOUBLE") != std::string_view::npos);
    }
    EXPECT_TRUE(bad_double_threw);

    bool bad_inf_threw = false;
    try {
        (void)env.get<double>("BAD_INF");
    } catch (const std::invalid_argument& error) {
        bad_inf_threw = (std::string_view(error.what()).find("BAD_INF") != std::string_view::npos);
    }
    EXPECT_TRUE(bad_inf_threw);
}

TEST(Ruvia, dotenv_options_use_explicit_policies) {
    const ruvia::test::scoped_temp_directory directory("ruvia_dotenv_policy");
    const auto first = write_temp_env(directory.path(), "ruvia_dotenv_policy_first.env", "KEY=first\n");
    const auto second = write_temp_env(directory.path(), "ruvia_dotenv_policy_second.env", "KEY=second\nNEW=value\n");
    const auto missing = directory.path() / "ruvia_dotenv_policy_missing.env";

    ruvia::env env;
    env.load_dotenv(first);
    EXPECT_EQ(env.get("KEY").value_or(""), std::string_view("first"));

    env.load_dotenv(second);
    EXPECT_EQ(env.get("KEY").value_or(""), std::string_view("first"));
    EXPECT_EQ(env.get("NEW").value_or(""), std::string_view("value"));

    env.load_dotenv(
        second, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override});
    EXPECT_EQ(env.get("KEY").value_or(""), std::string_view("second"));

    env.load_dotenv(missing);

    bool required_missing_threw = false;
    try {
        env.load_dotenv(
            missing, {.missing_file_ = ruvia::dotenv_missing_file_policy::require});
    } catch (const std::runtime_error&) {
        required_missing_threw = true;
    }
    EXPECT_TRUE(required_missing_threw);

    bool invalid_existing_policy_threw = false;
    try {
        env.load_dotenv(first,
            {.existing_variables_ = static_cast<ruvia::dotenv_existing_variable_policy>(0xFF)});
    } catch (const std::invalid_argument&) {
        invalid_existing_policy_threw = true;
    }
    EXPECT_TRUE(invalid_existing_policy_threw);

    bool invalid_missing_policy_threw = false;
    try {
        env.load_dotenv(
            first, {.missing_file_ = static_cast<ruvia::dotenv_missing_file_policy>(0xFF)});
    } catch (const std::invalid_argument&) {
        invalid_missing_policy_threw = true;
    }
    EXPECT_TRUE(invalid_missing_policy_threw);
}

TEST(Ruvia, dotenv_double_quoted_undefined_escapes_keep_backslash) {
    const ruvia::test::scoped_temp_directory directory("ruvia_dotenv_escapes");
    const auto path = write_temp_env(directory.path(), "ruvia_dotenv_escapes.env",
        R"(DOTENV_ESCAPE_DEFINED="a\nb\rc\td\"e\\f"
DOTENV_ESCAPE_UNDEFINED="\q\$x\'y\0 C:\Users"
DOTENV_ESCAPE_TRAILING="end\\"
DOTENV_ESCAPE_SINGLE='\n\q'
)");
    ruvia::env env;
    env.load_dotenv(path);
    EXPECT_EQ(env.get("DOTENV_ESCAPE_DEFINED").value_or(""), std::string_view("a\nb\rc\td\"e\\f"));
    EXPECT_EQ(
        env.get("DOTENV_ESCAPE_UNDEFINED").value_or(""), std::string_view(R"(\q\$x\'y\0 C:\Users)"));
    EXPECT_EQ(env.get("DOTENV_ESCAPE_TRAILING").value_or(""), std::string_view("end\\"));
    EXPECT_EQ(env.get("DOTENV_ESCAPE_SINGLE").value_or(""), std::string_view(R"(\n\q)"));
}

TEST(Ruvia, dotenv_duplicate_name_in_one_file_is_rejected) {
    const ruvia::test::scoped_temp_directory directory("ruvia_dotenv_duplicate");
    const auto earlier = write_temp_env(directory.path(), "ruvia_dotenv_duplicate_earlier.env", "DOTENV_DUP=earlier\n");
    const auto duplicate = write_temp_env(directory.path(), "ruvia_dotenv_duplicate.env",
        "DOTENV_DUP_OTHER=kept\nDOTENV_DUP=first\n\n# comment\nexport DOTENV_DUP = second\n");
    const auto utf8_path = [](const std::filesystem::path& path) {
        const auto text = path.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    };
    ruvia::env env;
    const auto failure = [&env, &duplicate](ruvia::dotenv_options options) {
        try {
            env.load_dotenv(duplicate, options);
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string{};
    };
    const auto expected = "invalid dotenv entry in " + utf8_path(duplicate) + ":5: duplicate variable name";

    // The whole file is rejected before any entry is applied, whichever policy
    // governs names that come from other sources.
    EXPECT_EQ(failure({}), expected);
    EXPECT_TRUE(!env.get("DOTENV_DUP").has_value());
    EXPECT_TRUE(!env.get("DOTENV_DUP_OTHER").has_value());

    env.load_dotenv(earlier);
    EXPECT_EQ(
        failure({.existing_variables_ = ruvia::dotenv_existing_variable_policy::override}), expected);
    EXPECT_EQ(env.get("DOTENV_DUP").value_or(""), std::string_view("earlier"));
    EXPECT_TRUE(!env.get("DOTENV_DUP_OTHER").has_value());
}

TEST(Ruvia, dotenv_errors_name_non_ascii_paths_in_utf8) {
    const ruvia::test::scoped_temp_directory temporary("ruvia-dotenv");
    const auto directory = temporary.path() /
                           std::filesystem::path(u8"\u914d\u7f6e-\u0442\u0435\u0441\u0442-\U0001F4C1");
    std::filesystem::create_directory(directory);
    const auto missing = directory / "missing.env";
    const auto invalid = directory / "invalid.env";
    {
        std::ofstream out(invalid, std::ios::binary | std::ios::trunc);
        out << "KEY=\"unterminated\n";
    }
    const auto utf8_path = [](const std::filesystem::path& path) {
        const auto text = path.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    };
    ruvia::env env;
    const auto failure = [&env](const std::filesystem::path& path, ruvia::dotenv_options options) {
        try {
            env.load_dotenv(path, options);
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
        return std::string{};
    };

    EXPECT_EQ(failure(missing, {.missing_file_ = ruvia::dotenv_missing_file_policy::require}),
        "dotenv file not found: " + utf8_path(missing));
    EXPECT_TRUE(failure(invalid, {}).starts_with("invalid dotenv entry in " + utf8_path(invalid) + ":1: "));
}

TEST(Ruvia, dotenv_preserves_utf8_values_and_escaped_characters) {
    const ruvia::test::scoped_temp_directory directory("ruvia_dotenv_utf8");
    const auto path = write_temp_env(directory.path(), "ruvia_dotenv_utf8_values.env",
        "DOTENV_UTF8_UNQUOTED=\xE4\xB8\xAD\xE6\x96\x87 # comment\n"
        "DOTENV_UTF8_SINGLE='caf\xC3\xA9'\n"
        "DOTENV_UTF8_DOUBLE=\"\xE4\xB8\xAD\\n\xE6\x96\x87\"\n"
        "DOTENV_UTF8_LITERAL_DOUBLE=\"caf\xC3\xA9\"\n"
        "DOTENV_UTF8_UNKNOWN_ESCAPE=\"\\\xC3\xA9\"\n");
    ruvia::env env;
    env.load_dotenv(path);

    EXPECT_EQ(env.get("DOTENV_UTF8_UNQUOTED").value_or(""), "\xE4\xB8\xAD\xE6\x96\x87");
    EXPECT_EQ(env.get("DOTENV_UTF8_SINGLE").value_or(""), "caf\xC3\xA9");
    EXPECT_EQ(env.get("DOTENV_UTF8_DOUBLE").value_or(""), "\xE4\xB8\xAD\n\xE6\x96\x87");
    EXPECT_EQ(env.get("DOTENV_UTF8_LITERAL_DOUBLE").value_or(""), "caf\xC3\xA9");
    EXPECT_EQ(env.get("DOTENV_UTF8_UNKNOWN_ESCAPE").value_or(""), "\\\xC3\xA9");
}

TEST(Ruvia, dotenv_invalid_utf8_rejects_all_entries_without_changing_prior_state) {
    const ruvia::test::scoped_temp_directory directory("ruvia_dotenv_utf8_invalid");
    const auto earlier = write_temp_env(directory.path(), "ruvia_dotenv_utf8_earlier.env", "DOTENV_UTF8_ROLLBACK=original\n");
    ruvia::env env;
    env.load_dotenv(earlier);
    const auto size = env.size();
    const auto loaded = env.loaded();
    constexpr std::string_view invalid_values[] = {
        "caf\xE9",
        "'caf\xE9'",
        "\"caf\xE9\"",
        "\"\\\xE9\"",
    };
    constexpr ruvia::dotenv_existing_variable_policy policies[] = {
        ruvia::dotenv_existing_variable_policy::preserve,
        ruvia::dotenv_existing_variable_policy::override,
    };
    for (const auto value : invalid_values) {
        const auto invalid = write_temp_env(directory.path(), "ruvia_dotenv_utf8_invalid.env",
            std::string("DOTENV_UTF8_ROLLBACK=changed\nDOTENV_UTF8_PARTIAL=not_committed\nDOTENV_UTF8_INVALID=") +
                std::string(value) + '\n');
        for (const auto policy : policies) {
            EXPECT_THROW(env.load_dotenv(invalid, {.existing_variables_ = policy}), std::invalid_argument);
            EXPECT_EQ(env.get("DOTENV_UTF8_ROLLBACK").value_or(""), "original");
            EXPECT_FALSE(env.get("DOTENV_UTF8_PARTIAL").has_value());
            EXPECT_FALSE(env.get("DOTENV_UTF8_INVALID").has_value());
            EXPECT_EQ(env.size(), size);
            EXPECT_EQ(env.loaded(), loaded);
        }
    }
}

TEST(Ruvia, independent_dotenv_instances_keep_values_and_policies_isolated) {
    const ruvia::test::scoped_temp_directory directory("ruvia_dotenv_isolation");
    const auto first = write_temp_env(directory.path(), "first.env", "PORT=8080\nMODE=first\n");
    const auto second = write_temp_env(directory.path(), "second.env", "PORT=9090\nENABLED=true\n");
    ruvia::env left;
    ruvia::env right;
    left.load_dotenv(first);
    right.load_dotenv(second);
    EXPECT_EQ(left.get<unsigned>("PORT"), 8080u);
    EXPECT_EQ(right.get<unsigned>("PORT"), 9090u);
    EXPECT_EQ(right.get<bool>("ENABLED"), true);
    EXPECT_FALSE(left.get("ENABLED").has_value());
    EXPECT_FALSE(right.get("MODE").has_value());
    left.load_dotenv(second);
    EXPECT_EQ(left.get<unsigned>("PORT"), 8080u);
    EXPECT_EQ(right.get<unsigned>("PORT"), 9090u);
    EXPECT_EQ(left.get<bool>("ENABLED"), true);
    left.load_dotenv(second, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override});
    EXPECT_EQ(left.get<unsigned>("PORT"), 9090u);
    EXPECT_EQ(right.get<unsigned>("PORT"), 9090u);
    EXPECT_EQ(left.get("MODE"), "first");
}

// Overlapping dynamic route shapes register and dispatch by the matcher's
// precedence (static before parameter before wildcard, with backtracking), and
// path-scoped middleware follows the request path the router matched rather
// than the route pattern. Public API only.

namespace {

// Stamps a header so a response shows whether this middleware ran at all.
class route_shapes_scope final {
public:
    explicit route_shapes_scope(std::string_view tag) noexcept
        : tag_(tag) {}

    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Route-Scope", tag_);
    }

private:
    std::string_view tag_;
};

class route_shapes_trace final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Route-Trace", "on");
    }
};

// Violates the single-shot next() contract, so a response shows whether a
// conditionally scoped frame really ran under the same chain rules.
class route_shapes_double_next final {
public:
    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
        co_await next_value();
    }
};

// Wraps unmatched requests but neither responds nor awaits next(): a chain
// that never finalizes the request.
class route_shapes_unfinished final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context&, ruvia::next&) {
        co_return;
    }
};

class route_shapes_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_shapes_controller)
    RUVIA_GET("/shapes/a/*", a_wildcard);
    RUVIA_GET("/shapes/:x/:y", pair);
    RUVIA_GET("/shapes/b/:z", b_param);
    RUVIA_GET("/shapes/w/:id", w_param);
    RUVIA_GET("/shapes/w/*", w_wildcard);
    RUVIA_GET("/:section/panel", panel);
    RUVIA_GET("/:section/panel/", panel_slash);
    RUVIA_GET("/admin/settings", settings);
    RUVIA_GET("/assets/*", asset);
    RUVIA_GET("/slash/static", static_bare);
    RUVIA_GET("/slash/static/", static_slash);
    RUVIA_GET("/slash/:id", dynamic_bare);
    RUVIA_GET("/slash/:id/", dynamic_slash);
    RUVIA_GET("/only/:id", only_dynamic);
    RUVIA_GET("/only-static", only_static);
    RUVIA_GET("/slash-links", slash_links);
    RUVIA_ROUTES_END

private:
    static ruvia::task<ruvia::http_response> labelled(
        ruvia::context& c, std::string_view label, std::string_view first, std::string_view second = {}) {
        std::string body;
        body.append(label);
        body.push_back(':');
        body.append(first);
        if (!second.empty()) {
            body.push_back('/');
            body.append(second);
        }
        co_return c.body(std::move(body));
    }

    ruvia::task<ruvia::http_response> a_wildcard(ruvia::context& c) {
        return labelled(c, "a-wild", c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> pair(ruvia::context& c) {
        return labelled(c, "pair", c.req().param("x").value_or("?"), c.req().param("y").value_or("?"));
    }

    ruvia::task<ruvia::http_response> b_param(ruvia::context& c) {
        return labelled(c, "b-param", c.req().param("z").value_or("?"));
    }

    ruvia::task<ruvia::http_response> w_param(ruvia::context& c) {
        return labelled(c, "w-param", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> w_wildcard(ruvia::context& c) {
        return labelled(c, "w-wild", c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> panel(ruvia::context& c) {
        return labelled(c, "panel", c.req().param("section").value_or("?"));
    }

    ruvia::task<ruvia::http_response> panel_slash(ruvia::context& c) {
        return labelled(c, "panel-slash", c.req().param("section").value_or("?"));
    }

    ruvia::task<ruvia::http_response> settings(ruvia::context& c) {
        return labelled(c, "settings", "admin");
    }

    ruvia::task<ruvia::http_response> asset(ruvia::context& c) {
        return labelled(c, "asset", c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> static_bare(ruvia::context& c) {
        return labelled(c, "static", "bare");
    }

    ruvia::task<ruvia::http_response> static_slash(ruvia::context& c) {
        return labelled(c, "static", "slash");
    }

    ruvia::task<ruvia::http_response> dynamic_bare(ruvia::context& c) {
        return labelled(c, "dyn", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> dynamic_slash(ruvia::context& c) {
        return labelled(c, "dyn-slash", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> only_dynamic(ruvia::context& c) {
        return labelled(c, "only", c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> only_static(ruvia::context& c) {
        return labelled(c, "only", "static");
    }

    ruvia::task<ruvia::http_response> slash_links(ruvia::context& c) {
        std::string body;
        body.append(c.url_for("/slash/static/"));
        body.push_back(' ');
        body.append(c.url_for("/slash/:id/", {"7"}));
        body.push_back(' ');
        body.append(c.url_for("/slash/:id", {"7"}));
        co_return c.body(std::move(body));
    }
};

// Refuses every request it guards, so a 403 shows the guarded route matched.
class route_shapes_deny final {
public:
    ruvia::task<void> invoke(ruvia::context&, ruvia::next&) {
        throw ruvia::http_error({.status_ = ruvia::http_status::forbidden});
        co_return;
    }
};

// Literal routes guarded by route-level middleware beside parameter routes
// that would capture an encoded spelling of the same path, plus resources
// whose Allow sets the router must report completely.
class route_shapes_equivalence_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_shapes_equivalence_controller)
    RUVIA_GET("/equiv/users/admin", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/users/:id", user);
    RUVIA_GET("/equiv/users/admin/:tab", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/users/:id/:tab", user);
    RUVIA_GET("/equiv/files/caf%C3%A9", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/files/:name", file);
    RUVIA_GET("/equiv/stream", stream);
    RUVIA_GET("/equiv/colon/%3Aid", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/colon/:id", user);
    RUVIA_GET("/equiv/star/%2A", guarded, route_shapes_deny);
    RUVIA_GET("/equiv/star/*", user);
    RUVIA_METHOD("EXT-A", "/equiv/many", user);
    RUVIA_METHOD("EXT-B", "/equiv/many", user);
    RUVIA_METHOD("EXT-C", "/equiv/many", user);
    RUVIA_METHOD("EXT-D", "/equiv/many", user);
    RUVIA_METHOD("EXT-E", "/equiv/many", user);
    RUVIA_METHOD("EXT-F", "/equiv/many", user);
    RUVIA_METHOD("EXT-G", "/equiv/many", user);
    RUVIA_METHOD("EXT-H", "/equiv/many", user);
    RUVIA_METHOD("EXT-I", "/equiv/many", user);
    RUVIA_METHOD("EXT-J", "/equiv/m%61ny", user);
    RUVIA_METHOD("Z-INDEX", "/indexed/z", guarded);
    RUVIA_METHOD("A-INDEX-TAIL", "/indexed/m", file);
    RUVIA_METHOD("A-INDEX", "/indexed/a%2Fb", user);
    RUVIA_METHOD("Z-INDEX", "/indexed/a/b", file);
    RUVIA_METHOD("A-INDEX", "/indexed/%61", guarded);
    RUVIA_METHOD("Z-INDEX", "/indexed/a%2fb", guarded);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> guarded(ruvia::context& c) {
        co_return c.body("guarded");
    }

    ruvia::task<ruvia::http_response> user(ruvia::context& c) {
        std::string body;
        body.append("user:");
        body.append(c.req().param("id").value_or("-"));
        co_return c.body(std::move(body));
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& c) {
        std::string body;
        body.append("file:");
        body.append(c.req().param("name").value_or("-"));
        co_return c.body(std::move(body));
    }

    ruvia::task<void> stream(ruvia::context& c) {
        if (co_await c.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write("chunk");
    }
};

[[nodiscard]] std::multiset<std::string_view> allow_methods(std::string_view value) {
    std::multiset<std::string_view> methods{};
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto token = value.substr(0, comma);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        methods.insert(token);
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return methods;
}

// A non-200 answer reads as its own value so a mismatch is visible in the check.
[[nodiscard]] std::string body_of(ruvia::test_app& app, std::string_view path) {
    const auto response = app.request(ruvia::test_request::get(path));
    if (response.status() != ruvia::http_status::ok) {
        return std::string("unexpected status");
    }
    return std::string(response.body());
}

[[nodiscard]] std::string scope_of(ruvia::test_app& app, std::string_view path) {
    const auto response = app.request(ruvia::test_request::get(path));
    if (response.status() != ruvia::http_status::ok) {
        return std::string("unexpected status");
    }
    const auto scope = response.header("X-Route-Scope");
    return scope.has_value() ? std::string(*scope) : std::string("none");
}

ruvia::task<ruvia::http_response> route_shapes_api_miss(ruvia::context& c) {
    c.draft().status(ruvia::http_status::not_found);
    co_return c.body("api-miss");
}

}  // namespace

TEST(Ruvia, route_shapes_static_side_wins_over_parameter_pair) {
    // "/shapes/a/*" and "/shapes/b/:z" both overlap "/shapes/:x/:y"; the literal
    // branch is tried first and the parameter pair serves everything else.
    ruvia::test_app app;
    EXPECT_EQ(body_of(app, "/shapes/a/b"), std::string("a-wild:b"));
    EXPECT_EQ(body_of(app, "/shapes/a/b/c"), std::string("a-wild:b/c"));
    EXPECT_EQ(body_of(app, "/shapes/a"), std::string("a-wild:"));
    EXPECT_EQ(body_of(app, "/shapes/b/q"), std::string("b-param:q"));
    EXPECT_EQ(body_of(app, "/shapes/c/d"), std::string("pair:c/d"));
}

TEST(Ruvia, route_shapes_parameter_wins_over_sibling_wildcard) {
    // A parameter is tried before the wildcard at the same node and the
    // wildcard takes over once the parameter branch fails.
    ruvia::test_app app;
    EXPECT_EQ(body_of(app, "/shapes/w/1"), std::string("w-param:1"));
    EXPECT_EQ(body_of(app, "/shapes/w/1/2"), std::string("w-wild:1/2"));
}

TEST(Ruvia, route_shapes_scoped_middleware_follows_the_request_path) {
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");

    // One parameterized route serves paths on both sides of the scope.
    EXPECT_EQ(body_of(app, "/admin/panel"), std::string("panel:admin"));
    EXPECT_EQ(scope_of(app, "/admin/panel"), std::string("admin"));
    EXPECT_EQ(body_of(app, "/admin/panel/"), std::string("panel-slash:admin"));
    EXPECT_EQ(scope_of(app, "/admin/panel/"), std::string("admin"));
    EXPECT_EQ(body_of(app, "/other/panel"), std::string("panel:other"));
    EXPECT_EQ(scope_of(app, "/other/panel"), std::string("none"));
    // Whole segments only.
    EXPECT_EQ(scope_of(app, "/administrator/panel"), std::string("none"));
    // Scope segments compare percent-decoded, the form the handler reads its
    // parameter in, so an encoded spelling cannot step around the scope; an
    // encoded slash stays inside its segment.
    EXPECT_EQ(body_of(app, "/%61dmin/panel"), std::string("panel:admin"));
    EXPECT_EQ(scope_of(app, "/%61dmin/panel"), std::string("admin"));
    EXPECT_EQ(scope_of(app, "/admin%2Fx/panel"), std::string("none"));

    // A route entirely inside the scope and one entirely outside it.
    EXPECT_EQ(scope_of(app, "/admin/settings"), std::string("admin"));
    EXPECT_EQ(scope_of(app, "/shapes/c/d"), std::string("none"));
    // An encoded spelling of a static route is that route, under its scope.
    EXPECT_EQ(body_of(app, "/%61dmin/s%65ttings"), std::string("settings:admin"));
    EXPECT_EQ(scope_of(app, "/%61dmin/s%65ttings"), std::string("admin"));
}

TEST(Ruvia, route_shapes_scoped_middleware_checks_wildcard_captures) {
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/assets/public"}, "public");

    EXPECT_EQ(scope_of(app, "/assets/public"), std::string("public"));
    EXPECT_EQ(scope_of(app, "/assets/public/app.js"), std::string("public"));
    EXPECT_EQ(scope_of(app, "/assets/private/key"), std::string("none"));
    EXPECT_EQ(scope_of(app, "/assets/publicity"), std::string("none"));
    EXPECT_EQ(scope_of(app, "/assets"), std::string("none"));
}

TEST(Ruvia, route_shapes_trailing_slash_is_a_distinct_path_for_every_route_kind) {
    // RFC 3986 path identity: "/a" and "/a/" name different resources, so
    // static and dynamic routes alike neither alias nor shadow each other.
    ruvia::test_app app;
    EXPECT_EQ(body_of(app, "/slash/static"), std::string("static:bare"));
    EXPECT_EQ(body_of(app, "/slash/static/"), std::string("static:slash"));
    EXPECT_EQ(body_of(app, "/slash/42"), std::string("dyn:42"));
    EXPECT_EQ(body_of(app, "/slash/42/"), std::string("dyn-slash:42"));
    EXPECT_EQ(body_of(app, "/slash/42//"), std::string("unexpected status"));

    EXPECT_EQ(body_of(app, "/only/1"), std::string("only:1"));
    EXPECT_EQ(app.request(ruvia::test_request::get("/only/1/")).status(), ruvia::http_status::not_found);
    EXPECT_EQ(body_of(app, "/only-static"), std::string("only:static"));
    EXPECT_EQ(app.request(ruvia::test_request::get("/only-static/")).status(), ruvia::http_status::not_found);
}

TEST(Ruvia, route_shapes_url_for_keeps_the_registered_trailing_slash) {
    ruvia::test_app app;
    const auto links = app.request(ruvia::test_request::get("/slash-links"));
    EXPECT_EQ(links.status(), ruvia::http_status::ok);
    EXPECT_EQ(links.body(), "/slash/static/ /slash/7/ /slash/7");
    EXPECT_EQ(body_of(app, "/slash/7/"), std::string("dyn-slash:7"));
    EXPECT_EQ(body_of(app, "/slash/7"), std::string("dyn:7"));
}

TEST(Ruvia, route_shapes_non_http_absolute_target_is_rejected) {
    // A generic-scheme absolute-form target would carry a path no front proxy
    // routes like this server; it is a 400 before any route or scope runs.
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");
    EXPECT_EQ(app.request(ruvia::test_request::get("x:admin/panel")).status(),
        ruvia::http_status::bad_request);
    EXPECT_EQ(app.request(ruvia::test_request::get("x:/admin/panel")).status(),
        ruvia::http_status::bad_request);
}

TEST(Ruvia, route_shapes_dot_segments_match_no_route) {
    // "/assets/public/../private/key" resolves to a resource under
    // "/assets/private" although its raw segments sit outside that scope.
    ruvia::test_app app;
    app.use_at<route_shapes_scope>({.prefix_ = "/assets/private"}, "private");
    for (const std::string_view path : {"/assets/public/../private/key", "/assets/public/%2E%2E/private/key",
             "/assets/./private/key", "/admin/panel/.", "/assets/%2e"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::not_found);
    }
    // Dots inside a segment are ordinary names.
    EXPECT_EQ(body_of(app, "/assets/..key/.env"), std::string("asset:..key/.env"));
    EXPECT_EQ(scope_of(app, "/assets/private/key"), std::string("private"));
}

TEST(Ruvia, route_shapes_conditional_frames_keep_chain_order_and_neighbours) {
    ruvia::test_app app;
    app.use<route_shapes_trace>();
    app.use_at<route_shapes_scope>({.prefix_ = "/admin"}, "admin");

    // The app-wide frame after a skipped conditional frame still runs.
    const auto outside = app.request(ruvia::test_request::get("/other/panel"));
    EXPECT_EQ(outside.status(), ruvia::http_status::ok);
    EXPECT_TRUE(outside.header("X-Route-Trace").has_value());
    EXPECT_TRUE(!outside.header("X-Route-Scope").has_value());

    const auto inside = app.request(ruvia::test_request::get("/admin/panel"));
    EXPECT_EQ(inside.status(), ruvia::http_status::ok);
    EXPECT_TRUE(inside.header("X-Route-Trace").has_value());
    EXPECT_TRUE(inside.header("X-Route-Scope").has_value());
}

TEST(Ruvia, route_shapes_conditional_frames_keep_single_shot_next) {
    ruvia::test_app app;
    app.use_at<route_shapes_double_next>({.prefix_ = "/admin"});

    const auto inside = app.request(ruvia::test_request::get("/admin/panel"));
    EXPECT_EQ(inside.status(), ruvia::http_status::internal_server_error);
    const auto outside = app.request(ruvia::test_request::get("/other/panel"));
    EXPECT_EQ(outside.status(), ruvia::http_status::ok);
    EXPECT_EQ(outside.body(), std::string_view("panel:other"));
}

TEST(Ruvia, route_shapes_error_responses_survive_an_unfinished_unmatched_chain) {
    ruvia::test_app app;
    app.use<route_shapes_unfinished>();

    // The 404 runs the unmatched chain, which never finalizes; it must still
    // yield a response rather than an exception escaping the server, and that
    // error response is not renegotiated into a 406 for the refused coding.
    const auto response =
        app.request(ruvia::test_request::get("/missing").header("Accept-Encoding", "*;q=0"));
    EXPECT_EQ(response.status(), ruvia::http_status::internal_server_error);
}

TEST(Ruvia, route_shapes_fallback_prefixes_compare_percent_decoded) {
    ruvia::test_app app;
    app.on_not_found({.prefix_ = "/api", .handler_ = &route_shapes_api_miss});

    // An escaped spelling of a registered scope is the same scope.
    bool rejected = false;
    try {
        app.on_not_found({.prefix_ = "/%61pi/", .handler_ = &route_shapes_api_miss});
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    EXPECT_TRUE(rejected);

    const auto escaped = app.request(ruvia::test_request::get("/%61pi/missing"));
    EXPECT_EQ(escaped.status(), ruvia::http_status::not_found);
    EXPECT_EQ(escaped.body(), std::string_view("api-miss"));
}

TEST(Ruvia, route_shapes_encoded_spellings_select_the_literal_route_and_its_middleware) {
    // RFC 3986 6.2.2.2: "%61dmin" is "admin". A parameter route must not
    // capture an encoded spelling of a guarded literal route, neither a static
    // one nor a literal segment inside a dynamic one.
    ruvia::test_app app;
    for (const std::string_view path : {"/equiv/users/admin", "/equiv/users/%61dmin", "/equiv/users/adm%69n",
             "/equiv/users/%61%64%6D%69%6E", "/equiv/users/admin/x", "/equiv/users/%61dmin/x",
             "/equiv/users/adm%69n/x"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::forbidden);
    }
    // Other names still reach the parameter route, decoded.
    EXPECT_EQ(body_of(app, "/equiv/users/%41dmin"), std::string("user:Admin"));
    EXPECT_EQ(body_of(app, "/equiv/users/bob/x"), std::string("user:bob"));
    // An encoded slash stays inside its segment: it names no literal route.
    EXPECT_EQ(body_of(app, "/equiv/users/admin%2Fx"), std::string("user:admin/x"));
}

TEST(Ruvia, route_shapes_hex_case_does_not_change_the_route) {
    // RFC 3986 6.2.2.1: hex digits in a percent-encoding are case-insensitive.
    ruvia::test_app app;
    for (const std::string_view path : {"/equiv/files/caf%C3%A9", "/equiv/files/caf%c3%a9", "/equiv/files/caf%C3%a9"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::forbidden);
    }
    EXPECT_EQ(body_of(app, "/equiv/files/caf%C3%A8"), std::string("file:caf\xC3\xA8"));
}

TEST(Ruvia, route_shapes_encoded_colon_or_star_literal_is_not_a_parameter_or_wildcard) {
    // A literal segment starting with ':' or a literal '*' can only be spelled
    // encoded ("%3Aid", "%2A"); it is a route of its own beside the parameter
    // or wildcard sibling, not a duplicate registration of it.
    ruvia::test_app app;
    for (const std::string_view path : {"/equiv/colon/%3Aid", "/equiv/colon/%3aid", "/equiv/star/%2A"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(path)).status(), ruvia::http_status::forbidden);
    }
    EXPECT_EQ(body_of(app, "/equiv/colon/42"), std::string("user:42"));
    EXPECT_EQ(body_of(app, "/equiv/star/x"), std::string("user:-"));
}

TEST(Ruvia, route_shapes_allow_names_every_extension_method_of_a_resource) {
    // RFC 9110 15.5.6: Allow lists the resource's supported methods, however
    // many extension methods it has and however each route spelled the path.
    const std::multiset<std::string_view> expected{
        {"OPTIONS", "EXT-A", "EXT-B", "EXT-C", "EXT-D", "EXT-E", "EXT-F", "EXT-G", "EXT-H", "EXT-I", "EXT-J"}};
    ruvia::test_app app;
    const auto options = app.request(ruvia::test_request::options("/equiv/many"));
    EXPECT_EQ(options.status(), ruvia::http_status::no_content);
    EXPECT_EQ(allow_methods(options.header("Allow").value_or("")), expected);
    const auto get = app.request(ruvia::test_request::get("/equiv/m%61ny"));
    EXPECT_EQ(get.status(), ruvia::http_status::method_not_allowed);
    EXPECT_EQ(allow_methods(get.header("Allow").value_or("")), expected);
    const auto extension = app.request(ruvia::test_request::method("EXT-J", "/equiv/many"));
    EXPECT_EQ(extension.status(), ruvia::http_status::ok);
}

TEST(Ruvia, route_shapes_response_stream_get_route_advertises_head) {
    // RFC 9110 9.1/9.3.2: a GET resource supports HEAD, streaming or not.
    ruvia::test_app app;
    const auto options = app.request(ruvia::test_request::options("/equiv/stream"));
    EXPECT_EQ(options.status(), ruvia::http_status::no_content);
    EXPECT_EQ(options.header("Allow").value_or(""), std::string_view("GET, HEAD, OPTIONS"));
    const auto post = app.request(ruvia::test_request::post("/equiv/stream"));
    EXPECT_EQ(post.status(), ruvia::http_status::method_not_allowed);
    EXPECT_EQ(post.header("Allow").value_or(""), std::string_view("GET, HEAD, OPTIONS"));
}

TEST(Ruvia, extension_routes_keep_method_and_canonical_path_identity) {
    struct route_case final {
        std::string_view method_;
        std::string_view path_;
        std::string_view body_;
    };
    constexpr route_case cases[] = {
        {"A-INDEX", "/indexed/a%2fb", "user:-"},
        {"Z-INDEX", "/indexed/a%2Fb", "guarded"},
        {"Z-INDEX", "/indexed/a/b", "file:-"},
        {"A-INDEX", "/indexed/a", "guarded"},
        {"Z-INDEX", "/indexed/z", "guarded"},
        {"A-INDEX-TAIL", "/indexed/m", "file:-"}};
    ruvia::test_app app;
    for (const auto& item : cases) {
        SCOPED_TRACE(std::string(item.method_) + " " + std::string(item.path_));
        const auto response = app.request(ruvia::test_request::method(item.method_, item.path_));
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(response.body(), item.body_);
    }
}

TEST(Ruvia, extension_route_misses_distinguish_method_and_resource_identity) {
    struct miss_case final {
        std::string_view method_;
        std::string_view path_;
        ruvia::http_status_code status_;
    };
    const miss_case cases[] = {
        {"A-INDEX", "/0-indexed", ruvia::http_status::not_found},
        {"A-INDEX", "/zz-indexed", ruvia::http_status::not_found},
        {"A-INDEX", "/indexed/a%2fbb", ruvia::http_status::not_found},
        {"A-INDEX", "/indexed/m", ruvia::http_status::method_not_allowed},
        {"A-INDEX-", "/indexed/a", ruvia::http_status::not_implemented},
        {"a-index", "/indexed/a", ruvia::http_status::not_implemented},
        {"000-NOT-REGISTERED-INDEX", "/indexed/a", ruvia::http_status::not_implemented},
        {"ZZZ-NOT-REGISTERED-INDEX", "/indexed/a", ruvia::http_status::not_implemented}};
    ruvia::test_app app;
    for (const auto& item : cases) {
        SCOPED_TRACE(std::string(item.method_) + " " + std::string(item.path_));
        const auto response = app.request(ruvia::test_request::method(item.method_, item.path_));
        EXPECT_EQ(response.status(), item.status_);
        if (item.status_ != ruvia::http_status::method_not_allowed) {
            EXPECT_FALSE(response.header("Allow").has_value());
        }
    }
}

TEST(Ruvia, extension_route_allow_groups_only_canonical_equivalent_paths) {
    ruvia::test_app app;
    const auto encoded = app.request(ruvia::test_request::options("/indexed/a%2Fb"));
    EXPECT_EQ(encoded.status(), ruvia::http_status::no_content);
    EXPECT_EQ(allow_methods(encoded.header("Allow").value_or("")),
        (std::multiset<std::string_view>{"OPTIONS", "A-INDEX", "Z-INDEX"}));
    const auto split = app.request(ruvia::test_request::method("A-INDEX", "/indexed/a/b"));
    EXPECT_EQ(split.status(), ruvia::http_status::method_not_allowed);
    EXPECT_EQ(allow_methods(split.header("Allow").value_or("")),
        (std::multiset<std::string_view>{"OPTIONS", "Z-INDEX"}));
    const auto plain = app.request(ruvia::test_request::options("/indexed/%61"));
    EXPECT_EQ(plain.status(), ruvia::http_status::no_content);
    EXPECT_EQ(allow_methods(plain.header("Allow").value_or("")),
        (std::multiset<std::string_view>{"OPTIONS", "A-INDEX"}));
}

namespace proxy_listener_test {
using namespace std::chrono_literals;

struct runtime final {
    asio::io_context io_;
    ruvia::blocking_pool pool_{{.thread_count_ = 1}};
    ruvia::event_loop_attachment attachment_{ruvia::attach_event_loop(io_, {.blocking_pool_ = &pool_})};
    ruvia::event_loop loop_{attachment_.loop()};
    std::jthread thread_{[this] { attachment_.run(); }};
    ~runtime() {
        attachment_.stop();
        thread_.join();
        pool_.join();
    }
};

ruvia::task<void> identify(ruvia::request_stream& stream) {
    const auto remote = stream.conn().remote();
    const auto text = remote.address().to_string();
    std::string body(text.data(), text.size());
    body += ":" + std::to_string(remote.port());
    body += ":" + std::string(stream.conn().client().address().to_string());
    body += stream.conn().via_trusted_proxy() ? ":trusted" : ":direct";
    if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
        co_await stream.response().write(body);
        co_await stream.response().end();
    }
}

ruvia::task<void> echo(ruvia::server_connection& connection) {
    while (auto bytes = co_await connection.read()) {
        co_await connection.write(*bytes);
    }
}

std::string head(std::uint16_t port, std::string_view forwarded = {}) {
    return "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n" + std::string(forwarded) + "\r\n";
}

std::string proxy_v2(bool local, std::uint16_t port) {
    std::string result("\r\n\r\n\0\r\nQUIT\n", 12);
    result.push_back(local ? '\x20' : '\x21');
    result.push_back('\x11');
    result.append("\0\x10", 2);  // IPv4 address block and a four-byte TLV.
    const unsigned char address[]{198, 51, 100, 23, 127, 0, 0, 1, 0x12, 0x34,
        static_cast<unsigned char>(port >> 8), static_cast<unsigned char>(port), 0xe0, 0, 1, 42};
    result.append(reinterpret_cast<const char*>(address), sizeof(address));
    return result;
}

ruvia::task<void> start(ruvia::http_server& server, ruvia::listen_config config) {
    server.listen(std::move(config));
    co_await server.start();
}

ruvia::task<void> adopt_read_prefix(ruvia::http_server& server, asio::ip::tcp::acceptor& acceptor,
    std::size_t bytes, std::optional<ruvia::ip_endpoint> remote = {}) {
    auto accepted = co_await ruvia::async_asio<asio::ip::tcp::socket>([&](auto handler) {
        acceptor.async_accept(std::move(handler));
    });
    if (accepted.error_code()) {
        throw std::system_error(accepted.error_code());
    }
    std::vector<std::byte> prefix(bytes);
    const auto read = co_await ruvia::async_asio<std::size_t>([&](auto handler) {
        asio::async_read(accepted.result(), asio::buffer(prefix), std::move(handler));
    });
    if (read.error_code()) {
        throw std::system_error(read.error_code());
    }
    server.adopt({.address_ = "127.0.0.1", .port_ = acceptor.local_endpoint().port()}, accepted.result().release(),
        {.prefix_ = prefix, .remote_ = remote});
    // The prefix owner dies before the session starts reading it.
}
}  // namespace proxy_listener_test

TEST(Ruvia, proxy_listener_requires_preface_and_uses_source_for_every_peer_fact) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::mutex mutex;
    std::vector<ruvia::ip_endpoint> admitted;
    std::vector<std::string> logged;
    ruvia::http_server server(runtime.loop_, {.request_stream_ = &identify,
                                                 .access_ = [&](const ruvia::access_log_record& record) noexcept {
            std::lock_guard lock(mutex);
            logged.emplace_back(record.remote_address().to_string()); },
                                                 .connection_admission_ = [&](const ruvia::connection_event& event) noexcept {
            std::lock_guard lock(mutex);
            admitted.push_back(event.remote());
            return true; }});
    const auto setup = [&]() -> ruvia::task<void> {
        ruvia::trusted_proxy_config trusted;
        trusted.cidrs_.emplace_back("203.0.113.0/24");
        server.trusted_proxies(std::move(trusted));
        co_await start(server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .proxy_protocol_ = true});
    };
    runtime.loop_.start(setup()).get();
    const auto request = head(port.endpoint().port(), "X-Forwarded-For: 192.0.2.77\r\n");
    auto response = ruvia::test::exchange(io, port.endpoint(),
        "PROXY TCP4 203.0.113.9 127.0.0.1 4567 " + std::to_string(port.endpoint().port()) + "\r\n" + request);
    EXPECT_NE(response.find("203.0.113.9:4567:192.0.2.77:trusted"), std::string::npos);
    response = ruvia::test::exchange(io, port.endpoint(), proxy_v2(false, port.endpoint().port()) + request);
    EXPECT_NE(response.find("198.51.100.23:4660:198.51.100.23:direct"), std::string::npos);
    response = ruvia::test::exchange(io, port.endpoint(), proxy_v2(true, port.endpoint().port()) + request);
    EXPECT_NE(response.find(":127.0.0.1:direct"), std::string::npos);
    {
        asio::ip::tcp::socket peer(io);
        peer.connect(port.endpoint());
        asio::write(peer, asio::buffer(request));
        std::error_code error;
        EXPECT_TRUE(ruvia::test::read_to_end(peer, error).empty());
        EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    }
    runtime.loop_.start(server.shutdown()).get();
    std::lock_guard lock(mutex);
    ASSERT_EQ(admitted.size(), 3u);
    EXPECT_EQ(admitted[0], ruvia::ip_endpoint(*ruvia::ip_address::parse("203.0.113.9"), 4567));
    EXPECT_EQ(admitted[1], ruvia::ip_endpoint(*ruvia::ip_address::parse("198.51.100.23"), 4660));
    EXPECT_EQ(admitted[2].address(), *ruvia::ip_address::parse("127.0.0.1"));
    EXPECT_EQ(logged, (std::vector<std::string>{"203.0.113.9", "198.51.100.23", "127.0.0.1"}));
}

TEST(Ruvia, proxy_listener_bounds_header_completion_and_rejects_http3) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    ruvia::http_server server(runtime.loop_, {.request_stream_ = &identify},
        {.request_header_timeout_ = 40ms, .header_completion_timeout_ = 60ms});
    runtime.loop_.start(start(server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .proxy_protocol_ = true})).get();
    asio::ip::tcp::socket peer(io);
    peer.connect(port.endpoint());
    asio::write(peer, asio::buffer("PROXY ", 6));
    const auto began = std::chrono::steady_clock::now();
    std::error_code error;
    EXPECT_TRUE(ruvia::test::read_to_end(peer, error).empty());
    EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 2s);
    runtime.loop_.start(server.shutdown()).get();
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::http_server invalid(runtime.loop_, {.request_stream_ = &identify});
    EXPECT_THROW(runtime.loop_.start(start(invalid,
                                         {.address_ = "127.0.0.1", .https_ = port.endpoint().port(), .http3_ = port.endpoint().port(), .proxy_protocol_ = true}))
                     .get(),
        std::invalid_argument);
    runtime.loop_.start(invalid.shutdown()).get();
#endif
}

TEST(Ruvia, adopted_prefix_is_replayed_for_http_raw_and_tls_with_remote_override) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    asio::ip::tcp::acceptor http(runtime.io_, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor raw(runtime.io_, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor tls(runtime.io_, {asio::ip::address_v4::loopback(), 0});
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server http_server(runtime.loop_, {.request_stream_ = &identify});
    ruvia::http_server raw_server(runtime.loop_, {.connection_ = &echo});
    ruvia::http_server tls_server(runtime.loop_, {.request_stream_ = &identify});
    const auto setup = [&]() -> ruvia::task<void> {
        co_await http_server.add_connection_service({.address_ = "127.0.0.1", .http_ = http.local_endpoint().port()});
        co_await raw_server.add_connection_service({.address_ = "127.0.0.1", .http_ = raw.local_endpoint().port()});
        co_await tls_server.add_connection_service({.address_ = "127.0.0.1", .https_ = tls.local_endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}});
        co_await http_server.start();
        co_await raw_server.start();
        co_await tls_server.start();
    };
    runtime.loop_.start(setup()).get();
    auto adopted = runtime.loop_.start(adopt_read_prefix(http_server, http, 11,
        ruvia::ip_endpoint(*ruvia::ip_address::parse("192.0.2.123"), 444)));
    auto response = ruvia::test::exchange(io, http.local_endpoint(), head(http.local_endpoint().port()));
    std::move(adopted).get();
    EXPECT_NE(response.find("192.0.2.123:444:192.0.2.123:direct"), std::string::npos);
    auto raw_adopted = runtime.loop_.start(adopt_read_prefix(raw_server, raw, 4));
    asio::ip::tcp::socket peer(io);
    peer.connect(raw.local_endpoint());
    asio::write(peer, asio::buffer("prefix-and-tail", 15));
    peer.shutdown(asio::ip::tcp::socket::shutdown_send);
    EXPECT_EQ(ruvia::test::read_to_end(peer), "prefix-and-tail");
    std::move(raw_adopted).get();
    auto tls_adopted = runtime.loop_.start(adopt_read_prefix(tls_server, tls, 7));
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.load_verify_file(identity.ca_file_.string());
    asio::ssl::stream<asio::ip::tcp::socket> secure(io, context);
    secure.set_verify_mode(asio::ssl::verify_peer);
    secure.lowest_layer().connect(tls.local_endpoint());
    secure.handshake(asio::ssl::stream_base::client);
    asio::write(secure, asio::buffer(head(tls.local_endpoint().port())));
    response = ruvia::test::read_to_end(secure);
    EXPECT_NE(response.find("127.0.0.1"), std::string::npos);
    std::move(tls_adopted).get();
    runtime.loop_.start(http_server.shutdown()).get();
    runtime.loop_.start(raw_server.shutdown()).get();
    runtime.loop_.start(tls_server.shutdown()).get();
}

TEST(Ruvia, standalone_dispatch_rejects_missing_ambiguous_and_mismatched_callbacks) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    ruvia::http_server absent(runtime.loop_, {});
    ruvia::http_server ambiguous(runtime.loop_, {.request_stream_ = &identify, .connection_ = &echo});
    ruvia::http_server mismatch(runtime.loop_, {.request_stream_ = &identify});
    for (auto* server : {&absent, &ambiguous}) {
        EXPECT_THROW(runtime.loop_.start(start(*server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port()})).get(), std::invalid_argument);
        runtime.loop_.start(server->shutdown()).get();
    }
    EXPECT_THROW(runtime.loop_.start(start(mismatch, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .dispatch_ = ruvia::listener_dispatch::connection})).get(), std::invalid_argument);
    runtime.loop_.start(mismatch.shutdown()).get();
}

TEST(Ruvia, proxy_source_is_the_router_rate_limit_key) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    std::unique_ptr<ruvia::http_server> server;
    const auto setup = [&]() -> ruvia::task<void> {
        auto routes = std::make_shared<ruvia::router>(runtime.loop_.worker());
        routes->use<ruvia::rate_limit<1, 60000>>();
        routes->add("GET", "/", [](ruvia::context& context) -> ruvia::task<ruvia::http_response> {
            co_return context.text("allowed");
        });
        routes->freeze();
        server = std::make_unique<ruvia::http_server>(runtime.loop_, ruvia::http_server_callbacks{
                                                                         .request_stream_ = [routes](ruvia::request_stream& stream) { return routes->dispatch(stream); }});
        co_await start(*server, {.address_ = "127.0.0.1", .http_ = port.endpoint().port(), .proxy_protocol_ = true});
    };
    runtime.loop_.start(setup()).get();
    const auto request = [&](std::string_view source) {
        return ruvia::test::exchange(io, port.endpoint(), "PROXY TCP4 " + std::string(source) + " 127.0.0.1 4567 " + std::to_string(port.endpoint().port()) + "\r\n" + head(port.endpoint().port()));
    };
    EXPECT_TRUE(request("198.51.100.10").starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(request("198.51.100.10").starts_with("HTTP/1.1 429"));
    EXPECT_TRUE(request("198.51.100.11").starts_with("HTTP/1.1 200"));
    runtime.loop_.start(server->shutdown()).get();
}

TEST(Ruvia, proxy_preface_precedes_tls_client_hello_and_preserves_effective_source) {
    using namespace proxy_listener_test;
    runtime runtime;
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    const ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    ruvia::http_server server(runtime.loop_, {.request_stream_ = &identify});
    runtime.loop_.start(start(server, {.address_ = "127.0.0.1", .https_ = port.endpoint().port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}}, .proxy_protocol_ = true})).get();
    asio::ssl::context context(asio::ssl::context::tls_client);
    context.load_verify_file(identity.ca_file_.string());
    asio::ssl::stream<asio::ip::tcp::socket> peer(io, context);
    peer.set_verify_mode(asio::ssl::verify_peer);
    peer.next_layer().connect(port.endpoint());
    const std::string preface = "PROXY TCP6 2001:db8::123 ::1 9876 " + std::to_string(port.endpoint().port()) + "\r\n";
    asio::write(peer.next_layer(), asio::buffer(preface));
    peer.handshake(asio::ssl::stream_base::client);
    asio::write(peer, asio::buffer(head(port.endpoint().port())));
    const auto response = ruvia::test::read_to_end(peer);
    EXPECT_NE(response.find("2001:db8::123:9876:2001:db8::123:direct"), std::string::npos);
    runtime.loop_.start(server.shutdown()).get();
}

namespace {

ruvia::tls_config tls_config(std::filesystem::path certificate_chain_file,
    std::filesystem::path private_key_file, std::string_view private_key_password = {}) {
    return {.identity_ = {
                .certificate_chain_file_ = std::move(certificate_chain_file),
                .private_key_file_ = std::move(private_key_file),
                .private_key_password_ = std::string(private_key_password),
            }};
}

class querying_callback_target final {
public:
    explicit querying_callback_target(int& destroyed) noexcept
        : destroyed_(&destroyed) {}

    querying_callback_target(querying_callback_target&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {
        (void)ruvia::app().workers();
    }

    ~querying_callback_target() {
        if (destroyed_ != nullptr) {
            (void)ruvia::app().workers();
            ++*destroyed_;
        }
    }

    void operator()(const ruvia::access_log_record&) const noexcept {}
    void operator()(const ruvia::connection_failure_record&) const noexcept {}

private:
    int* destroyed_;
};

// Destroying it re-enters the application lock through stats() and
// workers(). It must not call stop(): on a stopped app that leaves a pending
// stop which the next run() of the shared server fixture would consume.
class reentrant_hook_target final {
public:
    explicit reentrant_hook_target(int& destroyed) noexcept
        : destroyed_(&destroyed) {}

    reentrant_hook_target(reentrant_hook_target&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

    ~reentrant_hook_target() {
        if (destroyed_ != nullptr) {
            (void)ruvia::app().stats();
            (void)ruvia::app().workers();
            ++*destroyed_;
        }
    }

    void operator()() const {}

    ruvia::task<ruvia::http_response> operator()(ruvia::context& context, ruvia::http_error_info) const {
        co_return context.text("error");
    }

    ruvia::task<ruvia::http_response> operator()(ruvia::context& context) const {
        co_return context.text("not found");
    }

private:
    int* destroyed_;
};

}  // namespace

TEST(Ruvia, application_callback_replacement_retires_targets_outside_configuration_lock) {
    int access_destroyed = 0;
    int failure_destroyed = 0;
    auto& app = ruvia::app();
    app.on_access(querying_callback_target(access_destroyed));
    app.on_access(querying_callback_target(access_destroyed));
    EXPECT_EQ(access_destroyed, 1);
    app.on_access(nullptr);
    EXPECT_EQ(access_destroyed, 2);

    app.on_connection_failure(querying_callback_target(failure_destroyed));
    app.on_connection_failure(querying_callback_target(failure_destroyed));
    EXPECT_EQ(failure_destroyed, 1);
    app.on_connection_failure(nullptr);
    EXPECT_EQ(failure_destroyed, 2);
}

TEST(Ruvia, clearing_hooks_and_fallbacks_retires_targets_outside_configuration_lock) {
    int destroyed = 0;
    auto& app = ruvia::app();
    app.on_start(reentrant_hook_target(destroyed)).on_start(reentrant_hook_target(destroyed));
    app.on_start(nullptr);
    EXPECT_EQ(destroyed, 2);

    app.on_stop(reentrant_hook_target(destroyed));
    app.on_stop(nullptr);
    EXPECT_EQ(destroyed, 3);

    app.on_error(ruvia::http_error_handler(reentrant_hook_target(destroyed)));
    app.on_error(nullptr);
    EXPECT_EQ(destroyed, 4);

    app.on_not_found(ruvia::http_not_found_handler(reentrant_hook_target(destroyed)));
    app.on_not_found(nullptr);
    EXPECT_EQ(destroyed, 5);
}

TEST(Ruvia, listener_config_rejects_invalid_listener_and_tls_states_at_construction) {
    EXPECT_TRUE(throws_invalid([] { ruvia::app().listen({}); }));
    EXPECT_TRUE(throws_invalid([] { ruvia::app().listen({.address_ = {}, .http_ = 8080}); }));
    EXPECT_TRUE(throws_invalid([] { ruvia::app().listen({.address_ = "localhost", .http_ = 8080}); }));
    EXPECT_TRUE(throws_invalid([] {
        const std::string bad_address("127.0.0.1\0bad", 13);
        ruvia::app().listen({.address_ = bad_address, .http_ = 8080});
    }));
    EXPECT_TRUE(throws_invalid([] { ruvia::app().listen({.address_ = "127.0.0.1", .http_ = 0}); }));
    // Matching ports are supported, but still require a real TLS identity.
    EXPECT_TRUE(throws_invalid(
        [] { ruvia::app().listen({.address_ = "127.0.0.1", .http_ = 8080, .https_ = 8080}); }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().listen({.address_ = "127.0.0.1", .http_ = 8080, .auto_https_redirect_ = true});
    }));
    EXPECT_TRUE(
        throws_invalid([] { ruvia::app().listen({.address_ = "127.0.0.1", .https_ = 8443}); }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ = {.identity_ = {.certificate_chain_file_ = "cert.pem"}},
        });
    }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .http_ = 8080,
            .tls_ = {.identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"}},
        });
    }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ =
                {
                    .identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
                    .client_certificates_ =
                        {
                            .requirement_ = ruvia::tls_client_certificate_requirement::required,
                        },
                },
        });
    }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ =
                {
                    .identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
                    .max_outgoing_records_ = 0,
                },
        });
    }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ =
                {
                    .identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
                    .max_outgoing_records_ = (std::uint64_t{1} << 23) + 1,
                },
        });
    }));
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().server(
            {.process_signal_handlers_ = static_cast<ruvia::process_signal_handler_policy>(0xFF)});
    }));
}

TEST(Ruvia, tls_config_rejects_empty_or_duplicate_sni_identity) {
    const auto rejects = [](std::initializer_list<ruvia::tls_sni_config> sni) {
        return throws_invalid([&] {
            ruvia::app().listen({
                .address_ = "127.0.0.1",
                .https_ = 8443,
                .tls_ =
                    {
                        .identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
                        .sni_ = std::vector<ruvia::tls_sni_config>(sni),
                    },
            });
        });
    };
    const ruvia::tls_identity_config other{.certificate_chain_file_ = "other.pem", .private_key_file_ = "other.key"};
    EXPECT_TRUE(rejects({{.host_ = {}, .identity_ = other}}));
    EXPECT_TRUE(rejects({{.host_ = "example.com:443", .identity_ = other}}));
    EXPECT_TRUE(rejects({{.host_ = "127.0.0.1", .identity_ = other}}));
    EXPECT_TRUE(rejects({
        {.host_ = "Example.com", .identity_ = other},
        {.host_ = "example.COM",
            .identity_ = {.certificate_chain_file_ = "third.pem", .private_key_file_ = "third.key"}},
    }));
}

TEST(Ruvia, tls_config_rejects_invalid_or_duplicate_sni_wildcards) {
    const auto listen_with = [](std::vector<ruvia::tls_sni_config> sni) {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ =
                {
                    .identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
                    .sni_ = std::move(sni),
                },
        });
    };
    const ruvia::tls_identity_config other{.certificate_chain_file_ = "other.pem", .private_key_file_ = "other.key"};
    for (const char* host : {"*", "*.", "**.example.com", "*.*.example.com", "a.*.example.com", "*example.com",
             "w*.example.com", "*.example.com.", "*.127.0.0.1"}) {
        EXPECT_TRUE(throws_invalid([&] { listen_with({{.host_ = host, .identity_ = other}}); })) << host;
    }
    EXPECT_TRUE(throws_invalid([&] {
        listen_with({{.host_ = "*.Example.com", .identity_ = other}, {.host_ = "*.example.COM", .identity_ = other}});
    }));
}

// Each identity comes from exactly one complete source, and each local
// address is a distinct, specific numeric IP address.
TEST(Ruvia, tls_config_rejects_ambiguous_identity_sources_and_local_addresses) {
    const auto listen_with = [](ruvia::tls_config tls) {
        ruvia::app().listen({.address_ = "127.0.0.1", .https_ = 8443, .tls_ = std::move(tls)});
    };
    const auto rejects = [&](ruvia::tls_config tls) { return throws_invalid([&] { listen_with(std::move(tls)); }); };
    EXPECT_TRUE(rejects({.identity_ = {.certificate_chain_file_ = "cert.pem",
                             .private_key_file_ = "key.pem",
                             .certificate_chain_pem_ = "pem",
                             .private_key_pem_ = "pem"}}));
    EXPECT_TRUE(rejects({.identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_pem_ = "pem"}}));
    EXPECT_TRUE(rejects({.identity_ = {.certificate_chain_pem_ = "pem"}}));
    EXPECT_TRUE(rejects({.identity_ = {.private_key_pem_ = "pem"}}));

    const ruvia::tls_identity_config files{.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"};
    const auto rejects_local = [&](std::vector<ruvia::tls_local_address_config> local) {
        return rejects({.identity_ = files, .local_addresses_ = std::move(local)});
    };
    for (const char* address : {"", "localhost", "127.0.0.1:443", "0.0.0.0", "::", "::ffff:0.0.0.0"}) {
        EXPECT_TRUE(rejects_local({{.address_ = address, .identity_ = files}})) << address;
    }
    EXPECT_TRUE(rejects_local({{.address_ = "127.0.0.2"}}));
    EXPECT_TRUE(rejects_local(
        {{.address_ = "127.0.0.2", .identity_ = files}, {.address_ = "::ffff:127.0.0.2", .identity_ = files}}));
    // TLS settings require an HTTPS port.
    EXPECT_TRUE(throws_invalid([] {
        ruvia::app().listen({.address_ = "127.0.0.1",
            .http_ = 8080,
            .tls_ = {.identity_ = {.certificate_chain_pem_ = "pem", .private_key_pem_ = "pem"}}});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        ruvia::app().listen(
            {.address_ = "127.0.0.1", .http_ = 8080, .tls_ = {.local_addresses_ = {{.address_ = "127.0.0.2", .identity_ = files}}}});
    }));
}

TEST(Ruvia, tls_identity_owns_password_independently_of_caller_buffer) {
    const std::string expected(80, 's');
    std::optional<ruvia::tls_config> identity;
    {
        std::string password(expected);
        identity.emplace(tls_config("cert.pem", "key.pem", std::string_view(password)));
        password.assign(password.size(), 'x');
    }
    EXPECT_EQ(identity->identity_.private_key_password_, std::string_view(expected));
}

TEST(Ruvia, self_contained_app_callbacks_release_owned_state) {
    std::weak_ptr<int> access_state;
    {
        auto state_value = std::make_shared<int>(1);
        access_state = state_value;
        std::optional<ruvia::access_log_callback> moved;
        {
            // A move-only callable registers; moving the owner transfers the
            // captured state instead of releasing it.
            ruvia::access_log_callback callback_value(
                [state_value, guard = std::make_unique<int>(2)](const ruvia::access_log_record&) noexcept {
                    (void)state_value;
                    (void)guard;
                });
            moved.emplace(std::move(callback_value));
        }
        state_value.reset();
        EXPECT_TRUE(!access_state.expired());
        moved.reset();
        EXPECT_TRUE(access_state.expired());
    }

    std::weak_ptr<int> failure_state;
    {
        auto state_value = std::make_shared<int>(1);
        failure_state = state_value;
        ruvia::connection_failure_callback callback_value(
            [state_value](const ruvia::connection_failure_record&) noexcept { (void)state_value; });
        state_value.reset();
        EXPECT_TRUE(!failure_state.expired());
    }
    EXPECT_TRUE(failure_state.expired());

    std::weak_ptr<int> error_state;
    {
        auto state_value = std::make_shared<int>(1);
        error_state = state_value;
        ruvia::http_error_handler callback_value(
            [state_value](ruvia::context&, ruvia::http_error_info) -> ruvia::task<ruvia::http_response> {
                (void)state_value;
                co_return ruvia::http_response{};
            });
        state_value.reset();
        EXPECT_TRUE(!error_state.expired());
    }
    EXPECT_TRUE(error_state.expired());

    std::weak_ptr<int> not_found_state;
    {
        auto state_value = std::make_shared<int>(1);
        not_found_state = state_value;
        ruvia::http_not_found_handler callback_value(
            [state_value](ruvia::context&) -> ruvia::task<ruvia::http_response> {
                (void)state_value;
                co_return ruvia::http_response{};
            });
        state_value.reset();
        EXPECT_TRUE(!not_found_state.expired());
    }
    EXPECT_TRUE(not_found_state.expired());
}

TEST(Ruvia, app_callbacks_treat_null_function_targets_as_empty) {
    void (*hook_target)() = nullptr;
    ruvia::app_hook hook(hook_target);
    EXPECT_TRUE(!hook);

    void (*access_target)(const ruvia::access_log_record&) noexcept = nullptr;
    ruvia::access_log_callback access(std::move(access_target));
    EXPECT_TRUE(!access);

    ruvia::task<ruvia::http_response> (*error_target)(ruvia::context&, ruvia::http_error_info) = nullptr;
    ruvia::http_error_handler error(error_target);
    EXPECT_TRUE(!error);
}

TEST(Ruvia, noexcept_app_callback_retires_owner_before_target_destruction) {
    int destroyed = 0;
    bool owner_was_empty = false;
    ruvia::access_log_callback callback;
    const auto release = [&](int* value) noexcept {
        ++destroyed;
        owner_was_empty = !callback;
        if (owner_was_empty) {
            callback = nullptr;
        }
        delete value;
    };
    callback = [guard = std::unique_ptr<int, decltype(release)>(new int{0}, release)](
                   const ruvia::access_log_record&) noexcept {};
    callback = nullptr;
    EXPECT_TRUE(!callback);
    EXPECT_TRUE(owner_was_empty);
    EXPECT_EQ(destroyed, 1);
}

TEST(Ruvia, application_multi_listen_and_null_handler_validation) {
    auto& app = ruvia::app();
    EXPECT_TRUE(throws_invalid([&] {
        app.listen(std::initializer_list<ruvia::listen_config>{});
    }));
    EXPECT_TRUE(throws_invalid([&] {
        app.listen({
            {.address_ = "127.0.0.1", .http_ = 8080},
            {.address_ = "127.0.0.1", .http_ = 8080},
        });
    }));
    app.listen({
        {.address_ = "127.0.0.1", .http_ = 8080},
        {.address_ = "127.0.0.1", .http_ = 8081},
    });

    EXPECT_TRUE(throws_invalid([&] { app.on_start(ruvia::app_hook{}); }));
    EXPECT_TRUE(throws_invalid([&] { app.on_stop(ruvia::app_hook{}); }));
    app.on_start([] {}).on_start(nullptr);
    app.on_stop([] {}).on_stop(nullptr);

    EXPECT_TRUE(throws_invalid([&] { app.on_error(ruvia::http_error_handler{nullptr}); }));
    EXPECT_TRUE(throws_invalid([&] { app.on_not_found(ruvia::http_not_found_handler{nullptr}); }));
    app.on_error([](ruvia::context& c, ruvia::http_error_info info) -> ruvia::task<ruvia::http_response> {
        co_return c.error(info);
    });
    app.on_error(nullptr);
    app.on_not_found([](ruvia::context& c) -> ruvia::task<ruvia::http_response> {
        co_return c.text("missing");
    });
    app.on_not_found(nullptr);
}

// Route and controller middleware lists name types, so an entry there is
// default constructed. That does NOT mean such a middleware cannot be
// configured: the configuration travels as template parameters, which keeps it
// constexpr, keeps the type default constructible, and keeps the chain
// finalized at startup. These pin that, including the case a comma inside the
// template argument list makes look impossible through a variadic macro.

namespace route_middleware_config_test {
// Configuration carried in the type, so the middleware stays default
// constructible and the route macro needs no constructor arguments.
template <int level>
class configured_by_type final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Config", level == 2 ? "two" : "other");
    }
};

// Takes no configuration at all, so it is named bare in a route's list.
class plain_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Plain", "on");
    }
};

struct middleware_lifetime_observation final {
    unsigned live_{};
    unsigned constructed_{};
    unsigned destroyed_{};
};

class lifetime_middleware final {
public:
    lifetime_middleware(std::shared_ptr<middleware_lifetime_observation> observation, bool fail)
        : observation_(std::move(observation)) {
        if (fail) {
            throw std::runtime_error("middleware construction failed");
        }
        ++observation_->live_;
        ++observation_->constructed_;
    }

    ~lifetime_middleware() {
        --observation_->live_;
        ++observation_->destroyed_;
    }

    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }

private:
    std::shared_ptr<middleware_lifetime_observation> observation_;
};

// Two NTTPs: the comma lives inside the template argument list.
template <int a, int b>
class configured_by_two_values final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Config-Pair", a == 10 && b == 1000 ? "ok" : "bad");
    }
};

// A middleware may answer by itself: respond() before returning finalizes the
// response without running the rest of the chain.
class response_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next&) {
        c.draft().status(ruvia::http_status::accepted);
        c.respond(c.text("returned by middleware"));
        co_return;
    }
};

struct invalid_app_body_limit_middleware final {
    static constexpr std::size_t ruvia_request_body_limit_bytes = 16;

    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
    }
};

class configured_by_type_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/route-config")
    RUVIA_ROUTES_BEGIN(configured_by_type_controller)
    RUVIA_GET("/one", one, configured_by_type<2>);
    RUVIA_GET("/two", two, configured_by_two_values<10, 1000>);
    RUVIA_GET("/limited", limited, ruvia::rate_limit<10, 60'000>);
    RUVIA_POST("/small", small, ruvia::body_limit<16>);
    RUVIA_POST("/small-decoded", small, ruvia::body_limit<64>);
    RUVIA_POST("/default", default_body);
    RUVIA_GET("/answered", one, response_middleware);
    // Unparameterized and parameterized entries in ONE list: both are types, so
    // the typename pack takes them together and a bare name needs no braces.
    RUVIA_POST("/mixed", mixed, configured_by_type<2>, plain_middleware, ruvia::body_limit<32>,
        ruvia::rate_limit<10, 1000>);
    RUVIA_ROUTES_END
private:
    ruvia::task<ruvia::http_response> one(ruvia::context& c) {
        co_return c.text("one");
    }
    ruvia::task<ruvia::http_response> two(ruvia::context& c) {
        co_return c.text("two");
    }
    ruvia::task<ruvia::http_response> limited(ruvia::context& c) {
        co_return c.text("limited");
    }
    ruvia::task<ruvia::http_response> small(ruvia::context& c) {
        const auto body = co_await c.req().text();
        co_return c.body(body);
    }

    ruvia::task<ruvia::http_response> mixed(ruvia::context& c) {
        co_return c.text("mixed");
    }

    ruvia::task<ruvia::http_response> default_body(ruvia::context& c) {
        const auto body = co_await c.req().text();
        co_return c.body(body);
    }
};

// A controller-group rule stacked over a route rule: each declaration keeps its
// own counter, whether or not the two carry the same numbers.
class stacked_rate_limit_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/stacked-limit", ruvia::rate_limit<4, 60'000>)
    RUVIA_ROUTES_BEGIN(stacked_rate_limit_controller)
    RUVIA_GET("/same", limited, ruvia::rate_limit<4, 60'000>);
    RUVIA_GET("/looser", limited, ruvia::rate_limit<6, 60'000>);
    RUVIA_ROUTES_END
private:
    ruvia::task<ruvia::http_response> limited(ruvia::context& c) {
        co_return c.text("limited");
    }
};
}  // namespace route_middleware_config_test

using namespace route_middleware_config_test;  // NOLINT(google-build-using-namespace)

TEST(Ruvia, route_middleware_carries_its_configuration_in_the_type) {
    ruvia::test_app app;
    const auto one = app.request(ruvia::test_request::get("/route-config/one"));
    EXPECT_EQ(one.status(), ruvia::http_status::ok);
    EXPECT_EQ(one.header("X-Config").value_or(std::string_view{}), std::string_view("two"));

    // Two template arguments: the route macro is variadic, so the comma splits
    // its argument list -- but pasting __VA_ARGS__ back into the template
    // argument list rejoins them, which is why no wrapper type is needed.
    const auto two = app.request(ruvia::test_request::get("/route-config/two"));
    EXPECT_EQ(
        two.header("X-Config-Pair").value_or(std::string_view{}), std::string_view("ok"));
}

TEST(Ruvia, route_rate_limit_is_configured_without_a_generated_type) {
    ruvia::test_app app;
    // rate_limit<max, window> replaces the RUVIA_ROUTE_RATE_LIMIT macro,
    // whose only purpose was minting a named type to carry these two numbers.
    for (int i = 0; i < 10; ++i) {
        const auto limited = app.request(ruvia::test_request::get("/route-config/limited"));
        EXPECT_EQ(limited.status(), ruvia::http_status::ok);
        EXPECT_EQ(limited.body(), std::string_view("limited"));
    }
    const auto rejected = app.request(ruvia::test_request::get("/route-config/limited"));
    EXPECT_EQ(rejected.status(), ruvia::http_status::too_many_requests);
    EXPECT_EQ(
        rejected.header("X-RateLimit-Limit").value_or(std::string_view{}), std::string_view("10"));
}

TEST(Ruvia, route_rate_limit_rejection_reaches_the_error_handler) {
    ruvia::test_app app;
    int calls = 0;
    app.on_error([counter = &calls](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        ++*counter;
        c.draft().status(error.status());
        co_return c.text(error.code());
    });
    for (int i = 0; i < 10; ++i) {
        (void)app.request(ruvia::test_request::get("/route-config/limited"));
    }
    EXPECT_EQ(calls, 0);
    const auto rejected = app.request(ruvia::test_request::get("/route-config/limited"));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(rejected.status(), ruvia::http_status::too_many_requests);
    EXPECT_EQ(rejected.body(), std::string_view("too_many_requests"));
    EXPECT_TRUE(rejected.header("Retry-After").has_value());
    EXPECT_EQ(
        rejected.header("X-RateLimit-Limit").value_or(std::string_view{}), std::string_view("10"));
}

TEST(Ruvia, stacked_route_rate_limits_count_each_declaration_once_per_request) {
    ruvia::test_app app;
    for (const std::string_view target : {"/stacked-limit/same", "/stacked-limit/looser"}) {
        // The group rule allows four requests; a shared counter would be
        // consumed twice per request and reject the third.
        for (int i = 0; i < 4; ++i) {
            const auto admitted = app.request(ruvia::test_request::get(target));
            EXPECT_EQ(admitted.status(), ruvia::http_status::ok);
        }
        const auto rejected = app.request(ruvia::test_request::get(target));
        EXPECT_EQ(rejected.status(), ruvia::http_status::too_many_requests);
        EXPECT_EQ(
            rejected.header("X-RateLimit-Limit").value_or(std::string_view{}), std::string_view("4"));
    }
}

TEST(Ruvia, route_body_limit_is_declared_through_the_type) {
    ruvia::test_app app;

    // Within the route's ceiling.
    const auto small =
        app.request(ruvia::test_request::post("/route-config/small").body("0123456789"));
    EXPECT_EQ(small.status(), ruvia::http_status::ok);
    EXPECT_EQ(small.body(), std::string_view("0123456789"));

    // A sibling route without the declaration keeps the app-wide ceiling, so
    // the same body that the limited route would reject is fine here.
    const auto oversize_for_route = std::string(64, 'x');
    const auto unlimited =
        app.request(ruvia::test_request::post("/route-config/default").body(oversize_for_route));
    EXPECT_EQ(unlimited.status(), ruvia::http_status::ok);
    EXPECT_EQ(unlimited.body().size(), std::size_t{64});

    const auto rejected =
        app.request(ruvia::test_request::post("/route-config/small").body(oversize_for_route));
    EXPECT_EQ(rejected.status(), ruvia::http_status::content_too_large);
}

TEST(Ruvia, route_body_limit_bounds_the_decoded_representation) {
    ruvia::test_app app;
    const std::string plain(4096, 'x');
    auto encoded = ruvia::encode_http_content(
        ruvia::http_content_coding::gzip, plain, {.max_encoded_bytes_ = 64});
    EXPECT_TRUE(encoded.has_value());
    if (!encoded.has_value()) {
        return;
    }

    // The compressed body fits the route's ceiling on the wire, but the
    // route reads the decoded representation, which is far larger.
    const auto rejected = app.request(ruvia::test_request::post("/route-config/small-decoded")
            .header("Content-Encoding", "gzip")
            .body(*encoded));
    EXPECT_EQ(rejected.status(), ruvia::http_status::content_too_large);

    // A sibling without the declaration decodes up to the app-wide ceiling.
    const auto accepted = app.request(ruvia::test_request::post("/route-config/default")
            .header("Content-Encoding", "gzip")
            .body(*encoded));
    EXPECT_EQ(accepted.status(), ruvia::http_status::ok);
    EXPECT_EQ(accepted.body(), std::string_view(plain));
}

TEST(Ruvia, app_rejects_route_only_body_limit_middleware) {
    static_assert(!ruvia::middleware<ruvia::body_limit<16>> && ruvia::route_policy<ruvia::body_limit<16>> &&
                  !ruvia::middleware<ruvia::stream_body> && ruvia::route_policy<ruvia::stream_body>);
    ruvia::test_app app;
    bool body_limit_rejected = false;
    try {
        app.use<invalid_app_body_limit_middleware>();
    } catch (const std::invalid_argument&) {
        body_limit_rejected = true;
    }
    EXPECT_TRUE(body_limit_rejected);
}

TEST(Ruvia, route_middleware_list_mixes_bare_and_parameterized_types) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::post("/route-config/mixed").body("x"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.body(), std::string_view("mixed"));
    // The bare entry ran alongside the parameterized ones.
    EXPECT_EQ(response.header("X-Plain").value_or(std::string_view{}), std::string_view("on"));
    EXPECT_EQ(
        response.header("X-Config").value_or(std::string_view{}), std::string_view("two"));
}

TEST(Ruvia, route_middleware_returning_a_response_answers_without_the_handler) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::get("/route-config/answered"));
    EXPECT_EQ(response.status(), ruvia::http_status::accepted);
    EXPECT_EQ(response.body(), std::string_view("returned by middleware"));
}

TEST(Ruvia, test_app_releases_worker_middleware_after_startup_failure_and_normal_retirement) {
    const auto observation = std::make_shared<middleware_lifetime_observation>();
    {
        ruvia::test_app app;
        app.use<lifetime_middleware>(observation, false).use<lifetime_middleware>(observation, true);
        bool failed = false;
        try {
            (void)app.request(ruvia::test_request::get("/route-config/one"));
        } catch (const std::runtime_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(observation->constructed_, 1U);
        EXPECT_EQ(observation->destroyed_, 1U);
        EXPECT_EQ(observation->live_, 0U);
    }
    {
        ruvia::test_app app;
        app.use<lifetime_middleware>(observation, false);
        const auto response = app.request(ruvia::test_request::get("/route-config/one"));
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(observation->live_, 1U);
    }
    EXPECT_EQ(observation->constructed_, 2U);
    EXPECT_EQ(observation->destroyed_, 2U);
    EXPECT_EQ(observation->live_, 0U);
}

TEST(Ruvia, test_app_releases_registered_middleware_arguments_with_the_app) {
    const auto observation = std::make_shared<middleware_lifetime_observation>();
    {
        ruvia::test_app app;
        app.use<lifetime_middleware>(observation, false);
        app.use_at<lifetime_middleware>({.prefix_ = "/route-config"}, observation, false);
        // Each registration retains its own copy of the arguments.
        EXPECT_EQ(observation.use_count(), 3L);

        const auto response = app.request(ruvia::test_request::get("/route-config/one"));
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(observation->live_, 2U);

        // A registration the sealed app rejects does not keep its arguments.
        const auto running_count = observation.use_count();
        bool sealed = false;
        try {
            app.use<lifetime_middleware>(observation, false);
        } catch (const std::logic_error&) {
            sealed = true;
        }
        EXPECT_TRUE(sealed);
        EXPECT_EQ(observation.use_count(), running_count);
    }
    EXPECT_EQ(observation->live_, 0U);
    EXPECT_EQ(observation.use_count(), 1L);
}

TEST(Ruvia, app_wide_route_declarations_are_rejected_instead_of_ignored) {
    // Admission reads body_limit from route and controller lists only,
    // so an app-wide registration would silently bound nothing.
    ruvia::test_app app;
    const auto rejects = [](auto&& register_value) {
        try {
            register_value();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(rejects([&app] { app.use<invalid_app_body_limit_middleware>(); }));
    EXPECT_TRUE(rejects([&app] { app.use_at<invalid_app_body_limit_middleware>({.prefix_ = "/route-config"}); }));

    const auto response = app.request(ruvia::test_request::post("/route-config/default").body(std::string(64, 'x')));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
}

namespace ocsp_test {

using namespace std::chrono_literals;
using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;

struct runtime final {
    ruvia::blocking_pool pool_{{.thread_count_ = 1, .queue_capacity_ = 64}};
    asio::io_context first_io_, second_io_;
    ruvia::event_loop_attachment first_{ruvia::attach_event_loop(first_io_, {.blocking_pool_ = &pool_})};
    ruvia::event_loop_attachment second_{ruvia::attach_event_loop(second_io_, {.blocking_pool_ = &pool_})};
    std::array<ruvia::event_loop, 2> loops_{first_.loop(), second_.loop()};
    std::jthread first_thread_{[this] { first_.run(); }};
    std::jthread second_thread_{[this] { second_.run(); }};
    ~runtime() {
        first_.stop();
        second_.stop();
    }
};

struct issued_identity final {
    ruvia::test::scoped_temp_directory directory_{"ruvia-ocsp-tls"};
    X509* issuer_certificate_{};
    EVP_PKEY* issuer_key_{};
    std::unique_ptr<X509, decltype(&X509_free)> certificate_{nullptr, X509_free};
    std::filesystem::path certificate_file_;
    std::filesystem::path private_key_file_;

    issued_identity(ruvia::test::tls_identity& issuer, const char* common_name, long serial)
        : issuer_certificate_(SSL_CTX_get0_certificate(issuer.context_.native_handle())),
          issuer_key_(SSL_CTX_get0_privatekey(issuer.context_.native_handle())) {
        std::filesystem::permissions(directory_.path(), std::filesystem::perms::owner_all);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free);
        certificate_.reset(X509_new_ex(nullptr, nullptr));
        const std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
        if (!key || !certificate_ || !subject ||
            X509_set_version(certificate_.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate_.get()), serial) != 1 ||
            !X509_gmtime_adj(X509_getm_notBefore(certificate_.get()), -60) ||
            !X509_gmtime_adj(X509_getm_notAfter(certificate_.get()), 86400) ||
            X509_set_pubkey(certificate_.get(), key.get()) != 1 ||
            X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
            X509_set_subject_name(certificate_.get(), subject.get()) != 1 ||
            X509_set_issuer_name(certificate_.get(), X509_get_subject_name(issuer_certificate_)) != 1 ||
            ruvia::test::sign_tls_certificate(certificate_.get(), issuer_key_) <= 0) {
            throw std::runtime_error("cannot generate OCSP leaf identity");
        }
        certificate_file_ = directory_.path() / "cert.pem";
        private_key_file_ = directory_.path() / "key.pem";
        const std::unique_ptr<BIO, decltype(&BIO_free)> cert_out(
            BIO_new_file(certificate_file_.string().c_str(), "w"), BIO_free);
        const std::unique_ptr<BIO, decltype(&BIO_free)> key_out(
            BIO_new_file(private_key_file_.string().c_str(), "w"), BIO_free);
        if (!cert_out || PEM_write_bio_X509(cert_out.get(), certificate_.get()) != 1 ||
            !key_out || ruvia::test::write_tls_private_key(key_out.get(), key.get()) != 1) {
            throw std::runtime_error("cannot write OCSP leaf identity");
        }
    }
};

ruvia::tls_identity_config files(const issued_identity& identity) {
    return {.certificate_chain_file_ = identity.certificate_file_, .private_key_file_ = identity.private_key_file_};
}

// A real issuer-signed BasicOCSPResponse, with a nonce distinguishing renewals.
std::vector<std::byte> response(const issued_identity& identity, unsigned char nonce) {
    const std::unique_ptr<OCSP_CERTID, decltype(&OCSP_CERTID_free)> id(
        OCSP_cert_to_id(EVP_sha256(), identity.certificate_.get(), identity.issuer_certificate_), OCSP_CERTID_free);
    const std::unique_ptr<OCSP_BASICRESP, decltype(&OCSP_BASICRESP_free)> basic(OCSP_BASICRESP_new(), OCSP_BASICRESP_free);
    const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> current(X509_gmtime_adj(nullptr, -60), ASN1_TIME_free);
    const std::unique_ptr<ASN1_TIME, decltype(&ASN1_TIME_free)> next(X509_gmtime_adj(nullptr, 3600), ASN1_TIME_free);
    if (!id || !basic || !current || !next ||
        !OCSP_basic_add1_status(basic.get(), id.get(), V_OCSP_CERTSTATUS_GOOD, 0, nullptr, current.get(), next.get()) ||
        OCSP_basic_add1_nonce(basic.get(), &nonce, 1) != 1 ||
        OCSP_basic_sign(basic.get(), identity.issuer_certificate_, identity.issuer_key_, EVP_sha256(), nullptr, 0) != 1) {
        throw std::runtime_error("cannot sign OCSP test response");
    }
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> encoded(
        OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, basic.get()), OCSP_RESPONSE_free);
    const int size = encoded ? i2d_OCSP_RESPONSE(encoded.get(), nullptr) : -1;
    if (size <= 0) {
        throw std::runtime_error("cannot encode OCSP test response");
    }
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    auto* cursor = reinterpret_cast<unsigned char*>(result.data());
    if (i2d_OCSP_RESPONSE(encoded.get(), &cursor) != size) {
        throw std::runtime_error("cannot encode OCSP test response");
    }
    return result;
}

// Deadlines bound both TCP and OpenSSL's real client handshake; no fake peer.
template <typename initiation_type>
void client_io(asio::ip::tcp::socket& socket, initiation_type&& initiate) {
    auto& io = static_cast<asio::io_context&>(socket.get_executor().context());
    bool done = false;
    asio::error_code error;
    initiate([&](const asio::error_code& result) {
        error = result;
        done = true;
    });
    io.restart();
    io.run_for(3s);
    if (!done) {
        asio::error_code ignored;
        socket.close(ignored);
        io.restart();
        io.run();
        error = asio::error::timed_out;
    }
    if (error) {
        throw std::system_error(error, "OCSP test client I/O");
    }
}

std::unique_ptr<tls_stream> connect(asio::io_context& io, int version,
    const asio::ip::tcp::endpoint& endpoint, const char* name, bool request_status = true) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    if (SSL_CTX_set_min_proto_version(client.native_handle(), version) != 1 ||
        SSL_CTX_set_max_proto_version(client.native_handle(), version) != 1) {
        throw std::runtime_error("cannot configure OCSP test TLS version");
    }
    auto stream = std::make_unique<tls_stream>(io, client);
    if ((name && SSL_set_tlsext_host_name(stream->native_handle(), name) != 1) ||
        (request_status && SSL_set_tlsext_status_type(stream->native_handle(), TLSEXT_STATUSTYPE_ocsp) != 1)) {
        throw std::runtime_error("cannot request OCSP status");
    }
    client_io(stream->next_layer(), [&](auto completion) {
        stream->next_layer().async_connect(endpoint, std::move(completion));
    });
    client_io(stream->next_layer(), [&](auto completion) {
        stream->async_handshake(asio::ssl::stream_base::client, std::move(completion));
    });
    return stream;
}

std::vector<std::byte> received(SSL* ssl) {
    const unsigned char* bytes = nullptr;
    const auto size = SSL_get_tlsext_status_ocsp_resp(ssl, &bytes);
    if (size <= 0) {
        return std::vector<std::byte>();
    }
    const auto* begin = reinterpret_cast<const std::byte*>(bytes);
    return {begin, begin + size};
}

void expect_status(SSL* ssl, const issued_identity& identity,
    const std::vector<std::byte>& expected) {
    const auto bytes = received(ssl);
    ASSERT_EQ(bytes, expected);
    const std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get1_peer_certificate(ssl), X509_free);
    ASSERT_NE(peer, nullptr);
    EXPECT_EQ(X509_cmp(peer.get(), identity.certificate_.get()), 0);
    if (bytes.empty()) {
        return;
    }
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> parsed(
        d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(bytes.size())), OCSP_RESPONSE_free);
    ASSERT_NE(parsed, nullptr);
    ASSERT_EQ(OCSP_response_status(parsed.get()), OCSP_RESPONSE_STATUS_SUCCESSFUL);
    const std::unique_ptr<OCSP_BASICRESP, decltype(&OCSP_BASICRESP_free)> basic(
        OCSP_response_get1_basic(parsed.get()), OCSP_BASICRESP_free);
    const std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> trust(X509_STORE_new(), X509_STORE_free);
    ASSERT_NE(basic, nullptr);
    ASSERT_NE(trust, nullptr);
    ASSERT_EQ(X509_STORE_add_cert(trust.get(), identity.issuer_certificate_), 1);
    EXPECT_EQ(OCSP_basic_verify(basic.get(), nullptr, trust.get(), 0), 1);
    const std::unique_ptr<OCSP_CERTID, decltype(&OCSP_CERTID_free)> id(
        OCSP_cert_to_id(EVP_sha256(), peer.get(), identity.issuer_certificate_), OCSP_CERTID_free);
    ASSERT_NE(id, nullptr);
    int status = -1, reason = -1;
    ASN1_GENERALIZEDTIME *revoked = nullptr, *current = nullptr, *next = nullptr;
    ASSERT_EQ(OCSP_resp_find_status(basic.get(), id.get(), &status, &reason, &revoked, &current, &next), 1);
    EXPECT_EQ(status, V_OCSP_CERTSTATUS_GOOD);
}

}  // namespace ocsp_test

TEST(Ruvia, ocsp_stapling_selects_identity_and_replaces_certificate_status_pairs) {
    using namespace ocsp_test;
    ruvia::test::tls_identity issuer("ocsp-issuer-ca");
    issued_identity primary(issuer, "primary", 1), exact(issuer, "exact", 2), wildcard(issuer, "wildcard", 3), local(issuer, "local", 4);
    issued_identity renewed_primary(issuer, "renewed-primary", 5), renewed_exact(issuer, "renewed-exact", 6);
    runtime runtime;
    const auto caller = runtime.loops_[0];
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto port = reservation.endpoint().port();
    const asio::ip::tcp::endpoint first(asio::ip::make_address("127.0.0.1"), port);
    const asio::ip::tcp::endpoint second(asio::ip::make_address("127.0.0.2"), port);
    const ruvia::listener_endpoint listener{.address_ = "0.0.0.0", .port_ = port};
    const auto configuration = [&](bool renewed) {
        return ruvia::tls_config{
            .identity_ = files(renewed ? renewed_primary : primary),
            .ocsp_response_ = response(renewed ? renewed_primary : primary, renewed ? 11 : 1),
            .sni_ = {{.host_ = "api.example.test", .identity_ = files(renewed ? renewed_exact : exact), .ocsp_response_ = response(renewed ? renewed_exact : exact, renewed ? 12 : 2)},
                {.host_ = "*.example.test", .identity_ = files(wildcard), .ocsp_response_ = response(wildcard, renewed ? 13 : 3)},
                {.host_ = "empty.example.test", .identity_ = files(exact)}},
            .local_addresses_ = {{.address_ = "127.0.0.2", .identity_ = files(local), .ocsp_response_ = response(local, renewed ? 14 : 4)}}};
    };
    ruvia::http_server_group server(runtime.loops_, [](const ruvia::worker_ref&) {
        return ruvia::http_server_callbacks{.request_stream_ = [](ruvia::request_stream& stream) -> ruvia::task<void> {
            if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
                co_await stream.response().write("ocsp");
                co_await stream.response().end();
            }
        }};
    });
    const auto original = configuration(false);
    server.listen({.address_ = "0.0.0.0", .https_ = port, .tls_ = original, .dispatch_ = ruvia::listener_dispatch::request_stream});
    caller.start(server.start(caller.worker())).get();
    auto retained = connect(io, TLS1_3_VERSION, first, nullptr);
    expect_status(retained->native_handle(), primary, original.ocsp_response_);
    const auto check = [&](const ruvia::tls_config& config, bool renewed) {
        for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            SCOPED_TRACE(version);
            expect_status(connect(io, version, first, nullptr)->native_handle(), renewed ? renewed_primary : primary, config.ocsp_response_);
            expect_status(connect(io, version, first, "unknown.test")->native_handle(), renewed ? renewed_primary : primary, config.ocsp_response_);
            expect_status(connect(io, version, second, "API.example.TEST")->native_handle(), renewed ? renewed_exact : exact, config.sni_[0].ocsp_response_);
            expect_status(connect(io, version, first, "www.example.test")->native_handle(), wildcard, config.sni_[1].ocsp_response_);
            expect_status(connect(io, version, second, nullptr)->native_handle(), local, config.local_addresses_[0].ocsp_response_);
            expect_status(connect(io, version, second, "empty.example.test")->native_handle(), exact, config.sni_[2].ocsp_response_);
            EXPECT_TRUE(received(connect(io, version, first, nullptr, false)->native_handle()).empty());
        }
    };
    check(original, false);
    auto replacement = configuration(true);
    caller.start(server.replace_tls_identities(caller.worker(), listener, replacement)).get();
    check(replacement, true);
    expect_status(retained->native_handle(), primary, original.ocsp_response_);

    // Reject broken primary/SNI/address DER atomically, including trailing
    // bytes and BER encodings accepted by OpenSSL's permissive d2i parser.
    const std::array<std::vector<std::byte>, 3> invalid{
        std::vector<std::byte>{std::byte{0}},
        std::vector<std::byte>{std::byte{0x30}, std::byte{3}, std::byte{0x0a}, std::byte{1}, std::byte{3}, std::byte{0}},
        std::vector<std::byte>{std::byte{0x30}, std::byte{0x81}, std::byte{3}, std::byte{0x0a}, std::byte{1}, std::byte{3}}};
    for (const auto& bytes : invalid) {
        for (int entry = 0; entry != 3; ++entry) {
            auto rejected = replacement;
            auto& target = entry == 0 ? rejected.ocsp_response_ : entry == 1 ? rejected.sni_[0].ocsp_response_
                                                                             : rejected.local_addresses_[0].ocsp_response_;
            target = bytes;
            EXPECT_THROW(caller.start(server.replace_tls_identities(caller.worker(), listener, std::move(rejected))).get(), std::invalid_argument);
        }
    }
    auto library = std::make_shared<ruvia::tls_certificate_store>();
    ruvia::tls_config mixed{.ocsp_response_ = replacement.ocsp_response_, .certificate_store_ = library, .default_certificate_ = 1};
    EXPECT_THROW(caller.start(server.replace_tls_identities(caller.worker(), listener, std::move(mixed))).get(), std::invalid_argument);
    check(replacement, true);

    replacement.ocsp_response_.clear();
    replacement.sni_[0].ocsp_response_.clear();
    caller.start(server.replace_tls_identities(caller.worker(), listener, replacement)).get();
    check(replacement, true);
    retained.reset();
    caller.start(server.shutdown(caller.worker())).get();
}

namespace ruvia::test {

RUVIA_MODEL(accessor_surface_request, RUVIA_OPTIONAL_FIELD(message, ruvia::string));

RUVIA_MODEL(nested_model_item, RUVIA_REQUIRED_FIELD(id, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::string));

RUVIA_MODEL(nested_model_envelope, RUVIA_REQUIRED_FIELD(primary, nested_model_item),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<nested_model_item>),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

RUVIA_MODEL(nested_response_item, RUVIA_REQUIRED_FIELD(id, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::string));

RUVIA_MODEL(nested_response_envelope, RUVIA_REQUIRED_FIELD(primary, nested_response_item),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<nested_response_item>),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

#define RUVIA_TEST_BOOL_FIELD(field) RUVIA_OPTIONAL_FIELD(field, ruvia::bool_value)
RUVIA_MODEL(unlimited_field_count_response, RUVIA_TEST_BOOL_FIELD(f01),
    RUVIA_TEST_BOOL_FIELD(f02), RUVIA_TEST_BOOL_FIELD(f03), RUVIA_TEST_BOOL_FIELD(f04),
    RUVIA_TEST_BOOL_FIELD(f05), RUVIA_TEST_BOOL_FIELD(f06), RUVIA_TEST_BOOL_FIELD(f07),
    RUVIA_TEST_BOOL_FIELD(f08), RUVIA_TEST_BOOL_FIELD(f09), RUVIA_TEST_BOOL_FIELD(f10),
    RUVIA_TEST_BOOL_FIELD(f11), RUVIA_TEST_BOOL_FIELD(f12), RUVIA_TEST_BOOL_FIELD(f13),
    RUVIA_TEST_BOOL_FIELD(f14), RUVIA_TEST_BOOL_FIELD(f15), RUVIA_TEST_BOOL_FIELD(f16),
    RUVIA_TEST_BOOL_FIELD(f17), RUVIA_TEST_BOOL_FIELD(f18), RUVIA_TEST_BOOL_FIELD(f19),
    RUVIA_TEST_BOOL_FIELD(f20), RUVIA_TEST_BOOL_FIELD(f21), RUVIA_TEST_BOOL_FIELD(f22),
    RUVIA_TEST_BOOL_FIELD(f23), RUVIA_TEST_BOOL_FIELD(f24), RUVIA_TEST_BOOL_FIELD(f25),
    RUVIA_TEST_BOOL_FIELD(f26), RUVIA_TEST_BOOL_FIELD(f27), RUVIA_TEST_BOOL_FIELD(f28),
    RUVIA_TEST_BOOL_FIELD(f29), RUVIA_TEST_BOOL_FIELD(f30), RUVIA_TEST_BOOL_FIELD(f31),
    RUVIA_TEST_BOOL_FIELD(f32), RUVIA_TEST_BOOL_FIELD(f33), RUVIA_TEST_BOOL_FIELD(f34),
    RUVIA_TEST_BOOL_FIELD(f35), RUVIA_TEST_BOOL_FIELD(f36), RUVIA_TEST_BOOL_FIELD(f37),
    RUVIA_TEST_BOOL_FIELD(f38), RUVIA_TEST_BOOL_FIELD(f39), RUVIA_TEST_BOOL_FIELD(f40),
    RUVIA_TEST_BOOL_FIELD(f41), RUVIA_TEST_BOOL_FIELD(f42), RUVIA_TEST_BOOL_FIELD(f43),
    RUVIA_TEST_BOOL_FIELD(f44), RUVIA_TEST_BOOL_FIELD(f45), RUVIA_TEST_BOOL_FIELD(f46),
    RUVIA_TEST_BOOL_FIELD(f47), RUVIA_TEST_BOOL_FIELD(f48), RUVIA_TEST_BOOL_FIELD(f49),
    RUVIA_TEST_BOOL_FIELD(f50), RUVIA_TEST_BOOL_FIELD(f51), RUVIA_TEST_BOOL_FIELD(f52),
    RUVIA_TEST_BOOL_FIELD(f53), RUVIA_TEST_BOOL_FIELD(f54), RUVIA_TEST_BOOL_FIELD(f55),
    RUVIA_TEST_BOOL_FIELD(f56), RUVIA_TEST_BOOL_FIELD(f57), RUVIA_TEST_BOOL_FIELD(f58),
    RUVIA_TEST_BOOL_FIELD(f59), RUVIA_TEST_BOOL_FIELD(f60), RUVIA_TEST_BOOL_FIELD(f61),
    RUVIA_TEST_BOOL_FIELD(f62), RUVIA_TEST_BOOL_FIELD(f63), RUVIA_TEST_BOOL_FIELD(f64),
    RUVIA_TEST_BOOL_FIELD(f65));
#undef RUVIA_TEST_BOOL_FIELD

}  // namespace ruvia::test

namespace {

std::size_t default_evaluations = 0;
std::size_t predicate_evaluations = 0;
std::size_t nested_default_evaluations = 0;

std::string nested_default() {
    if (++nested_default_evaluations == 2) {
        throw std::runtime_error("array default failed");
    }
    return std::string(256, 'd');
}

int codec_default() {
    ++default_evaluations;
    return 7;
}

bool counted_predicate(const ruvia::string& value) {
    ++predicate_evaluations;
    return value.view() == "valid";
}

RUVIA_MODEL(codec_escaped_keys,
    RUVIA_REQUIRED_FIELD_NAME("caf\xC3\xA9", accented, ruvia::int32),
    RUVIA_REQUIRED_FIELD_NAME("tab\tkey", tabbed, ruvia::int32),
    RUVIA_REQUIRED_FIELD_NAME("ctl\x1F", control, ruvia::int32));

RUVIA_MODEL(codec_leaf,
    RUVIA_REQUIRED_FIELD_NAME("wire\"id", id, ruvia::uint64),
    RUVIA_OPTIONAL_FIELD(text, ruvia::string));
RUVIA_MODEL(codec_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<codec_node>));
RUVIA_MODEL(codec_envelope,
    RUVIA_REQUIRED_FIELD(single, codec_leaf),
    RUVIA_REQUIRED_FIELD(array, ruvia::array<codec_leaf>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<codec_leaf>),
    RUVIA_REQUIRED_FIELD(tree, codec_node));
RUVIA_MODEL(codec_presence,
    RUVIA_REQUIRED_FIELD(required, ruvia::string),
    RUVIA_OPTIONAL_FIELD(nullable, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(defaulted, ruvia::int32, RUVIA_DEFAULT(codec_default())),
    RUVIA_OPTIONAL_FIELD(emitted, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(codec_rules,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_CUSTOM(counted_predicate, "not valid")));
RUVIA_MODEL(codec_dynamic,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value),
    RUVIA_REQUIRED_FIELD(object, ruvia::json_object));
RUVIA_MODEL(codec_dynamic_sequences,
    RUVIA_REQUIRED_FIELD(dense, ruvia::array<ruvia::json_value>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<ruvia::json_value>));
RUVIA_MODEL(codec_empty);
RUVIA_MODEL(codec_root_nested,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(values, ruvia::array<ruvia::string>));
RUVIA_MODEL(codec_root_types,
    RUVIA_REQUIRED_FIELD(child, codec_root_nested),
    RUVIA_REQUIRED_FIELD(flag, ruvia::bool_value));
RUVIA_MODEL(codec_array_default,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT(nested_default())));

struct forward_branch;
RUVIA_MODEL(forward_root,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(branches, ruvia::array<forward_branch>));
RUVIA_MODEL(forward_branch,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(roots, ruvia::boxed_array<forward_root>));

}  // namespace

TEST(Ruvia, model_json_codec_supports_public_scalar_and_array_roots) {
    auto string_value = ruvia::from_json<ruvia::string>(R"("escaped\\\"text")");
    EXPECT_EQ(string_value.view(), std::string_view("escaped\\\"text"));
    auto integer = ruvia::from_json<ruvia::int64>("-9223372036854775808");
    EXPECT_EQ(integer.value_, INT64_MIN);
    EXPECT_EQ(std::string_view(ruvia::to_json(integer)), "-9223372036854775808");
    auto boolean = ruvia::from_json<ruvia::bool_value>("true");
    EXPECT_TRUE(boolean.value_);
    auto strings = ruvia::from_json<ruvia::array<ruvia::string>>(R"(["a","b"])");
    EXPECT_EQ(strings.size(), std::size_t{2});
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::boxed_array<ruvia::string>>(R"(["x",[1]])");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int64>("1 trailing"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::string>("null"); }));
}

TEST(Ruvia, model_json_scalar_roots_keep_short_output_in_inline_storage) {
    {
        const auto number = ruvia::to_json(ruvia::float_value(1.5f));
        const auto wide_number = ruvia::to_json(ruvia::double_value(2.25));
        const auto nonfinite = ruvia::to_json(ruvia::double_value(std::numeric_limits<double>::infinity()));
        const auto integer = ruvia::to_json(ruvia::int64(7));
        EXPECT_EQ(number, "1.5");
        EXPECT_EQ(wide_number, "2.25");
        EXPECT_EQ(nonfinite, "null");
        EXPECT_EQ(integer, "7");
    }
    {
        const auto encoded = ruvia::to_json(ruvia::double_value(std::numeric_limits<double>::max()));
        EXPECT_EQ(ruvia::from_json<ruvia::double_value>(encoded).value_, std::numeric_limits<double>::max());
    }
}

TEST(Ruvia, model_json_writer_replaces_ill_formed_utf8_with_replacement_characters) {
    // Each literal piece ends at a hex escape so the next letter is not read as a
    // further hex digit.
    std::string input("a\xD3\xC9");            // GBK lead/trail bytes: two maximal subparts
    input += "b\xE4\xBD";                      // truncated 3-byte sequence: one subpart
    input += "c\xC0\xAF";                      // overlong lead and stray continuation
    input += "d\xED\xA0\x80";                  // UTF-16 surrogate code point
    input += "e\xF4\x90\x80\x80";              // above U+10FFFF
    input += "f\xE4\xB8\xAD\xF0\x9F\x98\x80";  // well-formed U+4E2D and U+1F600
    input += "\"\n\xF0\x9F\x98";               // escapes, then a sequence cut at the end
    const std::string replacement("\xEF\xBF\xBD");
    std::string expected(std::string("\"a") + replacement + replacement);
    expected += std::string("b") + replacement;
    expected += std::string("c") + replacement + replacement;
    expected += std::string("d") + replacement + replacement + replacement;
    expected += std::string("e") + replacement + replacement + replacement + replacement;
    expected += "f\xE4\xB8\xAD\xF0\x9F\x98\x80";
    expected += std::string("\\\"\\n") + replacement + "\"";

    const auto output = ruvia::to_json(ruvia::string(input));
    EXPECT_EQ(std::string_view(output), std::string_view(expected));
    // The strict parser rejects ill-formed UTF-8, so a round trip proves the
    // output is well-formed.
    const auto reparsed = ruvia::from_json<ruvia::string>(output);
    EXPECT_TRUE(!reparsed.empty());

    ruvia::array<ruvia::string> nested;
    nested.emplace_back(std::string_view("\xFF"));
    EXPECT_EQ(std::string_view(ruvia::to_json(nested)), std::string_view(std::string("[\"") + replacement + "\"]"));
}

TEST(Ruvia, model_json_member_keys_escape_controls_and_keep_utf8) {
    codec_escaped_keys value;
    value.set<"accented">(1).set<"tabbed">(2).set<"control">(3);
    const auto output = ruvia::to_json(value);
    EXPECT_EQ(std::string_view(output),
        std::string_view("{\"caf\xC3\xA9\":1,\"tab\\tkey\":2,\"ctl\\u001f\":3}"));
    const auto reparsed = ruvia::from_json<codec_escaped_keys>(output);
    EXPECT_TRUE(reparsed == value);
}

TEST(Ruvia, model_json_codec_rejects_nested_wrong_null_and_duplicate_fields) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":null,"values":[]},"flag":true})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[1]},"flag":true})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[]},"flag":true,"flag":false})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[]},"flag":true}junk)");
    }));
}

TEST(Ruvia, model_json_codec_root_model_arrays_validate_and_roundtrip_recursively) {
    constexpr auto input = R"([{"name":"root","children":[{"name":"child"}]}])";
    auto dense = ruvia::from_json<ruvia::array<codec_node>>(input);
    auto boxed = ruvia::from_json<ruvia::boxed_array<codec_node>>(input);
    EXPECT_EQ(std::string_view(ruvia::to_json(dense)), input);
    EXPECT_EQ(std::string_view(ruvia::to_json(boxed)), input);
    for (const auto bad : {R"([{}])", R"([{"name":null}])",
             R"([{"name":"root","children":[{}]}])", R"([{"name":"a","name":"b"}])"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::array<codec_node>>(bad); }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::boxed_array<codec_node>>(bad); }));
    }
}

TEST(Ruvia, model_json_codec_forward_declared_graph_owns_nested_destination_values) {
    forward_root retained;
    constexpr std::string_view input =
        R"({"name":"root","branches":[{"name":"branch","roots":[{"name":"leaf"}]}]})";
    {
        auto parsed_value = ruvia::from_json<forward_root>(input);
        retained = std::move(parsed_value);
    }
    EXPECT_EQ(std::string_view(ruvia::to_json(retained)), input);
}

TEST(Ruvia, model_json_codec_array_default_exception_releases_previous_elements) {
    const std::string input("[{\"name\":\"" + std::string(256, 'a') +
                            "\"},{\"name\":\"" + std::string(256, 'b') + "\"}]");
    const auto check = [&]<typename array_t_type>() {
        nested_default_evaluations = 0;
        bool threw = false;
        try {
            (void)ruvia::from_json<array_t_type>(input);
        } catch (const std::runtime_error& error) {
            threw = std::string_view(error.what()) == "array default failed";
        }
        EXPECT_TRUE(threw);
        EXPECT_EQ(nested_default_evaluations, std::size_t{2});
    };
    check.template operator()<ruvia::array<codec_array_default>>();
    check.template operator()<ruvia::boxed_array<codec_array_default>>();
}

TEST(Ruvia, model_json_codec_owned_root_string_survives_input_destruction) {
    std::string input(R"("retained")");
    auto parsed_value = ruvia::from_json<ruvia::string>(input);
    input.assign(input.size(), 'x');
    EXPECT_EQ(parsed_value.view(), "retained");
}

TEST(Ruvia, model_json_codec_roundtrips_nested_and_recursive_models) {
    constexpr std::string_view input =
        R"({"single":{"wire\"id":18446744073709551615,"text":"a\\b\"c"},"array":[{"wire\"id":1}],"boxed":[{"wire\"id":2,"text":"two"}],"tree":{"name":"root","children":[{"name":"leaf","children":[{"name":"end"}]}]}})";
    auto parsed_value = ruvia::from_json<codec_envelope>(input);
    const auto& single = parsed_value.get<"single">();
    const auto& tree = parsed_value.get<"tree">();
    EXPECT_EQ(single.get<"id">().value_, std::uint64_t{18446744073709551615ULL});
    EXPECT_EQ(single.get<"text">()->view(), "a\\b\"c");
    EXPECT_EQ(parsed_value.get<"array">().size(), std::size_t{1});
    EXPECT_EQ(parsed_value.get<"boxed">().size(), std::size_t{1});
    EXPECT_EQ(tree.get<"children">()->front().get<"name">().view(), "leaf");

    const auto output = ruvia::to_json(parsed_value);
    EXPECT_EQ(std::string_view(output), input);
    auto reparsed = ruvia::from_json<codec_envelope>(output);
    EXPECT_EQ(reparsed.get<"single">().get<"id">().value_,
        std::uint64_t{18446744073709551615ULL});
    EXPECT_EQ(reparsed.get<"tree">().get<"children">()->front().get<"name">().view(),
        "leaf");
}

TEST(Ruvia, model_json_codec_handles_presence_defaults_and_serialization_options) {
    default_evaluations = 0;
    auto missing = ruvia::from_json<codec_presence>(R"({"required":"x"})");
    EXPECT_EQ(default_evaluations, std::size_t{1});
    EXPECT_TRUE(!missing.is_present<"defaulted">());
    EXPECT_TRUE(!missing.is_present<"nullable">());
    EXPECT_EQ(std::string_view(ruvia::to_json(missing)),
        std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));
    EXPECT_EQ(default_evaluations, std::size_t{1});

    const auto serialized = ruvia::to_json(missing);
    const std::string roundtrip_input(serialized);
    auto roundtrip = ruvia::from_json<codec_presence>(roundtrip_input);
    EXPECT_TRUE(roundtrip.is_present<"defaulted">());
    EXPECT_EQ(std::string_view(ruvia::to_json(roundtrip)),
        std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));

    auto values = ruvia::from_json<codec_presence>(
        R"({"required":"x","nullable":null,"empty":""})");
    EXPECT_TRUE(values.is_present<"nullable">() && values.is_null<"nullable">());
    EXPECT_EQ(std::string_view(ruvia::to_json(values)),
        std::string_view(R"({"required":"x","nullable":null,"defaulted":7,"emitted":null})"));
}

TEST(Ruvia, model_json_codec_validates_on_parse_and_serializes_without_running_validation_rules) {
    predicate_evaluations = 0;
    bool rule_rejected = false;
    try {
        (void)ruvia::from_json<codec_rules>(R"({"value":"invalid"})");
    } catch (const ruvia::http_error&) {
        rule_rejected = true;
    }
    EXPECT_TRUE(rule_rejected);
    EXPECT_EQ(predicate_evaluations, std::size_t{1});

    auto parsed_value = ruvia::from_json<codec_rules>(R"({"value":"valid"})");
    EXPECT_EQ(predicate_evaluations, std::size_t{2});
    parsed_value.set<"value">("invalid");
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed_value)), R"({"value":"invalid"})");
    EXPECT_EQ(predicate_evaluations, std::size_t{2});
}

TEST(Ruvia, model_json_codec_preserves_default_null_dynamic_array_elements) {
    const auto check = [](
                           std::initializer_list<std::string_view> tokens, std::string_view expected_array) {
        ruvia::array<ruvia::json_value> dense;
        ruvia::boxed_array<ruvia::json_value> boxed;
        for (const auto token : tokens) {
            if (token == "null") {
                dense.emplace_back();
                boxed.emplace_back();
            } else {
                const auto scalar = ruvia::json_value::parse(token);
                dense.push_back(scalar);
                boxed.push_back(scalar);
            }
        }
        codec_dynamic_sequences value;
        value.set<"dense">(std::move(dense));
        value.set<"boxed">(std::move(boxed));
        const auto output = ruvia::to_json(value);
        const auto expected = "{\"dense\":" + std::string(expected_array) +
                              ",\"boxed\":" + std::string(expected_array) + "}";
        EXPECT_EQ(std::string_view(output), std::string_view(expected));
        const auto reparsed = ruvia::from_json<codec_dynamic_sequences>(output);
        EXPECT_EQ(reparsed.get<"dense">().size(), tokens.size());
        EXPECT_EQ(reparsed.get<"boxed">().size(), tokens.size());
        if (reparsed.get<"dense">().size() != tokens.size() ||
            reparsed.get<"boxed">().size() != tokens.size()) {
            return;
        }
        std::size_t index = 0;
        for (const auto token : tokens) {
            EXPECT_EQ(reparsed.get<"dense">()[index].view(), token);
            EXPECT_EQ(reparsed.get<"boxed">()[index].view(), token);
            ++index;
        }
    };
    check({"null"}, "[null]");
    check({"null", "null", "null"}, "[null,null,null]");
    check({"null", "true", "null", R"("text")"}, R"([null,true,null,"text"])");
}

TEST(Ruvia, model_json_codec_roundtrips_default_dynamic_objects) {
    auto scalar = ruvia::json_value::parse("true");
    codec_dynamic value;
    value.set<"value">(std::move(scalar));
    value.set<"object">(ruvia::json_object{});
    const auto output = ruvia::to_json(value);
    EXPECT_EQ(std::string_view(output), R"({"value":true,"object":{}})");
    const auto reparsed = ruvia::from_json<codec_dynamic>(output);
    EXPECT_EQ(reparsed.get<"object">().view(), std::string_view("{}"));
    EXPECT_EQ(std::string_view(ruvia::to_json(reparsed)), std::string_view(output));
}

TEST(Ruvia, model_json_codec_owns_dynamic_tokens_and_preserves_retained_results) {
    {
        codec_dynamic retained;
        std::string retained_json;
        std::string input(R"({"value":{"keep":")");
        input.append(256, 'q');
        input += R"("},"object":{"n":1}})";
        auto parsed_value = ruvia::from_json<codec_dynamic>(input);
        retained = std::move(parsed_value);
        input.assign(input.size(), 'x');
        std::string expected_token(R"({"keep":")");
        expected_token.append(256, 'q');
        expected_token += R"("})";
        EXPECT_EQ(retained.get<"value">().view(), expected_token);
        retained_json = ruvia::to_json(retained);
        const auto saved = std::string(retained_json);
        const std::string temporary_input("{\"value\":\"" + std::string(160, 't') +
                                          "\",\"object\":{\"later\":true}}");
        for (std::size_t i = 0; i < 32; ++i) {
            {
                auto temporary = ruvia::from_json<codec_dynamic>(temporary_input);
                const auto output = ruvia::to_json(temporary);
                EXPECT_EQ(std::string_view(output), temporary_input);
            }
            EXPECT_EQ(retained.get<"value">().view(), expected_token);
            EXPECT_EQ(std::string_view(retained_json), saved);
        }
        const std::string invalid_input("{\"value\":\"" + std::string(160, 'i') + "\",\"object\":[]}");
        for (std::size_t i = 0; i < 32; ++i) {
            EXPECT_TRUE(ruvia::test::throws_on([&] {
                (void)ruvia::from_json<codec_dynamic>(invalid_input);
            }));
        }
    }
}

TEST(Ruvia, model_json_codec_roundtrips_empty_schema_and_rejects_non_object_root) {
    auto parsed_value = ruvia::from_json<codec_empty>(R"({})");
    const auto output = ruvia::to_json(parsed_value);
    EXPECT_EQ(std::string_view(output), "{}");
    auto reparsed = ruvia::from_json<codec_empty>(output);
    EXPECT_EQ(std::string_view(ruvia::to_json(reparsed)), "{}");
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<codec_empty>(R"([])"); }));
}

TEST(Ruvia, json_array_limits_apply_across_nested_collections_and_model_fields) {
    using nested_array = ruvia::array<ruvia::array<ruvia::int32>>;
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[[1,2],[3]]", {.max_array_elements_ = 5});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[[1,2],[3]]", {.max_array_elements_ = 4});
    }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[]", {.max_array_elements_ = 0});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[[]]", {.max_array_elements_ = 0});
    }));

    constexpr auto tree = R"({"name":"a","children":[{"name":"b","children":[{"name":"c"}]},{"name":"d"}]})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_node>(tree, {.max_array_elements_ = 3});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_node>(tree, {.max_array_elements_ = 2});
    }));
}

TEST(Ruvia, json_array_limits_include_dynamic_tokens_and_unknown_fields) {
    constexpr auto nested = R"({"value":[[1,2],[3]],"object":{}})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(nested, {.max_array_elements_ = 5});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(nested, {.max_array_elements_ = 4});
    }));

    constexpr auto dynamic = R"({"value":[1,2],"object":{"nested":[3]}})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(dynamic, {.max_array_elements_ = 3});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(dynamic, {.max_array_elements_ = 2});
    }));

    constexpr auto mixed = R"({"dense":[[1,2]],"boxed":[{"nested":[3]}]})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic_sequences>(mixed, {.max_array_elements_ = 5});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic_sequences>(mixed, {.max_array_elements_ = 4});
    }));

    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_empty>(R"({"unknown":[1,2]})", {.max_array_elements_ = 2});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_empty>(R"({"unknown":[1,2]})", {.max_array_elements_ = 1});
    }));
}

TEST(Ruvia, json_representation_budget_bounds_scalar_and_owned_string_results) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::int32>("1", {.max_representation_bytes_ = 0});
    }));

    const std::string text("\"" + std::string(4096, 'x') + "\"");
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = 1024});
    }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = 16384});
    }));

    // A literal string is retained as one sized copy and is charged once;
    // decoding an escaped string grows its buffer, so it is charged twice.
    constexpr std::size_t single_copy_budget = 4096 + 1024;
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = single_copy_budget});
    }));
    const std::string escaped("\"\\n" + std::string(4094, 'x') + "\"");
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(escaped, {.max_representation_bytes_ = single_copy_budget});
    }));
}

TEST(Ruvia, json_representation_budget_includes_decoded_field_names) {
    const std::string key(R"(\u0075)" + std::string(2048, 'x'));
    const std::string member(std::string("\"") + key + "\":0");
    const std::string single(std::string("{") + member + "}");
    const std::string repeated(std::string("{") + member + "," + member + "}");
    const auto one_key_budget = sizeof(codec_empty) + key.size() + 128;
    {
        auto parsed_value = ruvia::from_json<codec_empty>(single,
            {.max_representation_bytes_ = one_key_budget});
        EXPECT_EQ(std::string_view(ruvia::to_json(parsed_value)), "{}");
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)ruvia::from_json<codec_empty>(single,
                {.max_representation_bytes_ = sizeof(codec_empty) + 1024});
        }));
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)ruvia::from_json<codec_empty>(repeated,
                {.max_representation_bytes_ = one_key_budget});
        }));
    }
}

TEST(Ruvia, json_budget_failure_releases_partial_results_and_preserves_retained_values) {
    {
        auto retained = ruvia::from_json<ruvia::array<ruvia::string>>(
            R"(["retained value with owned storage"])");
        for (std::size_t index = 0; index < 8; ++index) {
            EXPECT_TRUE(ruvia::test::throws_on([&] {
                (void)ruvia::from_json<ruvia::boxed_array<ruvia::string>>(
                    R"(["first element with owned storage","second element with owned storage"])",
                    {.max_array_elements_ = 1});
            }));
            EXPECT_EQ(retained[0].view(), std::string_view("retained value with owned storage"));
        }
    }
}

TEST(Ruvia, model_json_codec_validates_field_rules_on_root_and_nested_model_arrays) {
    predicate_evaluations = 0;
    const auto valid_dense = ruvia::from_json<ruvia::array<codec_rules>>(R"([{"value":"valid"}])");
    EXPECT_EQ(valid_dense.size(), std::size_t{1});
    EXPECT_EQ(predicate_evaluations, std::size_t{1});

    const auto valid_boxed = ruvia::from_json<ruvia::boxed_array<codec_rules>>(R"([{"value":"valid"}])");
    EXPECT_EQ(valid_boxed.size(), std::size_t{1});
    EXPECT_EQ(predicate_evaluations, std::size_t{2});

    for (const bool boxed : {false, true}) {
        bool rejected = false;
        try {
            if (boxed) {
                (void)ruvia::from_json<ruvia::boxed_array<codec_rules>>(
                    R"([{"value":"valid"},{"value":"invalid"}])");
            } else {
                (void)ruvia::from_json<ruvia::array<codec_rules>>(
                    R"([{"value":"valid"},{"value":"invalid"}])");
            }
        } catch (const ruvia::http_error& error) {
            rejected = true;
            EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
            if (error.validation_issues().size() == 1) {
                EXPECT_EQ(error.validation_issues()[0].field(), std::string_view("[1].value"));
                EXPECT_EQ(error.validation_issues()[0].code(), std::string_view("custom"));
                EXPECT_EQ(error.validation_issues()[0].message(), std::string_view("not valid"));
            }
        }
        EXPECT_TRUE(rejected);
    }

    bool nested_rejected = false;
    try {
        (void)ruvia::from_json<ruvia::array<ruvia::array<codec_rules>>>(
            R"([[{"value":"valid"}],[{"value":"bad"}]])");
    } catch (const ruvia::http_error& error) {
        nested_rejected = true;
        EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
        if (error.validation_issues().size() == 1) {
            EXPECT_EQ(error.validation_issues()[0].field(), std::string_view("[1][0].value"));
            EXPECT_EQ(error.validation_issues()[0].code(), std::string_view("custom"));
        }
    }
    EXPECT_TRUE(nested_rejected);
}

namespace {

std::size_t evaluations{0};

int counted_default() {
    ++default_evaluations;
    return 11;
}

int initial_number() {
    ++evaluations;
    return 7;
}

std::string_view initial_name() {
    ++evaluations;
    return "seed";
}

int failing_initial() {
    ++evaluations;
    throw std::runtime_error("initial evaluation failed");
}

RUVIA_MODEL(initial_model,
    RUVIA_REQUIRED_FIELD(required, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(number, ruvia::int32, RUVIA_INITIAL(initial_number()),
        RUVIA_DEFAULT(9)),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string, RUVIA_INITIAL(initial_name())));

RUVIA_MODEL(failing_initial_model,
    RUVIA_OPTIONAL_FIELD(name, ruvia::string, RUVIA_INITIAL(std::string(128, 'n'))),
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_INITIAL(failing_initial())));

RUVIA_MODEL(required_initial_model,
    RUVIA_REQUIRED_FIELD(value, ruvia::int32, RUVIA_INITIAL(initial_number())));

RUVIA_MODEL(null_initial_model,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE, RUVIA_INITIAL(nullptr)));

RUVIA_MODEL(nested_initial_model,
    RUVIA_OPTIONAL_FIELD(child, initial_model),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<initial_model>));

RUVIA_MODEL(counted_lifecycle,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_INITIAL(initial_number()),
        RUVIA_DEFAULT(counted_default())));

constexpr std::size_t initial_payload_size = 4096;
std::size_t payload_evaluations{0};

std::string initial_payload() {
    ++payload_evaluations;
    return std::string(initial_payload_size, 'p');
}

RUVIA_MODEL(payload_initial_model,
    RUVIA_OPTIONAL_FIELD(payload, ruvia::string, RUVIA_INITIAL(initial_payload())));

}  // namespace

TEST(Ruvia, model_initial_values_only_run_for_explicit_construction) {
    evaluations = 0;
    initial_model model;
    EXPECT_EQ(evaluations, std::size_t{2});
    EXPECT_EQ(model.get<"number">()->value_, 7);
    EXPECT_EQ(model.get<"name">()->view(), std::string_view("seed"));
    EXPECT_TRUE(!model.is_present<"number">());
    EXPECT_TRUE(!model.is_present<"name">());

    auto parsed_value = ruvia::from_json<initial_model>(R"({"required":1})");
    EXPECT_TRUE(parsed_value.get<"number">()->value_ == 9);
    EXPECT_TRUE(!parsed_value.get<"name">());
    EXPECT_TRUE(!parsed_value.is_present<"number">());
    EXPECT_TRUE(!parsed_value.is_null<"name">());
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_initial_values_do_not_recur_through_moves_or_rebind) {
    evaluations = 0;
    initial_model retained;
    EXPECT_EQ(evaluations, std::size_t{2});
    auto parsed_value = ruvia::from_json<initial_model>(R"({"required":1,"number":3})");
    EXPECT_EQ(evaluations, std::size_t{2});
    retained = std::move(parsed_value);
    EXPECT_EQ(evaluations, std::size_t{2});
    EXPECT_EQ(retained.get<"number">()->value_, 3);
}

TEST(Ruvia, model_initial_exception_releases_partial_values) {
    evaluations = 0;
    bool failed = false;
    try {
        failing_initial_model model;
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()) == "initial evaluation failed";
    }
    EXPECT_TRUE(failed);
}

TEST(Ruvia, model_null_initial_is_a_value_state_without_allocation) {
    null_initial_model model;
    EXPECT_TRUE(model.is_null<"value">());
}

TEST(Ruvia, model_required_initial_is_not_used_by_parsing) {
    evaluations = 0;
    EXPECT_EQ(evaluations, std::size_t{0});
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_initial_model>("{}"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<required_initial_model>(""); }));
    EXPECT_EQ(evaluations, std::size_t{0});
}

TEST(Ruvia, model_initial_is_only_for_explicit_nested_construction) {
    evaluations = 0;
    nested_initial_model model;
    EXPECT_EQ(evaluations, std::size_t{0});
    (void)model.ensure<"child">();
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_initial_does_not_recur_in_nested_move_or_array_emplace) {
    evaluations = 0;
    initial_model child_value;
    nested_initial_model parent_value;
    parent_value.set<"child">(std::move(child_value));
    EXPECT_EQ(evaluations, std::size_t{2});

    initial_model array_child;
    parent_value.ensure<"children">().emplace_back(std::move(array_child));
    EXPECT_EQ(evaluations, std::size_t{4});
    EXPECT_EQ(parent_value.get<"children">()->size(), std::size_t{1});
}

TEST(Ruvia, model_initial_and_default_modes_are_evaluated_once_across_codecs_and_moves) {
    evaluations = 0;
    default_evaluations = 0;
    counted_lifecycle explicit_value;
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(default_evaluations, std::size_t{0});

    auto json = ruvia::from_json<counted_lifecycle>("{}");
    auto form = ruvia::from_form<counted_lifecycle>("");
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(default_evaluations, std::size_t{2});
    counted_lifecycle moved(std::move(json));
    EXPECT_TRUE(!json.get<"value">());
    EXPECT_EQ(moved.get<"value">()->value_, 11);
    EXPECT_EQ(form.get<"value">()->value_, 11);
    EXPECT_TRUE(!moved.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(moved)), std::string_view(R"({"value":11})"));
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(default_evaluations, std::size_t{2});
}

TEST(Ruvia, model_initial_fresh_container_elements_are_built_once) {
    payload_evaluations = 0;
    ruvia::array<payload_initial_model> dense;
    ruvia::boxed_array<payload_initial_model> boxed;
    dense.reserve(4);
    boxed.reserve(4);
    for (std::size_t index = 0; index != 4; ++index) {
        (void)dense.emplace_back();
        (void)boxed.emplace_back();
    }
    EXPECT_EQ(payload_evaluations, std::size_t{8});
    dense.resize(6);
    boxed.resize(6);
    EXPECT_EQ(payload_evaluations, std::size_t{12});
    for (const auto& value : dense) {
        EXPECT_EQ(value.get<"payload">()->size(), initial_payload_size);
    }
    for (const auto& value : boxed) {
        EXPECT_EQ(value.get<"payload">()->size(), initial_payload_size);
    }
}

RUVIA_MODEL(json_object_child, RUVIA_REQUIRED_FIELD(name, ruvia::string));
RUVIA_MODEL(json_object_envelope,
    RUVIA_REQUIRED_FIELD(child, json_object_child),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<json_object_child>));
RUVIA_MODEL(json_token_holder,
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<ruvia::json_value>));

RUVIA_MODEL(json_validated_child,
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(3, "name too short")));

// get<T>() may borrow the token's bytes, so only lvalue tokens offer it.
template <typename token_type>
concept token_offers_get = requires(token_type&& token) {
    std::forward<token_type>(token).template get<ruvia::string>();
};
template <typename token_type>
concept token_offers_field_get = requires(token_type&& token) {
    std::forward<token_type>(token).template get<ruvia::string>("name");
};
static_assert(token_offers_get<const ruvia::json_value&>);
static_assert(!token_offers_get<ruvia::json_value>);
static_assert(!token_offers_get<const ruvia::json_value>);
static_assert(token_offers_field_get<const ruvia::json_value&>);
static_assert(!token_offers_field_get<ruvia::json_value>);
static_assert(token_offers_field_get<const ruvia::json_object&>);
static_assert(!token_offers_field_get<ruvia::json_object>);
static_assert(!token_offers_field_get<const ruvia::json_object>);

// A lookup hands its value out by reference only from an lvalue and by move
// from a mutable rvalue, so no reference into a temporary lookup escapes.
using string_lookup = ruvia::json_field_lookup<ruvia::string>;
template <typename lookup_type>
concept lookup_offers_value = requires(lookup_type&& lookup) { std::forward<lookup_type>(lookup).value(); };
static_assert(std::is_same_v<decltype(std::declval<const string_lookup&>().value()), const std::optional<ruvia::string>&>);
static_assert(std::is_same_v<decltype(std::declval<string_lookup&>().value()), std::optional<ruvia::string>&>);
static_assert(std::is_same_v<decltype(std::declval<string_lookup>().value()), std::optional<ruvia::string>>);
static_assert(!lookup_offers_value<const string_lookup>);

using lookup_status = ruvia::unique_lookup_status;

TEST(Ruvia, json_token_moves_preserve_complete_tokens_and_reset_sources) {
    const std::string input("{\"payload\":\"" + std::string(160, 'x') + "\"}");
    const auto check = [&]<typename token_type>(std::string_view empty) {
        {
            for (const bool owning : {false, true}) {
                auto source = token_type::parse(input);
                if (owning) {
                    token_type owned;
                    owned = std::move(source);
                    source = std::move(owned);
                }
                token_type moved(std::move(source));
                EXPECT_EQ(moved.view(), std::string_view(input));
                EXPECT_EQ(source.view(), empty);
                token_type target;
                target = std::move(moved);
                EXPECT_EQ(target.view(), std::string_view(input));
                EXPECT_EQ(moved.view(), empty);
                auto* const self = &target;
                target = std::move(*self);
                EXPECT_EQ(target.view(), std::string_view(input));
                source = std::move(target);
                EXPECT_EQ(source.view(), std::string_view(input));
                EXPECT_EQ(target.view(), empty);
            }
        }
    };
    check.template operator()<ruvia::json_value>("null");
    check.template operator()<ruvia::json_object>("{}");
}

TEST(Ruvia, json_token_field_transfers_leave_the_source_model_serializable) {
    auto source = ruvia::from_json<json_token_holder>(R"({"object":{"keep":true},"items":[[1,2]]})");
    ruvia::json_object object(std::move(source.ensure<"object">()));
    EXPECT_EQ(object.view(), std::string_view(R"({"keep":true})"));
    EXPECT_EQ(ruvia::to_json(source), R"({"object":{},"items":[[1,2]]})");
    json_token_holder target;
    target.set<"object">(std::move(object));
    EXPECT_EQ(object.view(), std::string_view("{}"));
    auto& source_items = source.ensure<"items">();
    target.ensure<"items">().push_back(std::move(source_items[0]));
    EXPECT_TRUE(source_items[0].is_null());
    EXPECT_EQ(source_items[0].view(), std::string_view("null"));
    EXPECT_EQ(ruvia::to_json(source), R"({"object":{},"items":[null]})");
    EXPECT_EQ(ruvia::to_json(target), R"({"object":{"keep":true},"items":[[1,2]]})");
    target.set<"object">(std::move(target.ensure<"object">()));
    EXPECT_EQ(ruvia::to_json(target), R"({"object":{"keep":true},"items":[[1,2]]})");
}

TEST(Ruvia, json_value_parse_classifies_kinds_and_rejects_trailing_input) {
    const auto object = ruvia::json_value::parse(R"({"name":"ada"})");
    EXPECT_TRUE(object.is_object());
    EXPECT_TRUE(!object.is_array());
    EXPECT_TRUE(!object.is_null());
    EXPECT_EQ(object.view(), std::string_view(R"({"name":"ada"})"));

    const auto array_value = ruvia::json_value::parse("[1,2]");
    EXPECT_TRUE(array_value.is_array());

    const auto null_value = ruvia::json_value::parse("null");
    EXPECT_TRUE(null_value.is_null());

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(R"({"name":"ada"} trailing)"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("{"); }));
}

TEST(Ruvia, json_object_parse_requires_a_complete_object) {
    const auto json = ruvia::json_object::parse(R"({"name":"ada"})");
    EXPECT_EQ(json.view(), std::string_view(R"({"name":"ada"})"));

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse("[1,2]"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse("null"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(R"({"name":"ada"} 1)"); }));
}

TEST(Ruvia, json_object_default_represents_an_empty_object) {
    const ruvia::json_object object;
    EXPECT_EQ(object.view(), std::string_view("{}"));
    const auto parsed = ruvia::json_object::parse(object.view());
    EXPECT_TRUE(object == parsed);
    EXPECT_TRUE(object.get<ruvia::string>("missing").status() == lookup_status::absent);
    std::size_t fields = 0;
    EXPECT_TRUE(object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        ++fields;
        return true;
    }));
    EXPECT_EQ(fields, std::size_t{0});
}

TEST(Ruvia, json_object_get_reports_a_repeated_field_name_without_a_value) {
    const auto json = ruvia::json_object::parse(R"({"name":"first","other":"x","name":"second"})");
    const auto name = json.get<ruvia::string>("name");
    EXPECT_TRUE(name.status() == lookup_status::repeated);
    EXPECT_TRUE(!name.value().has_value());
    const auto token = json.get<ruvia::json_value>("name");
    EXPECT_TRUE(token.status() == lookup_status::repeated);
    EXPECT_TRUE(!token.value().has_value());
    const auto other = json.get<ruvia::string>("other");
    EXPECT_TRUE(other.status() == lookup_status::found);
    EXPECT_TRUE(other.value().has_value());
    EXPECT_EQ(other.value()->view(), std::string_view("x"));
    EXPECT_TRUE(json.get<ruvia::string>("missing").status() == lookup_status::absent);

    // Repetition is decided on decoded names and regardless of which
    // occurrence would convert to the requested type.
    const auto escaped = ruvia::json_object::parse(R"({"na\u006de":"first","name":"second"})");
    EXPECT_TRUE(escaped.get<ruvia::string>("name").status() == lookup_status::repeated);
    const auto mixed = ruvia::json_object::parse(R"({"count":"text","count":2})");
    const auto count = mixed.get<ruvia::int32>("count");
    EXPECT_TRUE(count.status() == lookup_status::repeated);
    EXPECT_TRUE(!count.value().has_value());
    const auto children = ruvia::json_object::parse(R"({"child":{"name":"ada"},"child":{"name":"bob"}})");
    EXPECT_TRUE(children.get<json_object_child>("child").status() == lookup_status::repeated);
    // A third occurrence does not change the answer.
    const auto triple = ruvia::json_object::parse(R"({"k":1,"k":2,"k":3})");
    EXPECT_TRUE(triple.get<ruvia::int32>("k").status() == lookup_status::repeated);

    const auto value = ruvia::json_value::parse(R"({"name":"first","name":"second","id":7})");
    EXPECT_TRUE(value.get<ruvia::string>("name").status() == lookup_status::repeated);
    const auto id = value.get<ruvia::int32>("id").value();
    EXPECT_TRUE(id.has_value());
    EXPECT_EQ(static_cast<std::int32_t>(*id), 7);
}

TEST(Ruvia, json_value_dynamic_traversal_decodes_keys_and_preserves_order) {
    auto json = ruvia::json_value::parse(R"({"a\u0062":1,"a\u0062":2,"nested":[3,4]})");
    int count = 0;
    EXPECT_TRUE(json.for_each_field([&](std::string_view key, const ruvia::json_value& value) {
        ++count;
        if (count < 3) {
            EXPECT_EQ(key, std::string_view("ab"));
            EXPECT_TRUE(value.is_number());
        } else {
            EXPECT_EQ(key, std::string_view("nested"));
            EXPECT_TRUE(value.is_array());
        }
        return true;
    }));
    EXPECT_EQ(count, 3);
}

TEST(Ruvia, json_value_dynamic_traversal_handles_whitespace_and_early_stop) {
    auto array_value = ruvia::json_value::parse("[1, \n 2,\t3]");
    int seen = 0;
    EXPECT_TRUE(array_value.for_each_element([&](const ruvia::json_value& value) {
        ++seen;
        EXPECT_TRUE(value.is_number());
        return true;
    }));
    EXPECT_EQ(seen, 3);

    auto object = ruvia::json_value::parse("{\"plain\": 1, \n \"escaped\\u03bb\": 2}");
    EXPECT_TRUE(object.for_each_field([](std::string_view key, const ruvia::json_value&) {
        return key == "plain" || key == "escapedλ";
    }));

    int stopped = 0;
    EXPECT_TRUE(!array_value.for_each_element([&](const ruvia::json_value&) {
        ++stopped;
        return false;
    }));
    EXPECT_EQ(stopped, 1);
}

TEST(Ruvia, json_value_dynamic_traversal_handles_empty_wrong_kind_and_early_stop) {
    auto empty = ruvia::json_value::parse("[]");
    auto object = ruvia::json_value::parse("{}");
    auto scalar = ruvia::json_value::parse("1");
    EXPECT_TRUE(empty.for_each_element([](const ruvia::json_value&) { return true; }));
    EXPECT_TRUE(object.for_each_field([](std::string_view, const ruvia::json_value&) { return true; }));
    EXPECT_TRUE(!scalar.for_each_element([](const ruvia::json_value&) { return true; }));

    auto array_value = ruvia::json_value::parse("[1,2,3]");
    int seen = 0;
    EXPECT_TRUE(!array_value.for_each_element([&](const ruvia::json_value&) {
        ++seen;
        return false;
    }));
    EXPECT_EQ(seen, 1);
}

TEST(Ruvia, json_value_object_traversal_reports_completion_and_stops_once) {
    const auto empty = ruvia::json_value::parse("{}");
    const auto object = ruvia::json_value::parse(R"({"a":1,"b":2,"c":3})");
    int calls = 0;
    const auto stop = [&](std::string_view, const ruvia::json_value&) {
        ++calls;
        return false;
    };
    EXPECT_TRUE(empty.for_each_field(stop));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(!object.for_each_field(stop));
    EXPECT_EQ(calls, 1);
    calls = 0;
    EXPECT_TRUE(!object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        return ++calls < 2;
    }));
    EXPECT_EQ(calls, 2);
    calls = 0;
    EXPECT_TRUE(object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        ++calls;
        return true;
    }));
    EXPECT_EQ(calls, 3);
}

TEST(Ruvia, json_value_dynamic_traversal_propagates_callback_exceptions_and_compares_tokens) {
    auto first = ruvia::json_value::parse("[1]");
    auto second = ruvia::json_value::parse("[1]");
    auto different = ruvia::json_value::parse("[1.0]");
    EXPECT_TRUE(first == second);
    EXPECT_TRUE(!(first == different));
    bool propagated = false;
    try {
        (void)first.for_each_element([](const ruvia::json_value&) -> bool {
            throw std::runtime_error("stop");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    EXPECT_TRUE(propagated);
}

TEST(Ruvia, json_value_get_reads_root_scalars_and_arrays_and_models) {
    auto string_value = ruvia::json_value::parse("\"hello\"");
    auto boolean = ruvia::json_value::parse("true");
    EXPECT_EQ(string_value.get<ruvia::string>()->view(), std::string_view("hello"));
    EXPECT_TRUE(*boolean.get<ruvia::bool_value>());

    auto valid_model = ruvia::json_value::parse(R"({"child":{"name":"ada"},"items":[{"name":"grace"}]})");
    EXPECT_TRUE(valid_model.get<json_object_envelope>().has_value());
    auto missing_required = ruvia::json_value::parse(R"({"child":{}})");
    EXPECT_TRUE(!missing_required.get<json_object_envelope>().has_value());
    auto invalid_nested_array = ruvia::json_value::parse(R"({"child":{"name":"ada"},"items":[{}]})");
    EXPECT_TRUE(!invalid_nested_array.get<json_object_envelope>().has_value());

    auto number = ruvia::json_value::parse("9007199254740993");
    const auto value = number.get<ruvia::uint64>();
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(static_cast<std::uint64_t>(*value), 9007199254740993ULL);

    auto array_value = ruvia::json_value::parse("[1,2]");
    const auto parsed_value = array_value.get<ruvia::array<ruvia::int32>>();
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_EQ(parsed_value->size(), 2U);
    EXPECT_TRUE(!number.get<ruvia::array<ruvia::int32>>().has_value());
    auto valid_rule = ruvia::json_value::parse(R"({"name":"ada"})");
    EXPECT_TRUE(valid_rule.get<json_validated_child>().has_value());
    auto invalid_rule = ruvia::json_value::parse(R"({"name":"ab"})");
    EXPECT_TRUE(!invalid_rule.get<json_validated_child>().has_value());

    auto obj_rule = ruvia::json_object::parse(R"({"good":{"name":"ada"},"bad":{"name":"ab"}})");
    const auto good = obj_rule.get<json_validated_child>("good");
    EXPECT_TRUE(good.status() == lookup_status::found);
    EXPECT_TRUE(good.value().has_value());
    // Present but failing the model's rules: found without a value.
    const auto bad = obj_rule.get<json_validated_child>("bad");
    EXPECT_TRUE(bad.status() == lookup_status::found);
    EXPECT_TRUE(!bad.value().has_value());
}
TEST(Ruvia, json_value_dynamic_traversal_reclaims_decoded_key_on_exception) {
    std::string body("{\"" + std::string(256, 'k') + "\\u0061\":1}");
    const auto json = ruvia::json_value::parse(body);
    bool thrown = false;
    try {
        // The throw depends on the visited key, as in a real visitor: one that
        // can never return leaves the traversal after it unreachable, which
        // optimizing compilers report (MSVC C4702).
        (void)json.for_each_field([](std::string_view key, const ruvia::json_value&) {
            if (key.ends_with('a')) {
                throw std::runtime_error("visitor");
            }
            return true;
        });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    EXPECT_TRUE(thrown);
}

TEST(Ruvia, json_value_get_reads_object_fields_and_rejects_non_objects) {
    auto json =
        ruvia::json_value::parse(R"({"name":"ada","age":2})");
    const auto age = json.get<ruvia::int32>("age");
    EXPECT_TRUE(age.status() == lookup_status::found);
    EXPECT_TRUE(age.value().has_value());
    EXPECT_EQ(static_cast<std::int32_t>(*age.value()), 2);
    const auto mismatched = json.get<ruvia::int32>("name");
    EXPECT_TRUE(mismatched.status() == lookup_status::found);
    EXPECT_TRUE(!mismatched.value().has_value());
    const auto missing = json.get<ruvia::string>("missing");
    EXPECT_TRUE(missing.status() == lookup_status::absent);
    EXPECT_TRUE(!missing.value().has_value());

    // A non-object token has no members.
    auto array_value = ruvia::json_value::parse("[1]");
    EXPECT_TRUE(array_value.get<ruvia::int32>("age").status() == lookup_status::absent);
}

TEST(Ruvia, json_dynamic_parse_enforces_cumulative_array_element_budget) {
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[]", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 1}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 3}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 4}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(R"({"a":{"b":[1,2]}})", {.max_array_elements_ = 1}); }));

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 1}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::json_object::parse(R"({"a":{"b":[1,2]},"c":[3]})", {.max_array_elements_ = 2});
    }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(" {} ", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse("[]"); }));

    const auto elements = [](std::size_t count) {
        std::string body(R"({"a":[)");
        for (std::size_t index = 0; index != count; ++index) {
            body.append(index == 0 ? "0" : ",0");
        }
        body.append("]}");
        return body;
    };
    const auto at_default_limit = elements(64 * 1024);
    const auto over_default_limit = elements(64 * 1024 + 1);
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(at_default_limit); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(at_default_limit); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(over_default_limit); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(over_default_limit); }));
}

using namespace ruvia::test;

TEST(Ruvia, model_json_parser_dispatches_decoded_keys) {
    const auto parsed_value = ruvia::from_json<accessor_surface_request>(
        R"({"mess\u0061ge":"ready"})");
    EXPECT_TRUE(parsed_value.get<"message">().has_value());
    if (!parsed_value.get<"message">()) {
        return;
    }
    EXPECT_EQ(parsed_value.get<"message">()->view(), std::string_view("ready"));
}

TEST(Ruvia, model_public_parsers_own_string_fields) {
    auto json = ruvia::from_json<accessor_surface_request>(
        std::string(R"({"message":"json-owned"})"));
    std::string form_input("message=form-owned");
    auto form = ruvia::from_form<accessor_surface_request>(form_input);
    form_input.assign(form_input.size(), 'x');

    if (!json.get<"message">() || !form.get<"message">()) {
        return;
    }
    EXPECT_EQ(json.get<"message">()->view(), std::string_view("json-owned"));
    EXPECT_EQ(form.get<"message">()->view(), std::string_view("form-owned"));
}

TEST(Ruvia, model_form_parser_keeps_malformed_percent_sequences_literally) {
    for (const std::string_view invalid : {"%", "%G0", "%0G"}) {
        for (const auto& body : {
                 std::move(std::string("message=ready&unknown=").append(invalid)),
                 std::move(std::string("unknown=").append(invalid)) + "&message=ready",
                 std::move(std::string("message=ready&").append(invalid)) + "=ignored"}) {
            const auto parsed = ruvia::from_form<accessor_surface_request>(body);
            EXPECT_TRUE(parsed.get<"message">().has_value());
        }
    }
    const auto literal = ruvia::from_form<accessor_surface_request>("message=100%");
    EXPECT_TRUE(literal.get<"message">().has_value());
    if (literal.get<"message">()) {
        EXPECT_EQ(literal.get<"message">()->view(), std::string_view("100%"));
    }
    const auto valid = ruvia::from_form<accessor_surface_request>(
        "message=hello+world&unknown=%00%26%3D%FF&%75nknown=value");
    EXPECT_TRUE(valid.get<"message">().has_value());
    if (valid.get<"message">()) {
        EXPECT_EQ(valid.get<"message">()->view(), std::string_view("hello world"));
    }
}

TEST(Ruvia, model_json_parser_rejects_invalid_field_types) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_model_item>(
            R"({"id":"wrong","label":"still parsed"})");
    }));
}

TEST(Ruvia, model_json_parser_fully_validates_unknown_and_duplicate_values) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"ready","unknown":[1,]})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"ready","unknown":"\ud83d"})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":{"broken":}})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":"\ud83d"})");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":"second"})");
    }));
}

TEST(Ruvia, model_json_parser_rejects_nested_structure_recursively) {
    constexpr auto body = R"({"primary":{},"items":[{"label":"missing id"}]})";
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_model_envelope>(body);
    }));
}

TEST(Ruvia, model_json_parser_enforces_depth_while_skipping_unknown_values) {
    std::string input(R"({"message":"ready","unknown":)");
    input.append(70, '[');
    input.append(70, ']');
    input.push_back('}');

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(input);
    }));
}

using namespace ruvia::test;

TEST(Ruvia, model_public_parser_accepts_valid_strings_and_rejects_invalid_input) {
    const auto model = ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready"})");
    EXPECT_TRUE(model.get<"message">().has_value());
    if (model.get<"message">().has_value()) {
        EXPECT_EQ(model.get<"message">()->view(), std::string_view("ready"));
    }

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":42})");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"incomplete")");
    }));
}

TEST(Ruvia, request_and_response_models_support_nested_arrays_and_optional_fields) {
    std::string input(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})");
    const auto parsed_value = ruvia::from_json<nested_model_envelope>(input);
    input.assign(input.size(), 'x');

    EXPECT_EQ(std::uint32_t(parsed_value.get<"primary">().get<"id">()), std::uint32_t{1});
    EXPECT_EQ(parsed_value.get<"items">().size(), std::size_t{1});
    EXPECT_TRUE(parsed_value.get<"tags">().has_value());
    EXPECT_TRUE(!parsed_value.get<"primary">().get<"label">().has_value());
    EXPECT_TRUE(parsed_value.get<"items">()[0].get<"label">().has_value());
    if (parsed_value.get<"items">()[0].get<"label">()) {
        EXPECT_EQ(parsed_value.get<"items">()[0].get<"label">()->view(), std::string_view("two"));
    }
    if (parsed_value.get<"tags">()) {
        EXPECT_EQ(parsed_value.get<"tags">()->size(), std::size_t{2});
        EXPECT_EQ((*parsed_value.get<"tags">())[0].view(), std::string_view("a"));
        EXPECT_EQ((*parsed_value.get<"tags">())[1].view(), std::string_view("b"));
    }

    nested_response_envelope response;
    response.ensure<"primary">().set<"id">(ruvia::uint32{1});
    response.ensure<"items">()
        .emplace_back()
        .set<"id">(ruvia::uint32{2})
        .set<"label">("two");
    response.ensure<"tags">().emplace_back("a");
    response.ensure<"tags">().emplace_back("b");
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})"));
}

RUVIA_MODEL(request_binding_params,
    RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_MIN(2, "id is too short")));

RUVIA_MODEL(request_binding_body,
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(2, "name is too short")));

RUVIA_MODEL(patch_binding_body,
    RUVIA_REQUIRED_FIELD(token, ruvia::string),
    RUVIA_OPTIONAL_FIELD(retries, ruvia::uint32, RUVIA_DEFAULT(0), RUVIA_MIN(1, "must be positive")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value));

RUVIA_MODEL(request_binding_out, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string));

RUVIA_MODEL(dynamic_binding_body,
    RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object),
    RUVIA_OPTIONAL_FIELD(numbers, ruvia::array<ruvia::uint32>));

RUVIA_MODEL(tags_binding_input, RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

namespace {

ruvia::task<request_binding_out> make_response(std::string_view id, std::string_view name) {
    request_binding_out response;
    response.set<"id">(id);
    response.set<"name">(name);
    co_return response;
}

}  // namespace

class request_binding_controller final {
public:
    RUVIA_ROUTES_BEGIN(request_binding_controller)
    RUVIA_PUT("/request-binding/:id", update, ruvia::path_model<request_binding_params>,
        ruvia::json_body<request_binding_body>);
    RUVIA_PATCH("/request-binding-patch", patch, ruvia::json_body<patch_binding_body>);
    RUVIA_POST("/request-binding-echo", echo, ruvia::json_body<request_binding_body>);
    RUVIA_POST("/request-binding-dynamic", dynamic, ruvia::json_body<dynamic_binding_body>);
    RUVIA_POST("/request-binding-form", echo, ruvia::form_body<request_binding_body>);
    RUVIA_GET("/request-binding-query-tags", tags, ruvia::query_model<tags_binding_input>);
    RUVIA_GET("/request-binding-header-tags", tags, ruvia::header_model<tags_binding_input>);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> tags(ruvia::context& c) {
        const bool present = c.req().validated<tags_binding_input>().is_present<"tags">();
        co_return c.text(std::string_view(present ? "present" : "absent"));
    }

    ruvia::task<ruvia::http_response> dynamic(ruvia::context& c) {
        co_return c.json(c.req().validated<dynamic_binding_body>());
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        co_return c.json(c.req().validated<request_binding_body>());
    }

    ruvia::task<ruvia::http_response> patch(ruvia::context& c) {
        const auto& body = c.req().validated<patch_binding_body>();
        if (!body.is_present<"remark">()) {
            co_return c.text("unchanged");
        }
        if (body.is_null<"remark">()) {
            co_return c.text("clear");
        }
        co_return c.text(body.get<"remark">()->view());
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto& params = c.req().validated<request_binding_params>();
        const auto& body = c.req().validated<request_binding_body>();
        auto response = co_await make_response(params.get<"id">().view(), body.get<"name">().view());
        co_return c.json(response);
    }
};

TEST(Ruvia, request_binding_enforces_defaults_and_exposes_patch_states) {
    ruvia::test_app app;
    for (const auto& [body, expected] : {
             std::pair{R"({"token":"ok","retries":1})", "unchanged"},
             std::pair{R"({"token":"ok","retries":1,"remark":null})", "clear"},
             std::pair{R"({"token":"ok","retries":1,"remark":"changed"})", "changed"}}) {
        const auto response = app.request(ruvia::test_request::patch("/request-binding-patch").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        EXPECT_EQ(response.body(), std::string_view(expected));
    }
    for (const auto& [body, code] : {
             std::pair{R"({"token":"ok"})", "too_small"},
             std::pair{R"({"retries":1})", "required"},
             std::pair{R"({"token":"ok","retries":1,"enabled":null})", "invalid_type"},
             std::pair{R"({"token":"ok","retries":1,"remark":""})", "too_small"},
             std::pair{R"({"token":"ok","retries":1,"remark":null,"remark":"changed"})", "duplicate"}}) {
        const auto response = app.request(ruvia::test_request::patch("/request-binding-patch").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find(code) != std::string_view::npos);
    }
}

TEST(Ruvia, request_binding_serializes_the_validated_model_without_copying_fields) {
    ruvia::test_app app;
    const auto ok = app.request(
        ruvia::test_request::post("/request-binding-echo").json(R"({"name":"A\u006c"})"));
    EXPECT_TRUE(ok.status() == ruvia::http_status::ok);
    EXPECT_EQ(ok.body(), std::string_view(R"({"name":"Al"})"));
    for (const auto body : {R"({"name":"A"})", R"({"name":null})", R"({})", R"({"name":"Al","name":"Bob"})"}) {
        const auto rejected = app.request(ruvia::test_request::post("/request-binding-echo").json(body));
        EXPECT_TRUE(rejected.status() == ruvia::http_status::bad_request);
    }
}

TEST(Ruvia, request_binding_form_keeps_malformed_percent_sequences_literally) {
    // WHATWG application/x-www-form-urlencoded parsing keeps a '%' that does not
    // start a valid escape.
    ruvia::test_app app;
    for (const std::string_view invalid : {"%", "%G0", "%0G"}) {
        for (const auto& body : {
                 std::move(std::string("name=Al").append(invalid)) + "&unknown=" + std::string(invalid),
                 std::move(std::string("unknown=").append(invalid)) + "&name=Al" + std::string(invalid)}) {
            const auto response = app.request(
                ruvia::test_request::post("/request-binding-form").form(body));
            EXPECT_TRUE(response.status() == ruvia::http_status::ok);
            EXPECT_EQ(response.body(), std::move(std::string(R"({"name":"Al)").append(invalid)) + R"("})");
        }
    }
    const auto valid = app.request(ruvia::test_request::post("/request-binding-form")
            .form("name=A%6C&unknown=%00%26%3D%FF"));
    EXPECT_TRUE(valid.status() == ruvia::http_status::ok);
    EXPECT_EQ(valid.body(), std::string_view(R"({"name":"Al"})"));
}

TEST(Ruvia, request_binding_validates_inputs_and_serializes_service_model) {
    ruvia::test_app app;
    const auto ok = app.request(
        ruvia::test_request::put("/request-binding/u1").json(R"({"name":"Al"})"));
    EXPECT_TRUE(ok.status() == ruvia::http_status::ok);
    EXPECT_TRUE(ok.body().find("\"id\":\"u1\"") != std::string_view::npos);
    EXPECT_TRUE(ok.body().find("\"name\":\"Al\"") != std::string_view::npos);

    const auto short_id = app.request(
        ruvia::test_request::put("/request-binding/x").json(R"({"name":"Al"})"));
    EXPECT_TRUE(short_id.status() == ruvia::http_status::bad_request);

    const auto short_name = app.request(
        ruvia::test_request::put("/request-binding/u1").json(R"({"name":"A"})"));
    EXPECT_TRUE(short_name.status() == ruvia::http_status::bad_request);
}

TEST(Ruvia, request_binding_dynamic_json_distinguishes_syntax_from_field_type_errors) {
    ruvia::test_app app;
    for (const auto body : {R"({"object":[1]})", R"({"object": [1]})", R"({"object":"x"})", R"({"object": "x"})"}) {
        const auto response = app.request(ruvia::test_request::post("/request-binding-dynamic").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find("invalid_type") != std::string_view::npos);
    }
    for (const auto body : {R"({"payload":})", R"({"payload": })", R"({"payload":[1,})", R"({"object":{"x":}})"}) {
        const auto response = app.request(ruvia::test_request::post("/request-binding-dynamic").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find("invalid json body") != std::string_view::npos);
        EXPECT_TRUE(response.body().find("validation_failed") == std::string_view::npos);
    }
}

TEST(Ruvia, request_binding_large_wrong_type_arrays_report_field_validation) {
    std::string elements{};
    for (std::size_t index = 0; index < 40000; ++index) {
        if (index != 0) {
            elements.push_back(',');
        }
        elements.push_back('0');
    }
    ruvia::test_app app;
    for (const auto& body : {
             std::string("{\"object\":[") + elements + "]}",
             std::string("{\"numbers\":[") + elements + ",\"wrong type\"]}"}) {
        const auto response = app.request(ruvia::test_request::post("/request-binding-dynamic").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find("validation_failed") != std::string_view::npos);
        EXPECT_TRUE(response.body().find("invalid_type") != std::string_view::npos);
        EXPECT_TRUE(response.body().find("invalid json body") == std::string_view::npos);
    }
}

TEST(Ruvia, request_binding_rejects_query_and_header_values_for_array_fields) {
    ruvia::test_app app;
    for (const auto target : {"/request-binding-query-tags", "/request-binding-header-tags"}) {
        const auto absent = app.request(ruvia::test_request::get(target));
        EXPECT_TRUE(absent.status() == ruvia::http_status::ok);
        EXPECT_EQ(absent.body(), std::string_view("absent"));
    }
    for (const auto& [target, code] : {
             std::pair{"/request-binding-query-tags?tags=a", "invalid_type"},
             std::pair{"/request-binding-query-tags?tags=a&tags=b", "duplicate"}}) {
        const auto response = app.request(ruvia::test_request::get(target));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find(code) != std::string_view::npos);
    }
    const auto header = app.request(ruvia::test_request::get("/request-binding-header-tags").header("Tags", "a"));
    EXPECT_TRUE(header.status() == ruvia::http_status::bad_request);
    EXPECT_TRUE(header.body().find("invalid_type") != std::string_view::npos);
}

namespace {

using ruvia::validator;

struct value_bound final {
    int value_;
};

struct bounded_value final {
    int value_;

    [[nodiscard]] bool operator<(value_bound bound) const noexcept {
        return value_ < bound.value_;
    }

    [[nodiscard]] bool operator>(value_bound bound) const noexcept {
        return value_ > bound.value_;
    }
};

RUVIA_MODEL(required_optional_model, RUVIA_REQUIRED_FIELD_NAME("requiredValue", required_value, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("optionalValue", optional_value, ruvia::string));

RUVIA_MODEL(precise_numeric_bounds,
    RUVIA_OPTIONAL_FIELD(unsigned_limit, ruvia::double_value,
        RUVIA_MAX(std::numeric_limits<std::uint64_t>::max(), "too large")),
    RUVIA_OPTIONAL_FIELD(signed_limit, ruvia::double_value,
        RUVIA_MIN(std::numeric_limits<std::int64_t>::min() + 1, "too small"),
        RUVIA_MAX(std::numeric_limits<std::int64_t>::max(), "too large")),
    RUVIA_OPTIONAL_FIELD(exact_minimum, ruvia::double_value,
        RUVIA_MIN(9007199254740993LL, "too small")));

RUVIA_MODEL(character_class_rules,
    RUVIA_OPTIONAL_FIELD(pattern_lower, ruvia::string, RUVIA_PATTERN(R"(^[\--0]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_not_lower, ruvia::string, RUVIA_PATTERN(R"(^[^\--0]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_upper, ruvia::string, RUVIA_PATTERN(R"(^[A-\]]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_not_upper, ruvia::string, RUVIA_PATTERN(R"(^[^A-\]]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_digits, ruvia::string, RUVIA_PATTERN(R"(^[\d]$)", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_tail, ruvia::string, RUVIA_PATTERN("^[a-]$", "invalid class")),
    RUVIA_OPTIONAL_FIELD(pattern_escaped_dash, ruvia::string, RUVIA_PATTERN(R"(^[a\-z]$)", "invalid class")));
inline constexpr auto wide_class_pattern = ruvia::fixed_string{"^[^aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]*$"};

RUVIA_MODEL(bounded_pattern_rules,
    RUVIA_OPTIONAL_FIELD(wide_class, ruvia::string, RUVIA_PATTERN(wide_class_pattern, "invalid class")),
    RUVIA_OPTIONAL_FIELD(backtracking, ruvia::string,
        RUVIA_PATTERN("^a*a*a*b$", "pattern match limit exceeded")));

template <ruvia::fixed_string field_name>
void check_character_class_rule(std::string_view value, bool matches, std::string_view code) {
    character_class_rules model;
    model.set<field_name>(value);
    validator result;
    ruvia::validate_model(model, result);
    EXPECT_EQ(result.ok(), matches);
    EXPECT_EQ(result.issues().size(), matches ? std::size_t{0} : std::size_t{1});
    if (!result.issues().empty()) {
        EXPECT_EQ(result.issues()[0].field(), field_name.view());
        EXPECT_EQ(result.issues()[0].code(), code);
    }
}

[[nodiscard]] std::exception_ptr capture_validation_exception() {
    validator validator_value;
    validator_value.add(std::string(128, 'f'), "required", std::string(256, 'm'));
    try {
        validator_value.throw_if_invalid({
            .status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation",
        });
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

}  // namespace

TEST(Ruvia, request_model_required_and_optional_fields_are_structural) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<required_optional_model>("{}");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_form<required_optional_model>("");
    }));
}

TEST(Ruvia, validator_required_flags_absent_values) {
    validator v;
    std::optional<std::string> present = std::string("x");
    std::optional<std::string> absent;
    v.required(present, "present");
    v.required(absent, "absent", "absent is required");

    EXPECT_TRUE(!v.ok());
    EXPECT_EQ(v.issues().size(), std::size_t{1});
    EXPECT_EQ(v.issues()[0].field(), std::string_view("absent"));
    EXPECT_EQ(v.issues()[0].code(), std::string_view("required"));
    EXPECT_EQ(v.issues()[0].message(), std::string_view("absent is required"));
}

TEST(Ruvia, validator_length_bounds_and_absent_skips) {
    validator v;
    std::optional<std::string> value = std::string("abc");
    v.min_length(value, "f", 2);  // 3 >= 2, ok
    v.max_length(value, "f", 5);  // 3 <= 5, ok
    EXPECT_TRUE(v.ok());

    v.min_length(value, "f", 5);  // 3 < 5 -> too_small
    v.max_length(value, "f", 2);  // 3 > 2 -> too_big
    // An absent value is never checked.
    std::optional<std::string> absent;
    v.min_length(absent, "g", 100);

    EXPECT_EQ(v.issues().size(), std::size_t{2});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(v.issues()[1].code(), std::string_view("too_big"));
}

TEST(Ruvia, validator_range_and_one_of) {
    validator v;
    std::optional<int> n = 5;
    v.range(n, "n", 1, 10);  // in range, ok
    EXPECT_TRUE(v.ok());
    v.range(n, "n", 6, 10);  // 5 < 6 -> too_small

    std::optional<std::string> s = std::string("b");
    v.one_of(s, "s", {"a", "b", "c"});  // allowed, ok
    v.one_of(s, "s", {"x", "y"});       // not allowed -> one_of

    EXPECT_EQ(v.issues().size(), std::size_t{2});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(v.issues()[1].code(), std::string_view("one_of"));
}

TEST(Ruvia, validator_range_upper_bound_inclusive_and_absent_skips) {
    validator v;
    // The upper bound is enforced independently of the lower bound.
    std::optional<int> high = 5;
    v.range(high, "high", 1, 3);  // 5 > 3 -> too_big
    EXPECT_EQ(v.issues().size(), std::size_t{1});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_big"));
    // Both bounds are inclusive: values exactly at min or max are accepted.
    std::optional<int> at_min = 1;
    std::optional<int> at_max = 10;
    v.range(at_min, "atMin", 1, 10);
    v.range(at_max, "atMax", 1, 10);
    EXPECT_EQ(v.issues().size(), std::size_t{1});
    // An absent value skips range validation entirely.
    std::optional<int> absent;
    v.range(absent, "absent", 1, 3);
    EXPECT_EQ(v.issues().size(), std::size_t{1});
}

TEST(Ruvia, validator_range_supports_heterogeneous_value_bounds) {
    const value_bound minimum{1};
    const value_bound maximum{10};
    validator valid;
    for (const int value : {1, 5, 10}) {
        valid.range(std::optional<bounded_value>{bounded_value{value}}, "custom", minimum, maximum);
    }
    valid.range(std::optional<bounded_value>{}, "absent", minimum, maximum);
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<bounded_value>{bounded_value{0}}, "below", minimum, maximum);
    invalid.range(std::optional<bounded_value>{bounded_value{11}}, "above", minimum, maximum);
    EXPECT_EQ(invalid.issues().size(), std::size_t{2});
    if (invalid.issues().size() != 2) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].field(), std::string_view("below"));
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[1].field(), std::string_view("above"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
}

TEST(Ruvia, validator_range_compares_signed_and_unsigned_integers_without_conversion) {
    validator valid;
    valid.range(std::optional<std::uint64_t>{5}, "unsigned", -1, 10);
    valid.range(std::optional<std::int64_t>{5}, "signed", 0U, 10U);
    valid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "wide_unsigned", 0, std::numeric_limits<std::uint64_t>::max());
    valid.range(std::optional<ruvia::uint64>{5}, "model_unsigned", -1, 10);
    valid.range(std::optional<int>{1}, "model_bounds", ruvia::int64{-1}, ruvia::uint64{10});
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<std::int64_t>{-1}, "negative", 0U, 10U);
    invalid.range(std::optional<std::uint64_t>{5}, "negative_upper", -10, -1);
    invalid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "wide_unsigned", 0, std::numeric_limits<std::int64_t>::max());
    invalid.range(std::optional<ruvia::int64>{-1}, "model_negative", 0U, 10U);
    EXPECT_EQ(invalid.issues().size(), std::size_t{4});
    if (invalid.issues().size() != 4) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[2].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
}

TEST(Ruvia, validator_range_compares_floats_and_large_integers_exactly) {
    validator valid;
    valid.range(std::optional<double>{18446744073709549568.0}, "below_unsigned_max",
        0, std::numeric_limits<std::uint64_t>::max());
    valid.range(std::optional<double>{-9223372036854775808.0}, "signed_minimum",
        std::numeric_limits<std::int64_t>::min(), 0);
    valid.range(std::optional<std::uint64_t>{9007199254740993ULL}, "above_float_minimum",
        9007199254740992.0, 9007199254740994.0);
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<double>{18446744073709551616.0}, "unsigned_max",
        0, std::numeric_limits<std::uint64_t>::max());
    invalid.range(std::optional<double>{9223372036854775808.0}, "signed_max",
        0, std::numeric_limits<std::int64_t>::max());
    invalid.range(std::optional<double>{-9223372036854775808.0}, "above_signed_minimum",
        std::numeric_limits<std::int64_t>::min() + 1, 0);
    invalid.range(std::optional<double>{9007199254740992.0}, "large_minimum",
        9007199254740993ULL, std::numeric_limits<std::uint64_t>::max());
    invalid.range(std::optional<std::uint64_t>{9007199254740993ULL}, "integer_maximum",
        0.0, 9007199254740992.0);
    EXPECT_EQ(invalid.issues().size(), std::size_t{5});
    if (invalid.issues().size() != 5) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[2].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[4].code(), std::string_view("too_big"));
}

TEST(Ruvia, validator_range_compares_fractional_bounds_and_nonfinite_values) {
    const auto infinity = std::numeric_limits<double>::infinity();
    validator valid;
    valid.range(std::optional<int>{-1}, "negative", -1.5, -0.5);
    valid.range(std::optional<unsigned>{0}, "zero", -0.5, 0.5);
    valid.range(std::optional<int>{1}, "positive", 0.5, 1.5);
    valid.range(std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()},
        "infinite_bounds", -infinity, infinity);
    valid.range(std::optional<double>{std::numeric_limits<double>::quiet_NaN()},
        "unordered", std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(valid.ok());

    validator invalid;
    invalid.range(std::optional<unsigned>{0}, "positive_minimum", 0.5, 1.0);
    invalid.range(std::optional<int>{0}, "negative_maximum", -1.0, -0.5);
    invalid.range(std::optional<double>{infinity}, "positive_infinity", 0,
        std::numeric_limits<std::uint64_t>::max());
    invalid.range(std::optional<double>{-infinity}, "negative_infinity",
        std::numeric_limits<std::int64_t>::min(), 0);
    EXPECT_EQ(invalid.issues().size(), std::size_t{4});
    if (invalid.issues().size() != 4) {
        return;
    }
    EXPECT_EQ(invalid.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(invalid.issues()[1].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[2].code(), std::string_view("too_big"));
    EXPECT_EQ(invalid.issues()[3].code(), std::string_view("too_small"));
}

TEST(Ruvia, model_numeric_rules_compare_float_fields_and_integer_bounds_exactly) {
    for (const auto body : {R"({"unsigned_limit":18446744073709549568})",
             R"({"signed_limit":9223372036854774784})",
             R"({"signed_limit":-9223372036854774784})",
             R"({"exact_minimum":9007199254740994})"}) {
        auto value = ruvia::from_json<precise_numeric_bounds>(body);
        validator result;
        ruvia::validate_model(value, result);
        EXPECT_TRUE(result.ok());
    }
    for (const auto& [body, code] : {
             std::pair{R"({"unsigned_limit":18446744073709551616})", "too_big"},
             std::pair{R"({"signed_limit":9223372036854775808})", "too_big"},
             std::pair{R"({"signed_limit":-9223372036854775808})", "too_small"},
             std::pair{R"({"exact_minimum":9007199254740992})", "too_small"}}) {
        bool rejected = false;
        try {
            (void)ruvia::from_json<precise_numeric_bounds>(body);
        } catch (const ruvia::http_error& error) {
            rejected = true;
            EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
            if (!error.validation_issues().empty()) {
                EXPECT_EQ(error.validation_issues()[0].code(), std::string_view(code));
            }
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, model_pattern_rules_match_escaped_character_ranges) {
    for (const auto& [value, matches] : {
             std::pair{"-", true}, std::pair{".", true}, std::pair{"/", true}, std::pair{"0", true},
             std::pair{",", false}, std::pair{"1", false}, std::pair{"a", false}}) {
        check_character_class_rule<"pattern_lower">(value, matches, "pattern");
        check_character_class_rule<"pattern_not_lower">(value, !matches, "pattern");
    }
    for (const auto& [value, matches] : {
             std::pair{"A", true}, std::pair{"Z", true}, std::pair{"\\", true}, std::pair{"]", true},
             std::pair{"@", false}, std::pair{"^", false}, std::pair{"a", false}}) {
        check_character_class_rule<"pattern_upper">(value, matches, "pattern");
        check_character_class_rule<"pattern_not_upper">(value, !matches, "pattern");
    }
}

TEST(Ruvia, model_pattern_rules_preserve_class_escapes_and_literal_hyphens) {
    for (const auto& [value, matches] : {
             std::pair{"0", true}, std::pair{"9", true}, std::pair{"d", false}, std::pair{"a", false},
             std::pair{"-", false}, std::pair{"00", false}}) {
        check_character_class_rule<"pattern_digits">(value, matches, "pattern");
    }
    for (const auto& [value, matches] : {
             std::pair{"a", true}, std::pair{"-", true}, std::pair{"b", false}, std::pair{"z", false}}) {
        check_character_class_rule<"pattern_tail">(value, matches, "pattern");
    }
    for (const auto& [value, matches] : {
             std::pair{"a", true}, std::pair{"-", true}, std::pair{"z", true}, std::pair{"m", false}}) {
        check_character_class_rule<"pattern_escaped_dash">(value, matches, "pattern");
    }
}

TEST(Ruvia, model_pattern_rules_bound_matching_work) {
    // A class costs one lookup per input byte however many members it lists.
    bounded_pattern_rules model;
    model.set<"wide_class">(std::string(4096, 'b'));
    validator class_result;
    ruvia::validate_model(model, class_result);
    EXPECT_TRUE(class_result.ok());
    model.set<"wide_class">("ba");
    validator class_mismatch;
    ruvia::validate_model(model, class_mismatch);
    EXPECT_EQ(class_mismatch.issues().size(), std::size_t{1});
    model.reset<"wide_class">();

    model.set<"backtracking">("aaab");
    validator short_result;
    ruvia::validate_model(model, short_result);
    EXPECT_TRUE(short_result.ok());

    // Cubic backtracking over a long non-matching input stops at the shared
    // matching work limit instead of running unbounded, and fails closed.
    model.set<"backtracking">(std::string(4096, 'a'));
    validator long_result;
    ruvia::validate_model(model, long_result);
    EXPECT_EQ(long_result.issues().size(), std::size_t{1});
    if (long_result.issues().size() == 1) {
        EXPECT_EQ(long_result.issues()[0].field(), std::string_view("backtracking"));
        EXPECT_EQ(long_result.issues()[0].code(), std::string_view("pattern"));
        EXPECT_EQ(long_result.issues()[0].message(), std::string_view("pattern match limit exceeded"));
    }
}

TEST(Ruvia, validator_one_of_absent_skips_and_range_accepts_doubles) {
    validator v;
    // one_of on an absent optional is skipped -- the one rule whose absent-skip branch
    // the other tests don't exercise (required/min_length/range already cover theirs).
    std::optional<std::string> absent;
    v.one_of(absent, "a", {"x", "y"});
    EXPECT_TRUE(v.ok());

    // range validates floating-point values, not just integers (a distinct template
    // instantiation and comparison path from the int cases above).
    std::optional<double> in_range = 0.5;
    v.range(in_range, "d", 0.0, 1.0);  // 0.0 <= 0.5 <= 1.0, ok
    EXPECT_TRUE(v.ok());
    std::optional<double> low = -0.1;
    v.range(low, "d", 0.0, 1.0);  // -0.1 < 0.0 -> too_small
    std::optional<double> high = 1.1;
    v.range(high, "d", 0.0, 1.0);  // 1.1 > 1.0 -> too_big
    EXPECT_EQ(v.issues().size(), std::size_t{2});
    EXPECT_EQ(v.issues()[0].code(), std::string_view("too_small"));
    EXPECT_EQ(v.issues()[1].code(), std::string_view("too_big"));

    // Both floating bounds are inclusive: a value exactly at min or max is accepted.
    std::optional<double> at_min = 0.0;
    std::optional<double> at_max = 1.0;
    v.range(at_min, "d", 0.0, 1.0);
    v.range(at_max, "d", 0.0, 1.0);
    EXPECT_EQ(v.issues().size(), std::size_t{2});  // unchanged
}

TEST(Ruvia, throw_if_invalid_exposes_typed_issues) {
    validator v;
    std::optional<std::string> absent;
    v.required(absent, "email", "email is required");
    std::optional<std::string> short_name = std::string("a");
    v.min_length(short_name, "name", 3, "too short");

    try {
        v.throw_if_invalid();
        EXPECT_TRUE(false);  // must have thrown
    } catch (const ruvia::http_error& error) {
        const auto issues = error.info().validation_issues();
        EXPECT_EQ(issues.size(), std::size_t{2});
        EXPECT_EQ(issues[0].field(), std::string_view("email"));
        EXPECT_EQ(issues[0].code(), std::string_view("required"));
        EXPECT_EQ(issues[0].message(), std::string_view("email is required"));
        EXPECT_EQ(issues[1].field(), std::string_view("name"));
        EXPECT_EQ(issues[1].code(), std::string_view("too_small"));
        EXPECT_EQ(issues[1].message(), std::string_view("too short"));
    }
}

TEST(Ruvia, throw_if_invalid_preserves_special_characters_as_typed_data) {
    validator v;
    v.add("f\"x", "code", "a\"b\\c");
    try {
        v.throw_if_invalid();
        EXPECT_TRUE(false);
    } catch (const ruvia::http_error& error) {
        const auto issues = error.info().validation_issues();
        EXPECT_EQ(issues.size(), std::size_t{1});
        EXPECT_EQ(issues[0].field(), std::string_view("f\"x"));
        EXPECT_EQ(issues[0].message(), std::string_view("a\"b\\c"));
    }
}

TEST(Ruvia, validation_failure_options_control_reported_error_info) {
    validator v;
    v.add("field", "required", "missing");

    try {
        v.throw_if_invalid({
            .status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation",
        });
        EXPECT_TRUE(false);
    } catch (const ruvia::http_error& error) {
        const auto info = error.info();
        EXPECT_EQ(info.status(), ruvia::http_status::unprocessable_content);
        EXPECT_EQ(info.code(), std::string_view("invalid_payload"));
        EXPECT_EQ(info.message(), std::string_view("payload failed validation"));
        EXPECT_EQ(info.validation_issues().size(), std::size_t{1});
    }
}

TEST(Ruvia, throw_if_invalid_survives_validator_destruction_and_exception_ptr) {
    const auto exception = capture_validation_exception();
    EXPECT_TRUE(exception != nullptr);

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::http_error& error) {
        EXPECT_EQ(std::string_view(error.what()), std::string_view("payload failed validation"));
        EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
        EXPECT_EQ(error.validation_issues()[0].field().size(), std::size_t{128});
        EXPECT_EQ(error.validation_issues()[0].message().size(), std::size_t{256});
    }
}

TEST(Ruvia, http_error_copy_move_and_assignment_preserve_owned_issues) {
    const auto exception = capture_validation_exception();

    try {
        std::rethrow_exception(exception);
    } catch (const ruvia::http_error& original) {
        ruvia::http_error copied(original);
        ruvia::http_error copy_assigned({.status_ = ruvia::http_status::bad_request});
        copy_assigned = original;

        ruvia::http_error moved(std::move(copied));
        ruvia::http_error move_assigned({.status_ = ruvia::http_status::bad_request});
        move_assigned = std::move(moved);

        for (const auto* error : {&copy_assigned, &move_assigned}) {
            EXPECT_EQ(error->validation_issues().size(), std::size_t{1});
            EXPECT_EQ(error->validation_issues()[0].field().size(), std::size_t{128});
            EXPECT_EQ(error->validation_issues()[0].message().size(), std::size_t{256});
            EXPECT_EQ(error->info().status(), ruvia::http_status::unprocessable_content);
            EXPECT_EQ(error->info().code(), std::string_view("invalid_payload"));
            EXPECT_EQ(std::string_view(error->what()),
                std::string_view("payload failed validation"));
        }
        EXPECT_EQ(copied.validation_issues().size(), std::size_t{0});
        EXPECT_EQ(moved.validation_issues().size(), std::size_t{0});
    }
}

TEST(Ruvia, validator_throw_if_invalid_raises_on_issues) {
    validator ok;
    ok.throw_if_invalid();  // no issues -> no throw

    validator bad;
    std::optional<std::string> absent;
    bad.required(absent, "x");
    bool threw = false;
    try {
        bad.throw_if_invalid();
    } catch (const ruvia::http_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, validation_diagnostic_text_preserves_complete_utf8_within_limit) {
    ruvia::validator validator;
    std::string message(ruvia::max_validation_text_bytes - 1, 'x');
    message += "中文";
    validator.add("field", "code", message);
    EXPECT_EQ(validator.issues()[0].message().size(), ruvia::max_validation_text_bytes - 1);
    EXPECT_EQ(validator.issues()[0].message(), std::string_view(message).substr(0, ruvia::max_validation_text_bytes - 1));
}

namespace validator_ownership_test {

void add_owned_issues(ruvia::validator& value) {
    value.add(std::string(128, 'f'), std::string(96, 'c'), std::string(256, 'm'));
    value.add("secondary", "invalid", "second issue");
}

void check_owned_issues(const ruvia::validator& value, std::size_t count = 2) {
    EXPECT_EQ(value.issues().size(), count);
    EXPECT_TRUE(!value.ok());
    if (value.issues().size() >= 2) {
        EXPECT_EQ(value.issues()[0].field(), std::string_view(std::string(128, 'f')));
        EXPECT_EQ(value.issues()[0].code(), std::string_view(std::string(96, 'c')));
        EXPECT_EQ(value.issues()[0].message(), std::string_view(std::string(256, 'm')));
        EXPECT_EQ(value.issues()[1].field(), std::string_view("secondary"));
        EXPECT_EQ(value.issues()[1].code(), std::string_view("invalid"));
        EXPECT_EQ(value.issues()[1].message(), std::string_view("second issue"));
    }
}

void check_owned_error(const ruvia::validator& value, std::size_t count = 2) {
    bool rejected = false;
    try {
        value.throw_if_invalid({.status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation"});
    } catch (const ruvia::http_error& error) {
        rejected = true;
        EXPECT_EQ(error.info().status(), ruvia::http_status::unprocessable_content);
        EXPECT_EQ(error.info().code(), std::string_view("invalid_payload"));
        EXPECT_EQ(std::string_view(error.what()), std::string_view("payload failed validation"));
        EXPECT_EQ(error.validation_issues().size(), count);
        if (error.validation_issues().size() >= 2) {
            EXPECT_EQ(error.validation_issues()[0].field(), std::string_view(std::string(128, 'f')));
            EXPECT_EQ(error.validation_issues()[0].message(), std::string_view(std::string(256, 'm')));
            EXPECT_EQ(error.validation_issues()[1].field(), std::string_view("secondary"));
        }
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, validator_copy_and_move_semantics_own_diagnostics_and_allow_reuse) {
    std::optional<ruvia::validator> copy;
    {
        ruvia::validator input;
        add_owned_issues(input);
        copy.emplace(input);
        check_owned_issues(input);
        check_owned_issues(*copy);
    }
    copy->add("later", "required", std::string(200, 'a'));
    check_owned_issues(*copy, 3);
    check_owned_error(*copy, 3);

    for (const bool move : {false, true}) {
        ruvia::validator destination;
        destination.add("seed", "old", std::string(180, 's'));
        ruvia::validator input;
        add_owned_issues(input);
        if (move) {
            destination = std::move(input);
            EXPECT_TRUE(input.ok());
            input.add("reused", "required", std::string(190, 'r'));
            EXPECT_EQ(input.issues().size(), std::size_t{1});
        } else {
            destination = input;
            check_owned_issues(input);
        }
        auto* alias = std::addressof(destination);
        destination = *alias;
        destination = std::move(*alias);
        check_owned_issues(destination);
        check_owned_error(destination);
    }
}

TEST(Ruvia, validation_issue_copy_and_move_outlive_validator) {
    static_assert(std::is_nothrow_move_constructible_v<ruvia::validation_issue>);
    const std::string field(128, 'f');
    const std::string code(96, 'c');
    const std::string message(256, 'm');
    std::optional<ruvia::validation_issue> copied;
    {
        validator input;
        input.add(field, code, message);
        copied.emplace(input.issues().front());
    }
    EXPECT_EQ(copied->field(), field);
    EXPECT_EQ(copied->code(), code);
    EXPECT_EQ(copied->message(), message);

    ruvia::validation_issue assigned = *copied;
    auto* alias = std::addressof(assigned);
    assigned = *alias;
    assigned = std::move(*alias);
    EXPECT_EQ(assigned.field(), field);
    EXPECT_EQ(assigned.code(), code);
    EXPECT_EQ(assigned.message(), message);
}

}  // namespace validator_ownership_test

namespace {

RUVIA_MODEL(lazy_path_item, RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_MIN(0, "must not be negative")));

RUVIA_MODEL(lazy_path_envelope,
    RUVIA_REQUIRED_FIELD_NAME("field_name_longer_than_small_string_storage", label, ruvia::string,
        RUVIA_MIN(1, "must not be empty")),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<lazy_path_item>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<lazy_path_item>));

}  // namespace

TEST(Ruvia, validator_model_paths_are_built_only_for_emitted_issues) {
    std::string body(R"({"field_name_longer_than_small_string_storage":"x","items":[)");
    for (std::size_t index = 0; index != 1024; ++index) {
        body.append(index == 0 ? R"({"value":1})" : R"(,{"value":1})");
    }
    body.append(R"(],"boxed":[{"value":1},{"value":2}]})");
    auto model = ruvia::from_json<lazy_path_envelope>(body);
    {
        validator result;
        ruvia::validate_model(model, result);
        EXPECT_TRUE(result.ok());

        model.set<"label">("");
        model.ensure<"items">()[700].set<"value">(-1);
        model.ensure<"boxed">()[1].set<"value">(-2);
        ruvia::validate_model(model, result);
        EXPECT_EQ(result.issues().size(), std::size_t{3});
        if (result.issues().size() == 3) {
            EXPECT_EQ(result.issues()[0].field(), std::string_view("field_name_longer_than_small_string_storage"));
            EXPECT_EQ(result.issues()[1].field(), std::string_view("items[700].value"));
            EXPECT_EQ(result.issues()[2].field(), std::string_view("boxed[1].value"));
            for (const auto& issue : result.issues()) {
                EXPECT_EQ(issue.code(), std::string_view("too_small"));
            }
        }
    }
}

TEST(Ruvia, validator_retains_all_model_validation_issues_beyond_64) {
    lazy_path_envelope model;
    model.set<"label">("ok");
    auto& items = model.ensure<"items">();
    constexpr std::size_t invalid_count = 80;
    for (std::size_t index = 0; index < invalid_count; ++index) {
        items.emplace_back().set<"value">(-1);
    }
    validator result;
    ruvia::validate_model(model, result);
    EXPECT_EQ(result.issues().size(), invalid_count);
    if (result.issues().size() == invalid_count) {
        EXPECT_EQ(result.issues().front().field(), std::string_view("items[0].value"));
        EXPECT_EQ(result.issues().back().field(), std::string_view("items[79].value"));
    }
}

namespace {

RUVIA_MODEL(email_rule_model,
    RUVIA_REQUIRED_FIELD(mail, ruvia::string, RUVIA_EMAIL("invalid email")));

}  // namespace

TEST(Ruvia, model_email_rule_rejects_empty_domain_labels_and_multiple_at_signs) {
    for (const auto* valid : {
             R"({"mail":"user@example.com"})",
             R"({"mail":"first.last@sub.example.co.uk"})",
             R"({"mail":"用户@例子.测试"})",
         }) {
        const auto parsed = ruvia::from_json<email_rule_model>(valid);
        validator result;
        ruvia::validate_model(parsed, result);
        EXPECT_TRUE(result.ok());
    }

    for (const auto* invalid : {
             R"({"mail":"a@.b"})",
             R"({"mail":"a@b.c."})",
             R"({"mail":"a@b..c"})",
             R"({"mail":"a@@b.c"})",
             R"({"mail":"a@b@c.d"})",
             R"({"mail":"@example.com"})",
             R"({"mail":"user@example"})",
         }) {
        bool rejected = false;
        try {
            (void)ruvia::from_json<email_rule_model>(invalid);
        } catch (const ruvia::http_error& error) {
            rejected = true;
            EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
            if (error.validation_issues().size() == 1) {
                EXPECT_EQ(error.validation_issues()[0].field(), std::string_view("mail"));
                EXPECT_EQ(error.validation_issues()[0].code(), std::string_view("email"));
            }
        }
        EXPECT_TRUE(rejected);
    }
}

namespace {
RUVIA_MODEL(narrow_values,
    RUVIA_REQUIRED_FIELD(i8, ruvia::int8),
    RUVIA_REQUIRED_FIELD(u8, ruvia::uint8),
    RUVIA_REQUIRED_FIELD(i16, ruvia::int16),
    RUVIA_REQUIRED_FIELD(u16, ruvia::uint16),
    RUVIA_REQUIRED_FIELD(real, ruvia::double_value));
RUVIA_MODEL(narrow_nullable, RUVIA_OPTIONAL_FIELD(value, ruvia::uint8, RUVIA_NULLABLE));
RUVIA_MODEL(wide_values,
    RUVIA_OPTIONAL_FIELD(i32, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(u32, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(i64, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(u64, ruvia::uint64));
RUVIA_MODEL(floating_nullable,
    RUVIA_OPTIONAL_FIELD(single, ruvia::float_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(real, ruvia::double_value, RUVIA_NULLABLE));

struct convertible_real final {
    constexpr operator float() const noexcept {
        return 1.25f;
    }
    constexpr operator double() const noexcept {
        return 2.5;
    }
};
}  // namespace

TEST(Ruvia, model_narrow_value_types_preserve_boundaries) {
    EXPECT_EQ(ruvia::int8(-128).value_, std::int8_t{-128});
    EXPECT_EQ(ruvia::uint8(255).value_, std::uint8_t{255});
    EXPECT_EQ(ruvia::int16(-32768).value_, std::int16_t{-32768});
    EXPECT_EQ(ruvia::uint16(65535).value_, std::uint16_t{65535});
    bool rejected = false;
    try {
        (void)ruvia::int8(128);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    rejected = false;
    try {
        (void)ruvia::uint8(-1);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    rejected = false;
    try {
        (void)ruvia::int16(32768);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    rejected = false;
    try {
        (void)ruvia::uint16(65536);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, model_narrow_values_parse_json_form_and_reject_float_as_integer) {
    auto json = ruvia::from_json<narrow_values>(R"({"i8":-128,"u8":255,"i16":-32768,"u16":65535,"real":1.5})");
    EXPECT_EQ(json.get<"i8">().value_, std::int8_t{-128});
    EXPECT_EQ(json.get<"u16">().value_, std::uint16_t{65535});
    EXPECT_EQ(json.get<"real">().value_, 1.5);
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<narrow_values>(R"({"i8":1.5,"u8":2,"i16":3,"u16":4,"real":1})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int8>("128"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int8>("-129"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint8>("-1"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint8>("256"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int16>("32768"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int16>("-32769"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint16>("65536"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::uint16>("-1"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_form<narrow_values>("i8=128&u8=255&i16=-32768&u16=65535&real=1.5");
    }));
    auto form = ruvia::from_form<narrow_values>("i8=-128&u8=255&i16=-32768&u16=65535&real=1.5");
    EXPECT_EQ(form.get<"i8">().value_, std::int8_t{-128});
}

TEST(Ruvia, model_narrow_assignment_checks_native_and_model_scalars_before_replacing_values) {
    narrow_nullable model;
    model.set<"value">(7);
    const auto check_rejected = [&](auto input) {
        bool rejected = false;
        try {
            model.set<"value">(input);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(model.get<"value">()->value_, std::uint8_t{7});
        EXPECT_TRUE(!model.is_null<"value">());
        EXPECT_TRUE(!model.is_present<"value">());
    };
    check_rejected(-1);
    check_rejected(256);
    check_rejected(ruvia::int32{-1});
    check_rejected(ruvia::uint64{256});
    auto parsed_value = ruvia::from_json<narrow_nullable>(R"({"value":null})");
    bool rejected = false;
    try {
        parsed_value.set<"value">(ruvia::int64{-1});
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(parsed_value.is_present<"value">() && parsed_value.is_null<"value">());
}

TEST(Ruvia, model_integer_assignment_rejects_out_of_range_without_replacing_values) {
    wide_values model;
    model.set<"i32">(7).set<"u32">(7).set<"i64">(7).set<"u64">(7);
    const auto original = ruvia::to_json(model);
    const auto check_rejected = [&](auto assign) {
        bool rejected = false;
        try {
            assign();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(ruvia::to_json(model), original);
    };
    check_rejected([&] { model.set<"i32">(std::int64_t{2147483648}); });
    check_rejected([&] { model.set<"i32">(std::int64_t{-2147483649}); });
    check_rejected([&] { model.set<"i32">(ruvia::uint64{4294967296}); });
    check_rejected([&] { model.set<"u32">(-1); });
    check_rejected([&] { model.set<"u32">(std::uint64_t{4294967296}); });
    check_rejected([&] { model.set<"u32">(ruvia::int64{-1}); });
    check_rejected([&] { model.set<"i64">((std::numeric_limits<std::uint64_t>::max)()); });
    check_rejected([&] { model.set<"i64">(ruvia::uint64{(std::numeric_limits<std::uint64_t>::max)()}); });
    check_rejected([&] { model.set<"u64">(-1); });
    check_rejected([&] { model.set<"u64">(ruvia::int64{-1}); });
}

TEST(Ruvia, model_floating_construction_rejects_finite_overflow_before_narrowing) {
    const auto check_rejected = [&](auto construct) {
        bool rejected = false;
        try {
            construct();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    };
    const double above_float_max = std::nextafter(
        static_cast<double>((std::numeric_limits<float>::max)()), std::numeric_limits<double>::infinity());
    check_rejected([&] { (void)ruvia::float_value(above_float_max); });
    check_rejected([&] { (void)ruvia::float_value(ruvia::double_value{-above_float_max}); });
    const long double above_float_max_extended = std::nextafter(
        static_cast<long double>((std::numeric_limits<float>::max)()), std::numeric_limits<long double>::infinity());
    check_rejected([&] { (void)ruvia::float_value(above_float_max_extended); });
    if constexpr ((std::numeric_limits<long double>::max)() > (std::numeric_limits<double>::max)()) {
        const long double above_double_max = std::nextafter(
            static_cast<long double>((std::numeric_limits<double>::max)()), std::numeric_limits<long double>::infinity());
        check_rejected([&] { (void)ruvia::double_value(above_double_max); });
        check_rejected([&] { (void)ruvia::double_value(-above_double_max); });
    }
}

TEST(Ruvia, model_floating_assignment_rejects_overflow_without_replacing_values_or_null) {
    auto value = ruvia::from_json<floating_nullable>(R"({"single":7,"real":9})");
    auto null_value = ruvia::from_json<floating_nullable>(R"({"single":null,"real":null})");
    const auto check_rejected = [&](auto assign) {
        for (auto* model : {&value, &null_value}) {
            bool rejected = false;
            try {
                assign(*model);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            EXPECT_TRUE(rejected);
        }
        EXPECT_EQ(value.get<"single">()->value_, 7.0f);
        EXPECT_EQ(value.get<"real">()->value_, 9.0);
        EXPECT_TRUE(!value.is_null<"single">() && !value.is_null<"real">());
        EXPECT_TRUE(value.is_present<"single">() && value.is_present<"real">());
        EXPECT_TRUE(null_value.is_null<"single">() && null_value.is_null<"real">());
        EXPECT_TRUE(null_value.is_present<"single">() && null_value.is_present<"real">());
        EXPECT_EQ(ruvia::to_json(null_value), R"({"single":null,"real":null})");
    };
    check_rejected([](auto& model) { model.template set<"single">((std::numeric_limits<double>::max)()); });
    check_rejected([](auto& model) { model.template set<"single">(ruvia::double_value{-(std::numeric_limits<double>::max)()}); });
    if constexpr ((std::numeric_limits<long double>::max)() > (std::numeric_limits<double>::max)()) {
        check_rejected([](auto& model) { model.template set<"real">((std::numeric_limits<long double>::max)()); });
    }
}

TEST(Ruvia, model_floating_construction_preserves_boundaries_rounding_and_scalar_inputs) {
    constexpr auto float_max = (std::numeric_limits<float>::max)();
    constexpr ruvia::float_value positive{static_cast<double>(float_max)};
    constexpr ruvia::float_value negative{ruvia::double_value{-static_cast<double>(float_max)}};
    constexpr ruvia::float_value rounded{1.0 + std::numeric_limits<float>::epsilon() / 4.0};
    constexpr ruvia::double_value double_boundary{static_cast<long double>((std::numeric_limits<double>::max)())};
    constexpr ruvia::double_value double_rounded{1.0L + std::numeric_limits<double>::epsilon() / 4.0L};
    EXPECT_EQ(positive.value_, float_max);
    EXPECT_EQ(negative.value_, -float_max);
    EXPECT_EQ(rounded.value_, 1.0f);
    EXPECT_EQ(double_boundary.value_, (std::numeric_limits<double>::max)());
    EXPECT_EQ(double_rounded.value_, 1.0);
    EXPECT_EQ(ruvia::float_value(17).value_, 17.0f);
    EXPECT_EQ(ruvia::double_value(ruvia::int32{-17}).value_, -17.0);
    EXPECT_EQ(ruvia::float_value(true).value_, 1.0f);
    EXPECT_EQ(ruvia::double_value(ruvia::bool_value{false}).value_, 0.0);
    EXPECT_EQ(ruvia::double_value(ruvia::float_value{1.25f}).value_, 1.25);
    constexpr ruvia::float_value custom_float{convertible_real{}};
    constexpr ruvia::double_value custom_double{convertible_real{}};
    EXPECT_EQ(custom_float.value_, 1.25f);
    EXPECT_EQ(custom_double.value_, 2.5);
}

TEST(Ruvia, model_floating_construction_and_assignment_preserve_explicit_nonfinite_inputs) {
    const auto infinity = std::numeric_limits<double>::infinity();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_EQ(ruvia::float_value(infinity).value_, std::numeric_limits<float>::infinity());
    EXPECT_EQ(ruvia::float_value(ruvia::double_value{-infinity}).value_, -std::numeric_limits<float>::infinity());
    EXPECT_TRUE(std::isnan(ruvia::float_value(ruvia::double_value{nan}).value_));
    EXPECT_EQ(ruvia::double_value(std::numeric_limits<long double>::infinity()).value_, infinity);
    EXPECT_TRUE(std::isnan(ruvia::double_value(std::numeric_limits<long double>::quiet_NaN()).value_));
    floating_nullable model;
    model.set<"single">(ruvia::double_value{infinity});
    model.set<"real">(nan);
    EXPECT_TRUE(std::isinf(model.get<"single">()->value_));
    EXPECT_TRUE(std::isnan(model.get<"real">()->value_));
    EXPECT_TRUE(!model.is_null<"single">() && !model.is_null<"real">());
    EXPECT_EQ(ruvia::to_json(model), R"({"single":null,"real":null})");
}

TEST(Ruvia, model_bytes_own_and_compare_without_allocator_identity) {
    std::byte input[] = {std::byte{0x01}, std::byte{0x02}};
    const auto bytes_value = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input), 2);
    ruvia::bytes first(bytes_value);
    ruvia::bytes second(bytes_value);

    EXPECT_TRUE(first == second);
    first.assign_owned(std::span<const std::uint8_t>{});
    EXPECT_TRUE(first.empty());
}

TEST(Ruvia, model_array_and_boxed_array_compare_values_in_order) {
    ruvia::array<ruvia::int16> first;
    ruvia::array<ruvia::int16> second;
    first.emplace_back(1);
    first.emplace_back(2);
    second.emplace_back(1);
    second.emplace_back(2);
    EXPECT_TRUE(first == second);
    second[1] = ruvia::int16(3);
    EXPECT_TRUE(!(first == second));
}

TEST(Ruvia, integer_decimal_limits) {
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<std::int64_t>("-9223372036854775808")), (std::numeric_limits<std::int64_t>::min)());
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<std::uint64_t>("18446744073709551615")), (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<int>("00042")), 42);
    EXPECT_EQ(std::get<0>(ruvia::parse_integer<int>("-0")), 0);
    for (auto text : {"128", "-129"}) {
        EXPECT_EQ(std::get<1>(ruvia::parse_integer<std::int8_t>(text)), ruvia::integer_parse_error::out_of_range);
    }
    EXPECT_EQ(std::get<1>(ruvia::parse_integer<std::uint64_t>("18446744073709551616")), ruvia::integer_parse_error::out_of_range);
}

TEST(Ruvia, integer_requires_complete_decimal_input) {
    for (auto text : {"", " ", "+1", " 1", "1 ", "1a", "0x10", "-", "1.0", "999999999999999999999999x"}) {
        EXPECT_EQ(std::get<1>(ruvia::parse_integer<int>(text)), ruvia::integer_parse_error::invalid_format);
    }
    EXPECT_EQ(std::get<1>(ruvia::parse_integer<unsigned>("-1")), ruvia::integer_parse_error::invalid_format);
    EXPECT_EQ(std::get<1>(ruvia::parse_integer<int>(std::string_view("12\0x", 4))), ruvia::integer_parse_error::invalid_format);
}

TEST(Ruvia, model_string_public_construction_owns_input) {
    std::string input(128, 'a');
    ruvia::string value(input);
    input.assign(input.size(), 'b');
    const std::string expected(128, 'a');

    EXPECT_EQ(value.view(), std::string_view(expected));
}

TEST(Ruvia, model_string_owned_assignment_is_alias_safe) {
    ruvia::string value(std::string(128, 'd'));
    const auto alias = value.view().substr(31, 64);

    value.assign_owned(alias);
    const std::string expected(64, 'd');

    EXPECT_EQ(value.view(), std::string_view(expected));
}

TEST(Ruvia, model_string_move_assignment_preserves_values_and_reclaims_storage) {
    const std::string source_text(128, 's');
    const std::string target_text(128, 't');
    ruvia::string source_value(source_text);
    ruvia::string target(target_text);

    target = std::move(source_value);
    EXPECT_EQ(target.view(), std::string_view(source_text));

    source_value.assign_owned(std::string(128, 'm'));
    const std::string moved_from_text(128, 'm');
    EXPECT_EQ(source_value.view(), std::string_view(moved_from_text));
}

TEST(Ruvia, model_string_assign_owned_rvalue_steals_storage) {
    const std::string expected(200, 'a');
    std::string input(expected);
    const char* const original_data = input.data();
    ruvia::string target;
    target.assign_owned(std::move(input));
    EXPECT_EQ(target.data(), original_data);
    EXPECT_EQ(target.view(), std::string_view(expected));
}

namespace {
RUVIA_MODEL(binary_model,
    RUVIA_REQUIRED_FIELD(bytes, ruvia::bytes),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::bytes, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::bytes, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(binary_array_model,
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::bytes>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<ruvia::bytes>));

void check_bytes(const ruvia::bytes& actual, std::span<const std::uint8_t> expected) {
    EXPECT_EQ(actual.size(), expected.size());
    EXPECT_TRUE(std::equal(actual.view().begin(), actual.view().end(), expected.begin()));
}
}  // namespace

TEST(Ruvia, model_json_bytes_roundtrip_empty_and_all_octets) {
    std::vector<std::uint8_t> octets(256);
    for (std::size_t i = 0; i < octets.size(); ++i) {
        octets[i] = static_cast<std::uint8_t>(i);
    }
    const auto encoded = ruvia::to_json(ruvia::bytes(std::span<const std::uint8_t>(octets)));
    EXPECT_TRUE(encoded.front() == '"' && encoded.back() == '"');
    EXPECT_TRUE(encoded.find("AAECAwQF") != std::string::npos);
    EXPECT_TRUE(encoded.find("/w==") != std::string::npos);
    auto parsed_value = ruvia::from_json<ruvia::bytes>(encoded);
    check_bytes(parsed_value, octets);
    auto empty = ruvia::from_json<ruvia::bytes>(R"("")");
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(std::string_view(ruvia::to_json(ruvia::bytes{})), R"("")");
}

TEST(Ruvia, model_json_bytes_accepts_padding_and_rejects_noncanonical_inputs) {
    auto one = ruvia::from_json<ruvia::bytes>(R"("Zg==")");
    auto two = ruvia::from_json<ruvia::bytes>(R"("Zm8=")");
    EXPECT_TRUE(one.size() == 1 && two.size() == 2);
    EXPECT_EQ(one.view().front(), std::uint8_t{0x66});
    EXPECT_EQ(two.view().back(), std::uint8_t{0x6f});
    auto escaped = ruvia::from_json<ruvia::bytes>(R"("\u005ag\u003d=")");
    EXPECT_EQ(escaped.view().front(), std::uint8_t{0x66});
    for (const auto input : {R"("Zg")", R"("Zh==")", R"("Zm=9")", R"("Zm9v=")",
             R"("Zm-v")", R"("Zm_v")", R"("Zm9v\n")", R"("Zm9=")", R"("====")",
             R"("Zg==AAAA")", R"("AA== ")", "null", "[1]", "123"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::bytes>(input); }));
    }
}

TEST(Ruvia, model_json_bytes_supports_nested_arrays_nullable_and_omit_empty) {
    auto nested = ruvia::from_json<binary_array_model>(R"({"items":["AQI=",""],"boxed":["/w=="]})");
    EXPECT_EQ(nested.get<"items">().size(), std::size_t{2});
    EXPECT_EQ(nested.get<"items">()[0].view().size(), std::size_t{2});
    EXPECT_EQ(nested.get<"boxed">().front().view().front(), std::uint8_t{255});
    auto model = ruvia::from_json<binary_model>(R"({"bytes":"AQI=","optional":null,"empty":""})");
    EXPECT_TRUE(model.is_null<"optional">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), R"({"bytes":"AQI=","optional":null})");
}

TEST(Ruvia, model_bytes_set_and_move_preserve_values) {
    binary_model model;
    const std::uint8_t raw[] = {1, 2, 3};
    model.set<"bytes">(std::span<const std::uint8_t>(raw));
    std::vector<std::uint8_t> owned({4, 5});
    model.set<"bytes">(std::move(owned));
    EXPECT_EQ(model.get<"bytes">().view()[1], std::uint8_t{5});
}

TEST(Ruvia, model_bytes_repeated_operations_reclaim_storage_and_keep_results) {
    {
        binary_model retained;
        {
            {
                std::vector<std::uint8_t> input(256, 0xff);
                retained.set<"bytes">(std::move(input));
            }
        }
        const auto saved = ruvia::to_json(retained.get<"bytes">());
        for (int i = 0; i < 32; ++i) {
            {
                auto parsed_value = ruvia::from_json<ruvia::bytes>(saved);
                EXPECT_TRUE(parsed_value == retained.get<"bytes">());
                const auto output = ruvia::to_json(parsed_value);
                EXPECT_EQ(output, saved);
                EXPECT_TRUE(ruvia::test::throws_on([&] {
                    (void)ruvia::from_json<ruvia::array<ruvia::bytes>>(
                        R"(["AQIDBA==","Zh=="])");
                }));
            }
            EXPECT_EQ(retained.get<"bytes">().view().front(), std::uint8_t{255});
        }
        std::vector<std::uint8_t> compatible(128, 7);
        retained.set<"bytes">(std::move(compatible));
        retained.ensure<"bytes">().assign_owned(retained.get<"bytes">().view().subspan(1));
        EXPECT_EQ(retained.get<"bytes">().size(), std::size_t{127});
        EXPECT_EQ(retained.get<"bytes">().view().back(), std::uint8_t{7});
        EXPECT_TRUE(saved.size() > 256);
    }
}

namespace {

class tracked_value final {
public:
    explicit tracked_value(int value) noexcept
        : value_(value) {
        ++alive_;
    }

    tracked_value(const tracked_value&) = delete;
    tracked_value& operator=(const tracked_value&) = delete;

    tracked_value(tracked_value&& other) noexcept
        : value_(other.value_) {
        other.value_ = -1;
        ++alive_;
    }

    tracked_value& operator=(tracked_value&&) = delete;

    ~tracked_value() {
        --alive_;
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    [[nodiscard]] static std::size_t alive() noexcept {
        return alive_;
    }

private:
    int value_;
    static inline std::size_t alive_{0};
};

}  // namespace

TEST(Ruvia, model_list_clear_and_destructor_release_owned_elements) {
    {
        ruvia::boxed_array<tracked_value> values;
        values.emplace_back(1);
        values.emplace_back(2);
        EXPECT_EQ(tracked_value::alive(), std::size_t{2});

        values.clear();
        EXPECT_TRUE(values.empty());
        EXPECT_EQ(tracked_value::alive(), std::size_t{0});

        values.emplace_back(3);
        EXPECT_EQ(tracked_value::alive(), std::size_t{1});
    }

    EXPECT_EQ(tracked_value::alive(), std::size_t{0});
}

TEST(Ruvia, model_list_move_assignment_keeps_element_resource_owner) {
    {
        ruvia::boxed_array<tracked_value> source_value;
        source_value.emplace_back(4);
        source_value.emplace_back(5);

        ruvia::boxed_array<tracked_value> target;
        target.emplace_back(9);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});

        target = std::move(source_value);
        EXPECT_EQ(target.size(), std::size_t{2});
        EXPECT_EQ(target[0].value(), 4);
        EXPECT_EQ(target[1].value(), 5);
        EXPECT_TRUE(source_value.empty());
        EXPECT_EQ(tracked_value::alive(), std::size_t{2});

        source_value.emplace_back(6);
        EXPECT_EQ(source_value.front().value(), 6);
        EXPECT_EQ(tracked_value::alive(), std::size_t{3});
    }

    EXPECT_EQ(tracked_value::alive(), std::size_t{0});
}

TEST(Ruvia, model_boxed_array_supports_standard_range_algorithms) {
    ruvia::boxed_array<tracked_value> values;
    EXPECT_TRUE(std::ranges::find_if(values, [](const auto&) { return true; }) == values.end());
    EXPECT_EQ(std::ranges::distance(values), std::ptrdiff_t{0});
    values.emplace_back(1);
    values.emplace_back(2);
    values.emplace_back(3);

    const auto found = std::ranges::find_if(values, [](const auto& value) { return value.value() == 2; });
    EXPECT_TRUE(found != values.end());
    if (found != values.end()) {
        EXPECT_TRUE(&*found == &values[1]);
    }
    EXPECT_EQ(std::ranges::distance(values), std::ptrdiff_t{3});
    const auto& constant_values = std::as_const(values);
    EXPECT_EQ(std::ranges::count_if(constant_values,
                  [](const auto& value) { return value.value() % 2 != 0; }),
        std::ptrdiff_t{2});
    EXPECT_TRUE(std::ranges::find_if(constant_values,
                    [](const auto& value) { return value.value() == 4; }) == constant_values.end());
}

RUVIA_MODEL(wrapped_scalars, RUVIA_OPTIONAL_FIELD(count, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::double_value), RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(delta, ruvia::int64));

RUVIA_MODEL(wrapped_scalars_response, RUVIA_OPTIONAL_FIELD(count, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(ratio, ruvia::double_value), RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(delta, ruvia::int64));

RUVIA_MODEL(
    wrapped_defaulted, RUVIA_OPTIONAL_FIELD(retries, ruvia::uint32, RUVIA_DEFAULT(3)));

TEST(Ruvia, model_wrapper_scalar_fields_parse_json_and_forms) {
    constexpr std::string_view body = R"({"count":36,"ratio":9.5,"enabled":true,"delta":-7})";

    const auto wrapped = ruvia::from_json<wrapped_scalars>(body);
    EXPECT_EQ(std::uint32_t(wrapped.get<"count">().value()), std::uint32_t{36});
    EXPECT_TRUE(double(wrapped.get<"ratio">().value()) == 9.5);
    EXPECT_TRUE(bool(wrapped.get<"enabled">().value()));
    EXPECT_EQ(std::int64_t(wrapped.get<"delta">().value()), std::int64_t{-7});

    const auto form = ruvia::from_form<wrapped_scalars>(
        "count=41&ratio=2.5&enabled=true&delta=-9");
    EXPECT_EQ(std::uint32_t(form.get<"count">().value()), std::uint32_t{41});
    EXPECT_TRUE(double(form.get<"ratio">().value()) == 2.5);
    EXPECT_TRUE(bool(form.get<"enabled">().value()));
}

TEST(Ruvia, model_wrapper_scalar_fields_reject_mistyped_values_but_allow_missing_optional_fields) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>(R"({"count":"nan"})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>(R"({"count":null})");
    }));

    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<wrapped_scalars>("{}");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_form<wrapped_scalars>("count=not-a-number");
    }));
}

TEST(Ruvia, model_wrapper_scalar_fields_round_trip_through_json) {
    const auto parsed_value = ruvia::from_json<wrapped_scalars>(
        R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})");

    wrapped_scalars_response response;
    response.set<"count">(*parsed_value.get<"count">())
        .set<"ratio">(*parsed_value.get<"ratio">())
        .set<"enabled">(*parsed_value.get<"enabled">())
        .set<"delta">(*parsed_value.get<"delta">());
    const auto json = ruvia::to_json(response);
    EXPECT_EQ(std::string_view(json),
        std::string_view(R"({"count":1,"ratio":2.5,"enabled":false,"delta":-3})"));
}

TEST(Ruvia, model_wrapper_scalar_fields_apply_defaults) {
    const auto defaulted = ruvia::from_json<wrapped_defaulted>("{}");
    EXPECT_EQ(std::uint32_t(defaulted.get<"retries">().value()), std::uint32_t{3});
}

namespace {

RUVIA_MODEL(comparable_model,
    RUVIA_OPTIONAL_FIELD(number, ruvia::int32, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(text, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(comparable_tree,
    RUVIA_REQUIRED_FIELD(value, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<comparable_model>),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<comparable_tree>));

}  // namespace

TEST(Ruvia, model_equality_compares_state_and_value_but_not_presence) {
    comparable_model missing;
    comparable_model expected_value;
    expected_value.set<"number">(7);
    EXPECT_TRUE(missing != expected_value);

    auto parsed_value = ruvia::from_json<comparable_model>(R"({"number":7})");
    EXPECT_TRUE(parsed_value.is_present<"number">());
    EXPECT_TRUE(parsed_value == expected_value);

    expected_value.set<"number">(8);
    EXPECT_TRUE(parsed_value != expected_value);
    expected_value.set<"number">(nullptr);
    EXPECT_TRUE(parsed_value != expected_value);
}

TEST(Ruvia, model_equality_distinguishes_missing_and_null) {
    comparable_model missing;
    comparable_model null_value;
    null_value.set<"number">(nullptr);
    EXPECT_TRUE(missing != null_value);

    missing.set<"number">(nullptr);
    EXPECT_TRUE(missing == null_value);
}

TEST(Ruvia, model_equality_ignores_resources_for_normalized_values) {
    comparable_model left;
    comparable_model right;
    left.set<"text">(std::string_view("same value"));
    right.set<"text">(std::string_view("same value"));
    EXPECT_TRUE(left == right);

    right.set<"text">(std::string_view("different"));
    EXPECT_TRUE(left != right);
}

TEST(Ruvia, model_equality_compares_nested_arrays_and_recursive_boxed_values) {
    constexpr auto input = R"({"value":1,"items":[{"text":"a"},{"text":"b"}],"children":[{"value":2,"children":[{"value":3}]}]})";
    {
        auto first = ruvia::from_json<comparable_tree>(input);
        auto second = ruvia::from_json<comparable_tree>(input);
        EXPECT_TRUE(first == second);
        second.ensure<"items">()[0].set<"text">("b");
        EXPECT_TRUE(first != second);
        second.ensure<"items">()[0].set<"text">("a");
        EXPECT_TRUE(first == second);
        second.ensure<"children">().front().ensure<"children">().front().set<"value">(4);
        EXPECT_TRUE(first != second);
    }
}

namespace {

int evaluated_default() {
    ++evaluations;
    return 7;
}

int failing_default_value() {
    ++evaluations;
    throw std::runtime_error("default evaluation failed");
}

std::string owned_default_value() {
    ++evaluations;
    return std::string(128, 'd');
}

ruvia::json_value dynamic_null_default() {
    ++evaluations;
    return ruvia::json_value::parse("null");
}

RUVIA_MODEL(optional_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_DEFAULT(evaluated_default()), RUVIA_MIN(5, "too small")));
RUVIA_MODEL(nullable_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_NULLABLE, RUVIA_DEFAULT(evaluated_default())));
RUVIA_MODEL(null_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_NULLABLE, RUVIA_DEFAULT(nullptr)));
RUVIA_MODEL(nullable_dynamic_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE, RUVIA_DEFAULT(dynamic_null_default())));
RUVIA_MODEL(nonnullable_dynamic_default,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_DEFAULT(dynamic_null_default())));
RUVIA_MODEL(failing_default_model,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_DEFAULT(failing_default_value())));
RUVIA_MODEL(owned_default_model,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT(owned_default_value())));

}  // namespace

TEST(Ruvia, model_default_expression_is_not_evaluated_for_supplied_or_invalid_input) {
    evaluations = 0;
    optional_default constructed;
    EXPECT_TRUE(!constructed.get<"value">());
    EXPECT_EQ(evaluations, std::size_t{0});
    const auto valid_json = ruvia::from_json<optional_default>(R"({"value":9})");
    EXPECT_EQ(evaluations, std::size_t{0});
    EXPECT_EQ(valid_json.get<"value">()->value_, 9);
    for (auto input : {R"({"value":null})", R"({"value":"wrong"})",
             R"({"value":8,"value":9})", R"({"value":)"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<optional_default>(input); }));
        EXPECT_EQ(evaluations, std::size_t{0});
    }
    auto null = ruvia::from_json<nullable_default>(R"({"value":null})");
    EXPECT_TRUE(null.is_null<"value">());
    EXPECT_EQ(evaluations, std::size_t{0});
    const auto valid_form = ruvia::from_form<optional_default>("value=9");
    EXPECT_EQ(valid_form.get<"value">()->value_, 9);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<optional_default>("value=wrong"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<optional_default>("value=8&value=9"); }));
    EXPECT_EQ(evaluations, std::size_t{0});
}

TEST(Ruvia, model_default_expression_runs_once_for_absence_not_for_moves) {
    evaluations = 0;
    auto parsed_value = ruvia::from_json<optional_default>("{}");
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_TRUE(!parsed_value.is_present<"value">());
    EXPECT_EQ(parsed_value.get<"value">()->value_, std::int32_t{7});
    optional_default moved(std::move(parsed_value));
    optional_default assigned;
    assigned = std::move(moved);
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(assigned.get<"value">()->value_, std::int32_t{7});
    assigned.reset<"value">();
    EXPECT_TRUE(!assigned.get<"value">());
    EXPECT_EQ(evaluations, std::size_t{1});
    const auto form = ruvia::from_form<optional_default>("");
    EXPECT_TRUE(!form.is_present<"value">() && form.get<"value">()->value_ == 7);
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_default_expression_exceptions_unwind_partial_owned_fields) {
    evaluations = 0;
    const std::string prefix("{\"name\":\"" + std::string(128, 'n') + "\"");
    {
        auto supplied = ruvia::from_json<failing_default_model>(std::string(prefix) + ",\"value\":9}");
        EXPECT_EQ(supplied.get<"value">()->value_, 9);
        EXPECT_EQ(evaluations, std::size_t{0});
    }
    for (std::size_t index = 0; index < 32; ++index) {
        bool failed = false;
        try {
            (void)ruvia::from_json<failing_default_model>(std::string(prefix) + "}");
        } catch (const std::runtime_error& error) {
            failed = std::string_view(error.what()) == "default evaluation failed";
        }
        EXPECT_TRUE(failed);
        EXPECT_EQ(evaluations, index + 1);
    }
}

TEST(Ruvia, model_default_assignment_obeys_field_nullability) {
    evaluations = 0;
    auto null = ruvia::from_json<null_default>("{}");
    EXPECT_TRUE(null.is_null<"value">() && !null.is_present<"value">());
    auto supplied = ruvia::from_json<null_default>(R"({"value":9})");
    EXPECT_TRUE(!supplied.is_null<"value">() && supplied.get<"value">()->value_ == 9);
    auto dynamic = ruvia::from_json<nullable_dynamic_default>("{}");
    EXPECT_TRUE(dynamic.is_null<"value">() && !dynamic.is_present<"value">());
    EXPECT_EQ(evaluations, std::size_t{1});
    auto valid = ruvia::from_json<nonnullable_dynamic_default>(R"({"value":1})");
    EXPECT_TRUE(!valid.is_null<"value">());
    EXPECT_EQ(evaluations, std::size_t{1});
    bool rejected = false;
    try {
        (void)ruvia::from_json<nonnullable_dynamic_default>("{}");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_owned_default_is_normalized_to_the_model_resource) {
    evaluations = 0;
    {
        owned_default_model retained;
        {
            auto parsed_value = ruvia::from_json<owned_default_model>("{}");
            EXPECT_EQ(evaluations, std::size_t{1});
            retained = std::move(parsed_value);
        }
        EXPECT_EQ(evaluations, std::size_t{1});
        EXPECT_EQ(retained.get<"value">()->view(), std::string_view(std::string(128, 'd')));
        EXPECT_TRUE(!retained.is_present<"value">());
        retained.reset<"value">();
    }
    EXPECT_EQ(evaluations, std::size_t{1});
}

namespace {

RUVIA_MODEL(remark_request, RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE));

RUVIA_MODEL(remark_with_default_request,
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));

RUVIA_MODEL(required_remark_request, RUVIA_REQUIRED_FIELD(remark, ruvia::string));

RUVIA_MODEL(json_bag_request, RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<ruvia::json_value>));

RUVIA_MODEL(json_bag_response, RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object));

RUVIA_MODEL(emission_options,
    RUVIA_OPTIONAL_FIELD_NAME("p\"lain", plain, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(omitted, ruvia::string, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(emitted, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
    RUVIA_OPTIONAL_FIELD(both, ruvia::string, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, RUVIA_EMIT_NULL));

}  // namespace

TEST(Ruvia, nullable_optional_string_accepts_json_null) {
    const auto parsed_value =
        ruvia::from_json<remark_request>(R"({"remark":null})");
    EXPECT_TRUE(!parsed_value.get<"remark">().has_value());
    EXPECT_TRUE(parsed_value.is_present<"remark">());
    EXPECT_TRUE(parsed_value.is_null<"remark">());
}

TEST(Ruvia, nullable_optional_string_still_accepts_text) {
    const auto parsed_value =
        ruvia::from_json<remark_request>(R"({"remark":"note"})");
    if (!parsed_value.get<"remark">()) {
        return;
    }
    EXPECT_EQ(parsed_value.get<"remark">()->view(), std::string_view("note"));
}

TEST(Ruvia, nullable_null_does_not_apply_default) {
    const auto parsed_value = ruvia::from_json<remark_with_default_request>(
        R"({"remark":null})");
    EXPECT_TRUE(!parsed_value.get<"remark">().has_value());
    EXPECT_TRUE(parsed_value.is_present<"remark">());
    EXPECT_TRUE(parsed_value.is_null<"remark">());

    const auto missing =
        ruvia::from_json<remark_with_default_request>("{}");
    if (!missing.get<"remark">()) {
        return;
    }
    EXPECT_EQ(missing.get<"remark">()->view(), std::string_view("fallback"));
}

TEST(Ruvia, required_string_rejects_json_null) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<required_remark_request>(R"({"remark":null})");
    }));
}

TEST(Ruvia, json_value_model_fields_accept_any_json_token) {
    const auto parsed_value = ruvia::from_json<json_bag_request>(
        R"({"payload":[1,{"k":null}],"object":{"a":true},"items":[null,"x",{"z":2}]})");
    if (!parsed_value.get<"payload">() || !parsed_value.get<"object">() ||
        !parsed_value.get<"items">()) {
        return;
    }
    EXPECT_TRUE(parsed_value.get<"payload">()->is_array());
    EXPECT_EQ(parsed_value.get<"payload">()->view(), std::string_view(R"([1,{"k":null}])"));
    EXPECT_EQ(parsed_value.get<"object">()->view(), std::string_view(R"({"a":true})"));
    const auto& items = *parsed_value.get<"items">();
    EXPECT_EQ(items.size(), std::size_t{3});
    EXPECT_TRUE(items[0].is_null());
    EXPECT_TRUE(items[1].is_string());
}

TEST(Ruvia, json_object_model_field_rejects_non_objects) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<json_bag_request>(R"({"object":[1]})");
    }));

    const auto null_object =
        ruvia::from_json<json_bag_request>(R"({"object":null})");
    EXPECT_TRUE(!null_object.get<"object">().has_value());
    EXPECT_TRUE(null_object.is_present<"object">());
    EXPECT_TRUE(null_object.is_null<"object">());
}

TEST(Ruvia, json_value_model_fields_own_tokens_for_from_json) {
    auto parsed_value = ruvia::from_json<json_bag_request>(
        std::string(R"({"payload":{"keep":true}})"));
    if (!parsed_value.get<"payload">()) {
        return;
    }
    EXPECT_TRUE(parsed_value.get<"payload">()->is_object());
    EXPECT_EQ(parsed_value.get<"payload">()->view(), std::string_view(R"({"keep":true})"));
}

TEST(Ruvia, json_value_response_fields_write_raw_tokens) {
    auto parsed_value = ruvia::from_json<json_bag_request>(
        R"({"payload":[1,2],"object":{"a":1}})");
    if (!parsed_value.get<"payload">() || !parsed_value.get<"object">()) {
        return;
    }

    json_bag_response response;
    response.set<"payload">(*parsed_value.get<"payload">());
    response.set<"object">(std::move(parsed_value.ensure<"object">()));
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"payload":[1,2],"object":{"a":1}})"));
}

TEST(Ruvia, model_json_emission_options_write_expected_output_for_all_states) {
    // Each pair of bits selects missing, explicit null, empty, or text. The
    // Cartesian product also covers every comma position after omitted fields.
    constexpr std::string_view names[] = {R"("p\"lain")", R"("omitted")", R"("emitted")", R"("both")"};
    for (unsigned combination = 0; combination != 256; ++combination) {
        emission_options value;
        const auto assign = [&]<ruvia::fixed_string field>(unsigned state_value) {
            if (state_value == 1) {
                value.set<field>(nullptr);
            } else if (state_value == 2) {
                value.set<field>("");
            } else if (state_value == 3) {
                value.set<field>("\"\n");
            }
        };
        assign.template operator()<"plain">(combination & 3);
        assign.template operator()<"omitted">((combination >> 2) & 3);
        assign.template operator()<"emitted">((combination >> 4) & 3);
        assign.template operator()<"both">((combination >> 6) & 3);

        std::string expected("{");
        for (unsigned index = 0; index != 4; ++index) {
            const auto state_value = (combination >> (index * 2)) & 3;
            if ((state_value == 0 && index < 2) || (state_value == 2 && (index == 1 || index == 3))) {
                continue;
            }
            if (expected.size() != 1) {
                expected.push_back(',');
            }
            expected.append(names[index]);
            expected.push_back(':');
            expected.append(state_value < 2 ? "null" : state_value == 2 ? R"("")"
                                                                        : R"("\"\n")");
        }
        expected.push_back('}');
        const auto output = ruvia::to_json(value);
        EXPECT_EQ(std::string_view(output), std::string_view(expected));
    }
}

namespace {

RUVIA_MODEL(required_value, RUVIA_REQUIRED_FIELD(value, ruvia::string));
RUVIA_MODEL(optional_value, RUVIA_OPTIONAL_FIELD(value, ruvia::string));
RUVIA_MODEL(required_nullable_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(optional_nullable_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_MODEL(nullable_default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_MODEL(invalid_default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::uint32, RUVIA_DEFAULT(3), RUVIA_MIN(5, "too small")));
RUVIA_MODEL(named_value,
    RUVIA_OPTIONAL_FIELD_NAME("wire_value", value, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));
RUVIA_MODEL(patch_value,
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(nested_values,
    RUVIA_REQUIRED_FIELD(children, ruvia::array<nullable_default_value>));
RUVIA_MODEL(nullable_kinds,
    RUVIA_REQUIRED_FIELD(flag, ruvia::bool_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(number, ruvia::uint32, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(child, required_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::string>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(boxes, ruvia::boxed_array<required_value>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(payload, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(object, ruvia::json_object, RUVIA_NULLABLE));
RUVIA_MODEL(dynamic_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value));
RUVIA_MODEL(nullable_dynamic_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(required_dynamic_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value));
RUVIA_MODEL(required_nullable_dynamic_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(dynamic_object,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_object));
RUVIA_MODEL(owned_values,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string, RUVIA_DEFAULT("a default string long enough to allocate storage")));
RUVIA_MODEL(nullable_response,
    RUVIA_REQUIRED_FIELD(required, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(dynamic, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(form_unrepresentable_values,
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(data, ruvia::bytes),
    RUVIA_OPTIONAL_FIELD(child, required_value),
    RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string));

// Models spell value presence only as has_value; entity is_set means "assigned,
// null included" and must not appear on models with a different meaning.
template <typename model_type>
concept model_offers_has_value = requires(const model_type& model) { model.template has_value<"optional">(); };
template <typename model_type>
concept model_offers_is_set = requires(const model_type& model) { model.template is_set<"optional">(); };
static_assert(model_offers_has_value<nullable_response>);
static_assert(!model_offers_is_set<nullable_response>);

template <typename model_type>
void check_presence_matrix(bool required, bool nullable_value) {
    for (auto input : {R"({"value":42})", R"({"value":false})", R"({"value":[]})", R"({"value":{}})"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(input); }));
    }
    for (auto input : {R"({"value":null,"value":"note"})",
             R"({"value":"note","value":null})", R"({"value":null,"value":null})",
             R"({"value":42,"value":"note"})", R"({"value":"note","va\u006cue":"again"})"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(input); }));
    }
    EXPECT_EQ(ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>("{}"); }), required);
    EXPECT_EQ(
        ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(R"({"value":null})"); }),
        !nullable_value);
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<model_type>(R"({"value":"note"})"); }));
}

}  // namespace

TEST(Ruvia, model_field_presence_and_nullability_are_independent) {
    check_presence_matrix<required_value>(true, false);
    check_presence_matrix<optional_value>(false, false);
    check_presence_matrix<required_nullable_value>(true, true);
    check_presence_matrix<optional_nullable_value>(false, true);
    auto null = ruvia::from_json<required_nullable_value>(R"({"value":null})");
    auto text = ruvia::from_json<required_nullable_value>(R"({"value":"note"})");
    EXPECT_TRUE(!null.get<"value">());
    EXPECT_TRUE(text.get<"value">()->view() == "note");
}

TEST(Ruvia, model_patch_public_states_distinguish_omission_clear_and_assignment) {
    std::optional<std::string> remark(std::in_place, "original");
    auto apply = [&](std::string_view json) {
        const auto patch = ruvia::from_json<patch_value>(json);
        if (!patch.is_present<"remark">()) {
            return;
        }
        if (patch.is_null<"remark">()) {
            remark.reset();
        } else {
            remark.emplace(patch.get<"remark">()->view());
        }
    };
    apply("{}");
    EXPECT_TRUE(remark == "original");
    apply(R"({"remark":null})");
    EXPECT_TRUE(!remark);
    apply(R"({"remark":"replacement"})");
    EXPECT_TRUE(remark == "replacement");
    apply(R"({"remark":""})");
    EXPECT_TRUE(remark && remark->empty());
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<patch_value>(R"({"enabled":null})"); }));
    const auto disabled = ruvia::from_json<patch_value>(R"({"enabled":false})");
    EXPECT_TRUE(disabled.is_present<"enabled">() && !disabled.is_null<"enabled">());
    EXPECT_TRUE(!bool(*disabled.get<"enabled">()));
}

TEST(Ruvia, model_defaults_only_fill_optional_missing_values) {
    auto missing = ruvia::from_json<nullable_default_value>("{}");
    EXPECT_TRUE(missing.get<"value">()->view() == "fallback");
    EXPECT_TRUE(!missing.is_present<"value">());
    auto form = ruvia::from_form<default_value>("");
    EXPECT_TRUE(!form.is_present<"value">() && form.get<"value">()->view() == "fallback");
    bool default_rule_rejected = false;
    try {
        (void)ruvia::from_form<invalid_default_value>("");
    } catch (const ruvia::http_error&) {
        default_rule_rejected = true;
    }
    EXPECT_TRUE(default_rule_rejected);
}

TEST(Ruvia, model_input_presence_survives_application_mutation_and_moves) {
    auto absent = ruvia::from_json<nullable_default_value>("{}");
    auto present = ruvia::from_json<nullable_default_value>(R"({"value":null})");
    absent.set<"value">("application value");
    EXPECT_TRUE(!absent.is_present<"value">());
    present.ensure<"value">().assign_owned(std::string_view("replacement"));
    EXPECT_TRUE(present.is_present<"value">() && !present.is_null<"value">());
    present.reset<"value">();
    EXPECT_TRUE(present.is_present<"value">() && !present.is_null<"value">());
    present.set<"value">(nullptr);
    nullable_default_value moved(std::move(present));
    EXPECT_TRUE(moved.is_present<"value">() && moved.is_null<"value">());
    EXPECT_TRUE(!present.is_present<"value">() && !present.is_null<"value">());
    absent = std::move(moved);
    EXPECT_TRUE(absent.is_present<"value">() && absent.is_null<"value">());
    EXPECT_TRUE(!moved.is_present<"value">());
}

TEST(Ruvia, model_named_form_and_nested_fields_keep_input_presence) {
    auto named = ruvia::from_json<named_value>(R"({"wire_value":null})");
    EXPECT_TRUE(named.is_present<"value">() && named.is_null<"value">());
    auto text = ruvia::from_form<named_value>("wire_value=null");
    EXPECT_TRUE(text.is_present<"value">() && !text.is_null<"value">());
    EXPECT_TRUE(text.get<"value">()->view() == "null");
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<named_value>("wire_value=a&wire_value=b"); }));
    auto nested = ruvia::from_json<nested_values>(R"({"children":[{},{"value":null},{"value":"note"}]})");
    const auto& children = nested.get<"children">();
    EXPECT_TRUE(!children[0].is_present<"value">() && children[0].get<"value">()->view() == "fallback");
    EXPECT_TRUE(children[1].is_present<"value">() && children[1].is_null<"value">());
    EXPECT_TRUE(children[2].is_present<"value">() && !children[2].is_null<"value">());
}

TEST(Ruvia, model_form_rejects_values_for_fields_without_form_representation) {
    auto absent = ruvia::from_form<form_unrepresentable_values>("note=a&unknown=b");
    EXPECT_TRUE(absent.is_present<"note">() && !absent.is_present<"tags">());
    for (const auto body : {"tags=a", "tags=a&tags=b", "data=a", "child=a", "payload=1", "object=%7B%7D"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<form_unrepresentable_values>(body); }));
    }
}

TEST(Ruvia, model_nullable_applies_to_scalar_container_nested_and_dynamic_types) {
    auto parsed_value = ruvia::from_json<nullable_kinds>(
        R"({"flag":null,"number":null,"child":null,"items":null,"boxes":null,"payload":null,"object":null})");
    EXPECT_TRUE(parsed_value.is_present<"flag">() && parsed_value.is_null<"flag">() && !parsed_value.get<"flag">());
    EXPECT_TRUE(parsed_value.is_null<"number">() && !parsed_value.get<"number">());
    EXPECT_TRUE(parsed_value.is_null<"child">() && !parsed_value.get<"child">());
    EXPECT_TRUE(parsed_value.is_null<"items">() && !parsed_value.get<"items">());
    EXPECT_TRUE(parsed_value.is_null<"boxes">() && !parsed_value.get<"boxes">());
    EXPECT_TRUE(parsed_value.is_null<"payload">() && !parsed_value.get<"payload">());
    EXPECT_TRUE(parsed_value.is_null<"object">() && !parsed_value.get<"object">());
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":[]})"); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":{"nested":null}})"); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":[null,false,1]})"); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":{"nested":null}})"); }));
}

TEST(Ruvia, model_nonnullable_dynamic_assignment_rejects_null_without_changing_value) {
    auto model = ruvia::from_json<dynamic_value>(R"({"value":1})");
    auto null = ruvia::json_value::parse("null");
    bool rejected = false;
    try {
        model.set<"value">(std::move(null));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(model.is_present<"value">());
    EXPECT_TRUE(!model.is_null<"value">());
    EXPECT_EQ(model.get<"value">()->view(), std::string_view("1"));
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":1})"));
    EXPECT_TRUE(null.is_null());
}

TEST(Ruvia, model_nonnullable_dynamic_setter_preserves_absence_on_null_rejection) {
    dynamic_value model;
    bool rejected = false;
    try {
        model.set<"value">(ruvia::json_value::parse("null"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(!model.get<"value">());
    EXPECT_TRUE(!model.is_null<"value">() && !model.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

    model.set<"value">(ruvia::json_value::parse(R"({"nested":null})"));
    EXPECT_EQ(model.get<"value">()->view(), std::string_view(R"({"nested":null})"));
    EXPECT_TRUE(!model.is_null<"value">() && !model.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":{"nested":null}})"));
}

TEST(Ruvia, model_nullable_dynamic_setter_keeps_value_null_and_input_presence_consistent) {
    for (const auto input : {"{}", R"({"value":null})"}) {
        auto model = ruvia::from_json<nullable_dynamic_value>(input);
        const bool present = model.is_present<"value">();
        model.reset<"value">();
        EXPECT_TRUE(!model.get<"value">() && !model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

        model.set<"value">(nullptr);
        EXPECT_TRUE(!model.get<"value">() && model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":null})"));

        model.set<"value">(ruvia::json_value::parse("[null,false,1]"));
        EXPECT_EQ(model.get<"value">()->view(), std::string_view("[null,false,1]"));
        EXPECT_TRUE(!model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":[null,false,1]})"));

        model.reset<"value">();
        EXPECT_TRUE(!model.get<"value">() && !model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

        model.set<"value">(ruvia::json_value::parse("true"));
        const auto null = ruvia::json_value::parse("null");
        model.set<"value">(null);
        EXPECT_TRUE(!model.get<"value">() && model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":null})"));
    }
}

TEST(Ruvia, model_required_dynamic_setters_preserve_required_and_nullable_contracts) {
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_dynamic_value>("{}"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_dynamic_value>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_nullable_dynamic_value>("{}"); }));
    auto model = ruvia::from_json<required_dynamic_value>(R"({"value":1})");
    bool rejected = false;
    try {
        model.set<"value">(ruvia::json_value::parse("null"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(model.get<"value">().view(), std::string_view("1"));
    EXPECT_TRUE(model.is_present<"value">() && !model.is_null<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":1})"));
    model.set<"value">(ruvia::json_value::parse("false"));
    EXPECT_EQ(model.get<"value">().view(), std::string_view("false"));
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":false})"));

    required_nullable_dynamic_value response;
    ruvia::validator missing;
    ruvia::validate_model(response, missing);
    EXPECT_TRUE(!missing.ok());
    response.set<"value">(nullptr);
    EXPECT_TRUE(!response.get<"value">() && response.is_null<"value">());
    EXPECT_TRUE(!response.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)), std::string_view(R"({"value":null})"));
    ruvia::validator valid;
    ruvia::validate_model(response, valid);
    EXPECT_TRUE(valid.ok());
    auto parsed = ruvia::from_json<required_nullable_dynamic_value>(R"({"value":null})");
    parsed.set<"value">(ruvia::json_value::parse("42"));
    EXPECT_EQ(parsed.get<"value">()->view(), std::string_view("42"));
    EXPECT_TRUE(parsed.is_present<"value">() && !parsed.is_null<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed)), std::string_view(R"({"value":42})"));
    parsed.set<"value">(ruvia::json_value::parse("null"));
    EXPECT_TRUE(!parsed.get<"value">() && parsed.is_null<"value">() && parsed.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed)), std::string_view(R"({"value":null})"));
}

TEST(Ruvia, model_nullable_response_emits_explicit_null_without_emitting_absence) {
    nullable_response response;
    response.set<"required">(nullptr);
    EXPECT_TRUE(response.is_null<"required">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)), std::string_view(R"({"required":null})"));
    response.set<"optional">(nullptr);
    auto token = ruvia::json_value::parse("null");
    response.set<"dynamic">(std::move(token));
    EXPECT_TRUE(response.is_null<"dynamic">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":null,"optional":null,"dynamic":null})"));
    response.reset<"optional">();
    response.set<"required">("note");
    EXPECT_TRUE(!response.is_null<"required">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":"note","dynamic":null})"));
    response.set_null<"optional">().reset<"optional">().set<"optional">("val");
    EXPECT_TRUE(response.has_value<"optional">());
    response.set_null<"optional">();
    EXPECT_TRUE(!response.has_value<"optional">() && response.is_null<"optional">());
    response.reset<"optional">();
    EXPECT_TRUE(!response.has_value<"optional">() && !response.is_null<"optional">());
}

TEST(Ruvia, model_dynamic_owned_tokens_and_presence_survive_rebinding_and_reclaim_memory) {
    const std::string text(256, 'x');
    const std::string body(std::string("{\"value\":{\"text\":\"") + text + "\"},\"object\":{\"text\":\"" + text + "\"}}");
    {
        owned_values retained;
        {
            auto input = body;
            auto parsed_value = ruvia::from_json<owned_values>(input);
            input.assign(input.size(), '?');
            owned_values moved(std::move(parsed_value));
            EXPECT_TRUE(moved.is_present<"value">() && !moved.is_present<"note">());
            retained = std::move(moved);
        }
        EXPECT_TRUE(retained.is_present<"value">() && retained.is_present<"object">());
        EXPECT_TRUE(!retained.is_present<"note">() && retained.get<"note">().has_value());
        EXPECT_TRUE(retained.get<"value">()->get<ruvia::string>("text").value()->view() == text);
        for (int index = 0; index < 32; ++index) {
            {
                auto parsed_value = ruvia::from_json<owned_values>(body);
                EXPECT_TRUE(parsed_value.is_present<"value">());
            }
            for (const auto& invalid : {std::string(std::string_view(body).substr(0, body.size() - 1)) + ",\"value\":null}",
                     std::string(std::string_view(body).substr(0, body.size() - 1)) + ",\"note\":42}", std::string(std::string_view(body).substr(0, body.size() - 1))}) {
                EXPECT_TRUE(ruvia::test::throws_on(
                    [&] { (void)ruvia::from_json<owned_values>(invalid); }));
            }
            EXPECT_TRUE(retained.get<"object">()->get<ruvia::string>("text").value()->view() == text);
        }
    }
}

namespace {

RUVIA_MODEL(resource_child,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));
RUVIA_MODEL(resource_parent,
    RUVIA_REQUIRED_FIELD(title, ruvia::string),
    RUVIA_OPTIONAL_FIELD(child, resource_child),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<resource_child>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<resource_child>));
RUVIA_MODEL(resource_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<resource_node>));
RUVIA_MODEL(resource_dense_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<resource_dense_node>));
RUVIA_MODEL(resource_text_sequences,
    RUVIA_OPTIONAL_FIELD(dense, ruvia::array<ruvia::boxed_array<ruvia::string>>),
    RUVIA_OPTIONAL_FIELD(boxed, ruvia::boxed_array<ruvia::string>));

}  // namespace

TEST(Ruvia, model_assignment_recursively_outlives_source_owner) {
    const std::string text(160, 'a');
    resource_parent target;
    {
        target.set<"title">(ruvia::string(text));
        resource_child source_value;
        source_value.set<"name">(text);
        source_value.ensure<"tags">().emplace_back(text);
        target.set<"child">(std::move(source_value));
        resource_child other;
        other.set<"name">(text);
        other.ensure<"tags">().emplace_back(text);
        // Public insertion accepts a const model and owns it recursively.
        const auto& borrowed = other;
        target.ensure<"children">().emplace_back(borrowed);
        target.ensure<"boxed">().emplace_back(std::move(other));
    }
    EXPECT_EQ(target.get<"title">().view(), std::string_view(text));
    const auto verify = [&](const resource_child& child_value) {
        EXPECT_EQ(child_value.get<"name">().view(), std::string_view(text));
        EXPECT_EQ(child_value.get<"tags">()->front().view(), std::string_view(text));
    };
    verify(*target.get<"child">());
    verify(target.get<"children">()->front());
    verify(target.get<"boxed">()->front());
}

TEST(Ruvia, model_array_construction_and_mutable_assignment_keep_values) {
    const std::string text(160, 'b');
    ruvia::array<ruvia::string> strings;
    strings.emplace_back(text);
    strings.emplace_back().assign_owned(text);
    ruvia::array<resource_child> children;
    children.emplace_back().set<"name">(text);
    ruvia::boxed_array<ruvia::string> boxed;
    boxed.emplace_back(text);
    {
        strings.front() = ruvia::string(text);
        resource_child replacement;
        replacement.set<"name">(text);
        children.front() = std::move(replacement);
        boxed.front() = ruvia::string(text);
    }
    for (const auto& value : strings) {
        EXPECT_EQ(value.view(), std::string_view(text));
    }
    EXPECT_EQ(children.front().get<"name">().view(), std::string_view(text));
    EXPECT_EQ(boxed.front().view(), std::string_view(text));
}

TEST(Ruvia, model_array_resize_accepts_its_own_fill_element) {
    const std::string text(160, 'd');
    ruvia::array<ruvia::string> values;
    values.emplace_back(text);
    const auto count = values.capacity() + 8;
    values.resize(count, values.front());
    EXPECT_EQ(values.size(), count);
    for (const auto& value : values) {
        EXPECT_EQ(value.view(), std::string_view(text));
    }
}

TEST(Ruvia, model_recursive_assignment_can_promote_an_owned_child) {
    const std::string text(160, 'e');
    resource_node root;
    root.set<"name">("parent");
    auto& child_value = root.ensure<"children">().emplace_back();
    child_value.set<"name">(text);
    child_value.ensure<"children">().emplace_back().set<"name">("grandchild");
    root = std::move(child_value);
    EXPECT_EQ(root.get<"name">().view(), std::string_view(text));
    EXPECT_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("grandchild"));

    auto& children = root.ensure<"children">();
    children.front().ensure<"children">().emplace_back().set<"name">(text);
    children = std::move(children.front().ensure<"children">());
    EXPECT_EQ(children.size(), std::size_t{1});
    EXPECT_EQ(children.front().get<"name">().view(), std::string_view(text));

    resource_dense_node dense;
    auto& dense_children = dense.ensure<"children">();
    dense_children.emplace_back().ensure<"children">().emplace_back().set<"name">(text);
    dense_children = std::move(dense_children.front().ensure<"children">());
    EXPECT_EQ(dense_children.size(), std::size_t{1});
    EXPECT_EQ(dense_children.front().get<"name">().view(), std::string_view(text));
}

TEST(Ruvia, model_moves_reject_a_source_that_owns_the_destination) {
    const std::string text(160, 'f');
    const auto rejects = [](auto&& operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    resource_node root;
    root.set<"name">(text);
    auto& child_value = root.ensure<"children">().emplace_back();
    child_value.set<"name">("child");
    auto& grandchildren = child_value.ensure<"children">();
    grandchildren.emplace_back().set<"name">("grandchild");

    EXPECT_TRUE(rejects([&] { child_value = std::move(root); }));
    EXPECT_TRUE(rejects([&] { grandchildren = std::move(root.ensure<"children">()); }));
    EXPECT_TRUE(rejects([&] { grandchildren.push_back(std::move(root)); }));
    EXPECT_TRUE(rejects([&] { root.ensure<"children">().emplace_back(std::move(root)); }));
    EXPECT_TRUE(rejects([&] { child_value.set<"children">(std::move(root.ensure<"children">())); }));

    EXPECT_EQ(root.get<"name">().view(), std::string_view(text));
    EXPECT_EQ(root.get<"children">()->size(), std::size_t{1});
    EXPECT_TRUE(&root.get<"children">()->front() == &child_value);
    EXPECT_EQ(child_value.get<"name">().view(), std::string_view("child"));
    EXPECT_EQ(grandchildren.size(), std::size_t{1});
    EXPECT_EQ(grandchildren.front().get<"name">().view(), std::string_view("grandchild"));

    // A field reassigned from its own value does not own its destination.
    root.set<"children">(std::move(root.ensure<"children">()));
    EXPECT_EQ(root.get<"children">()->front().get<"name">().view(), std::string_view("child"));

    resource_dense_node dense;
    dense.set<"name">(text);
    auto& dense_children = dense.ensure<"children">();
    auto& dense_child = dense_children.emplace_back();
    dense_child.set<"name">("child");
    auto& dense_grandchildren = dense_child.ensure<"children">();

    EXPECT_TRUE(rejects([&] { dense_child = std::move(dense); }));
    EXPECT_TRUE(rejects([&] { dense_grandchildren = std::move(dense_children); }));
    EXPECT_TRUE(rejects([&] { dense_grandchildren.emplace_back(std::move(dense)); }));
    EXPECT_TRUE(rejects([&] { dense_children.emplace_back(std::move(dense)); }));

    EXPECT_EQ(dense.get<"name">().view(), std::string_view(text));
    EXPECT_EQ(dense_children.size(), std::size_t{1});
    EXPECT_EQ(dense_child.get<"name">().view(), std::string_view("child"));
    EXPECT_TRUE(dense_grandchildren.empty());
}

TEST(Ruvia, model_self_field_and_aliased_element_moves_preserve_values) {
    const std::string first(160, 'a');
    const std::string second(160, 'b');
    resource_text_sequences value;
    value.ensure<"dense">().emplace_back().emplace_back(first);
    value.ensure<"dense">().emplace_back().emplace_back(second);
    value.set<"dense">(std::move(value.ensure<"dense">()));
    ASSERT_EQ(value.get<"dense">()->size(), std::size_t{2});
    EXPECT_EQ((*value.get<"dense">())[0].front().view(), std::string_view(first));
    EXPECT_EQ((*value.get<"dense">())[1].front().view(), std::string_view(second));

    value.ensure<"boxed">().emplace_back(first);
    value.ensure<"boxed">().emplace_back(second);
    value.set<"boxed">(std::move(value.ensure<"boxed">()));
    ASSERT_EQ(value.get<"boxed">()->size(), std::size_t{2});
    EXPECT_EQ((*value.get<"boxed">())[0].view(), std::string_view(first));
    EXPECT_EQ((*value.get<"boxed">())[1].view(), std::string_view(second));

    ruvia::array<ruvia::string> values;
    values.emplace_back(first);
    while (values.size() < values.capacity()) {
        values.emplace_back(second);
    }
    const char* const original_text = values.front().data();
    values.push_back(std::move(values.front()));
    EXPECT_EQ(values.back().view(), std::string_view(first));
    EXPECT_EQ(values.back().data(), original_text);
    EXPECT_TRUE(values.front().empty());
}

TEST(Ruvia, model_scalar_array_moves_transfer_storage) {
    const auto check = []<typename sequence_type>() {
        sequence_type source;
        sequence_type target;
        for (int index = 0; index < 64; ++index) {
            source.push_back(ruvia::int32(index));
        }
        target.push_back(ruvia::int32(-1));
        target = std::move(source);
        EXPECT_TRUE(source.empty());
        EXPECT_EQ(target.size(), std::size_t{64});
        for (std::size_t index = 0; index < target.size(); ++index) {
            EXPECT_EQ(target[index].value_, static_cast<int>(index));
        }
    };
    check.template operator()<ruvia::array<ruvia::int32>>();
    check.template operator()<ruvia::boxed_array<ruvia::int32>>();
}

static_assert(!std::is_copy_constructible_v<resource_parent>);
static_assert(!std::is_constructible_v<resource_parent, resource_child&>);
