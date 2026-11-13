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
#include <charconv>
#include <cmath>
#include <concepts>
#include <deque>
#include <exception>
#include <functional>
#include <future>
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

#include <asio.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/socket_base.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/http_client.h"

#include "content_decoding_fixture.h"
#include "http_server_fixture.h"

#ifdef RUVIA_ENABLE_HTTP2
#include "ruvia/http/http2_connection.h"

#include "http_server_fixture.h"
#endif

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

namespace http_client_request_controls_test {

class peer final {
public:
    peer(asio::io_context& io, const ruvia::worker_ref& worker, bool http2, bool silent)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          gate_(io),
          done_(worker),
          http2_(http2),
          silent_(silent) {
        gate_.expires_at(std::chrono::steady_clock::time_point::max());
        encoded_ = *ruvia::encode_http_content(ruvia::http_content_coding::gzip, "streamed representation",
            {.max_encoded_bytes_ = 1024});
    }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port());
    }
    void start() {
        asio::co_spawn(acceptor_.get_executor(), run(), [this](std::exception_ptr failure) {
            if (!stopping_) {
                failure_ = failure;
            }
            finished_ = true;
            done_.notify();
        });
    }
    void release() {
        gate_.cancel();
    }
    void stop() {
        stopping_ = true;
        std::error_code ignored;
        gate_.cancel();
        socket_.close(ignored);
        acceptor_.close(ignored);
    }
    ruvia::task<void> join() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string encoded_{};
    std::string received_{};
    bool final_sent_{};
    bool tail_sent_{};

private:
    asio::awaitable<void> wait_gate() {
        std::error_code ignored;
        co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
    asio::awaitable<void> run() {
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        if (http2_) {
#ifdef RUVIA_ENABLE_HTTP2
            co_await run_http2();
#else
            throw std::logic_error("HTTP/2 fixture unavailable");
#endif
        } else {
            co_await asio::async_read_until(socket_, asio::dynamic_buffer(received_), "\r\n\r\n", asio::use_awaitable);
            if (!silent_) {
                const std::string response =
                    "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n"
                    "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: " +
                    std::to_string(encoded_.size()) + "\r\n\r\n";
                co_await asio::async_write(socket_, asio::buffer(response), asio::use_awaitable);
                co_await asio::async_write(socket_, asio::buffer(encoded_.data(), 1), asio::use_awaitable);
                final_sent_ = true;
                co_await wait_gate();
                if (stopping_) {
                    co_return;
                }
                co_await asio::async_write(socket_, asio::buffer(encoded_.data() + 1, encoded_.size() - 1), asio::use_awaitable);
                tail_sent_ = true;
            }
        }
        std::array<char, 4096> input;
        std::error_code error;
        while (!error) {
            co_await socket_.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        }
    }
#ifdef RUVIA_ENABLE_HTTP2
    asio::awaitable<void> run_http2() {
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket_, asio::buffer(preface), asio::use_awaitable);
        EXPECT_EQ(std::string_view(preface.data(), preface.size()), ruvia::http2_client_preface);
        std::string wire;
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        std::uint32_t stream = 0;
        while (stream == 0) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket_, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid HTTP/2 request frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket_, asio::buffer(payload), asio::use_awaitable);
            }
            if (header->type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                stream = header->stream_id_;
                ruvia::hpack_decoder decoder;
                EXPECT_FALSE(decoder.decode(payload, [this](std::string_view name, std::string_view value) {
                                        received_.append(name).append(": ").append(value).append("\n");
                                        return true;
                                    })
                        .has_value());
            }
        }
        if (silent_) {
            co_return;
        }
        wire.clear();
        std::string fields;
        ruvia::hpack_encoder::encode_header(fields, ":status", "103");
        ruvia::hpack_encoder::encode_header(fields, "link", "</style.css>; rel=preload");
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, stream, fields);
        fields.clear();
        ruvia::hpack_encoder::encode_header(fields, ":status", "200");
        ruvia::hpack_encoder::encode_header(fields, "content-encoding", "gzip");
        ruvia::hpack_encoder::encode_header(fields, "content-length", std::to_string(encoded_.size()));
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, stream, fields);
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 0, stream, std::string_view(encoded_).substr(0, 1));
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        final_sent_ = true;
        co_await wait_gate();
        if (stopping_) {
            co_return;
        }
        wire.clear();
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::data, 1, stream, std::string_view(encoded_).substr(1));
        co_await asio::async_write(socket_, asio::buffer(wire), asio::use_awaitable);
        tail_sent_ = true;
    }
#endif
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer gate_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    bool http2_;
    bool silent_;
    bool stopping_{};
    bool finished_{};
};

void raw_stream(bool http2, bool configured_raw) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    peer upstream(io, worker, http2, false);
    const auto url = upstream.url();
    upstream.start();
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = url,
                                                         .decode_content_ = !configured_raw,
                                                         .read_timeout_ = std::chrono::milliseconds(5),
                                                         .request_timeout_ = std::chrono::seconds(3),
                                                         .versions_ = http2 ? ruvia::http_client_versions{.http1_ = false} : ruvia::http_client_versions{.http2_ = false}});
        try {
            bool informed = false;
            auto request = client.client().get("/raw");
            request.header("Content-Type", "unparsed forwarded value")
                .configure({.decode_content_ = configured_raw ? std::optional<bool>{} : std::optional<bool>{false},
                    .field_source_ = ruvia::http_request_field_source::forwarded,
                    .timeouts_ = {.read_ = ruvia::http_client_timeout_override::disabled()},
                    .informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> fields, ruvia::stop_token stop) -> ruvia::task<void> {
                        EXPECT_EQ(status.value(), 103U);
                        EXPECT_EQ(fields.front().value(), "</style.css>; rel=preload");
                        EXPECT_EQ(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20), stop), ruvia::timer_sleep_result::elapsed);
                        informed = true;
                    }});
            auto response = co_await request.send();
            EXPECT_TRUE(informed);
            EXPECT_EQ(response.framing(), http2 ? ruvia::http_client_response_framing::stream : ruvia::http_client_response_framing::fixed_length);
            EXPECT_EQ(response.content_length(), upstream.encoded_.size());
            EXPECT_EQ(response.header("content-encoding"), "gzip");
            EXPECT_TRUE(response.header("content-length").has_value());
            const auto first = co_await response.read();
            EXPECT_TRUE(first.has_value());
            EXPECT_FALSE(upstream.tail_sent_);
            std::string bytes;
            if (first) {
                bytes.append(reinterpret_cast<const char*>(first->data()), first->size());
            }
            EXPECT_EQ(bytes, std::string_view(upstream.encoded_).substr(0, 1));
            (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20));
            upstream.release();
            auto rest = co_await response.bytes();
            bytes.append(rest.text());
            EXPECT_EQ(bytes, upstream.encoded_);
            EXPECT_NE(upstream.received_.find("unparsed forwarded value"), std::string::npos);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        upstream.stop();
        co_await upstream.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void read_timeout(bool http2, bool total_only) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    peer upstream(io, worker, http2, true);
    const auto url = upstream.url();
    upstream.start();
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = url,
                                                         .read_timeout_ = std::chrono::seconds(2),
                                                         .request_timeout_ = total_only ? std::chrono::milliseconds(30) : std::chrono::milliseconds(2000),
                                                         .versions_ = http2 ? ruvia::http_client_versions{.http1_ = false} : ruvia::http_client_versions{.http2_ = false}});
        std::optional<ruvia::http_client_error> observed_error;
        try {
            auto request = client.client().get("/silent");
            request.configure({.timeouts_ = {.read_ = total_only ? ruvia::http_client_timeout_override::disabled()
                                                                 : ruvia::http_client_timeout_override::after(std::chrono::milliseconds(20))}});
            bool timed_out = false;
            try {
                (void)co_await request.send();
            } catch (const ruvia::http_client_error& error) {
                timed_out = error.code() == std::make_error_code(std::errc::timed_out);
                observed_error.emplace(error);
            }
            EXPECT_TRUE(timed_out);
            EXPECT_TRUE(observed_error.has_value());
            if (observed_error) {
                EXPECT_TRUE(observed_error->request_written());
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        EXPECT_TRUE(observed_error.has_value());
        if (observed_error) {
            const auto* connection = observed_error->connection_info();
            EXPECT_NE(connection, nullptr);
            if (connection != nullptr) {
                EXPECT_NE(connection->id_, 0u);
                EXPECT_EQ(connection->peer_ip_.to_string(), "127.0.0.1");
                EXPECT_NE(connection->peer_port_, 0u);
                EXPECT_FALSE(connection->reused_);
            }
        }
        upstream.stop();
        co_await upstream.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http_client_request_controls_test

TEST(Ruvia, http_client_http1_raw_stream_forwarded_fields_and_request_timeouts) {
    for (const bool configured_raw : {false, true}) {
        http_client_request_controls_test::raw_stream(false, configured_raw);
    }
    for (const bool total_only : {false, true}) {
        http_client_request_controls_test::read_timeout(false, total_only);
    }
}
#ifdef RUVIA_ENABLE_HTTP2
TEST(Ruvia, http_client_http2_raw_stream_forwarded_fields_and_request_timeouts) {
    for (const bool configured_raw : {false, true}) {
        http_client_request_controls_test::raw_stream(true, configured_raw);
    }
    for (const bool total_only : {false, true}) {
        http_client_request_controls_test::read_timeout(true, total_only);
    }
}

TEST(Ruvia, http_client_http2_informational_backpressure_does_not_block_other_streams) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket socket(io);
    ruvia::worker_signal peer_done(worker);
    bool finished = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        co_await acceptor.async_accept(socket, asio::use_awaitable);
        std::array<char, ruvia::http2_client_preface.size()> preface;
        co_await asio::async_read(socket, asio::buffer(preface), asio::use_awaitable);
        std::string wire;
        ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
        co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        unsigned requests = 0;
        while (requests != 2) {
            std::array<char, ruvia::http2_frame_header_bytes> bytes;
            co_await asio::async_read(socket, asio::buffer(bytes), asio::use_awaitable);
            const auto header = ruvia::parse_http2_frame_header(bytes);
            if (!header) {
                throw std::runtime_error("invalid multiplexed frame");
            }
            std::string payload(header->length_, '\0');
            if (!payload.empty()) {
                co_await asio::async_read(socket, asio::buffer(payload), asio::use_awaitable);
            }
            if (header->type_ != static_cast<std::uint8_t>(ruvia::http2_frame_type::headers)) {
                continue;
            }
            wire.clear();
            std::string fields;
            if (++requests == 1) {
                ruvia::hpack_encoder::encode_header(fields, ":status", "103");
                ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 4, header->stream_id_, fields);
                fields.clear();
            }
            ruvia::hpack_encoder::encode_header(fields, ":status", "200");
            ruvia::hpack_encoder::encode_header(fields, "content-length", "0");
            ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 5, header->stream_id_, fields);
            co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        }
        std::array<char, 4096> bytes;
        std::error_code error;
        while (!error) {
            co_await socket.async_read_some(asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        finished = true;
        peer_done.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
        ruvia::http_client client(attachment.loop(), {.base_url_ = url,
                                                         .request_timeout_ = std::chrono::seconds(3),
                                                         .versions_ = ruvia::http_client_versions{.http1_ = false}});
        ruvia::task_scope operations(worker);
        ruvia::worker_signal entered(worker);
        ruvia::stop_source release;
        bool callback_entered = false;
        bool first_returned = false;
        const auto first = [&]() -> ruvia::task<void> {
            try {
                auto request = client.client().get("/first");
                request.configure({.informational_ = [&](ruvia::http_status_code, std::span<const ruvia::http_header>, ruvia::stop_token stop) -> ruvia::task<void> {
                    callback_entered = true;
                    entered.notify();
                    ruvia::stop_group cancellation(stop, release.token());
                    (void)co_await ruvia::sleep_for(worker, std::chrono::hours(1), cancellation.token());
                }});
                auto response = co_await request.send();
                EXPECT_TRUE(response.ok());
                first_returned = true;
            } catch (...) {
                failure = std::current_exception();
                entered.notify();
            }
        };
        operations.spawn(first());
        try {
            while (!callback_entered && !failure) {
                co_await entered.wait();
            }
            if (!failure) {
                auto second = co_await client.client().get("/second").send();
                EXPECT_TRUE(second.ok());
                EXPECT_FALSE(first_returned);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        release.request_stop();
        co_await operations.join();
        co_await client.shutdown();
        std::error_code ignored;
        socket.close(ignored);
        acceptor.close(ignored);
        while (!finished) {
            co_await peer_done.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
}
#endif

TEST(Ruvia, http_client_ipv6_dial_target_preserves_logical_authority) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::acceptor acceptor(io);
    std::error_code ipv6_error;
    acceptor.open(asio::ip::tcp::v6(), ipv6_error);
    if (!ipv6_error) {
        acceptor.bind({asio::ip::address_v6::loopback(), 0}, ipv6_error);
    }
    if (ipv6_error) {
        GTEST_SKIP() << "IPv6 loopback unavailable: " << ipv6_error.message();
    }
    acceptor.listen();
    const auto port = acceptor.local_endpoint().port();
    const auto authority = std::string("[2001:db8::1]:") + std::to_string(port);
    const auto logical_url = "http://" + authority;
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    ruvia::task_scope peers(worker);
    std::exception_ptr failure;
    std::string received;
    const auto peer = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        const auto accepted = co_await ruvia::async_asio<void>([&](auto completion) {
            acceptor.async_accept(socket, std::move(completion));
        });
        if (accepted.error_code()) {
            throw std::system_error(accepted.error_code());
        }
        const auto read = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
            asio::async_read_until(socket, asio::dynamic_buffer(received), "\r\n\r\n", std::move(completion));
        });
        if (read.error_code()) {
            throw std::system_error(read.error_code());
        }
        constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        const auto written = co_await ruvia::async_asio<std::size_t>([&](auto completion) {
            asio::async_write(socket, asio::buffer(reply), std::move(completion));
        });
        if (written.error_code()) {
            throw std::system_error(written.error_code());
        }
    };
    const auto run = [&]() -> ruvia::task<void> {
        peers.spawn(peer());
        ruvia::http_client client(attachment.loop(), {.base_url_ = logical_url,
                                                         .dial_host_ = "::1",
                                                         .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            auto response = co_await client.client().get("/").send();
            EXPECT_EQ(response.status().value(), 200);
            EXPECT_NE(received.find(authority), std::string::npos);
            EXPECT_EQ(response.connection_info().peer_ip_.to_string(), "::1");
            EXPECT_EQ(response.connection_info().peer_port_, port);
            EXPECT_EQ(response.content_length(), 0u);
            const auto addresses = client.client().dns_snapshot();
            EXPECT_EQ(addresses.size(), 1u);
            if (!addresses.empty()) {
                EXPECT_EQ(addresses.front().ip_.to_string(), "::1");
                EXPECT_EQ(addresses.front().port_.value(), port);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        std::error_code ignored;
        acceptor.close(ignored);
        co_await peers.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

TEST(Ruvia, http_client_metadata_values_preserve_strong_ip_identity) {
    const auto parsed_address = ruvia::ip_address::parse("2001:db8:1234:5678:90ab:cdef:1234:5678");
    ASSERT_TRUE(parsed_address.has_value());
    std::optional<ruvia::http_client_error> saved_error;
    {
        ruvia::http_client_connection_info source;
        source.id_ = 17;
        source.peer_ip_ = *parsed_address;
        source.peer_port_ = 443;
        auto copied = source;
        ruvia::http_client_connection_info assigned;
        assigned = source;
        assigned = std::move(copied);
        EXPECT_EQ(assigned.peer_ip_, *parsed_address);

        ruvia::http_client_endpoint_address endpoint(*parsed_address, 443);
        auto copied_endpoint = endpoint;
        ruvia::http_client_endpoint_address assigned_endpoint;
        assigned_endpoint = endpoint;
        assigned_endpoint = std::move(copied_endpoint);
        EXPECT_EQ(assigned_endpoint.ip_, *parsed_address);

        const ruvia::http_client_error error(
            std::make_error_code(std::errc::timed_out), "observed timeout", std::nullopt, &source);
        saved_error.emplace(error);
    }
    const auto* connection = saved_error->connection_info();
    EXPECT_NE(connection, nullptr);
    if (connection != nullptr) {
        EXPECT_EQ(connection->id_, 17u);
        EXPECT_EQ(connection->peer_ip_, *parsed_address);
    }
}

TEST(Ruvia, http_client_dial_and_dns_ports_reject_invalid_original_integer_domain) {
    for (const int invalid : {-1, 65536}) {
        EXPECT_THROW(
            (void)ruvia::http_client_endpoint_address("127.0.0.1", invalid),
            std::invalid_argument);
        ruvia::http_client_endpoint_address endpoint("127.0.0.1", 443);
        EXPECT_THROW(endpoint.port_ = invalid, std::invalid_argument);
        ruvia::http_client_config config{.base_url_ = "http://127.0.0.1:8001"};
        EXPECT_THROW(config.dial_port_ = invalid, std::invalid_argument);
        EXPECT_THROW(
            ((void)ruvia::http_client_config{.base_url_ = "http://127.0.0.1:8001", .dial_port_ = invalid}),
            std::invalid_argument);
    }
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto reject_zero_dial = [&]() {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001", .dial_port_ = 0});
    };
    EXPECT_THROW(reject_zero_dial(), std::invalid_argument);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        const std::array addresses{ruvia::http_client_endpoint_address("127.0.0.1", 0)};
        EXPECT_THROW(client.client().update_dns(addresses), std::invalid_argument);
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
}

TEST(Ruvia, http_client_builders_enforce_consumed_state_websocket_options_and_digest_redirect_strip) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto url = "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());

    std::string second_hop_request;
    const auto serve_redirect = [&]() -> ruvia::task<void> {
        asio::ip::tcp::socket socket(io);
        const auto accepted = co_await ruvia::async_asio([&](auto handler) {
            acceptor.async_accept(socket, std::move(handler));
        });
        if (accepted.error_code()) {
            co_return;
        }
        std::string first_hop;
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read_until(socket, asio::dynamic_buffer(first_hop), "\r\n\r\n", std::move(handler));
        });
        // Drain the 4-byte body ("body") if not yet in first_hop.
        const auto header_end = first_hop.find("\r\n\r\n");
        const std::size_t body_have = header_end == std::string::npos ? 0 : first_hop.size() - (header_end + 4);
        if (body_have < 4) {
            std::array<char, 4> rest{};
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read(socket, asio::buffer(rest.data(), 4 - body_have), std::move(handler));
            });
        }
        constexpr std::string_view redirect_reply =
            "HTTP/1.1 303 See Other\r\nLocation: /landed\r\nContent-Length: 0\r\n\r\n";
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(socket, asio::buffer(redirect_reply), std::move(handler));
        });
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_read_until(socket, asio::dynamic_buffer(second_hop_request), "\r\n\r\n", std::move(handler));
        });
        constexpr std::string_view final_reply =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
            asio::async_write(socket, asio::buffer(final_reply), std::move(handler));
        });
    };

    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::task_scope peers(worker);
        peers.spawn(serve_redirect());

        ruvia::http_client client(attachment.loop(), {.base_url_ = url});
        auto builder = client.client().post("/submit");
        builder.header("Content-Type", "text/plain")
            .header("Digest", "SHA-256=abc=")
            .header("Repr-Digest", "sha-256=:abc=:")
            .header("X-Trace", "keep-me")
            .body("body");
        auto pending = builder.send();
        EXPECT_THROW(builder.redirects(1), std::logic_error);
        auto response = co_await std::move(pending);
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "ok");

        EXPECT_THROW(
            client.client().websocket("/ws").configure({.dial_port_ = 0}),
            std::invalid_argument);
        EXPECT_THROW(
            client.client().websocket("/ws").configure({.authority_ = "user@example.test"}),
            std::invalid_argument);

        co_await client.shutdown();
        co_await peers.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();

    EXPECT_TRUE(second_hop_request.starts_with("GET /landed HTTP/1.1\r\n"));
    EXPECT_NE(second_hop_request.find("X-Trace: keep-me\r\n"), std::string::npos);
    EXPECT_EQ(second_hop_request.find("Digest:"), std::string::npos);
    EXPECT_EQ(second_hop_request.find("Repr-Digest:"), std::string::npos);
    EXPECT_EQ(second_hop_request.find("Content-Type:"), std::string::npos);
}

TEST(Ruvia, http_client_plain_connect_omits_origin_cookies_even_for_matching_authority) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto authority = "127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
    const auto url = "http://" + authority;

    std::string get_request_head;
    std::string connect_request_head;
    const auto serve_origin_and_proxy = [&]() -> ruvia::task<void> {
        // Connection 1: two ordinary origin GET requests to seed and verify cookie jar.
        {
            asio::ip::tcp::socket socket(io);
            const auto accepted = co_await ruvia::async_asio([&](auto handler) {
                acceptor.async_accept(socket, std::move(handler));
            });
            if (accepted.error_code()) {
                co_return;
            }
            std::string first_get;
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read_until(socket, asio::dynamic_buffer(first_get), "\r\n\r\n", std::move(handler));
            });
            constexpr std::string_view seed_reply =
                "HTTP/1.1 200 OK\r\nSet-Cookie: jar_secret=stored; Path=/\r\nContent-Length: 2\r\n\r\nok";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(seed_reply), std::move(handler));
            });

            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read_until(socket, asio::dynamic_buffer(get_request_head), "\r\n\r\n", std::move(handler));
            });
            constexpr std::string_view verify_reply =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(verify_reply), std::move(handler));
            });
        }
        // Connection 2: plain CONNECT tunnel whose tunnel_authority matches the client's authority.
        {
            asio::ip::tcp::socket socket(io);
            const auto accepted = co_await ruvia::async_asio([&](auto handler) {
                acceptor.async_accept(socket, std::move(handler));
            });
            if (accepted.error_code()) {
                co_return;
            }
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_read_until(socket, asio::dynamic_buffer(connect_request_head), "\r\n\r\n", std::move(handler));
            });
            constexpr std::string_view connect_reply =
                "HTTP/1.1 200 Connection Established\r\n\r\n";
            (void)co_await ruvia::async_asio<std::size_t>([&](auto handler) {
                asio::async_write(socket, asio::buffer(connect_reply), std::move(handler));
            });
        }
    };

    const auto run = [&]() -> ruvia::task<void> {
        const auto worker = attachment.loop().worker();
        ruvia::task_scope peers(worker);
        peers.spawn(serve_origin_and_proxy());

        ruvia::http_client client(attachment.loop(), {
                                                         .base_url_ = url,
                                                         .versions_ = ruvia::http_client_versions{.http2_ = false},
                                                         .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send,
                                                         .cookies_ = {{"configured_secret", "1"}},
                                                     });
        auto seeded = co_await client.client().get("/seed").send();
        const auto seeded_body = co_await seeded.text();
        EXPECT_EQ(seeded_body.text(), "ok");

        auto verified = co_await client.client().get("/verify").send();
        const auto verified_body = co_await verified.text();
        EXPECT_EQ(verified_body.text(), "ok");

        auto opened = co_await client.client().connect(authority).open();
        EXPECT_NE(opened.tunnel(), nullptr);
        if (opened.tunnel() != nullptr) {
            co_await opened.tunnel()->finish();
        }

        co_await client.shutdown();
        co_await peers.join();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();

    EXPECT_NE(get_request_head.find("cookie: configured_secret=1; jar_secret=stored\r\n"), std::string::npos);
    EXPECT_TRUE(connect_request_head.starts_with("CONNECT " + authority + " HTTP/1.1\r\n"));
    EXPECT_EQ(connect_request_head.find("Cookie:"), std::string::npos);
    EXPECT_EQ(connect_request_head.find("cookie:"), std::string::npos);
    EXPECT_EQ(connect_request_head.find("configured_secret"), std::string::npos);
    EXPECT_EQ(connect_request_head.find("jar_secret"), std::string::npos);
}

namespace client_upgrade_tests {

struct peer_observation final {
    std::string request_;
    std::string payload_;
};

// A real TCP peer writes one coalesced response, half-closes its sending side,
// then observes the client's bytes through EOF. No production protocol parser
// participates on the peer side.
template <typename client_fn_type>
peer_observation exchange(std::string_view wire, client_fn_type&& exercise) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto& worker = attachment.loop().worker();
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_finished(worker);
    peer_observation observed;
    std::exception_ptr peer_failure;
    bool peer_done = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        const auto head_bytes = co_await asio::async_read_until(
            socket, asio::dynamic_buffer(observed.request_), "\r\n\r\n", asio::use_awaitable);
        observed.payload_ = observed.request_.substr(head_bytes);
        observed.request_.resize(head_bytes);
        co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        std::array<char, 4096> bytes;
        for (;;) {
            std::error_code error;
            const auto count = co_await socket.async_read_some(
                asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
            observed.payload_.append(bytes.data(), count);
            if (error == asio::error::eof || error == asio::error::connection_reset) {
                co_return;
            }
            if (error) {
                throw std::system_error(error);
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done = true;
        peer_finished.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = url,
                .request_timeout_ = std::chrono::seconds(3),
                .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            co_await exercise(client, worker);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        while (!peer_done) {
            co_await peer_finished.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    return observed;
}

}  // namespace client_upgrade_tests

TEST(Ruvia, http1_client_generic_upgrade_preserves_early_binary_bytes_and_caller_fields) {
    for (const std::string_view protocol : {"chat/7", "websocket"}) {
        SCOPED_TRACE(protocol);
        std::string wire =
            "HTTP/1.1 103 Early Hints\r\n\r\n"
            "HTTP/1.1 101 Switching Protocols\r\nConnection: keep-alive, Upgrade\r\nUpgrade: ";
        wire.append(protocol);
        wire.append("\r\n\r\n");
        constexpr std::string_view early("early\0bytes", 11);
        constexpr std::string_view outgoing("client\0bytes", 12);
        wire.append(early);
        const auto observed = client_upgrade_tests::exchange(wire,
            [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                unsigned informational_count = 0;
                auto request = client.client().get("/generic");
                request.header("Connection", "Upgrade")
                    .header("Upgrade", protocol)
                    .header("Sec-WebSocket-Protocol", "caller-owned")
                    .header("Sec-WebSocket-Key", "caller-owned-key");
                request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>,
                                                         ruvia::stop_token) -> ruvia::task<void> {
                    EXPECT_EQ(status.value(), 103);
                    ++informational_count;
                    co_return;
                }});
                auto opened = co_await request.upgrade();
                EXPECT_EQ(opened.response(), nullptr);
                if (opened.tunnel() == nullptr) {
                    throw std::runtime_error("generic Upgrade did not open a tunnel");
                }
                auto& tunnel = *opened.tunnel();
                EXPECT_EQ(tunnel.status(), ruvia::http_status::switching_protocols);
                EXPECT_EQ(tunnel.header("upgrade"), std::optional(protocol));
                EXPECT_EQ(informational_count, 1u);
                EXPECT_EQ(tunnel.informational_responses().size(), std::size_t{1});
                co_await tunnel.write(outgoing);
                co_await tunnel.finish();
                std::string received;
                while (auto bytes = co_await tunnel.text()) {
                    received.append(*bytes);
                }
                EXPECT_EQ(received, early);
            });
        EXPECT_TRUE(observed.request_.starts_with("GET /generic HTTP/1.1\r\n"));
        EXPECT_NE(observed.request_.find(": caller-owned\r\n"), std::string::npos);
        EXPECT_NE(observed.request_.find(": caller-owned-key\r\n"), std::string::npos);
        EXPECT_EQ(observed.payload_, outgoing);
    }
}

TEST(Ruvia, http1_client_generic_upgrade_rejection_is_an_ordinary_response) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 426 Upgrade Required\r\nContent-Length: 6\r\nX-Rejected: yes\r\n\r\ndenied",
        [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            auto request = client.client().get("/rejected");
            request.header("Connection", "Upgrade").header("Upgrade", "chat/7");
            auto result = co_await request.upgrade();
            EXPECT_EQ(result.tunnel(), nullptr);
            if (result.response() == nullptr) {
                throw std::runtime_error("rejected Upgrade lost its HTTP response");
            }
            auto& response = *result.response();
            EXPECT_EQ(response.status().value(), 426);
            EXPECT_EQ(response.header("x-rejected"), std::optional<std::string_view>("yes"));
            EXPECT_EQ(response.framing(), ruvia::http_client_response_framing::fixed_length);
            EXPECT_EQ(response.content_length(), std::optional<std::uint64_t>(6));
            const auto body = co_await response.stream().read_all();
            EXPECT_EQ(body.text(), "denied");
        });
}

TEST(Ruvia, http1_client_generic_upgrade_rejects_invalid_switch_agreement) {
    for (const std::string_view fields : {
             "Upgrade: chat/7\r\n",
             "Connection: Upgrade\r\n",
             "Connection: Upgrade\r\nUpgrade: other/1\r\n",
             "Connection: close, Upgrade\r\nUpgrade: chat/7\r\n"}) {
        SCOPED_TRACE(fields);
        std::string wire = "HTTP/1.1 101 Switching Protocols\r\n";
        wire.append(fields);
        wire.append("\r\nnot-a-tunnel");
        client_upgrade_tests::exchange(wire,
            [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                auto request = client.client().get("/invalid-switch");
                request.header("Connection", "Upgrade").header("Upgrade", "chat/7");
                bool rejected = false;
                try {
                    auto result = co_await request.upgrade();
                    if (result.tunnel() != nullptr) {
                        result.tunnel()->abort();
                    }
                } catch (const ruvia::http_client_error& error) {
                    rejected = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_error);
                }
                EXPECT_TRUE(rejected);
            });
    }
}

TEST(Ruvia, http1_client_informational_callbacks_backpressure_coalesced_heads) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 103 Early Hints\r\nX-Order: first\r\n\r\n"
        "HTTP/1.1 102 Processing\r\nX-Order: second\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
        [](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            ruvia::worker_signal entered(worker);
            ruvia::worker_signal release(worker);
            std::vector<unsigned> statuses;
            bool returned = false;
            std::exception_ptr failure;
            auto request = client.client().get("/hints");
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header> headers,
                                                     ruvia::stop_token stop) -> ruvia::task<void> {
                EXPECT_FALSE(stop.stop_requested());
                statuses.push_back(status.value());
                EXPECT_EQ(headers.size(), std::size_t{1});
                const auto expected = statuses.size() == 1 ? "first" : "second";
                entered.notify();
                co_await release.wait();
                if (!headers.empty()) {
                    EXPECT_EQ(headers.front().value(), expected);
                }
            }});
            const auto send = [&]() -> ruvia::task<void> {
                try {
                    auto response = co_await request.send();
                    returned = true;
                    EXPECT_EQ(response.informational_responses().size(), std::size_t{2});
                    const auto body = co_await response.stream().read_all();
                    EXPECT_EQ(body.text(), "ok");
                } catch (...) {
                    failure = std::current_exception();
                }
                entered.notify();
            };
            ruvia::task_scope requests(worker);
            requests.spawn(send());
            for (std::size_t count = 1; count <= 2; ++count) {
                co_await entered.wait();
                if (failure || returned) {
                    break;
                }
                EXPECT_EQ(statuses.size(), count);
                EXPECT_FALSE(returned);
                release.notify();
            }
            co_await requests.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            EXPECT_EQ(statuses, (std::vector<unsigned>{103, 102}));
            EXPECT_TRUE(returned);
        });
}

TEST(Ruvia, http1_client_informational_callback_failure_terminates_exchange) {
    client_upgrade_tests::exchange(
        "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
        [](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            auto request = client.client().get("/callback-failure");
            request.configure({.informational_ = [](ruvia::http_status_code, std::span<const ruvia::http_header>,
                                                     ruvia::stop_token) -> ruvia::task<void> {
                throw std::runtime_error("informational callback failed");
                co_return;
            }});
            bool failed = false;
            try {
                auto response = co_await request.send();
                static_cast<void>(response);
            } catch (const std::runtime_error& error) {
                failed = std::string_view(error.what()) == "informational callback failed";
            }
            EXPECT_TRUE(failed);
        });
}

TEST(Ruvia, http1_client_response_framing_comes_from_parser_precedence) {
    struct framing_case final {
        std::string_view method_;
        std::string_view wire_;
        ruvia::http_client_response_framing framing_;
        std::optional<std::uint64_t> length_;
        std::string_view body_;
    };
    using framing = ruvia::http_client_response_framing;
    const framing_case cases[] = {
        {"GET", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", framing::fixed_length, 2, "ok"},
        {"GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", framing::chunked, std::nullopt, "ok"},
        {"GET", "HTTP/1.1 200 OK\r\n\r\nok", framing::close_delimited, std::nullopt, "ok"},
        {"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n", framing::none, 42, ""},
        {"GET", "HTTP/1.1 204 No Content\r\n\r\n", framing::none, std::nullopt, ""},
        {"GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", framing::chunked, std::nullopt, ""},
        {"GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\nok", framing::fixed_length, 2, "ok"},
    };
    for (const auto& scenario : cases) {
        SCOPED_TRACE(scenario.wire_);
        client_upgrade_tests::exchange(scenario.wire_,
            [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
                auto response = co_await client.client().request(scenario.method_, "/framing").send();
                EXPECT_EQ(response.framing(), scenario.framing_);
                EXPECT_EQ(response.content_length(), scenario.length_);
                const auto body = co_await response.stream().read_all();
                EXPECT_EQ(body.text(), scenario.body_);
                EXPECT_EQ(response.framing(), scenario.framing_);
                EXPECT_EQ(response.content_length(), scenario.length_);
            });
    }
}

TEST(Ruvia, http1_client_informational_callback_observes_deadline_and_explicit_stop) {
    for (const bool deadline : {false, true}) {
        SCOPED_TRACE(deadline);
        client_upgrade_tests::exchange(
            "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
            [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
                ruvia::stop_source stop;
                ruvia::worker_signal entered(worker);
                const auto handle = client.client().with_options(
                    {.timeout_ = deadline ? std::optional<ruvia::timeout_duration>(std::chrono::milliseconds(500)) : std::nullopt,
                        .stop_token_ = stop.token()});
                auto request = handle.get("/cancel-hints");
                bool callback_stopped = false;
                bool callback_entered = false;
                bool completed = false;
                std::exception_ptr failure;
                std::optional<std::error_code> code;
                request.configure({.informational_ = [&](ruvia::http_status_code, std::span<const ruvia::http_header>,
                                                         ruvia::stop_token token) -> ruvia::task<void> {
                    callback_entered = true;
                    entered.notify();
                    const auto result = co_await ruvia::sleep_for(worker, std::chrono::seconds(2), token);
                    callback_stopped = result == ruvia::timer_sleep_result::stop_requested && token.stop_requested();
                }});
                const auto send = [&]() -> ruvia::task<void> {
                    try {
                        auto response = co_await request.send();
                        static_cast<void>(response);
                    } catch (const ruvia::http_client_error& error) {
                        code = error.code();
                    } catch (...) {
                        failure = std::current_exception();
                    }
                    completed = true;
                    entered.notify();
                };
                ruvia::task_scope requests(worker);
                requests.spawn(send());
                while (!callback_entered && !completed) {
                    co_await entered.wait();
                }
                if (!deadline) {
                    stop.request_stop();
                }
                co_await requests.join();
                if (failure) {
                    std::rethrow_exception(failure);
                }
                EXPECT_TRUE(callback_entered);
                EXPECT_TRUE(callback_stopped);
                EXPECT_EQ(code, deadline ? std::make_error_code(std::errc::timed_out)
                                         : std::make_error_code(std::errc::operation_canceled));
            });
    }
}

TEST(Ruvia, http1_client_buffered_upload_receives_hints_before_blocked_write_completes) {
    enum class outcome { complete,
        cancel,
        early_final };
    constexpr std::size_t content_bytes = 16 * 1024 * 1024;
    for (const auto expected : {outcome::complete, outcome::cancel, outcome::early_final}) {
        SCOPED_TRACE(static_cast<int>(expected));
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        const auto& worker = attachment.loop().worker();
        asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
        asio::ip::tcp::socket socket(io);
        asio::steady_timer drain_gate(io);
        drain_gate.expires_after(std::chrono::seconds(10));
        ruvia::worker_signal peer_finished(worker);
        ruvia::worker_signal callback_entered(worker);
        ruvia::worker_signal callback_release(worker);
        std::size_t received_body = 0;
        bool peer_done = false;
        bool stopping = false;
        std::exception_ptr peer_failure;
        const auto serve = [&]() -> asio::awaitable<void> {
            co_await peer.async_accept(socket, asio::use_awaitable);
            socket.set_option(asio::socket_base::receive_buffer_size(4096));
            std::string request;
            const auto head_bytes = co_await asio::async_read_until(
                socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            EXPECT_TRUE(request.starts_with("POST /buffered HTTP/1.1\r\n"));
            received_body = request.size() - head_bytes;
            constexpr std::string_view hints = "HTTP/1.1 103 Early Hints\r\nX-Phase: blocked-write\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(hints), asio::use_awaitable);
            std::error_code gate_error;
            co_await drain_gate.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
            if (stopping) {
                co_return;
            }
            EXPECT_EQ(gate_error, asio::error::operation_aborted);
            if (expected == outcome::early_final) {
                constexpr std::string_view rejected = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\n\r\n";
                co_await asio::async_write(socket, asio::buffer(rejected), asio::use_awaitable);
                co_return;
            }
            socket.set_option(asio::socket_base::receive_buffer_size(1024 * 1024));
            std::array<char, 65536> bytes;
            while (received_body < content_bytes) {
                const auto count = co_await socket.async_read_some(
                    asio::buffer(bytes.data(), std::min(bytes.size(), content_bytes - received_body)), asio::use_awaitable);
                EXPECT_TRUE(std::all_of(bytes.begin(), bytes.begin() + count, [](char value) { return value == 'b'; }));
                received_body += count;
            }
            constexpr std::string_view accepted = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(socket, asio::buffer(accepted), asio::use_awaitable);
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr error) {
            if (!stopping) {
                peer_failure = error;
            }
            peer_done = true;
            peer_finished.notify();
        });
        std::exception_ptr failure;
        bool saw_callback = false;
        bool completed = false;
        std::optional<std::error_code> error_code;
        const auto run = [&]() -> ruvia::task<void> {
            const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
            ruvia::http_client client(attachment.loop(),
                {.base_url_ = url, .request_timeout_ = std::chrono::seconds(5), .versions_ = ruvia::http_client_versions{.http2_ = false}});
            ruvia::stop_source stop;
            const auto handle = client.client().with_options({.stop_token_ = stop.token()});
            auto request = handle.post("/buffered");
            std::string content(content_bytes, 'b');
            request.body(content);
            std::string().swap(content);
            request.configure({.informational_ = [&](ruvia::http_status_code status, std::span<const ruvia::http_header>,
                                                     ruvia::stop_token token) -> ruvia::task<void> {
                EXPECT_EQ(status.value(), 103);
                EXPECT_LT(received_body, content_bytes);
                EXPECT_LT(handle.stats().plaintext_bytes_sent_, content_bytes);
                saw_callback = true;
                callback_entered.notify();
                co_await callback_release.wait();
                if (expected == outcome::cancel) {
                    EXPECT_TRUE(token.stop_requested());
                } else {
                    drain_gate.cancel();
                }
            }});
            const auto send = [&]() -> ruvia::task<void> {
                try {
                    auto response = co_await request.send();
                    EXPECT_NE(expected, outcome::cancel);
                    EXPECT_EQ(response.status().value(), expected == outcome::early_final ? 413 : 200);
                    const auto body = co_await response.stream().read_all();
                    EXPECT_EQ(body.text(), expected == outcome::early_final ? "" : "ok");
                } catch (const ruvia::http_client_error& error) {
                    error_code = error.code();
                } catch (...) {
                    failure = std::current_exception();
                }
                completed = true;
                callback_entered.notify();
            };
            ruvia::task_scope requests(worker);
            requests.spawn(send());
            while (!saw_callback && !completed) {
                co_await callback_entered.wait();
            }
            if (expected == outcome::cancel) {
                stop.request_stop();
            }
            callback_release.notify();
            co_await requests.join();
            co_await client.shutdown();
            stopping = true;
            drain_gate.cancel();
            peer.close();
            std::error_code ignored;
            socket.close(ignored);
            while (!peer_done) {
                co_await peer_finished.wait();
            }
            attachment.stop();
        };
        auto root = attachment.loop().start(run());
        attachment.run();
        std::move(root).get();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(saw_callback);
        if (expected == outcome::cancel) {
            EXPECT_EQ(error_code, std::make_error_code(std::errc::operation_canceled));
        } else {
            EXPECT_FALSE(error_code.has_value());
        }
        if (expected == outcome::complete) {
            EXPECT_EQ(received_body, content_bytes);
        } else {
            EXPECT_LT(received_body, content_bytes);
        }
    }
}

TEST(Ruvia, http1_client_borrowed_small_chunk_pauses_read_inactivity_until_next_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto& worker = attachment.loop().worker();
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::socket socket(io);
    asio::steady_timer tail_gate(io);
    tail_gate.expires_after(std::chrono::seconds(5));
    ruvia::worker_signal peer_finished(worker);
    bool peer_done = false;
    bool stopping = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        co_await peer.async_accept(socket, asio::use_awaitable);
        std::string request;
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view first = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\na";
        co_await asio::async_write(socket, asio::buffer(first), asio::use_awaitable);
        std::error_code gate_error;
        co_await tail_gate.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
        if (!stopping) {
            EXPECT_EQ(gate_error, asio::error::operation_aborted);
            co_await asio::async_write(socket, asio::buffer("b", 1), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr error) {
        if (!stopping) {
            peer_failure = error;
        }
        peer_done = true;
        peer_finished.notify();
    });
    std::exception_ptr failure;
    const auto run = [&]() -> ruvia::task<void> {
        const auto url = "http://127.0.0.1:" + std::to_string(peer.local_endpoint().port());
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = url, .read_timeout_ = std::chrono::milliseconds(50), .request_timeout_ = std::chrono::seconds(3), .versions_ = ruvia::http_client_versions{.http2_ = false}});
        try {
            auto response = co_await client.client().get("/slow-consumer").send();
            const auto first = co_await response.stream().text();
            EXPECT_EQ(first, std::optional<std::string_view>("a"));
            (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(200));
            EXPECT_EQ(first, std::optional<std::string_view>("a"));
            tail_gate.cancel();
            const auto tail = co_await response.stream().text();
            EXPECT_EQ(tail, std::optional<std::string_view>("b"));
            EXPECT_FALSE((co_await response.stream().text()).has_value());
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        stopping = true;
        tail_gate.cancel();
        peer.close();
        std::error_code ignored;
        socket.close(ignored);
        while (!peer_done) {
            co_await peer_finished.wait();
        }
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
}

namespace websocket_client_test {

template <typename value_type>
ruvia::task<bool> rejects_running_operation(ruvia::task<value_type>&& operation) {
    try {
        static_cast<void>(co_await std::move(operation));
    } catch (const std::logic_error&) {
        co_return true;
    }
    co_return false;
}

ruvia::task<void> await_output(ruvia::task<void>&& operation) {
    co_await std::move(operation);
}

ruvia::task<void> receive_payload(ruvia::task<std::optional<ruvia::websocket_message>>&& operation,
    std::string& payload) {
    const auto message = co_await std::move(operation);
    if (!message) {
        throw std::runtime_error("WebSocket client message expected");
    }
    payload.assign(message->payload());
}

}  // namespace websocket_client_test

namespace {

[[nodiscard]] std::string make_server_handshake_response(std::string_view request) {
    ruvia::http1_server_request_parse_state parsed;
    parsed.parse_head(request, 0);
    if (!parsed.head_ready()) {
        throw std::runtime_error("invalid WebSocket handshake request");
    }
    auto handshake = ruvia::make_websocket_server_handshake(parsed.request(), {});
    std::string response{};
    handshake.for_each_response_part([&response](std::string_view part) {
        response.append(part);
    });
    return response;
}

void check_peer_exchange(bool reply_close,
    bool truncated_frame = false) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool saw_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        // The first frame can arrive in the same transport write as the upgrade.
        std::string response(make_server_handshake_response(request));
        if (truncated_frame) {
            response.append("\x81\x05hi", 4);
        } else {
            response.append("\x82\x05hello", 7);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        if (truncated_frame) {
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
            co_return;
        }
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        saw_close = header_value[0] == 0x88 && (header_value[1] & 0x80) != 0;
        std::array<unsigned char, 129> payload_value{};
        co_await asio::async_read(socket, asio::buffer(payload_value.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        if (reply_close) {
            const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
            co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool succeeded = false;
    bool returned_eof = false;
    ruvia::websocket_termination termination{};
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_sent = 0;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        std::optional<ruvia::websocket> websocket;
        try {
            websocket.emplace(co_await client.client().websocket("/").connect());
            const auto greeting = co_await websocket->read();
            if (truncated_frame) {
                returned_eof = !greeting.has_value();
            } else {
                EXPECT_TRUE(greeting.has_value());
                if (greeting) {
                    EXPECT_EQ(greeting->payload(), std::string_view("hello"));
                }
                if (reply_close) {
                    auto close_operation = websocket->close({});
                    ruvia::task_scope closing(client.worker());
                    closing.spawn(websocket_client_test::await_output(std::move(close_operation)));
                    const bool read_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket->read());
                    const bool write_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket->text("overlap"));
                    const bool close_rejected = co_await websocket_client_test::rejects_running_operation(
                        websocket->close({}));
                    co_await closing.join();
                    EXPECT_TRUE(read_rejected && write_rejected && close_rejected);
                } else {
                    co_await websocket->close({});
                }
                succeeded = true;
            }
        } catch (...) {
            failure = std::current_exception();
        }
        if (websocket) {
            termination = websocket->termination();
            bytes_received = websocket->bytes_received();
            bytes_sent = websocket->bytes_sent();
        }
        websocket.reset();
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    if (truncated_frame) {
        EXPECT_TRUE(returned_eof);
        EXPECT_TRUE(termination == ruvia::websocket_termination::eof);
        EXPECT_EQ(bytes_received, 0U);
    } else {
        EXPECT_TRUE(saw_close);
        EXPECT_TRUE(succeeded);
        // The peer answering Close completes the client's own handshake;
        // otherwise its EOF ends the session.
        EXPECT_TRUE(termination == (reply_close ? ruvia::websocket_termination::local_close : ruvia::websocket_termination::eof));
        // Received "\x82\x05hello" (7 bytes) and the 4-byte Close echo; sent a
        // masked Close carrying 1000 (2 + 4 + 2 bytes).
        EXPECT_EQ(bytes_received, reply_close ? 11U : 7U);
        EXPECT_EQ(bytes_sent, 8U);
    }
}

// RFC 6455 section 7.1.1: after the close handshake the server closes the
// TCP connection first; the client waits for that, bounded by
// close_handshake_timeout_, and only then closes without failing.
void check_server_closes_first(bool client_initiates, bool server_closes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool server_closed = false;
    bool client_eof = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response(make_server_handshake_response(request));
        const std::array<unsigned char, 7> close{0x88, 5, 3, 0xe8, 'b', 'y', 'e'};
        if (!client_initiates) {
            response.append(reinterpret_cast<const char*>(close.data()), close.size());
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        std::array<unsigned char, 129> close_payload{};
        co_await asio::async_read(
            socket, asio::buffer(close_payload.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        std::string after_close{};
        if (client_initiates) {
            after_close.append(reinterpret_cast<const char*>(close.data()), close.size());
        }
        // Data after Close is ignored by the client.
        after_close.append("\x81\x01x", 3);
        co_await asio::async_write(socket, asio::buffer(after_close), asio::use_awaitable);
        asio::steady_timer delay(io, std::chrono::milliseconds(50));
        co_await delay.async_wait(asio::use_awaitable);
        if (server_closes) {
            server_closed = true;
            // Fails if the client already closed; the order check below reports that.
            std::error_code ignored;
            socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
        }
        std::array<char, 64> sink{};
        std::error_code error;
        while (!error) {
            (void)co_await socket.async_read_some(asio::buffer(sink), asio::redirect_error(asio::use_awaitable, error));
        }
        client_eof = true;
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool completed = false;
    bool completed_after_server_close = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.close_handshake_timeout_ = server_closes ? std::chrono::milliseconds(10000)
                                                                                                                          : std::chrono::milliseconds(100)})
                                 .connect();
            auto& handle = websocket;
            EXPECT_TRUE(handle.connected());
            EXPECT_TRUE(handle.subprotocol().empty());
            if (client_initiates) {
                co_await handle.close({});
            } else {
                const auto message = co_await handle.read();
                EXPECT_TRUE(!message.has_value());
            }
            EXPECT_TRUE(!handle.connected());
            EXPECT_EQ(handle.peer_close_code(), std::optional<std::uint16_t>(1000));
            EXPECT_EQ(handle.peer_close_reason(), std::string_view("bye"));
            EXPECT_EQ(websocket.peer_close_code(), std::optional<std::uint16_t>(1000));
            EXPECT_EQ(websocket.peer_close_reason(), std::string_view("bye"));
            completed = true;
            completed_after_server_close = server_closed;
            EXPECT_TRUE(websocket.termination() == (client_initiates ? ruvia::websocket_termination::local_close : ruvia::websocket_termination::peer_close));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        // Let the peer observe the client's end of the connection.
        for (int i = 0; i < 500 && failure == nullptr && !client_eof && peer_failure == nullptr; ++i) {
            (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(10));
        }
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(client_eof);
    EXPECT_EQ(completed_after_server_close, server_closes);
}

}  // namespace

TEST(Ruvia, websocket_client_negotiates_deflate_and_can_skip_individual_messages) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool offered = false;
    bool compressed = false;
    bool uncompressed = false;
    const std::string payload_value(4096, 'a');
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        offered = request.find("permessage-deflate") != std::string_view::npos;
        // The server handshake negotiates and serializes the extension once;
        // appending a second offer would make the response invalid.
        const auto response = make_server_handshake_response(request);
        const auto extension = response.find("permessage-deflate");
        EXPECT_TRUE(extension != std::string_view::npos);
        if (extension != std::string_view::npos) {
            EXPECT_TRUE(response.find("permessage-deflate", extension + 1) == std::string_view::npos);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        ruvia::http_io_buffer protocol_input;
        ruvia::websocket_connection protocol(protocol_input, {.compression_ = {.enabled_ = true}});
        std::array<char, 8192> bytes_value{};
        unsigned messages = 0;
        while (messages != 4) {
            const auto count = co_await socket.async_read_some(asio::buffer(bytes_value), asio::use_awaitable);
            if (messages == 0) {
                compressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) != 0;
            }
            if (messages == 1) {
                uncompressed = (static_cast<unsigned char>(bytes_value[0]) & 0x40) == 0;
            }
            protocol_input.append(std::string_view(bytes_value.data(), count));
            while (auto event = protocol.next_event()) {
                if (const auto* message = event->message()) {
                    EXPECT_EQ(message->payload(), payload_value);
                    ++messages;
                    EXPECT_TRUE(protocol.submit_frame(ruvia::websocket_opcode::text, message->payload()) == ruvia::websocket_frame_submit_status::accepted);
                    const auto output = protocol.output_plan();
                    co_await asio::async_write(socket, asio::buffer(output.bytes()), asio::use_awaitable);
                    (void)protocol.consume_output(output.bytes().size());
                }
            }
        }
        socket.close();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        {
            auto cold = client.client().websocket("/").deflate({.compression_ = {.enabled_ = true}}).connect();
        }
        auto websocket = co_await client.client().websocket("/").deflate({.compression_ = {.enabled_ = true}}).connect();
        auto& handle = websocket;
        {
            auto first_write = handle.text("discarded");
            auto second_write = handle.binary(std::as_bytes(std::span(std::string_view("discarded"))));
            auto byte_write = handle.binary(std::as_bytes(std::span("discarded", 9)));
            auto owned_text = handle.text(std::string("discarded"));
            auto owned_binary = [&] {
                const std::string source("discarded");
                return handle.binary(std::as_bytes(std::span(source)));
            }();
            auto first_read = handle.read();
            auto second_read = handle.read();
            auto first_close = handle.close();
            auto second_close = handle.close({});
            auto ping = handle.ping("discarded");
            auto owned_ping = handle.ping(std::string("discarded"));
            auto pong = handle.pong("discarded");
            auto default_pong = handle.pong();
            auto owned_pong = handle.pong(std::string("discarded"));
            auto send_msg = handle.send(
                ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, "discarded"));
        }

        std::string mutable_payload(payload_value);
        auto first = handle.text(mutable_payload);
        auto second = handle.text(std::string(payload_value), {.compress_ = false});
        mutable_payload.assign("changed before starting the write");
        auto first_read = handle.read();
        auto second_read = handle.read();
        std::string first_payload{};
        {
            ruvia::task_scope writing(client.worker());
            writing.spawn(websocket_client_test::await_output(std::move(first)));
            const bool write_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.text("overlap"));
            const bool close_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.close({}));
            // A failed close releases its read claim and leaves the running write claimed.
            const bool write_still_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.text("overlap"));
            ruvia::task_scope reading(client.worker());
            reading.spawn(websocket_client_test::receive_payload(std::move(first_read), first_payload));
            const bool read_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.read());
            // close() may overlap the running read, but not the running write.
            const bool close_during_write_rejected = co_await websocket_client_test::rejects_running_operation(
                handle.close({}));
            co_await writing.join();
            co_await reading.join();
            EXPECT_TRUE(write_rejected && close_rejected && write_still_rejected && read_rejected &&
                        close_during_write_rejected);
        }
        EXPECT_EQ(first_payload, payload_value);
        co_await std::move(second);
        const auto second_message = co_await std::move(second_read);
        EXPECT_TRUE(second_message.has_value());
        if (second_message) {
            EXPECT_EQ(second_message->payload(), payload_value);
        }
        for (unsigned i = 2; i < 4; ++i) {
            const std::string source(payload_value);
            auto operation = [&] {
                if (i == 2) {
                    return handle.send(
                        ruvia::websocket_message::borrow(ruvia::websocket_opcode::text, source));
                }
                return handle.binary(std::as_bytes(std::span(std::string_view(source))));
            }();
            co_await std::move(operation);
            const auto message = co_await handle.read();
            EXPECT_TRUE(message.has_value());
            if (message) {
                EXPECT_EQ(message->payload(), payload_value);
            }
            EXPECT_TRUE(websocket.connected());
            EXPECT_TRUE(handle.connected());
        }
        client.close();
        co_await client.shutdown();
        peer.close();
        attachment.stop();
    };
    const auto run_to_completion = [&]() -> ruvia::task<void> {
        try {
            co_await run();
        } catch (...) {
            // A rejected handshake must report its error, not leave the test's
            // event-loop work guard running until the whole suite times out.
            peer.close();
            attachment.stop();
            throw;
        }
    };
    auto root = attachment.loop().start(run_to_completion());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(offered);
    EXPECT_TRUE(compressed);
    EXPECT_TRUE(uncompressed);
}

TEST(Ruvia, websocket_client_event_loop_stop_joins_pending_connect) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::steady_timer gate(io, std::chrono::steady_clock::time_point::max());
    std::promise<void> request_received;
    auto received_value = request_received.get_future();
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        request_received.set_value();
        std::error_code ignored;
        co_await gate.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    };
    asio::co_spawn(io, serve(), asio::detached);

    bool connect_failed = false;
    const auto connect_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error&) {
            connect_failed = true;
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(connect_client());
    std::thread driver([&] { attachment.run(); });
    if (received_value.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        asio::post(io, [&gate] { gate.cancel(); });
        driver.join();
        std::move(root).get();
        throw std::runtime_error("WebSocket peer did not receive the upgrade request");
    }
    received_value.get();
    attachment.stop();
    asio::post(io, [&gate] { gate.cancel(); });
    driver.join();
    std::move(root).get();
    EXPECT_TRUE(connect_failed);
}

TEST(Ruvia, websocket_client_handshake_sends_heads_beyond_the_retained_buffer) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    // Larger than the handshake head buffer first tried, within the limit.
    const std::string large_value(20 * 1024, 'v');
    std::exception_ptr peer_failure;
    bool received_large_header = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        ruvia::http1_server_request_parse_state parsed;
        parsed.parse_head(request);
        received_large_header = parsed.head_ready() && parsed.request().header("x-large") == large_value;
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) { peer_failure = failure; });

    bool connected = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const ruvia::http_header_view large_headers[]{{"X-Large", large_value}};
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").headers(large_headers).connect();
            connected = websocket.connected();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(received_large_header);
    EXPECT_TRUE(connected);
}

// One User-Agent rule for every client: the configured agent (default "Ruvia")
// unless the application headers carry their own field, which replaces it; an
// empty agent sends none.
TEST(Ruvia, websocket_client_handshake_sends_the_default_user_agent_unless_headers_supply_one) {
    struct case_type final {
        std::span<const ruvia::http_header_view> headers_;
        std::optional<std::string_view> user_agent_;
        std::string_view expected_;
    };
    const ruvia::http_header_view custom[]{{"User-Agent", "custom/1"}};
    const case_type cases[]{
        {.expected_ = "Ruvia"},
        {.headers_ = custom, .expected_ = "custom/1"},
        {.user_agent_ = "configured/2", .expected_ = "configured/2"},
        {.headers_ = custom, .user_agent_ = "configured/2", .expected_ = "custom/1"},
        {.user_agent_ = "", .expected_ = {}},
    };
    for (const auto& test_case : cases) {
        auto& io = ruvia::test::new_test_io_context();
        auto attachment = ruvia::attach_event_loop(io);
        asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        std::string request{};
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await peer.async_accept(asio::use_awaitable);
            co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            const auto response = make_server_handshake_response(request);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            std::array<char, 1024> input{};
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
            }
        };
        asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) { peer_failure = failure; });

        const auto run_client = [&]() -> ruvia::task<void> {
            const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
            ruvia::http_client_config config{.base_url_ = base_url};
            if (test_case.user_agent_) {
                config.user_agent_ = *test_case.user_agent_;
            }
            ruvia::http_client client(attachment.loop(), config);
            std::exception_ptr failure;
            try {
                auto websocket = co_await client.client().websocket("/").headers(test_case.headers_).connect();
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            peer.close();
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        };
        auto root = attachment.loop().start(run_client());
        attachment.run();
        std::move(root).get();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        ruvia::http1_server_request_parse_state parsed;
        parsed.parse_head(request);
        EXPECT_TRUE(parsed.head_ready());
        std::size_t user_agents = 0;
        for (const auto& field : parsed.request().headers()) {
            if (ruvia::http_ascii_equals_ignore_case(field.name(), "user-agent")) {
                ++user_agents;
                EXPECT_EQ(field.value(), test_case.expected_);
            }
        }
        EXPECT_EQ(user_agents, test_case.expected_.empty() ? 0U : 1U);
    }
}

TEST(Ruvia, websocket_client_event_loop_stop_joins_connected_heartbeat_and_pending_read) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::promise<void> ping_received;
    auto ping_ready = ping_received.get_future();
    std::exception_ptr peer_failure;
    bool observed_client_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        const auto payload_bytes = static_cast<std::size_t>(header_value[1] & 0x7f);
        if (header_value[0] != 0x89 || (header_value[1] & 0x80) == 0 || payload_bytes > 125) {
            throw std::runtime_error("expected a masked client heartbeat ping");
        }
        std::array<unsigned char, 4> mask{};
        co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
        std::array<unsigned char, 125> payload_value{};
        co_await asio::async_read(
            socket, asio::buffer(payload_value.data(), payload_bytes), asio::use_awaitable);
        ping_received.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input),
            asio::redirect_error(asio::use_awaitable, error))) {
        }
        observed_client_close = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    bool connected = false;
    bool read_cancelled = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        auto websocket = co_await client.client().websocket("/").lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20),
                                                                                .pong_timeout_ = std::chrono::seconds(5)}})
                             .connect();
        connected = websocket.connected();
        try {
            read_cancelled = !(co_await websocket.read()).has_value();
        } catch (const std::system_error&) {
            read_cancelled = true;
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    if (ping_ready.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        attachment.stop();
        driver.join();
        std::move(root).get();
        if (peer_failure != nullptr) {
            std::rethrow_exception(peer_failure);
        }
        throw std::runtime_error("WebSocket client did not send its heartbeat ping");
    }
    ping_ready.get();
    attachment.stop();
    driver.join();
    std::move(root).get();
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(connected);
    EXPECT_TRUE(read_cancelled);
    EXPECT_TRUE(observed_client_close);
}

TEST(Ruvia, websocket_client_heartbeat_pings_after_a_blocked_message_write) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    constexpr std::size_t payload_size = std::size_t{16} * 1024 * 1024;
    std::promise<void> ping_received;
    auto ping_ready = ping_received.get_future();
    std::exception_ptr peer_failure;
    std::uint64_t message_bytes = 0;
    bool message_complete = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        // Stop reading for many ping intervals so the large client write stays
        // blocked while its heartbeat becomes due.
        asio::steady_timer pause(socket.get_executor(), std::chrono::milliseconds(200));
        co_await pause.async_wait(asio::use_awaitable);

        std::vector<char> discard(64 * 1024);
        for (;;) {
            std::array<unsigned char, 2> header_value{};
            co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
            if ((header_value[1] & 0x80) == 0) {
                throw std::runtime_error("expected masked client frames");
            }
            std::uint64_t payload_bytes = header_value[1] & 0x7f;
            if (payload_bytes >= 126) {
                std::array<unsigned char, 8> length_value{};
                const std::size_t length_size = payload_bytes == 126 ? 2 : 8;
                co_await asio::async_read(socket, asio::buffer(length_value.data(), length_size), asio::use_awaitable);
                payload_bytes = 0;
                for (std::size_t i = 0; i < length_size; ++i) {
                    payload_bytes = (payload_bytes << 8) | length_value[i];
                }
            }
            std::array<unsigned char, 4> mask{};
            co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
            for (auto remaining = payload_bytes; remaining != 0;) {
                const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, discard.size()));
                co_await asio::async_read(socket, asio::buffer(discard.data(), chunk), asio::use_awaitable);
                remaining -= chunk;
            }
            const auto opcode = header_value[0] & 0x0f;
            if (opcode == 0x9) {
                if (payload_bytes > 125) {
                    throw std::runtime_error("expected a client heartbeat ping");
                }
                break;
            }
            if (opcode != 0x2 && opcode != 0x0) {
                throw std::runtime_error("unexpected client frame before the heartbeat ping");
            }
            message_bytes += payload_bytes;
            message_complete = (header_value[0] & 0x80) != 0;
        }
        ping_received.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input),
            asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    bool wrote = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_size).lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20), .pong_timeout_ = std::chrono::seconds(5)}}).connect();
        const std::string payload(payload_size, 'x');
        co_await websocket.binary(std::as_bytes(std::span(std::string_view(payload))));
        wrote = true;
        try {
            (void)co_await websocket.read();
        } catch (const std::system_error&) {
        }
        co_await client.shutdown();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    if (ping_ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        attachment.stop();
        driver.join();
        std::move(root).get();
        if (peer_failure != nullptr) {
            std::rethrow_exception(peer_failure);
        }
        throw std::runtime_error("WebSocket client did not ping after its blocked write");
    }
    ping_ready.get();
    attachment.stop();
    driver.join();
    std::move(root).get();
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(wrote);
    EXPECT_TRUE(message_complete);
    EXPECT_EQ(message_bytes, std::uint64_t{payload_size});
}

// A peer that stops reading blocks the application write and defers the
// heartbeat Ping. The liveness budget (Ping interval plus Pong timeout) still
// bounds that stall, independently of any transport write timeout.
TEST(Ruvia, websocket_client_heartbeat_aborts_a_write_stalled_by_a_non_reading_peer) {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    constexpr std::size_t payload_size = std::size_t{16} * 1024 * 1024;
    // Held open without reading, so the client's large write fills the
    // socket buffers and stalls until the client gives up.
    std::optional<asio::ip::tcp::socket> stalled;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n",
            asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        stalled.emplace(std::move(socket));
    };
    asio::co_spawn(io, serve(), [&peer_failure](std::exception_ptr failure) {
        peer_failure = failure;
    });

    std::optional<std::error_code> write_error;
    std::promise<void> write_finished;
    auto write_done = write_finished.get_future();
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .write_timeout_ = std::nullopt,
                                                         .request_timeout_ = std::nullopt});
        auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_size).lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(50), .pong_timeout_ = std::chrono::milliseconds(100)}}).connect();
        const std::string payload(payload_size, 'x');
        try {
            co_await websocket.binary(std::as_bytes(std::span(std::string_view(payload))));
        } catch (const std::system_error& error) {
            write_error = error.code();
        }
        co_await client.shutdown();
        write_finished.set_value();
    };
    auto root = attachment.loop().start(run_client());
    std::thread driver([&] { attachment.run(); });
    // Well under the 30 s default write timeout the configuration disabled.
    const bool finished = write_done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    attachment.stop();
    driver.join();
    std::move(root).get();
    if (!finished) {
        throw std::runtime_error("WebSocket client write stalled by a non-reading peer never timed out");
    }
    if (peer_failure != nullptr) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(write_error.has_value());
    if (write_error) {
        EXPECT_TRUE(*write_error == std::errc::timed_out);
    }
}

TEST(Ruvia, websocket_client_close_requires_peer_close) {
    check_peer_exchange(false);
    check_peer_exchange(true);
}

TEST(Ruvia, websocket_client_read_rejects_eof_during_incomplete_frame) {
    check_peer_exchange(false, true);
}

TEST(Ruvia, websocket_client_lets_the_server_close_the_connection_first) {
    for (const bool client_initiates : {false, true}) {
        check_server_closes_first(client_initiates, true);
        check_server_closes_first(client_initiates, false);
    }
}

TEST(Ruvia, websocket_client_reads_frame_above_default_message_limit) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    std::string payload_value(ruvia::default_max_websocket_message_bytes + 1, '\0');
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>('a' + i % 26);
    }
    bool saw_close = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response(make_server_handshake_response(request));
        response.push_back('\x82');
        response.push_back('\x7f');
        const auto length = static_cast<std::uint64_t>(payload_value.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            response.push_back(static_cast<char>((length >> shift) & 0xffU));
        }
        response.append(payload_value);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<unsigned char, 2> header_value{};
        co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
        saw_close = header_value[0] == 0x88 && (header_value[1] & 0x80) != 0;
        std::array<unsigned char, 129> close_payload{};
        co_await asio::async_read(socket, asio::buffer(close_payload.data(), 4 + (header_value[1] & 0x7f)), asio::use_awaitable);
        const std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
        co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool matched = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        // A single frame larger than the protocol's default input bound must be
        // delivered intact when the configured message limit admits it.
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url, .read_timeout_ = std::chrono::seconds(10)});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").max_message_bytes(payload_value.size()).connect();
            const auto message = co_await websocket.read();
            matched = message.has_value() && message->payload() == payload_value;
            co_await websocket.close({});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(matched);
    EXPECT_TRUE(saw_close);
}

TEST(Ruvia, websocket_client_rejects_untrusted_tls_peer) {
    asio::ssl::context tls(asio::ssl::context::tls_server);
    const auto require = [](bool success) {
        if (!success) {
            throw std::runtime_error("could not generate test certificate");
        }
    };
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
    require(generator != nullptr);
    require(EVP_PKEY_keygen_init(generator.get()) == 1);
    require(EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1);
    EVP_PKEY* raw_key = nullptr;
    require(EVP_PKEY_generate(generator.get(), &raw_key) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
    require(certificate != nullptr);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    require(name != nullptr);
    require(X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_subject_name(certificate.get(), name.get()) == 1);
    require(X509_set_issuer_name(certificate.get(), name.get()) == 1);
    require(ruvia::test::sign_tls_certificate(certificate.get(), key.get()) > 0);
    require(SSL_CTX_use_certificate(tls.native_handle(), certificate.get()) == 1);
    require(SSL_CTX_use_PrivateKey(tls.native_handle(), key.get()) == 1);

    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool handshake_rejected = false;
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), tls);
        std::error_code error;
        co_await stream.async_handshake(asio::ssl::stream_base::server,
            asio::redirect_error(asio::use_awaitable, error));
        handshake_rejected = static_cast<bool>(error);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool tls_failure = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("https://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error& error) {
            tls_failure = error.code() == ruvia::make_error_code(ruvia::http_client_errc::tls_failed);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(tls_failure);
    EXPECT_TRUE(handshake_rejected);
}

namespace {

struct client_frame final {
    unsigned char opcode_;
    std::string payload_{};
};

asio::awaitable<client_frame> read_client_frame(asio::ip::tcp::socket& socket) {
    std::array<unsigned char, 2> header_value{};
    co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
    if ((header_value[1] & 0x80) == 0) {
        throw std::runtime_error("expected masked client frames");
    }
    std::uint64_t payload_bytes = header_value[1] & 0x7f;
    if (payload_bytes >= 126) {
        std::array<unsigned char, 8> length_value{};
        const std::size_t length_size = payload_bytes == 126 ? 2 : 8;
        co_await asio::async_read(socket, asio::buffer(length_value.data(), length_size), asio::use_awaitable);
        payload_bytes = 0;
        for (std::size_t i = 0; i < length_size; ++i) {
            payload_bytes = (payload_bytes << 8) | length_value[i];
        }
    }
    std::array<unsigned char, 4> mask{};
    co_await asio::async_read(socket, asio::buffer(mask), asio::use_awaitable);
    std::string payload_value(static_cast<std::size_t>(payload_bytes), '\0');
    co_await asio::async_read(socket, asio::buffer(payload_value), asio::use_awaitable);
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        payload_value[i] = static_cast<char>(payload_value[i] ^ static_cast<char>(mask[i % mask.size()]));
    }
    co_return client_frame{static_cast<unsigned char>(header_value[0] & 0x0f), std::move(payload_value)};
}

// Answers the client's Close and ends the peer.
asio::awaitable<void> answer_client_close(asio::ip::tcp::socket& socket) {
    constexpr std::array<unsigned char, 4> close{0x88, 2, 3, 0xe8};
    co_await asio::async_write(socket, asio::buffer(close), asio::use_awaitable);
    socket.shutdown(asio::ip::tcp::socket::shutdown_send);
}

void check_heartbeat_policy(bool enabled) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto worker = attachment.loop().worker();
    ruvia::worker_signal peer_done(worker);
    std::exception_ptr peer_failure;
    std::size_t pings = 0;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        try {
            for (;;) {
                const auto frame = co_await read_client_frame(socket);
                if (frame.opcode_ == 0x8) {
                    co_await answer_client_close(socket);
                    co_return;
                }
                if (frame.opcode_ == 0x9) {
                    ++pings;
                    continue;  // Deliberately never acknowledge heartbeat Pings.
                }
                if (frame.opcode_ == 0x1) {
                    constexpr std::string_view ready = "\x81\x05ready";
                    co_await asio::async_write(socket, asio::buffer(ready), asio::use_awaitable);
                }
            }
        } catch (const std::system_error& error) {
            if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset) {
                throw;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url,
                                                         .read_timeout_ = std::chrono::seconds(5),
                                                         .request_timeout_ = std::nullopt});
        std::exception_ptr failure;
        try {
            ruvia::websocket_lifecycle_options lifecycle;
            lifecycle.heartbeat_.ping_interval_ =
                enabled ? std::optional(std::chrono::milliseconds(20)) : std::nullopt;
            lifecycle.heartbeat_.pong_timeout_ =
                enabled ? std::optional(std::chrono::milliseconds(50)) : std::nullopt;
            auto websocket = co_await client.client().websocket("/").lifecycle(lifecycle).connect();
            if (!enabled) {
                (void)co_await ruvia::sleep_for(websocket.worker(), std::chrono::milliseconds(200));
                EXPECT_TRUE(websocket.connected());
            }
            co_await websocket.text("check");
            const auto ready = co_await websocket.read();
            EXPECT_TRUE(ready && ready->payload() == "ready");
            if (enabled) {
                const auto started = std::chrono::steady_clock::now();
                EXPECT_FALSE((co_await websocket.read()).has_value());
                EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
                EXPECT_TRUE(!websocket.connected());
                EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::timeout);
            } else {
                co_await websocket.close();
                EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::local_close);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        // Opening can fail before the peer accepts. Cancel that pending accept
        // before joining its completion so the original exception is reported.
        peer.close();
        co_await peer_done.wait();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(enabled ? pings != 0 : pings == 0);
}

enum class client_failure_scenario : std::uint8_t {
    protocol_error,
    message_too_large,
    aborted,
};

// The peer sends the scenario's frame, then drains the client until it ends.
void check_client_failure_termination(client_failure_scenario scenario) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto worker = attachment.loop().worker();
    ruvia::worker_signal peer_done(worker);
    std::exception_ptr peer_failure;
    std::string close_payload{};
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        std::string response(make_server_handshake_response(request));
        if (scenario == client_failure_scenario::protocol_error) {
            // Reserved data opcode 0x3 (RFC 6455 section 5.2).
            response.append("\x83\x00", 2);
        } else if (scenario == client_failure_scenario::message_too_large) {
            response.append("\x81\x05hello", 7);
        }
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        try {
            for (;;) {
                auto frame = co_await read_client_frame(socket);
                if (frame.opcode_ == 0x8) {
                    close_payload = std::move(frame.payload_);
                }
            }
        } catch (const std::system_error& error) {
            if (error.code() != asio::error::eof && error.code() != asio::error::connection_reset) {
                throw;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    ruvia::websocket_termination termination{};
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_sent = 0;
    const auto run = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url, .read_timeout_ = std::chrono::seconds(5)});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").max_message_bytes(4).connect();
            EXPECT_TRUE(websocket.termination() == ruvia::websocket_termination::none);
            if (scenario == client_failure_scenario::aborted) {
                websocket.abort();
            } else {
                EXPECT_FALSE((co_await websocket.read()).has_value());
            }
            // A later abort keeps the first cause.
            websocket.abort();
            termination = websocket.termination();
            bytes_received = websocket.bytes_received();
            bytes_sent = websocket.bytes_sent();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        co_await peer_done.wait();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    // A rejected frame is never counted; the failure Close carries only its
    // status code (2 + 4 + 2 bytes).
    EXPECT_EQ(bytes_received, 0U);
    switch (scenario) {
        case client_failure_scenario::protocol_error:
            EXPECT_TRUE(termination == ruvia::websocket_termination::protocol_error);
            EXPECT_EQ(close_payload, std::string_view("\x03\xea", 2));
            EXPECT_EQ(bytes_sent, 8U);
            break;
        case client_failure_scenario::message_too_large:
            EXPECT_TRUE(termination == ruvia::websocket_termination::message_too_large);
            EXPECT_EQ(close_payload, std::string_view("\x03\xf1", 2));
            EXPECT_EQ(bytes_sent, 8U);
            break;
        case client_failure_scenario::aborted:
            EXPECT_TRUE(termination == ruvia::websocket_termination::aborted);
            EXPECT_TRUE(close_payload.empty());
            EXPECT_EQ(bytes_sent, 0U);
            break;
    }
}

}  // namespace

TEST(Ruvia, websocket_client_active_receive_times_out_an_unanswered_heartbeat) {
    check_heartbeat_policy(true);
}

TEST(Ruvia, websocket_client_termination_reports_protocol_failures_and_local_abort) {
    check_client_failure_termination(client_failure_scenario::protocol_error);
    check_client_failure_termination(client_failure_scenario::message_too_large);
    check_client_failure_termination(client_failure_scenario::aborted);
}

TEST(Ruvia, websocket_client_absent_ping_interval_disables_heartbeat) {
    check_heartbeat_policy(false);
}

TEST(Ruvia, websocket_client_keeps_a_peer_whose_pong_waits_for_the_next_read) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        bool answered = false;
        for (;;) {
            const auto frame = co_await read_client_frame(socket);
            if (frame.opcode_ == 0x8) {
                co_await answer_client_close(socket);
                co_return;
            }
            if (frame.opcode_ != 0x9) {
                continue;
            }
            std::string pong({static_cast<char>(0x8a), static_cast<char>(frame.payload_.size())});
            pong.append(frame.payload_);
            if (!answered) {
                pong.append("\x81\x05ready", 7);
                answered = true;
            }
            co_await asio::async_write(socket, asio::buffer(pong), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    bool connected_before_read = false;
    bool received = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").lifecycle({.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds(20),
                                                                                    .pong_timeout_ = std::chrono::milliseconds(50)}})
                                 .connect();
            // The answered Ping's Pong stays unread far past its timeout.
            (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(300));
            connected_before_read = websocket.connected();
            const auto message = co_await websocket.read();
            received = message.has_value() && message->payload() == std::string_view("ready");
            co_await websocket.close({});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(connected_before_read);
    EXPECT_TRUE(received);
}

TEST(Ruvia, websocket_client_read_progresses_between_back_to_back_writes) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const auto response = make_server_handshake_response(request);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        bool answered = false;
        for (;;) {
            const auto frame = co_await read_client_frame(socket);
            if (frame.opcode_ == 0x8) {
                co_await answer_client_close(socket);
                co_return;
            }
            if (!answered) {
                // Arrives while the client keeps writing.
                constexpr std::string_view hello = "\x81\x05hello";
                co_await asio::async_write(socket, asio::buffer(hello), asio::use_awaitable);
                answered = true;
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    constexpr std::size_t write_count = 1000;
    std::size_t writes = 0;
    std::size_t writes_before_read = write_count;
    bool received = false;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
            ruvia::task_scope reader(client.worker());
            const auto receive = [&]() -> ruvia::task<void> {
                const auto message = co_await websocket.read();
                writes_before_read = writes;
                received = message.has_value() && message->payload() == std::string_view("hello");
            };
            reader.spawn(receive());
            for (; writes != write_count; ++writes) {
                co_await websocket.binary(std::as_bytes(std::span(std::string_view("x"))));
            }
            co_await reader.join();
            co_await websocket.close({});
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(received);
    EXPECT_TRUE(writes_before_read < write_count);
}

TEST(Ruvia, websocket_client_handshake_rejection_records_upstream_http_status) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    asio::ip::tcp::acceptor peer(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await peer.async_accept(asio::use_awaitable);
        std::string request{};
        co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view rejected =
            "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(rejected), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) { peer_failure = failure; });
    std::optional<std::error_code> error_code;
    std::optional<ruvia::http_status_code> status_code;
    const auto run_client = [&]() -> ruvia::task<void> {
        const std::string base_url("http://127.0.0.1:" + std::string(std::to_string(peer.local_endpoint().port())));
        ruvia::http_client client(attachment.loop(), {.base_url_ = base_url});
        std::exception_ptr failure;
        try {
            auto websocket = co_await client.client().websocket("/").connect();
        } catch (const ruvia::http_client_error& error) {
            error_code = error.code();
            status_code = error.status();
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.close();
        attachment.stop();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    auto root = attachment.loop().start(run_client());
    attachment.run();
    std::move(root).get();
    if (peer_failure) {
        std::rethrow_exception(peer_failure);
    }
    EXPECT_TRUE(error_code == ruvia::make_error_code(ruvia::http_client_errc::handshake_rejected));
    EXPECT_TRUE(status_code == ruvia::http_status::unauthorized);
}

namespace ruvia::test::dynamic_origin_tests {

struct observed_configuration final {
    worker_signal& retired_;
    bool& destroyed_;
    ~observed_configuration() {
        destroyed_ = true;
        retired_.notify();
    }
};
// One real persistent connection. An empty reply deliberately leaves a request
// pending until its client closes the transport.
class peer final {
public:
    peer(asio::io_context& io, const worker_ref& worker, std::span<const std::string_view> replies)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io),
          replies_(replies),
          received_(worker),
          finished_(worker) {
        asio::co_spawn(io, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            complete_ = true;
            received_.notify();
            finished_.notify();
        });
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port());
    }

    task<void> received() {
        while (requests_.empty() && !complete_) {
            co_await received_.wait();
        }
        if (requests_.empty()) {
            throw std::runtime_error("HTTP peer ended before receiving a request");
        }
    }

    task<void> join() {
        while (!complete_) {
            co_await finished_.wait();
        }
        if (failure_ && !aborted_) {
            std::rethrow_exception(failure_);
        }
    }

    void abort() noexcept {
        aborted_ = true;
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
    }

    std::vector<std::string> requests_{};
    bool client_closed_{};

private:
    asio::awaitable<void> serve() {
        socket_ = co_await acceptor_.async_accept(asio::use_awaitable);
        std::string input;
        for (const auto reply : replies_) {
            const auto count = co_await asio::async_read_until(socket_, asio::dynamic_buffer(input),
                "\r\n\r\n", asio::use_awaitable);
            requests_.emplace_back(input.substr(0, count));
            input.erase(0, count);
            received_.notify();
            if (!reply.empty()) {
                co_await asio::async_write(socket_, asio::buffer(reply), asio::use_awaitable);
            }
        }
        std::array<char, 1> trailing;
        std::error_code error;
        (void)co_await socket_.async_read_some(asio::buffer(trailing),
            asio::redirect_error(asio::use_awaitable, error));
        client_closed_ = error == asio::error::eof || error == asio::error::connection_reset;
        if (!client_closed_) {
            throw std::runtime_error("HTTP peer expected client transport retirement");
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    std::span<const std::string_view> replies_;
    worker_signal received_;
    worker_signal finished_;
    std::exception_ptr failure_;
    bool complete_{};
    bool aborted_{};
};

[[nodiscard]] task<void> expect_closed(task<http_client_response> operation) {
    try {
        (void)co_await std::move(operation);
        ADD_FAILURE() << "retired origin admitted an HTTP request";
    } catch (const http_client_error& error) {
        EXPECT_EQ(error.code(), make_error_code(http_client_errc::closing));
    }
}

}  // namespace ruvia::test::dynamic_origin_tests

TEST(Ruvia, http_client_dynamic_origins_own_inputs_and_atomically_replace_one_alias) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), std::span<const ruvia::http_client_config>{});
        std::exception_ptr failure;
        try {
            std::string alias = "default";
            std::string url = "http://127.0.0.1:8001";
            auto update = client.upsert({.alias_ = alias, .base_url_ = url});
            alias.assign("changed");
            url.assign("http://127.0.0.1:8999");
            co_await std::move(update);
            auto original = client.client();
            EXPECT_EQ(original.port(), 8001);
            auto cold_send = original.get("/cold").send();
            auto cold_upload = original.post("/cold-upload").upload();

            EXPECT_THROW((void)client.upsert({.base_url_ = "http://127.0.0.1:8002/path"}), std::invalid_argument);
            EXPECT_EQ(client.client().port(), 8001);
            EXPECT_THROW((void)client.upsert({.alias_ = "", .base_url_ = "http://127.0.0.1:8002"}), std::invalid_argument);
            EXPECT_THROW((void)client.remove(""), std::invalid_argument);
            EXPECT_FALSE(client.remove("missing"));

            co_await client.upsert({.alias_ = "unchanged", .base_url_ = "http://127.0.0.1:8003"});
            auto unchanged = client.client("unchanged");
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
            EXPECT_EQ(client.client().port(), 8002);
            EXPECT_EQ(original.port(), 8001);
            EXPECT_EQ(unchanged.port(), 8003);
            co_await ruvia::test::dynamic_origin_tests::expect_closed(std::move(cold_send));
            try {
                (void)co_await std::move(cold_upload);
                ADD_FAILURE() << "retired origin admitted a cold upload";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::closing));
            }

            auto removed = client.client();
            auto removed_send = removed.get("/removed").send();
            EXPECT_TRUE(client.remove("default"));
            EXPECT_FALSE(client.remove("default"));
            try {
                (void)client.client("default");
                ADD_FAILURE() << "removed alias remained visible";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::not_configured));
            }
            EXPECT_EQ(removed.port(), 8002);
            EXPECT_EQ(client.client().port(), 8003);
            co_await ruvia::test::dynamic_origin_tests::expect_closed(std::move(removed_send));
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8004"});
            EXPECT_EQ(client.client().port(), 8004);
            EXPECT_EQ(client.client("unchanged").port(), 8003);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        EXPECT_THROW((void)client.upsert({.base_url_ = "http://127.0.0.1:8005"}), ruvia::http_client_error);
        EXPECT_THROW((void)client.remove("default"), ruvia::http_client_error);
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_dynamic_origins_preserve_other_pool_connections_and_cookies) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto loop = attachment.loop();
    constexpr std::string_view reply =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nSet-Cookie: stable=1; Path=/\r\n\r\n";
    const std::array replies{reply, reply};
    ruvia::test::dynamic_origin_tests::peer peer(io, loop.worker(), replies);
    const auto url = peer.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.alias_ = "stable", .base_url_ = url, .request_timeout_ = std::chrono::seconds(2), .versions_ = ruvia::http_client_versions{.http2_ = false}, .received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send});
        std::exception_ptr failure;
        try {
            auto stable = client.client("stable");
            auto first = co_await stable.get("/first").send();
            EXPECT_TRUE(first.stream().complete());
            const auto before = stable.stats().completed_requests_;
            co_await client.upsert({.alias_ = "other", .base_url_ = "http://127.0.0.1:8001"});
            co_await client.upsert({.alias_ = "other", .base_url_ = "http://127.0.0.1:8002"});
            EXPECT_TRUE(client.remove("other"));
            auto second = co_await client.client("stable").get("/second").send();
            EXPECT_TRUE(second.stream().complete());
            EXPECT_EQ(stable.stats().completed_requests_, before + 1);
            EXPECT_EQ(peer.requests_.size(), 2U);
            if (peer.requests_.size() == 2) {
                EXPECT_NE(peer.requests_[1].find("stable=1"), std::string_view::npos);
            }
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            peer.abort();
        }
        co_await peer.join();
        EXPECT_TRUE(peer.client_closed_);
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_dynamic_origin_replacement_cancels_and_joins_live_requests) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto loop = attachment.loop();
    constexpr std::array replies{std::string_view{}};
    ruvia::test::dynamic_origin_tests::peer peer(io, loop.worker(), replies);
    const auto url = peer.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.base_url_ = url, .request_timeout_ = std::chrono::seconds(2)});
        ruvia::task_scope requests(loop.worker());
        requests.spawn(ruvia::test::dynamic_origin_tests::expect_closed(client.client().get("/pending").send()));
        co_await peer.received();
        co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
        co_await requests.join();
        co_await peer.join();
        EXPECT_TRUE(peer.client_closed_);
        EXPECT_EQ(client.client().port(), 8002);
        co_await client.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_redirect_policy_can_register_an_origin_and_remains_fail_closed) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto loop = attachment.loop();
    constexpr std::array target_replies{std::string_view("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
    ruvia::test::dynamic_origin_tests::peer target(io, loop.worker(), target_replies);
    const auto target_url = target.url();
    const auto redirect = "HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: " + target_url + "/destination\r\n\r\n";
    const std::array source_replies{std::string_view(redirect), std::string_view(redirect), std::string_view(redirect), std::string_view(redirect)};
    ruvia::test::dynamic_origin_tests::peer source(io, loop.worker(), source_replies);
    const auto source_url = source.url();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(loop, {.base_url_ = source_url,
                                            .request_timeout_ = std::chrono::seconds(2),
                                            .versions_ = ruvia::http_client_versions{.http2_ = false}});
        std::exception_ptr failure;
        try {
            auto denied = co_await client.client().get("/without-policy").send();
            EXPECT_EQ(denied.status().value(), 302);
            EXPECT_TRUE(target.requests_.empty());

            auto unconfigured = client.client().get("/policy-without-origin");
            unconfigured.on_redirect([](const ruvia::http_client_redirect_hop&, ruvia::stop_token) -> ruvia::task<bool> { co_return true; });
            auto still_denied = co_await unconfigured.send();
            EXPECT_EQ(still_denied.status().value(), 302);
            EXPECT_TRUE(target.requests_.empty());

            std::size_t decisions = 0;
            auto request = client.client().get("/with-policy");
            request.header("Authorization", "Bearer private");
            request.header("Proxy-Authorization", "Basic private");
            request.header("Cookie", "source=private");
            request.on_redirect([&](const ruvia::http_client_redirect_hop& hop, ruvia::stop_token stop) -> ruvia::task<bool> {
                ++decisions;
                EXPECT_EQ(hop.target_origin().host(), "127.0.0.1");
                co_await client.upsert({.alias_ = "discovered", .base_url_ = target_url, .versions_ = ruvia::http_client_versions{.http2_ = false}},
                    {.stop_token_ = std::move(stop)});
                co_return true;
            });
            auto accepted = co_await request.send();
            EXPECT_EQ(accepted.status().value(), 200);
            EXPECT_EQ(decisions, 1U);
            EXPECT_EQ(target.requests_.size(), 1U);
            if (!target.requests_.empty()) {
                const auto& wire = target.requests_.front();
                EXPECT_TRUE(wire.starts_with("GET /destination HTTP/1.1\r\n"));
                EXPECT_EQ(wire.find("private"), std::string_view::npos);
            }

            auto rejected = client.client().get("/policy-denies-registered");
            rejected.on_redirect([&](const ruvia::http_client_redirect_hop&, ruvia::stop_token) -> ruvia::task<bool> {
                ++decisions;
                co_return false;
            });
            auto result = co_await rejected.send();
            EXPECT_EQ(result.status().value(), 302);
            EXPECT_EQ(decisions, 2U);
            EXPECT_EQ(target.requests_.size(), 1U);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            source.abort();
            target.abort();
        }
        co_await source.join();
        co_await target.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_retired_origin_handles_and_cold_operations_expire_with_owner) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_ref> retired;
        std::optional<ruvia::task<ruvia::http_client_response>> cold;
        std::optional<ruvia::task<ruvia::http_client_exchange>> upload;
        std::optional<ruvia::task<ruvia::http_client_tunnel_result>> tunnel;
        {
            ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
            retired.emplace(client.client());
            cold.emplace(retired->get("/cold").send());
            upload.emplace(client.client().post("/cold-upload").upload());
            tunnel.emplace(client.client().connect("127.0.0.1:8001").open());
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
            co_await client.shutdown();
        }
        EXPECT_THROW((void)retired->port(), std::logic_error);
        try {
            (void)co_await std::move(*cold);
            ADD_FAILURE() << "cold request survived its origin owner";
        } catch (const std::logic_error&) {
        }
        try {
            (void)co_await std::move(*upload);
            ADD_FAILURE() << "cold upload survived its origin owner";
        } catch (const std::logic_error&) {
        }
        try {
            (void)co_await std::move(*tunnel);
            ADD_FAILURE() << "cold tunnel survived its origin owner";
        } catch (const std::logic_error&) {
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_dynamic_origins_reject_non_worker_mutation) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
    EXPECT_THROW((void)client.upsert({.base_url_ = "http://127.0.0.1:8002"}), std::logic_error);
    EXPECT_THROW((void)client.remove("default"), std::logic_error);
    const auto run = [&]() -> ruvia::task<void> {
        EXPECT_EQ(client.client().port(), 8001);
        co_await client.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_cold_upload_and_tunnel_pin_replaced_origins_without_handles) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        std::exception_ptr failure;
        try {
            auto upload = client.client().post("/cold-upload").upload();
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8002"});
            try {
                (void)co_await std::move(upload);
                ADD_FAILURE() << "cold upload entered a replaced origin";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::closing));
            }
            auto tunnel = client.client().connect("127.0.0.1:8002").open();
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8003"});
            try {
                (void)co_await std::move(tunnel);
                ADD_FAILURE() << "cold tunnel entered a replaced origin";
            } catch (const ruvia::http_client_error& error) {
                EXPECT_EQ(error.code(), ruvia::make_error_code(ruvia::http_client_errc::closing));
            }
            auto send = client.client().get("/cold-send").send();
            EXPECT_TRUE(client.remove("default"));
            co_await ruvia::test::dynamic_origin_tests::expect_closed(std::move(send));
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_retired_origin_resources_release_after_last_cold_borrow) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), std::span<const ruvia::http_client_config>{});
        ruvia::worker_signal retired(worker);
        bool destroyed = false;
        bool expired = false;
        std::weak_ptr<ruvia::test::dynamic_origin_tests::observed_configuration> observed;
        {
            auto marker = std::make_shared<ruvia::test::dynamic_origin_tests::observed_configuration>(retired, destroyed);
            observed = marker;
            co_await client.upsert({.base_url_ = "http://127.0.0.1:8001",
                .address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([marker](const ruvia::ip_address&, std::uint16_t) { return marker != nullptr; })});
        }
        std::optional<ruvia::http_client_ref> old(client.client());
        std::optional<ruvia::task<ruvia::http_client_exchange>> cold(old->post("/cold").upload());
        EXPECT_TRUE(client.remove("default"));
        EXPECT_FALSE(observed.expired());
        old.reset();
        EXPECT_FALSE(observed.expired());
        cold.reset();
        ruvia::worker_timer_registration deadline;
        (void)worker.schedule_timer(deadline, ruvia::deadline_after(std::chrono::seconds(2)),
            [&](ruvia::worker_timer_outcome outcome) noexcept {
                if (outcome == ruvia::worker_timer_outcome::expired) {
                    expired = true;
                    retired.notify();
                }
            });
        while (!destroyed && !expired) {
            co_await retired.wait();
        }
        deadline.cancel();
        EXPECT_TRUE(destroyed);
        EXPECT_TRUE(observed.expired());
        co_await client.shutdown();
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

TEST(Ruvia, http_client_tls_upsert_prepares_owned_paths_and_password_off_worker) {
    ruvia::test::tls_identity identity("localhost");
    const auto encrypted_key = identity.private_key_file().parent_path() / "encrypted.pem";
    const std::string password("secret\0part", 11);
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(encrypted_key.string().c_str(), "w"), BIO_free);
        ASSERT_NE(output, nullptr);
        ASSERT_EQ(PEM_write_bio_PrivateKey(output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle()),
                      EVP_aes_256_cbc(), reinterpret_cast<const unsigned char*>(password.data()),
                      static_cast<int>(password.size()), nullptr, nullptr),
            1);
    }
    auto& io = ruvia::test::new_test_io_context();
    ruvia::blocking_pool blocking({.thread_count_ = 1});
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &blocking});
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        std::exception_ptr failure;
        try {
            std::string alias = "secure";
            std::string url = "https://localhost:8443";
            ruvia::http_client_config config{.alias_ = alias, .base_url_ = url};
            config.tls_.ca_file_ = identity.ca_file_;
            config.tls_.identity_.emplace();
            config.tls_.identity_->certificate_chain_file_ = identity.ca_file_;
            config.tls_.identity_->private_key_file_ = encrypted_key;
            config.tls_.identity_->private_key_password_.assign(password);
            auto update = client.upsert(config);
            alias.assign("overwritten");
            url.assign("http://127.0.0.1:8999");
            config.tls_.ca_file_ = encrypted_key.parent_path() / "missing-ca.pem";
            config.tls_.identity_->private_key_file_ = encrypted_key.parent_path() / "missing-key.pem";
            config.tls_.identity_->private_key_password_.assign("incorrect");
            co_await std::move(update);
            auto secure = client.client("secure");
            EXPECT_EQ(secure.scheme(), ruvia::http_scheme::https);
            EXPECT_EQ(secure.port(), 8443);
            EXPECT_EQ(client.client().port(), 8001);

            config.alias_ = "secure";
            config.base_url_ = "https://localhost:9443";
            auto failed_update = client.upsert(config);
            bool failed = false;
            try {
                co_await std::move(failed_update);
            } catch (const std::exception&) {
                failed = true;
            }
            EXPECT_TRUE(failed);
            EXPECT_EQ(client.client("secure").port(), 8443);
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
    blocking.join();
}

TEST(Ruvia, http_client_tls_upsert_without_blocking_pool_fails_without_publication) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        try {
            co_await client.upsert({.base_url_ = "https://localhost:8443"});
            ADD_FAILURE() << "TLS update used a worker without a blocking pool";
        } catch (const ruvia::blocking_operation_rejected& error) {
            EXPECT_EQ(error.status(), ruvia::blocking_status::unavailable);
        }
        EXPECT_EQ(client.client().port(), 8001);
        auto cold = client.upsert({.base_url_ = "https://localhost:9443"});
        co_await client.shutdown();
        try {
            co_await std::move(cold);
            ADD_FAILURE() << "cold origin update survived shutdown";
        } catch (const std::logic_error&) {
        }
    };
    ruvia::test::run_worker_tasks(attachment, run());
}

namespace ruvia::test::dynamic_origin_tests {

enum class cancellation_case { timeout,
    caller_stop,
    owner_close };

void exercise_origin_preparation_cancellation(cancellation_case mode) {
    tls_identity identity("localhost");
    auto& io = new_test_io_context();
    blocking_pool blocking({.thread_count_ = 1, .queue_capacity_ = 4});
    std::promise<void> release;
    auto gate = release.get_future().share();
    ASSERT_EQ(blocking.submit([gate] { gate.wait(); }), blocking_submit_status::accepted);
    auto attachment = attach_event_loop(io, {.blocking_pool_ = &blocking});
    const auto worker = attachment.loop().worker();
    const auto run = [&]() -> task<void> {
        http_client client(attachment.loop(), {.base_url_ = "http://127.0.0.1:8001"});
        http_client_config config{.base_url_ = "https://localhost:8443"};
        config.tls_.ca_file_ = identity.ca_file_;
        stop_source stop;
        bool update_finished = false;
        bool shutdown_finished = false;
        std::exception_ptr failure;
        const auto update = [&]() -> task<void> {
            try {
                co_await client.upsert(config, {.timeout_ = mode == cancellation_case::timeout
                                                                ? timeout_duration{5}
                                                                : timeout_duration{2000},
                                                   .stop_token_ = stop.token()});
                ADD_FAILURE() << "cancelled TLS preparation published an origin";
            } catch (const http_client_error& error) {
                const auto expected = mode == cancellation_case::owner_close ? make_error_code(http_client_errc::closing)
                                      : mode == cancellation_case::timeout   ? std::make_error_code(std::errc::timed_out)
                                                                             : std::make_error_code(std::errc::operation_canceled);
                EXPECT_EQ(error.code(), expected);
            } catch (...) {
                failure = std::current_exception();
            }
            update_finished = true;
        };
        const auto shutdown = [&]() -> task<void> {
            co_await client.shutdown();
            shutdown_finished = true;
        };
        task_scope tasks(worker);
        tasks.spawn(update());
        if (mode == cancellation_case::caller_stop) {
            stop.request_stop();
        } else if (mode == cancellation_case::owner_close) {
            tasks.spawn(shutdown());
        }
        (void)co_await sleep_for(worker, std::chrono::milliseconds(20));
        EXPECT_FALSE(update_finished);
        EXPECT_FALSE(shutdown_finished);
        release.set_value();
        co_await tasks.join();
        EXPECT_TRUE(update_finished);
        if (mode != cancellation_case::owner_close) {
            EXPECT_EQ(client.client().port(), 8001);
        } else {
            EXPECT_TRUE(shutdown_finished);
        }
        co_await client.shutdown();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    run_worker_tasks(attachment, run());
    blocking.join();
}

void exercise_redirect_policy_cancellation(cancellation_case mode) {
    auto& io = new_test_io_context();
    auto attachment = attach_event_loop(io);
    const auto worker = attachment.loop().worker();
    constexpr std::array replies{std::string_view(
        "HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: http://127.0.0.1:8123/next\r\n\r\n")};
    peer source(io, worker, replies);
    const auto url = source.url();
    const auto run = [&]() -> task<void> {
        http_client client(attachment.loop(), {.base_url_ = url, .versions_ = ruvia::http_client_versions{.http2_ = false}});
        stop_source stop;
        worker_signal changed(worker);
        bool entered = false;
        bool exited = false;
        bool send_finished = false;
        std::exception_ptr failure;
        auto request = client.client().with_options({.timeout_ = mode == cancellation_case::timeout ? timeout_duration{200} : timeout_duration{2000},
                                                        .stop_token_ = stop.token()})
                           .get("/redirect");
        request.on_redirect([&](const http_client_redirect_hop& hop, stop_token token) -> task<bool> {
            entered = true;
            changed.notify();
            EXPECT_EQ(hop.target_origin().host(), "127.0.0.1");
            EXPECT_EQ(co_await sleep_for(worker, std::chrono::seconds(10), token), timer_sleep_result::stop_requested);
            EXPECT_EQ(hop.target_origin().host(), "127.0.0.1");
            EXPECT_EQ(hop.target_origin().port(), 8123);
            exited = true;
            co_return true;
        });
        const auto send = [&]() -> task<void> {
            try {
                (void)co_await request.send();
                ADD_FAILURE() << "cancelled redirect policy sent a follow-up request";
            } catch (const http_client_error& error) {
                EXPECT_EQ(error.code(), mode == cancellation_case::timeout
                                            ? std::make_error_code(std::errc::timed_out)
                                            : std::make_error_code(std::errc::operation_canceled));
                EXPECT_NE(error.connection_info(), nullptr);
                EXPECT_TRUE(exited);
            } catch (...) {
                failure = std::current_exception();
            }
            send_finished = true;
            changed.notify();
        };
        task_scope requests(worker);
        requests.spawn(send());
        while (!entered && !send_finished) {
            co_await changed.wait();
        }
        if (entered && mode == cancellation_case::caller_stop) {
            stop.request_stop();
        } else if (entered && mode == cancellation_case::owner_close) {
            client.close();
        }
        co_await requests.join();
        co_await client.shutdown();
        EXPECT_TRUE(entered);
        EXPECT_TRUE(exited);
        if (failure || !entered) {
            source.abort();
        }
        co_await source.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    run_worker_tasks(attachment, run());
}

}  // namespace ruvia::test::dynamic_origin_tests

TEST(Ruvia, http_client_tls_preparation_cancellation_and_shutdown_join_without_publication) {
    using namespace ruvia::test::dynamic_origin_tests;
    for (const auto mode : {cancellation_case::timeout, cancellation_case::caller_stop, cancellation_case::owner_close}) {
        exercise_origin_preparation_cancellation(mode);
    }
}

TEST(Ruvia, http_client_async_redirect_policy_joins_on_deadline_stop_and_owner_shutdown) {
    using namespace ruvia::test::dynamic_origin_tests;
    for (const auto mode : {cancellation_case::timeout, cancellation_case::caller_stop, cancellation_case::owner_close}) {
        exercise_redirect_policy_cancellation(mode);
    }
}

namespace ruvia::test::raw_tcp {

// A real independent TCP/TLS peer. A stalled peer consumes no bytes, making
// write deadlines exercise kernel backpressure rather than a fake completion.
class byte_peer final {
public:
    explicit byte_peer(std::size_t bytes = 0, asio::ssl::context* tls = nullptr, bool stall_handshake = false)
        : acceptor_(io_, {asio::ip::tcp::v4(), 0}),
          socket_(io_),
          timer_(io_),
          deadline_(io_),
          work_(asio::make_work_guard(io_)),
          bytes_(bytes),
          tls_(tls),
          stall_handshake_(stall_handshake),
          done_(asio::co_spawn(io_, serve(), asio::use_future)),
          thread_([this] { io_.run(); }) {}

    ~byte_peer() {
        asio::post(io_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            socket_.close(ignored);
            timer_.cancel();
            deadline_.cancel();
            work_.reset();
        });
        thread_.join();
        finish();
    }

    [[nodiscard]] tcp_client_config config() const {
        return {.host_ = "127.0.0.1", .port_ = acceptor_.local_endpoint().port(), .tls_ = {.mode_ = client_tls_mode::disabled}, .connect_timeout_ = std::chrono::seconds(2), .read_timeout_ = std::chrono::seconds(2), .write_timeout_ = std::chrono::seconds(2)};
    }

    [[nodiscard]] std::size_t received_bytes() const noexcept {
        return received_bytes_.load();
    }

    [[nodiscard]] int tls_version() const noexcept {
        return tls_version_.load();
    }

    void finish() {
        if (done_.valid()) {
            try {
                done_.get();
            } catch (const std::system_error&) {
                // Peer failure is expected for cancellation and rejected TLS.
            }
        }
    }

private:
    template <typename stream_type>
    asio::awaitable<void> exchange(stream_type& stream) {
        if (bytes_ == 0) {
            timer_.expires_at((asio::steady_timer::time_point::max)());
            co_await timer_.async_wait(asio::use_awaitable);
            co_return;
        }
        std::string bytes(bytes_, '\0');
        std::error_code error;
        received_bytes_ = co_await asio::async_read(stream, asio::buffer(bytes),
            asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            throw std::system_error(error);
        }
        co_await asio::async_write(stream, asio::buffer(bytes), asio::use_awaitable);
    }

    asio::awaitable<void> serve() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                std::error_code ignored;
                acceptor_.close(ignored);
                socket_.close(ignored);
                timer_.cancel();
            }
        });
        co_await acceptor_.async_accept(socket_, asio::use_awaitable);
        socket_.set_option(asio::socket_base::receive_buffer_size(4096));
        if (stall_handshake_) {
            co_await exchange(socket_);
        } else if (tls_) {
            asio::ssl::stream<asio::ip::tcp::socket&> stream(socket_, *tls_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            tls_version_ = SSL_version(stream.native_handle());
            co_await exchange(stream);
            co_await stream.async_shutdown(asio::use_awaitable);
        } else {
            co_await exchange(socket_);
            socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer timer_;
    asio::steady_timer deadline_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::size_t bytes_;
    asio::ssl::context* tls_;
    bool stall_handshake_;
    std::atomic<std::size_t> received_bytes_{};
    std::atomic<int> tls_version_{};
    std::future<void> done_;
    std::thread thread_;
};

[[nodiscard]] task<void> read_all(tcp_client& client, std::string& received) {
    while (const auto bytes = co_await client.read()) {
        received.append(as_chars(*bytes));
    }
}

[[nodiscard]] task<void> byte_exchange(event_loop loop, tcp_client_config config) {
    tcp_client client(loop, config);
    co_await client.connect();
    // Read and write are intentionally concurrent; the peer waits for both
    // owned payloads before replying, so a serialized transport would deadlock.
    task_scope tasks(client.worker());
    std::string received;
    tasks.spawn(read_all(client, received));
    auto copied = [&] {
        const std::string temporary(5000, 'a');
        return client.write(std::string_view(temporary));
    }();
    auto moved = [&] {
        std::string owned(5000, 'b');
        return client.write(std::move(owned));
    }();
    // Both temporary sources have died before
    // either cold write starts; their payloads must now belong to the client.
    co_await std::move(copied);
    co_await std::move(moved);
    co_await tasks.join();
    EXPECT_EQ(received, std::string(5000, 'a') + std::string(5000, 'b'));
    EXPECT_FALSE((co_await client.read()).has_value());
    co_await client.shutdown();
}

[[nodiscard]] task<void> alpn_exchange(event_loop loop, tcp_client_config config, std::string expected) {
    tcp_client client(loop, config);
    config.alpn_.clear();
    config.alpn_.emplace_back("not-offered");
    co_await client.connect();
    EXPECT_EQ(client.alpn(), expected);
    co_await client.write("ALPN");
    std::string received;
    co_await read_all(client, received);
    EXPECT_EQ(received, "ALPN");
    co_await client.shutdown();
}

[[nodiscard]] task<void> expect_io_failure(tcp_client& client, bool writing, operation_options options,
    std::errc expected, std::promise<void>* started = nullptr) {
    if (started) {
        // Nested tasks enter the transport without yielding. Publish readiness
        // on the next worker turn, after cancellation is armed and socket I/O
        // has started, rather than announcing a still-cold operation.
        if (!client.worker().post([started] { started->set_value(); }).accepted()) {
            throw std::runtime_error("TCP operation readiness was rejected");
        }
    }
    bool failed = false;
    try {
        if (writing) {
            co_await client.write(std::string(16 * 1024 * 1024, 'x'), options);
        } else {
            (void)co_await client.read(options);
        }
    } catch (const std::system_error& error) {
        EXPECT_EQ(error.code(), std::make_error_code(expected));
        failed = true;
    }
    EXPECT_TRUE(failed);
}

[[nodiscard]] task<void> deadline_case(event_loop loop, tcp_client_config config, bool writing) {
    if (writing) {
        config.write_timeout_ = std::chrono::milliseconds(30);
    } else {
        config.read_timeout_ = std::chrono::milliseconds(30);
    }
    tcp_client client(loop, config);
    co_await client.connect();
    co_await expect_io_failure(client, writing, {.timeout_ = std::chrono::seconds(5)}, std::errc::timed_out);
    co_await client.shutdown();
}

[[nodiscard]] task<void> cancellation_case(tcp_client& client, stop_token token, bool writing, bool connecting) {
    if (connecting) {
        bool cancelled = false;
        try {
            co_await client.connect({.stop_token_ = token});
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::make_error_code(std::errc::operation_canceled));
            cancelled = true;
        }
        EXPECT_TRUE(cancelled);
    } else {
        co_await client.connect();
        co_await expect_io_failure(client, writing, {.stop_token_ = token}, std::errc::operation_canceled);
    }
    co_await client.shutdown();
}

[[nodiscard]] task<void> overlap_case(event_loop loop, tcp_client_config config, bool writing) {
    tcp_client client(loop, config);
    co_await client.connect();
    task_scope scope(client.worker());
    scope.spawn(expect_io_failure(client, writing, {}, std::errc::operation_canceled));
    // Yield after spawn so the first operation owns its direction.
    (void)co_await sleep_for(client.worker(), std::chrono::milliseconds(10));
    bool rejected = false;
    try {
        if (writing) {
            co_await client.write("overlap");
        } else {
            (void)co_await client.read();
        }
    } catch (const std::logic_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    client.close();
    co_await scope.join();
    co_await client.shutdown();
}

[[nodiscard]] task<void> retired_cold_case(event_loop loop, tcp_client_config config) {
    auto client = std::make_unique<tcp_client>(loop, config);
    co_await client->connect();
    {
        auto cold_write = client->write(std::string(5000, 'x'));
        auto cold_read = client->read();
        auto shutdown = client->shutdown();
        client.reset();
        co_await std::move(shutdown);
        // Expired tasks are destroyed cold, with no borrowed backend remaining.
    }
}

}  // namespace ruvia::test::raw_tcp

using namespace ruvia::test::raw_tcp;

TEST(Ruvia, tcp_client_exchanges_owned_bytes_with_concurrent_read_and_eof) {
    byte_peer peer(10000);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(byte_exchange(pool.loop(0), peer.config())).get();
    pool.join();
}

TEST(Ruvia, tcp_client_tls_and_mutual_tls_authenticate_san_and_exchange_bytes) {
    for (const bool mutual : {false, true}) {
        ruvia::test::tls_identity server("server.example");
        ruvia::test::tls_identity identity("client.example");
        if (mutual) {
            server.context_.load_verify_file(identity.ca_file_.string());
            server.context_.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
        }
        byte_peer peer(10000, &server.context_);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        if (mutual) {
            config.tls_.identity_.emplace();
            config.tls_.identity_->certificate_chain_file_ = identity.ca_file_;
            config.tls_.identity_->private_key_file_ = identity.private_key_file();
        }
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_tls11_exchanges_bytes_and_still_authenticates_san) {
    for (const bool matching_san : {true, false}) {
        ruvia::test::tls_identity server("server.example",
            matching_san ? "DNS:server.example" : "DNS:other.example");
        auto* context = server.context_.native_handle();
        ASSERT_EQ(SSL_CTX_set_min_proto_version(context, TLS1_1_VERSION), 1);
        ASSERT_EQ(SSL_CTX_set_max_proto_version(context, TLS1_1_VERSION), 1);
        ASSERT_EQ(SSL_CTX_set_cipher_list(context, "ECDHE-RSA-AES128-SHA"), 1);
        // Independent legacy peer only: production clients retain their shared
        // security callback and certificate/key-strength checks.
        SSL_CTX_set_security_level(context, 0);
        byte_peer peer(10000, &server.context_);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        if (matching_san) {
            pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
            EXPECT_EQ(peer.tls_version(), TLS1_1_VERSION);
            EXPECT_EQ(peer.received_bytes(), 10000U);
        } else {
            ruvia::tcp_client client(pool.loop(0), config);
            EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
            pool.loop(0).start(client.shutdown()).get();
        }
        pool.join();
    }
}

TEST(Ruvia, tcp_client_minimum_tls_version_rejects_downgrade_before_application_bytes) {
    for (const auto minimum : {ruvia::tls_version::tls_1_2, ruvia::tls_version::tls_1_3}) {
        for (const int version : {TLS1_1_VERSION, TLS1_2_VERSION, TLS1_3_VERSION}) {
            SCOPED_TRACE(static_cast<int>(minimum));
            SCOPED_TRACE(version);
            ruvia::test::tls_identity server("server.example");
            auto* context = server.context_.native_handle();
            ASSERT_EQ(SSL_CTX_set_min_proto_version(context, version), 1);
            ASSERT_EQ(SSL_CTX_set_max_proto_version(context, version), 1);
            if (version == TLS1_1_VERSION) {
                ASSERT_EQ(SSL_CTX_set_cipher_list(context, "ECDHE-RSA-AES128-SHA"), 1);
                SSL_CTX_set_security_level(context, 0);
            }
            byte_peer peer(10000, &server.context_);
            auto config = peer.config();
            config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
            config.tls_.minimum_version_ = minimum;
            config.tls_.ca_file_ = server.ca_file_;
            config.tls_.server_name_.emplace("server.example");
            ruvia::event_loop_pool pool({.loop_count_ = 1});
            pool.start();
            if (version >= static_cast<int>(minimum)) {
                pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
                EXPECT_EQ(peer.tls_version(), version);
                EXPECT_EQ(peer.received_bytes(), 10000U);
            } else {
                ruvia::tcp_client client(pool.loop(0), config);
                EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
                pool.loop(0).start(client.shutdown()).get();
                EXPECT_EQ(peer.received_bytes(), 0U);
            }
            pool.join();
        }
    }
}

TEST(Ruvia, tcp_client_rejects_invalid_tls_minimum_configuration) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::tcp_client_config config{.host_ = "127.0.0.1", .port_ = 443};
    config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
    config.tls_.minimum_version_ = static_cast<ruvia::tls_version>(0x0301);
    EXPECT_THROW(ruvia::tcp_client(pool.loop(0), config), std::invalid_argument);
}

TEST(Ruvia, tcp_client_rejects_tls10_and_h2_below_tls12_before_application_bytes) {
    for (const int version : {TLS1_VERSION, TLS1_1_VERSION}) {
        for (const bool h2_only : {false, true}) {
            ruvia::test::tls_identity server("server.example");
            auto* context = server.context_.native_handle();
            ASSERT_EQ(SSL_CTX_set_min_proto_version(context, version), 1);
            ASSERT_EQ(SSL_CTX_set_max_proto_version(context, version), 1);
            ASSERT_EQ(SSL_CTX_set_cipher_list(context, "ECDHE-RSA-AES128-SHA"), 1);
            SSL_CTX_set_security_level(context, 0);
            SSL_CTX_set_alpn_select_cb(context, [](SSL*, const unsigned char** selected, unsigned char* size, const unsigned char*, unsigned int, void*) -> int {
                    static constexpr unsigned char h2[] = {'h', '2'};
                    *selected = h2;
                    *size = sizeof(h2);
                    return SSL_TLSEXT_ERR_OK; }, nullptr);
            byte_peer peer(4, &server.context_);
            auto config = peer.config();
            config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
            config.tls_.ca_file_ = server.ca_file_;
            config.tls_.server_name_.emplace("server.example");
            config.alpn_.emplace_back("h2");
            if (!h2_only) {
                config.alpn_.emplace_back("http/1.1");
            }
            ruvia::event_loop_pool pool({.loop_count_ = 1});
            ruvia::tcp_client client(pool.loop(0), config);
            pool.start();
            EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
            pool.loop(0).start(client.shutdown()).get();
            pool.join();
            peer.finish();
            if (version == TLS1_1_VERSION && !h2_only) {
                EXPECT_EQ(peer.tls_version(), TLS1_1_VERSION);
            }
            EXPECT_EQ(peer.received_bytes(), 0U);
        }
    }
}

TEST(Ruvia, tcp_client_tls_rejects_wrong_san_even_when_common_name_matches) {
    ruvia::test::tls_identity server("wanted.example", "DNS:other.example");
    byte_peer peer(0, &server.context_);
    auto config = peer.config();
    config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
    config.tls_.ca_file_ = server.ca_file_;
    config.tls_.server_name_.emplace("wanted.example");
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::tcp_client client(pool.loop(0), config);
    pool.start();
    EXPECT_THROW(pool.loop(0).start(client.connect()).get(), std::system_error);
    pool.loop(0).start(client.shutdown()).get();
    pool.join();
}

TEST(Ruvia, tcp_client_owns_opaque_alpn_offers_and_reports_server_selection) {
    for (const auto& protocol : {std::string("embed.v1"), std::string(255, 'x'), std::string("bi\0n", 4)}) {
        std::vector<unsigned char> server_offer{static_cast<unsigned char>(protocol.size())};
        server_offer.insert(server_offer.end(), protocol.begin(), protocol.end());
        ruvia::test::tls_identity server("server.example");
        SSL_CTX_set_alpn_select_cb(server.context_.native_handle(), [](SSL*, const unsigned char** output, unsigned char* output_size, const unsigned char* offered, unsigned int offered_size, void* argument) -> int {
                const auto& supported = *static_cast<const std::vector<unsigned char>*>(argument);
                unsigned char* selected = nullptr;
                if (SSL_select_next_proto(&selected, output_size, supported.data(),
                        static_cast<unsigned int>(supported.size()), offered, offered_size) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_ALERT_FATAL;
                }
                *output = selected;
                return SSL_TLSEXT_ERR_OK; }, &server_offer);
        byte_peer peer(4, &server.context_);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        config.alpn_.emplace_back("h2");
        config.alpn_.emplace_back(protocol);
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(alpn_exchange(pool.loop(0), config, protocol)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_rejects_invalid_alpn_names_size_and_plaintext_offers) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    ruvia::tcp_client_config config{.host_ = "127.0.0.1", .port_ = 443};
    config.alpn_.emplace_back();
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    config.alpn_.front().assign(256, 'x');
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    config.alpn_.clear();
    for (int index = 0; index < 256; ++index) {
        config.alpn_.emplace_back(255, 'x');
    }
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    config.alpn_.clear();
    config.alpn_.emplace_back("embed.v1");
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
}

TEST(Ruvia, tcp_client_bounds_read_and_backpressured_write_independently) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(deadline_case(pool.loop(0), peer.config(), writing)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_rejects_overlapping_reads_and_writes_without_aborting_first_operation) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        pool.loop(0).start(overlap_case(pool.loop(0), peer.config(), writing)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_honors_precancellation_for_connect_read_and_write) {
    for (const int direction : {0, 1, 2}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), peer.config());
        ruvia::stop_source stop;
        stop.request_stop();
        pool.start();
        pool.loop(0).start(cancellation_case(client, stop.token(), direction == 2, direction == 0)).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_cancels_inflight_reads_and_writes_from_another_thread) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), peer.config());
        ruvia::stop_source stop;
        pool.start();
        pool.loop(0).start(client.connect()).get();
        std::promise<void> started;
        std::promise<void> other_started;
        auto operation = pool.loop(0).start(expect_io_failure(client, writing, {.stop_token_ = stop.token()}, std::errc::operation_canceled, &started));
        auto other = pool.loop(0).start(expect_io_failure(client, !writing, {}, std::errc::operation_canceled, &other_started));
        started.get_future().get();
        other_started.get_future().get();
        // Only one direction owns this token. Its cancellation must also end
        // the opposite in-flight operation when the connection is retired.
        stop.request_stop();
        std::move(operation).get();
        std::move(other).get();
        pool.loop(0).start(client.shutdown()).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_shutdown_retires_cold_buffers_after_owner_destruction) {
    byte_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    pool.loop(0).start(retired_cold_case(pool.loop(0), peer.config())).get();
    pool.join();
}

TEST(Ruvia, tcp_client_loop_stop_joins_pending_reads_and_writes) {
    for (const bool writing : {false, true}) {
        byte_peer peer;
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), peer.config());
        pool.start();
        pool.loop(0).start(client.connect()).get();
        std::promise<void> started;
        auto operation = pool.loop(0).start(expect_io_failure(client, writing, {}, std::errc::operation_canceled, &started));
        started.get_future().get();
        pool.stop();
        std::move(operation).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_tls_handshake_has_deadline_and_inflight_cancellation) {
    for (const bool cancelling : {false, true}) {
        ruvia::test::tls_identity server("server.example");
        byte_peer peer(0, nullptr, true);
        auto config = peer.config();
        config.tls_.mode_ = ruvia::client_tls_mode::verify_identity;
        config.tls_.ca_file_ = server.ca_file_;
        config.tls_.server_name_.emplace("server.example");
        config.connect_timeout_ = std::chrono::milliseconds(cancelling ? 2000 : 30);
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        ruvia::tcp_client client(pool.loop(0), config);
        ruvia::stop_source stop;
        pool.start();
        auto operation = pool.loop(0).start(client.connect({.stop_token_ = stop.token()}));
        if (cancelling) {
            const auto barrier = [&]() -> ruvia::task<void> {
                (void)co_await ruvia::sleep_for(client.worker(), std::chrono::milliseconds(10));
            };
            pool.loop(0).start(barrier()).get();
            stop.request_stop();
        }
        bool failed = false;
        try {
            std::move(operation).get();
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::make_error_code(cancelling ? std::errc::operation_canceled : std::errc::timed_out));
            failed = true;
        }
        EXPECT_TRUE(failed);
        pool.loop(0).start(client.shutdown()).get();
        pool.join();
    }
}

TEST(Ruvia, tcp_client_resolves_dns_through_its_event_loop_blocking_pool) {
    byte_peer peer(10000);
    auto config = peer.config();
    config.host_ = "localhost";
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    ruvia::event_loop_pool pool({.loop_count_ = 1, .blocking_pool_ = &lookups});
    pool.start();
    pool.loop(0).start(byte_exchange(pool.loop(0), config)).get();
    pool.join();
}

TEST(Ruvia, tcp_client_checks_ports_before_narrowing_and_rejects_zero) {
    EXPECT_THROW(static_cast<void>(ruvia::tcp_client_config{.host_ = "127.0.0.1", .port_ = 70000}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(ruvia::tcp_client_config{.host_ = "127.0.0.1", .port_ = -1}), std::invalid_argument);
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const ruvia::tcp_client_config config{.host_ = "127.0.0.1", .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}};
    EXPECT_THROW((ruvia::tcp_client(pool.loop(0), config)), std::invalid_argument);
    pool.join();
}

TEST(Ruvia, tcp_client_cold_connect_remains_safe_after_public_owner_destruction) {
    byte_peer peer;
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    auto client = std::make_unique<ruvia::tcp_client>(pool.loop(0), peer.config());
    auto cold = client->connect();
    client.reset();
    pool.start();
    EXPECT_THROW(pool.loop(0).start(std::move(cold)).get(), std::runtime_error);
    pool.join();
}

namespace origin_routing_tests {

using namespace std::chrono_literals;

// What one accepted connection saw: TLS server name and ALPN offer, and the
// Host / :authority of each request in order.
struct observed_connection final {
    std::string server_name_;
    std::string offered_alpn_;
    std::string selected_alpn_;
    std::deque<std::string> authorities_;
    // HTTP/2 RST_STREAM error codes the client sent.
    std::deque<ruvia::http2_error_code> resets_;
    // HTTP/1 request content bytes read before the response.
    std::size_t content_bytes_{0};
    bool closed_{false};
};

// A real TCP or TLS origin on the client's own io_context. HTTP/1 requests
// receive their target as the body; "/hold" waits for release(), "/drain"
// first reads the complete chunked request content, "/early" answers before
// reading any content, and "/redirect" answers 302 to redirect_location_. A
// TLS connection that negotiated h2 is served by the sans-I/O HTTP/2 core, and
// an HTTP/1 Upgrade request is switched with 101 and echoes its bytes.
class routing_peer final {
public:
    routing_peer(asio::io_context& io, asio::ssl::context* tls, std::string_view address = "127.0.0.1")
        : acceptor_(io, {asio::ip::make_address(address), 0}),
          gate_(io),
          tls_(tls) {
        gate_.expires_at(asio::steady_timer::time_point::max());
        if (tls_ != nullptr) {
            SSL_CTX_set_alpn_select_cb(tls_->native_handle(), select_alpn, nullptr);
        }
        ++active_;
        asio::co_spawn(io, accept(), asio::detached);
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void release() {
        gate_.expires_at(asio::steady_timer::time_point::min());
    }
    void stop() {
        std::error_code ignored;
        acceptor_.close(ignored);
        release();
    }
    [[nodiscard]] bool finished() const noexcept {
        return active_ == 0;
    }

    std::deque<observed_connection> connections_;
    std::size_t held_{0};
    std::string redirect_location_;
    // Sent as Alt-Svc with every HTTP/1 200 response when not empty.
    std::string alt_svc_;
    std::vector<std::string> extra_alt_svc_headers_;

private:
    // asio owns the SSL app data; the observation uses its own slot.
    [[nodiscard]] static int observation_index() {
        static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        return index;
    }
    static int select_alpn(SSL* ssl, const unsigned char** out, unsigned char* out_size, const unsigned char* in,
        unsigned int in_size, void*) {
        auto& observed = *static_cast<observed_connection*>(SSL_get_ex_data(ssl, observation_index()));
        observed.offered_alpn_.assign(reinterpret_cast<const char*>(in), in_size);
        static constexpr unsigned char preference[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        unsigned char* selected = nullptr;
        if (SSL_select_next_proto(&selected, out_size, preference, sizeof(preference), in, in_size) != OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        *out = selected;
        observed.selected_alpn_.assign(reinterpret_cast<const char*>(selected), *out_size);
        return SSL_TLSEXT_ERR_OK;
    }

    asio::awaitable<void> accept() {
        for (;;) {
            std::error_code error;
            auto socket = co_await acceptor_.async_accept(asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            auto& observed = connections_.emplace_back();
            ++active_;
            asio::co_spawn(acceptor_.get_executor(), serve(std::move(socket), observed), asio::detached);
        }
        --active_;
    }

    asio::awaitable<void> serve(asio::ip::tcp::socket socket, observed_connection& observed) {
        try {
            if (tls_ == nullptr) {
                co_await serve_http1(socket, observed);
            } else {
                asio::ssl::stream<asio::ip::tcp::socket&> stream(socket, *tls_);
                SSL_set_ex_data(stream.native_handle(), observation_index(), &observed);
                co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
                if (const char* name = SSL_get_servername(stream.native_handle(), TLSEXT_NAMETYPE_host_name)) {
                    observed.server_name_ = name;
                }
                if (observed.selected_alpn_ == "h2") {
                    co_await serve_http2(stream, observed);
                } else {
                    co_await serve_http1(stream, observed);
                }
            }
        } catch (const std::exception&) {
            // A client close or abort ends the connection; callers inspect closed_.
        }
        observed.closed_ = true;
        --active_;
    }

    template <typename stream_type>
    asio::awaitable<void> serve_http1(stream_type& stream, observed_connection& observed) {
        std::string input;
        for (;;) {
            const auto head_bytes = co_await asio::async_read_until(stream, asio::dynamic_buffer(input), "\r\n\r\n", asio::use_awaitable);
            const std::string head = input.substr(0, head_bytes);
            input.erase(0, head_bytes);
            const auto host = head.find("\r\nHost: ");
            const auto host_end = head.find("\r\n", host + 2);
            observed.authorities_.push_back(host == std::string::npos ? std::string() : head.substr(host + 8, host_end - host - 8));
            const auto target = head.substr(head.find(' ') + 1, head.find(' ', head.find(' ') + 1) - head.find(' ') - 1);
            if (head.find("\r\nUpgrade: ") != std::string::npos) {
                constexpr std::string_view switched = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: chat\r\n\r\n";
                co_await asio::async_write(stream, asio::buffer(switched), asio::use_awaitable);
                std::array<char, 256> bytes{};
                for (;;) {
                    const auto count = co_await stream.async_read_some(asio::buffer(bytes), asio::use_awaitable);
                    co_await asio::async_write(stream, asio::buffer(bytes.data(), count), asio::use_awaitable);
                }
            }
            if (target == "/hold") {
                ++held_;
                std::error_code ignored;
                co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            }
            if (target == "/drain") {
                // Chunked content up to its last chunk, read before answering.
                const auto end = co_await asio::async_read_until(stream, asio::dynamic_buffer(input), "\r\n0\r\n\r\n", asio::use_awaitable);
                observed.content_bytes_ += end;
                input.erase(0, end);
            }
            if (target == "/redirect" || target == "/drain-redirect") {
                if (target == "/drain-redirect") {
                    const auto end = co_await asio::async_read_until(
                        stream, asio::dynamic_buffer(input), "\r\n0\r\n\r\n", asio::use_awaitable);
                    observed.content_bytes_ += end;
                    input.erase(0, end);
                }
                const std::string response = "HTTP/1.1 302 Found\r\nLocation: " +
                                             redirect_location_ + "\r\nContent-Length: 0\r\n\r\n";
                co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
                continue;
            }
            std::string alt_svc_lines = alt_svc_.empty() ? std::string() : "Alt-Svc: " + alt_svc_ + "\r\n";
            for (const auto& extra : extra_alt_svc_headers_) {
                alt_svc_lines += "Alt-Svc: " + extra + "\r\n";
            }
            const std::string response = "HTTP/1.1 200 OK\r\n" + alt_svc_lines +
                                         "Content-Length: " + std::to_string(target.size()) + "\r\n\r\n" + target;
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
    }

    template <typename stream_type>
    asio::awaitable<void> serve_http2(stream_type& stream, observed_connection& observed) {
        auto connection = ruvia::http2_connection::server();
        std::deque<ruvia::http2_request_head_event> unreleased;
        std::string output;
        std::array<char, 16384> input{};
        for (;;) {
            ruvia::test::drain_output(connection, output);
            if (!output.empty()) {
                co_await asio::async_write(stream, asio::buffer(output), asio::use_awaitable);
                output.clear();
            }
            const auto count = co_await stream.async_read_some(asio::buffer(input), asio::use_awaitable);
            const std::string_view bytes(input.data(), count);
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 input");
                }
                while (auto event = connection.next_event()) {
                    if (const auto* closed = event->stream_closed();
                        closed != nullptr && closed->source() == ruvia::http2_stream_close_source::peer) {
                        observed.resets_.push_back(closed->error());
                    }
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    const auto built = request->build_request();
                    observed.authorities_.emplace_back(built.authority());
                    ruvia::http_response response;
                    const auto submitted = connection.submit_response_head(request->stream_id(), response,
                        ruvia::plan_buffered_http_response_write(built.known_method(), response));
                    if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted)) {
                        throw std::runtime_error("HTTP/2 response rejected");
                    }
                    if (built.target() == "/early") {
                        // Keep receiving the request content after the
                        // complete response, so only the client ends it.
                        unreleased.push_back(std::move(*request));
                    } else if (connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
                        throw std::runtime_error("HTTP/2 response rejected");
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer gate_;
    asio::ssl::context* tls_;
    std::size_t active_{0};
};

// Runs `exercise` against a client of `config`, then retires the client and
// every peer connection before the loop stops.
template <typename exercise_type>
void run_clients(ruvia::event_loop_attachment& attachment, std::initializer_list<routing_peer*> peers,
    std::span<const ruvia::http_client_config> configs, const ruvia::http_client_worker_config& worker_config,
    exercise_type&& exercise) {
    std::exception_ptr failure;
    auto loop = attachment.loop();
    const auto& worker = loop.worker();
    const auto run = [&]() -> ruvia::task<void> {
        {
            ruvia::http_client client(loop, configs, worker_config);
            try {
                co_await exercise(client, worker);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
        }
        for (auto* peer : peers) {
            peer->stop();
        }
        for (auto* peer : peers) {
            while (!peer->finished()) {
                (void)co_await ruvia::sleep_for(worker, 1ms);
            }
        }
        attachment.stop();
    };
    auto root = loop.start(run());
    attachment.run();
    std::move(root).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

template <typename exercise_type>
void run_client(asio::io_context& io, ruvia::event_loop_attachment& attachment, std::initializer_list<routing_peer*> peers,
    const ruvia::http_client_config& config, exercise_type&& exercise) {
    (void)io;
    run_clients(attachment, peers, std::span<const ruvia::http_client_config>(&config, 1), {},
        std::forward<exercise_type>(exercise));
}

[[nodiscard]] ruvia::http_client_request_options route(std::optional<std::string_view> authority,
    std::optional<std::string_view> server_name = std::nullopt) {
    ruvia::http_client_request_options options;
    if (authority) {
        options.authority_.emplace(*authority);
    }
    if (server_name) {
        options.server_name_.emplace(*server_name);
    }
    return options;
}

ruvia::task<std::uint64_t> get(ruvia::http_client& client, std::string_view target, ruvia::http_client_request_options options,
    bool expect_reused) {
    auto response = co_await client.client().get(target).configure(std::move(options)).send();
    EXPECT_TRUE(response.ok());
    EXPECT_EQ(response.connection_info().reused_, expect_reused);
    const auto body = co_await response.text();
    EXPECT_EQ(body.text(), target);
    co_return response.connection_info().id_;
}

}  // namespace origin_routing_tests

TEST(Ruvia, http_client_request_routes_send_their_host_and_sni_and_share_connections_by_server_name) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("a.example", "DNS:a.example,DNS:b.example,DNS:c.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://alias.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.versions_ = ruvia::http_client_versions{.http2_ = false};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        EXPECT_EQ(client.client().stats().open_connections_, 0U);
        const auto a = co_await get(client, "/a1", route("a.example"), false);
        // HTTP/1 keep-alive reuse follows (dial, server name); Host is not part of the key.
        const auto a_again = co_await get(client, "/a2", route("a.example"), true);
        EXPECT_EQ(a_again, a);
        const auto b = co_await get(client, "/b1", route("b.example:8443"), false);
        EXPECT_NE(b, a);
        const auto c = co_await get(client, "/c1", route("c.example", "a.example"), true);
        EXPECT_EQ(c, a);
        EXPECT_EQ(client.client().stats().open_connections_, 2U);
        EXPECT_THROW((void)client.client().get("/").configure(route("user@a.example")), std::invalid_argument);
        EXPECT_THROW((void)client.client().get("/").configure(route("a.example:0")), std::invalid_argument);
        EXPECT_THROW((void)client.client().get("/").configure(route(std::nullopt, "bad host")), std::invalid_argument);
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    const auto& first = peer.connections_[0];
    EXPECT_EQ(first.server_name_, "a.example");
    EXPECT_EQ(first.authorities_, (std::deque<std::string>{"a.example", "a.example", "c.example"}));
    const auto& second = peer.connections_[1];
    EXPECT_EQ(second.server_name_, "b.example");
    EXPECT_EQ(second.authorities_, (std::deque<std::string>{"b.example:8443"}));
}

TEST(Ruvia, http_client_request_dial_overrides_reach_another_listener) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer alias_peer(io, nullptr);
    routing_peer other_peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(alias_peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&alias_peer, &other_peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        (void)co_await get(client, "/alias", {}, false);
        ruvia::http_client_request_options options = route("visitor.example:8080");
        options.dial_host_.emplace("127.0.0.1");
        options.dial_port_ = other_peer.port();
        (void)co_await get(client, "/other", std::move(options), false);
        // The alias connection is not the overridden route's.
        options = route("visitor.example:8080");
        options.dial_host_.emplace("127.0.0.1");
        options.dial_port_ = alias_peer.port();
        (void)co_await get(client, "/back", std::move(options), true);
    });
    ASSERT_EQ(other_peer.connections_.size(), 1U);
    EXPECT_EQ(other_peer.connections_[0].authorities_, (std::deque<std::string>{"visitor.example:8080"}));
    ASSERT_EQ(alias_peer.connections_.size(), 1U);
    EXPECT_EQ(alias_peer.connections_[0].authorities_.size(), 2U);
    EXPECT_EQ(alias_peer.connections_[0].authorities_.back(), "visitor.example:8080");
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http_client_upgrade_on_a_negotiated_h2_origin_uses_an_http11_connection) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        auto ordinary = co_await client.client().get("/ordinary").send();
        EXPECT_TRUE(ordinary.protocol_version() == ruvia::http_protocol_version::http2);
        (void)co_await ordinary.text();
        auto upgraded = co_await client.client().get("/chat").header("Connection", "Upgrade").header("Upgrade", "chat").upgrade();
        auto* tunnel = upgraded.tunnel();
        if (tunnel == nullptr) {
            ADD_FAILURE() << "Upgrade was not switched";
            co_return;
        }
        EXPECT_EQ(tunnel->status().value(), 101);
        EXPECT_TRUE(tunnel->protocol_version() == ruvia::http_protocol_version::http11);
        tunnel->abort();
        // Ordinary requests keep multiplexing on the h2 connection.
        auto again = co_await client.client().get("/again").send();
        EXPECT_TRUE(again.protocol_version() == ruvia::http_protocol_version::http2);
        EXPECT_TRUE(again.connection_info().reused_);
        (void)co_await again.text();
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_EQ(peer.connections_[0].selected_alpn_, "h2");
    EXPECT_EQ(peer.connections_[0].authorities_, (std::deque<std::string>{"origin.example", "origin.example"}));
    EXPECT_EQ(peer.connections_[1].offered_alpn_, std::string_view("\x08http/1.1"));
    EXPECT_EQ(peer.connections_[1].selected_alpn_, "http/1.1");
}
#endif

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http_client_request_versions_override_the_alias_offer) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        auto multiplexed = co_await client.client().get("/h2").send();
        EXPECT_TRUE(multiplexed.protocol_version() == ruvia::http_protocol_version::http2);
        (void)co_await multiplexed.text();
        ruvia::http_client_request_options http1;
        http1.versions_ = ruvia::http_client_versions{.http2_ = false};
        auto serial = co_await client.client().get("/h1").configure(std::move(http1)).send();
        EXPECT_TRUE(serial.protocol_version() == ruvia::http_protocol_version::http11);
        const auto body = co_await serial.text();
        EXPECT_EQ(body.text(), "/h1");
        ruvia::http_client_request_options none;
        none.versions_ = ruvia::http_client_versions{.http1_ = false, .http2_ = false};
        EXPECT_THROW((void)client.client().get("/").configure(std::move(none)), std::invalid_argument);
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_EQ(peer.connections_[0].selected_alpn_, "h2");
    EXPECT_EQ(peer.connections_[1].offered_alpn_, std::string_view("\x08http/1.1"));
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
TEST(Ruvia, http_client_unreachable_alt_svc_falls_back_to_tcp_and_rests) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    // A UDP endpoint that never answers a QUIC Initial.
    asio::ip::udp::socket sink(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::size_t datagrams = 0;
    bool sink_done = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        std::array<char, 2048> bytes{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            std::error_code error;
            (void)co_await sink.async_receive_from(asio::buffer(bytes), sender, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            ++datagrams;
        }
        sink_done = true; }, asio::detached);
    peer.alt_svc_ = "h3=\":" + std::to_string(sink.local_endpoint().port()) + "\"";
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = true};
    config.connect_timeout_ = 300ms;
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        auto learned = co_await client.client().get("/learn").send();
        EXPECT_TRUE(learned.protocol_version() == ruvia::http_protocol_version::http11);
        (void)co_await learned.text();
        // The advertised alternative never answers: the request falls back to TCP.
        auto fallback = co_await client.client().get("/fallback").send();
        EXPECT_TRUE(fallback.protocol_version() == ruvia::http_protocol_version::http11);
        const auto fallback_body = co_await fallback.text();
        EXPECT_EQ(fallback_body.text(), "/fallback");
        (void)co_await ruvia::sleep_for(worker, 100ms);
        EXPECT_NE(datagrams, 0U);
        const auto attempted = datagrams;
        // The failed alternative rests even though every response re-advertises it.
        for (int request = 0; request != 3; ++request) {
            auto rested = co_await client.client().get("/rested").send();
            EXPECT_TRUE(rested.protocol_version() == ruvia::http_protocol_version::http11);
            (void)co_await rested.text();
        }
        (void)co_await ruvia::sleep_for(worker, 100ms);
        EXPECT_EQ(datagrams, attempted);
        std::error_code ignored;
        sink.close(ignored);
        while (!sink_done) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
    });
}

TEST(Ruvia, http_client_origin_less_alias_follows_alt_svc_per_route) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    asio::ip::udp::socket sink(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::size_t datagrams = 0;
    bool sink_done = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        std::array<char, 2048> bytes{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            std::error_code error;
            (void)co_await sink.async_receive_from(asio::buffer(bytes), sender, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            ++datagrams;
        }
        sink_done = true; }, asio::detached);
    const auto alternative_port = sink.local_endpoint().port();
    peer.alt_svc_ = "h3=\":" + std::to_string(alternative_port) + "\"";
    std::vector<std::uint16_t> validated_ports;
    ruvia::http_client_config config{.base_url_ = "https://"};
    config.address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([&validated_ports](const ruvia::ip_address&, std::uint16_t port) {
        validated_ports.push_back(port);
        return true;
    });
    config.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = true};
    config.connect_timeout_ = 300ms;
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    const auto route = [&peer] {
        ruvia::http_client_request_options options;
        options.authority_ = std::string("origin.example");
        options.dial_host_ = std::string("127.0.0.1");
        options.dial_port_ = ruvia::port_number(peer.port());
        return options;
    };
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        auto learned = co_await client.client().get("/learn").configure(route()).send();
        EXPECT_TRUE(learned.protocol_version() == ruvia::http_protocol_version::http11);
        (void)co_await learned.text();
        // The route's advertised alternative is tried, then TCP serves the request.
        auto fallback = co_await client.client().get("/fallback").configure(route()).send();
        EXPECT_TRUE(fallback.protocol_version() == ruvia::http_protocol_version::http11);
        const auto body = co_await fallback.text();
        EXPECT_EQ(body.text(), "/fallback");
        (void)co_await ruvia::sleep_for(worker, 100ms);
        EXPECT_NE(datagrams, 0U);
        std::error_code ignored;
        sink.close(ignored);
        while (!sink_done) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
    });
    // The advertised endpoint passed the alias's address validator before dialing.
    EXPECT_NE(std::ranges::find(validated_ports, alternative_port), validated_ports.end());
}

TEST(Ruvia, http_client_second_alt_svc_clear_header_evicts_advertised_service) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    asio::ip::udp::socket sink(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::size_t datagrams = 0;
    bool sink_done = false;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        std::array<char, 2048> bytes{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            std::error_code error;
            (void)co_await sink.async_receive_from(asio::buffer(bytes), sender, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                break;
            }
            ++datagrams;
        }
        sink_done = true; }, asio::detached);

    const auto alternative_port = sink.local_endpoint().port();
    // First advertise h3, then clear it in a second Alt-Svc header field on the same response (RFC 7838 §3).
    peer.alt_svc_ = "h3=\":" + std::to_string(alternative_port) + "\"";
    peer.extra_alt_svc_headers_ = {"clear"};

    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = true};
    config.connect_timeout_ = 300ms;
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        auto initial = co_await client.client().get("/cleared-in-same-response").send();
        EXPECT_EQ(initial.protocol_version(), ruvia::http_protocol_version::http11);
        (void)co_await initial.text();

        // Subsequent request must stay on TCP without dialing the cleared UDP alternative.
        peer.alt_svc_.clear();
        peer.extra_alt_svc_headers_.clear();
        auto follow_up = co_await client.client().get("/after-clear").send();
        EXPECT_EQ(follow_up.protocol_version(), ruvia::http_protocol_version::http11);
        const auto follow_up_body = co_await follow_up.text();
        EXPECT_EQ(follow_up_body.text(), "/after-clear");
        (void)co_await ruvia::sleep_for(worker, 50ms);
        EXPECT_EQ(datagrams, 0U);

        // Sanity check: advertising without trailing clear does attempt HTTP/3, and a subsequent
        // response carrying h3 followed by Alt-Svc: clear evicts the previously cached entry too.
        peer.alt_svc_ = "h3=\":" + std::to_string(alternative_port) + "\"; ma=3600";
        auto advertised = co_await client.client().get("/advertise").send();
        (void)co_await advertised.text();
        peer.extra_alt_svc_headers_ = {"clear"};
        // Force a TCP response that carries both h3 and trailing clear before HTTP/3 is attempted:
        auto evicted = co_await client.client().get("/evict").configure({.versions_ = ruvia::http_client_versions{.http2_ = false, .http3_ = false}}).send();
        (void)co_await evicted.text();
        peer.alt_svc_.clear();
        peer.extra_alt_svc_headers_.clear();

        auto after_evict = co_await client.client().get("/after-evict").send();
        EXPECT_EQ(after_evict.protocol_version(), ruvia::http_protocol_version::http11);
        (void)co_await after_evict.text();
        (void)co_await ruvia::sleep_for(worker, 50ms);
        EXPECT_EQ(datagrams, 0U);

        std::error_code ignored;
        sink.close(ignored);
        while (!sink_done) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
    });
}
#endif

TEST(Ruvia, http_client_connection_limits_bound_open_and_idle_connections_without_preallocation) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    config.connection_limits_ = {.max_open_ = 2, .max_idle_http1_ = 1, .max_idle_multiplexed_ = 0};
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        const auto handle = client.client();
        EXPECT_EQ(handle.stats().open_connections_, 0U);
        std::optional<std::uint64_t> first_id;
        std::optional<std::uint64_t> second_id;
        ruvia::task_scope held(worker);
        const auto hold = [&](std::optional<std::uint64_t>& id) -> ruvia::task<void> {
            auto response = co_await handle.get("/hold").send();
            id = response.connection_info().id_;
            (void)co_await response.text();
        };
        held.spawn(hold(first_id));
        held.spawn(hold(second_id));
        while (peer.held_ != 2) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
        EXPECT_EQ(handle.stats().open_connections_, 2U);
        // max_open_ is reached by two active connections: a third request
        // waits for admission until its acquisition bound.
        auto waiting = handle.get("/late");
        waiting.configure({.timeouts_ = {.acquire_ = ruvia::http_client_timeout_override::after(50ms)}});
        try {
            (void)co_await waiting.send();
            ADD_FAILURE() << "admission beyond max_open_ succeeded";
        } catch (const std::system_error& error) {
            EXPECT_EQ(error.code(), std::errc::timed_out);
            const auto* client_error = dynamic_cast<const ruvia::http_client_error*>(&error);
            EXPECT_NE(client_error, nullptr);
            if (client_error) {
                EXPECT_FALSE(client_error->request_written());
            }
        }
        peer.release();
        co_await held.join();
        // Beyond max_idle_http1_ the least recently used idle connection closed.
        for (int attempt = 0; attempt != 1000 && handle.stats().open_connections_ != 1; ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
        EXPECT_EQ(handle.stats().open_connections_, 1U);
        EXPECT_EQ(handle.stats().idle_connections_, 1U);
        auto reused = co_await handle.get("/reuse").send();
        EXPECT_TRUE(reused.connection_info().reused_);
        const auto kept = reused.connection_info().id_;
        EXPECT_TRUE(kept == *first_id || kept == *second_id);
        (void)co_await reused.text();
        EXPECT_TRUE(handle.close_connection(kept));
        for (int attempt = 0; attempt != 1000 && handle.stats().open_connections_ != 0; ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 1ms);
        }
        EXPECT_EQ(handle.stats().open_connections_, 0U);
        EXPECT_EQ(handle.stats().idle_connections_, 0U);
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_TRUE(peer.connections_[0].closed_);
    EXPECT_TRUE(peer.connections_[1].closed_);
}

TEST(Ruvia, http_client_upload_progress_keeps_the_response_head_wait_alive) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.read_timeout_ = 100ms;
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        // The origin reads the complete request before it answers; the upload
        // outlasts read_timeout_ while every chunk arrives within it.
        auto exchange = co_await client.client().post("/drain").upload();
        for (int chunk = 0; chunk != 6; ++chunk) {
            co_await exchange.body().write(std::string_view("chunk"));
            (void)co_await ruvia::sleep_for(worker, 50ms);
        }
        co_await exchange.body().end();
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.ok());
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "/drain");
    });
    ASSERT_EQ(peer.connections_.size(), 1U);
    EXPECT_GT(peer.connections_[0].content_bytes_, 6U * 5U);
}

TEST(Ruvia, http_client_upgrade_tunnels_follow_their_own_idle_timeout) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.read_timeout_ = 50ms;
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        const auto upgrade = [&](ruvia::http_client_tunnel_config tunnel) {
            return client.client().get("/chat").header("Connection", "Upgrade").header("Upgrade", "chat").upgrade(tunnel);
        };
        {
            // Without an idle timeout, read_timeout_ no longer bounds the tunnel.
            auto opened = co_await upgrade({});
            auto* tunnel = opened.tunnel();
            if (tunnel == nullptr) {
                ADD_FAILURE() << "Upgrade was not switched";
                co_return;
            }
            (void)co_await ruvia::sleep_for(worker, 200ms);
            co_await tunnel->write(std::string_view("ping"));
            const auto echoed = co_await tunnel->text();
            EXPECT_TRUE(echoed == std::optional<std::string_view>("ping"));
        }
        {
            auto opened = co_await upgrade({.idle_timeout_ = std::chrono::milliseconds(150)});
            auto* tunnel = opened.tunnel();
            if (tunnel == nullptr) {
                ADD_FAILURE() << "Upgrade was not switched";
                co_return;
            }
            // Traffic in either direction restarts the idle bound.
            for (int round = 0; round != 3; ++round) {
                (void)co_await ruvia::sleep_for(worker, 75ms);
                co_await tunnel->write(std::string_view("tick"));
                EXPECT_TRUE((co_await tunnel->text()) == std::optional<std::string_view>("tick"));
            }
            std::optional<std::error_code> failure;
            try {
                (void)co_await tunnel->read();
            } catch (const ruvia::http_client_error& error) {
                failure = error.code();
            }
            EXPECT_EQ(failure, std::make_error_code(std::errc::timed_out));
        }
    });
}

TEST(Ruvia, http1_client_abandoned_upload_keeps_the_early_response) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        auto exchange = co_await client.client().post("/early").upload();
        EXPECT_TRUE(exchange.body().writable());
        co_await exchange.body().write(std::string_view("partial"));
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.ok());
        exchange.body().abandon();
        EXPECT_FALSE(exchange.body().complete());
        EXPECT_FALSE(exchange.body().writable());
        EXPECT_THROW((void)exchange.body().write(std::string_view("late")), ruvia::http_client_error);
        const auto body = co_await response.text();
        EXPECT_EQ(body.text(), "/early");
        // The unfinished request leaves the connection unusable.
        auto next = co_await client.client().get("/next").send();
        EXPECT_FALSE(next.connection_info().reused_);
        (void)co_await next.text();
    });
    ASSERT_EQ(peer.connections_.size(), 2U);
    EXPECT_TRUE(peer.connections_[0].closed_);
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_client_abandoned_upload_ends_only_its_sending_half) {
    using namespace origin_routing_tests;
    ruvia::test::tls_identity identity("origin.example");
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, &identity.context_);
    ruvia::http_client_config config{.base_url_ = "https://origin.example", .dial_host_ = "127.0.0.1", .dial_port_ = peer.port()};
    config.request_timeout_ = 5s;
    config.tls_.ca_file_ = identity.ca_file_;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        // The origin answers the head at once and completes its response
        // while the request content is still open.
        auto exchange = co_await client.client().post("/early").upload();
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.ok());
        EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http2);
        exchange.body().abandon();
        EXPECT_FALSE(exchange.body().complete());
        const auto body = co_await response.text();
        EXPECT_TRUE(body.empty());
        EXPECT_TRUE(response.stream().complete());
        for (int attempt = 0; attempt != 200 && (peer.connections_.empty() || peer.connections_[0].resets_.empty()); ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 5ms);
        }
    });
    ASSERT_EQ(peer.connections_.size(), 1U);
    EXPECT_EQ(peer.connections_[0].resets_, (std::deque<ruvia::http2_error_code>{ruvia::http2_error_code::no_error}));
}
#endif

TEST(Ruvia, http_client_worker_idle_bound_spans_aliases) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer first_peer(io, nullptr);
    routing_peer second_peer(io, nullptr);
    const std::string first_url = "http://127.0.0.1:" + std::to_string(first_peer.port());
    const std::string second_url = "http://127.0.0.1:" + std::to_string(second_peer.port());
    const std::array configs{ruvia::http_client_config{.alias_ = "first", .base_url_ = first_url},
        ruvia::http_client_config{.alias_ = "second", .base_url_ = second_url}};
    run_clients(attachment, {&first_peer, &second_peer}, configs, {.max_idle_http1_ = 1},
        [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            const auto first = client.client("first");
            const auto second = client.client("second");
            auto a = co_await first.get("/a").send();
            (void)co_await a.text();
            EXPECT_EQ(first.stats().idle_connections_, 1U);
            auto b = co_await second.get("/b").send();
            (void)co_await b.text();
            // Each alias allows 64 idle connections; the worker keeps one, so
            // the least recently used, of the other alias, closes.
            for (int attempt = 0; attempt != 200 && first.stats().open_connections_ != 0; ++attempt) {
                (void)co_await ruvia::sleep_for(worker, 5ms);
            }
            EXPECT_EQ(first.stats().open_connections_, 0U);
            EXPECT_EQ(second.stats().idle_connections_, 1U);
        });
    ASSERT_EQ(first_peer.connections_.size(), 1U);
    EXPECT_TRUE(first_peer.connections_[0].closed_);
}

TEST(Ruvia, http_client_alias_without_a_fixed_origin_routes_each_request_to_its_authority) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    routing_peer other_peer(io, nullptr);
    const auto authority = "127.0.0.1:" + std::to_string(peer.port());
    const auto other_authority = "127.0.0.1:" + std::to_string(other_peer.port());
    peer.redirect_location_ = "http://" + other_authority + "/landing";
    ruvia::http_client_config config{.base_url_ = "http://"};
    config.request_timeout_ = 5s;
    config.address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([](const ruvia::ip_address& ip, std::uint16_t) { return ip == *ruvia::ip_address::parse("127.0.0.1"); });
    run_client(io, attachment, {&peer, &other_peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
        const auto handle = client.client();
        const auto to = [](std::string_view value) {
            ruvia::http_client_request_options options;
            options.authority_.emplace(value);
            return options;
        };
        const auto fails_with = [&](ruvia::http_client_request request) -> ruvia::task<std::optional<std::error_code>> {
            try {
                (void)co_await request.send();
            } catch (const ruvia::http_client_error& error) {
                co_return error.code();
            }
            co_return std::nullopt;
        };
        bool rejected = false;
        try {
            (void)co_await handle.get("/").send();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        auto response = co_await handle.get("/direct").configure(to(authority)).send();
        const auto direct = co_await response.text();
        EXPECT_EQ(direct.text(), "/direct");
        // Every dial address passes the validator.
        const auto rejected_dial = co_await fails_with(std::move(handle.get("/").configure(to("127.0.0.2:" + std::to_string(peer.port())))));
        EXPECT_EQ(rejected_dial, ruvia::make_error_code(ruvia::http_client_errc::resolve_failed));
        // Without a policy a cross-origin redirect is not followed.
        auto unfollowed = co_await handle.get("/redirect").configure(to(authority)).send();
        EXPECT_EQ(unfollowed.status().value(), 302);
        (void)co_await unfollowed.text();
        // An accepted origin is reached through this alias with its authority.
        auto followed = co_await handle.get("/redirect").configure(to(authority)).on_redirect([](const ruvia::http_client_redirect_hop&, ruvia::stop_token) -> ruvia::task<bool> {
                                                                                     co_return true;
                                                                                 })
                            .send();
        const auto landing = co_await followed.text();
        EXPECT_EQ(landing.text(), "/landing");
    });
    ASSERT_EQ(peer.connections_.size(), 1U);
    EXPECT_EQ(peer.connections_[0].authorities_.front(), authority);
    ASSERT_EQ(other_peer.connections_.size(), 1U);
    EXPECT_EQ(other_peer.connections_[0].authorities_, (std::deque<std::string>{other_authority}));
}

TEST(Ruvia, http_client_alias_without_a_fixed_origin_requires_an_address_validator) {
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    EXPECT_THROW(ruvia::http_client(attachment.loop(), {.base_url_ = "http://"}), std::invalid_argument);
    ruvia::http_client_config cookies{.base_url_ = "https://"};
    cookies.address_validator_ = std::make_shared<const ruvia::http_client_address_validator>([](const ruvia::ip_address&, std::uint16_t) { return true; });
    cookies.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    EXPECT_THROW(ruvia::http_client(attachment.loop(), cookies), std::invalid_argument);
}

TEST(Ruvia, http_client_dns_refresh_queries_only_live_routes_and_retires_moved_connections) {
    using namespace origin_routing_tests;
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    routing_peer peer(io, nullptr, "0.0.0.0");
    const std::string base_url = "http://localhost:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    config.dns_refresh_interval_ = 30ms;
    run_client(io, attachment, {&peer}, config, [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
        const auto handle = client.client();
        (void)co_await ruvia::sleep_for(worker, 150ms);
        // No connection, no query.
        EXPECT_EQ(handle.stats().dns_refreshes_, 0U);
        const std::array moved{ruvia::http_client_endpoint_address("127.0.0.2", peer.port())};
        handle.update_dns(moved);
        auto first = co_await handle.get("/first").send();
        EXPECT_EQ(first.connection_info().peer_ip_.to_string(), "127.0.0.2");
        (void)co_await first.text();
        // The refresh finds localhost elsewhere and retires the moved connection.
        for (int attempt = 0; attempt != 400 && !peer.connections_[0].closed_; ++attempt) {
            (void)co_await ruvia::sleep_for(worker, 5ms);
        }
        EXPECT_TRUE(peer.connections_[0].closed_);
        EXPECT_GE(handle.stats().dns_refreshes_, 1U);
        auto second = co_await handle.get("/second").send();
        EXPECT_NE(second.connection_info().peer_ip_.to_string(), "127.0.0.2");
        (void)co_await second.text();
        lookups.join();
    });
}

TEST(Ruvia, http_client_dns_refresh_concurrency_is_bounded_per_worker) {
    using namespace origin_routing_tests;
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    routing_peer peer(io, nullptr);
    const std::string base_url = "http://localhost:" + std::to_string(peer.port());
    std::array configs{ruvia::http_client_config{.alias_ = "first", .base_url_ = base_url},
        ruvia::http_client_config{.alias_ = "second", .base_url_ = base_url}};
    for (auto& config : configs) {
        config.request_timeout_ = 5s;
        config.dns_refresh_interval_ = 20ms;
    }
    run_clients(attachment, {&peer}, configs, {.max_concurrent_dns_refreshes_ = 1},
        [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            const auto first = client.client("first");
            const auto second = client.client("second");
            auto a = co_await first.get("/a").send();
            (void)co_await a.text();
            auto b = co_await second.get("/b").send();
            (void)co_await b.text();
            // Hold the only lookup thread: the first refresh started stays
            // running, and every other period of either alias defers.
            std::promise<void> release;
            auto released = release.get_future();
            ruvia::task_scope blocker(worker);
            blocker.spawn([](ruvia::worker_ref owner, std::future<void>& gate) -> ruvia::task<void> {
                (void)co_await ruvia::run_blocking(std::move(owner), ruvia::operation_options{}, [&gate] { gate.wait(); });
            }(worker, released));
            const auto started = [&] { return first.stats().dns_refreshes_ + second.stats().dns_refreshes_; };
            for (int attempt = 0; attempt != 200 && started() == 0; ++attempt) {
                (void)co_await ruvia::sleep_for(worker, 5ms);
            }
            (void)co_await ruvia::sleep_for(worker, 150ms);
            EXPECT_EQ(started(), 1U);
            release.set_value();
            co_await blocker.join();
            for (int attempt = 0; attempt != 200 && started() < 3; ++attempt) {
                (void)co_await ruvia::sleep_for(worker, 5ms);
            }
            EXPECT_GE(started(), 3U);
        });
    lookups.join();
}

TEST(Ruvia, http_client_redirect_hop_observation_and_streaming_upload_redirect) {
    using namespace origin_routing_tests;
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io);
    routing_peer peer(io, nullptr);
    routing_peer second_peer(io, nullptr);
    const std::string base_url = "http://127.0.0.1:" + std::to_string(peer.port());
    const std::string second_url = "http://127.0.0.1:" + std::to_string(second_peer.port());
    peer.redirect_location_ = second_url + "/final-destination";
    std::array configs{
        ruvia::http_client_config{.alias_ = "default", .base_url_ = base_url},
        ruvia::http_client_config{.alias_ = "second", .base_url_ = second_url},
    };
    run_clients(attachment, {&peer, &second_peer}, configs, {},
        [&](ruvia::http_client& client, const ruvia::worker_ref&) -> ruvia::task<void> {
            const auto handle = client.client();
            std::vector<std::string> observed_targets;
            std::vector<std::uint16_t> observed_statuses;

            // 1) Streaming upload after write + end follows 302 -> GET and reports hop to on_redirect.
            auto upload_exchange = co_await handle.post("/drain-redirect")
                                       .on_redirect([&](const ruvia::http_client_redirect_hop& hop) {
                                           observed_targets.emplace_back(hop.target_url());
                                           observed_statuses.push_back(hop.status().value());
                                           EXPECT_EQ(hop.hop_index(), 0U);
                                           EXPECT_EQ(hop.source_target(), "/drain-redirect");
                                           EXPECT_TRUE(hop.cross_origin());
                                           EXPECT_EQ(hop.connection_info().peer_port_, peer.port());
                                           return true;
                                       })
                                       .upload();
            co_await upload_exchange.body().write("upload-payload");
            co_await upload_exchange.body().end();
            auto redirected_resp = co_await upload_exchange.response();
            EXPECT_EQ(redirected_resp.status(), ruvia::http_status::ok);
            const auto landed_body = co_await redirected_resp.text();
            EXPECT_EQ(landed_body.text(), "/final-destination");
            EXPECT_EQ(observed_targets.size(), 1U);
            if (!observed_targets.empty()) {
                EXPECT_EQ(observed_targets[0], second_url + "/final-destination");
                EXPECT_EQ(observed_statuses[0], 302U);
            }

            // 2) Early 302 during streaming upload stops the writer and follows redirect.
            auto early_upload = co_await handle.post("/redirect").upload();
            auto early_resp = co_await early_upload.response();
            EXPECT_FALSE(early_upload.body().writable());
            const auto early_body = co_await early_resp.text();
            EXPECT_EQ(early_body.text(), "/final-destination");

            // 3) Returning false from on_redirect stops following and returns the 302 response.
            auto stopped_resp = co_await handle.get("/redirect")
                                    .on_redirect([](const ruvia::http_client_redirect_hop&) {
                                        return false;
                                    })
                                    .send();
            EXPECT_EQ(stopped_resp.status(), ruvia::http_status::found);
            (void)co_await stopped_resp.text();

            // 4) Subsequent hop connection failure reports redirect_hops() == 1 after hop 0 succeeded.
            second_peer.stop();
            bool first_hop_recorded = false;
            std::size_t failed_hop_count = 0;
            try {
                (void)co_await handle.get("/redirect")
                    .configure({.fresh_connection_ = true})
                    .on_redirect([&](const ruvia::http_client_redirect_hop& hop) {
                        first_hop_recorded = (hop.hop_index() == 0);
                        return true;
                    })
                    .send();
            } catch (const ruvia::http_client_error& error) {
                failed_hop_count = error.redirect_hops();
                EXPECT_TRUE(error.followed_redirect());
            }
            EXPECT_TRUE(first_hop_recorded);
            EXPECT_EQ(failed_hop_count, 1U);
        });
}

TEST(Ruvia, http_client_non_destructive_dns_resolution_preserves_active_connections) {
    using namespace origin_routing_tests;
    ruvia::blocking_pool lookups({.thread_count_ = 1});
    auto& io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &lookups});
    routing_peer peer(io, nullptr, "0.0.0.0");
    const std::string base_url = "http://localhost:" + std::to_string(peer.port());
    ruvia::http_client_config config{.base_url_ = base_url};
    config.request_timeout_ = 5s;
    run_client(io, attachment, {&peer}, config,
        [&](ruvia::http_client& client, const ruvia::worker_ref& worker) -> ruvia::task<void> {
            const auto handle = client.client();
            const std::array pinned{ruvia::http_client_endpoint_address("127.0.0.2", peer.port())};
            handle.update_dns(pinned);

            auto first = co_await handle.get("/first").send();
            EXPECT_EQ(first.connection_info().peer_ip_.to_string(), "127.0.0.2");
            (void)co_await first.text();

            // Non-destructive DNS resolution returns sorted, deduplicated addresses
            // without retiring the active 127.0.0.2 connection or changing dns_snapshot().
            const auto resolved_handle = co_await handle.resolve_dns();
            EXPECT_FALSE(resolved_handle.empty());
            EXPECT_TRUE(std::ranges::is_sorted(resolved_handle));

            const auto resolved_standalone = co_await ruvia::resolve_dns(worker, "localhost");
            EXPECT_EQ(resolved_handle, resolved_standalone);

            const auto resolved_loop = co_await ruvia::resolve_dns(attachment.loop(), "127.0.0.1");
            EXPECT_EQ(resolved_loop.size(), 1U);
            if (!resolved_loop.empty()) {
                EXPECT_EQ(resolved_loop[0].to_string(), "127.0.0.1");
            }

            EXPECT_FALSE(peer.connections_[0].closed_);
            auto second = co_await handle.get("/second").send();
            EXPECT_TRUE(second.connection_info().reused_);
            EXPECT_EQ(second.connection_info().peer_ip_.to_string(), "127.0.0.2");
            (void)co_await second.text();
            lookups.join();
        });
}

namespace {
class test_worker final {
public:
    explicit test_worker(asio::io_context& io)
        : attachment_(ruvia::attach_event_loop(io, {.queue_capacity_ = 8, .blocking_pool_ = &host_lookups_})),
          handle_(attachment_.loop().worker()) {}

    test_worker(const test_worker&) = delete;
    test_worker& operator=(const test_worker&) = delete;

    ~test_worker() {
        host_lookups_.join();
    }

    // Declared before the attachment, which borrows it for "localhost" lookups.
    ruvia::blocking_pool host_lookups_{{.thread_count_ = 1}};
    ruvia::event_loop_attachment attachment_;
    ruvia::worker_ref handle_;
};

class loopback_response_server final {
public:
    loopback_response_server(asio::io_context& io_context, const ruvia::worker_ref& worker_value,
        std::initializer_list<std::string_view> bodies,
        std::chrono::milliseconds response_delay = std::chrono::milliseconds::zero(),
        bool close_without_response = false,
        std::size_t responses_before_close = 0)
        : io_context_(io_context),
          acceptor_(io_context, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker_value),
          response_delay_(response_delay),
          close_without_response_(close_without_response),
          responses_before_close_(responses_before_close) {
        responses_.reserve(bodies.size());
        for (const auto& body : bodies) {
            responses_.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: ");
            responses_.back().append(std::to_string(body.size())).append("\r\nX-Peer: retained\r\nConnection: close\r\n\r\n").append(body);
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        accept_next();
    }

    // Runs once on the worker when the next request head has arrived, while its
    // (possibly delayed) response is still pending: an in-flight exchange.
    void on_request(std::function<void()> callback) {
        on_request_ = std::move(callback);
    }

    // A client that ended before its connection reached this server would
    // otherwise leave the pending accept, and wait(), unfinished forever.
    void stop_accepting() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }

    [[nodiscard]] ruvia::task<void> wait() {
        co_await done_.wait();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    void accept_next() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(io_context_);
        acceptor_.async_accept(*socket, [this, socket](const std::error_code& error) {
            if (error) {
                fail(error);
                return;
            }
            auto request = std::make_shared<asio::streambuf>();
            asio::async_read_until(*socket, *request, "\r\n\r\n",
                [this, socket, request](const std::error_code& read_error, std::size_t) {
                    if (read_error) {
                        fail(read_error);
                        return;
                    }
                    if (auto callback = std::exchange(on_request_, {})) {
                        callback();
                    }
                    if (close_without_response_ && next_response_ >= responses_before_close_) {
                        std::error_code ignored;
                        socket->close(ignored);
                        done_.notify();
                        return;
                    }
                    const auto response_index = next_response_++;
                    auto write_response = [this, socket, response_index] {
                        asio::async_write(*socket, asio::buffer(responses_[response_index]),
                            [this, socket](const std::error_code& write_error, std::size_t) {
                                if (write_error) {
                                    fail(write_error);
                                    return;
                                }
                                if (next_response_ == responses_.size()) {
                                    done_.notify();
                                } else {
                                    accept_next();
                                }
                            });
                    };
                    if (response_delay_ == std::chrono::milliseconds::zero()) {
                        write_response();
                    } else {
                        auto timer = std::make_shared<asio::steady_timer>(io_context_, response_delay_);
                        timer->async_wait([timer, write_response = std::move(write_response)](
                                              const std::error_code& timer_error) mutable {
                            if (!timer_error) {
                                write_response();
                            }
                        });
                    }
                });
        });
    }

    void fail(const std::error_code& error) {
        if (failure_ == nullptr) {
            failure_ = std::make_exception_ptr(std::system_error(error));
            done_.notify();
        }
    }

    asio::io_context& io_context_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::string> responses_{};
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    std::function<void()> on_request_;
    std::size_t next_response_{0};
    std::chrono::milliseconds response_delay_;
    bool close_without_response_;
    std::size_t responses_before_close_;
};

[[nodiscard]] ruvia::http_client_config local_http_client_config(std::string_view base_url) {
    return ruvia::http_client_config{.base_url_ = base_url};
}

class upload_peer final {
public:
    static constexpr std::size_t early_hint_count = 300;
    static constexpr std::size_t rejected_body_size = 32 * 1024;
    enum class mode_type { chunked,
        known_length,
        continue_value,
        continue_timeout,
        early_final,
        early_final_while_uploading,
        early_final_orderly_while_uploading };
    upload_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, mode_type mode)
        : io_(io),
          acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          socket_(io),
          response_release_(io, asio::steady_timer::time_point::max()),
          done_(worker_value),
          mode_(mode) {}
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr error) {
            std::error_code ignored;
            socket_.close(ignored);
            failure_ = error;
            complete_ = true;
            done_.notify();
        });
    }
    void stop() {
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
        response_release_.cancel();
    }
    void release_response() {
        response_release_.expires_at(asio::steady_timer::time_point::min());
    }
    ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string head_{};
    std::string body_{};
    ruvia::http_request_trailers trailers_;
    std::size_t uploaded_bytes_{0};

private:
    asio::awaitable<void> serve() {
        auto& socket = socket_;
        co_await acceptor_.async_accept(socket, asio::use_awaitable);
        std::string input{};
        const auto head_size = co_await asio::async_read_until(socket, asio::dynamic_buffer(input), "\r\n\r\n", asio::use_awaitable);
        head_.assign(input.data(), head_size);
        input.erase(0, head_size);
        if (mode_ == mode_type::early_final) {
            constexpr std::string_view reply = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno";
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
            co_return;
        }
        if (mode_ == mode_type::early_final_while_uploading ||
            mode_ == mode_type::early_final_orderly_while_uploading) {
            // Prove that a large upload is already in progress before sending
            // a final response. The client's small response queue exercises
            // body delivery while the upload is stopped.
            if (input.empty()) {
                co_await asio::async_read(
                    socket, asio::dynamic_buffer(input), asio::transfer_exactly(1), asio::use_awaitable);
            }
            uploaded_bytes_ = input.size();
            const std::string body(rejected_body_size, 'r');
            const std::string head = std::move(std::string("HTTP/1.1 413 Content Too Large\r\nContent-Length: ").append(std::to_string(body.size()))) +
                                     "\r\nConnection: close\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(body), asio::use_awaitable);
            if (mode_ == mode_type::early_final_orderly_while_uploading) {
                // Keep the socket open until the client consumes the response.
                // Closing with unread request bytes can reset the connection and
                // discard response bytes on Windows.
                socket.shutdown(asio::ip::tcp::socket::shutdown_send);
                std::error_code ignored;
                co_await response_release_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
                co_return;
            }
            asio::steady_timer response_visible(io_, std::chrono::milliseconds(10));
            co_await response_visible.async_wait(asio::use_awaitable);
            std::error_code ignored;
            socket.shutdown(asio::ip::tcp::socket::shutdown_receive, ignored);
            co_return;
        }
        if (mode_ == mode_type::continue_value) {
            if (!input.empty()) {
                throw std::runtime_error("upload arrived before 100 Continue");
            }
            constexpr std::string_view early_hint =
                "HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\n";
            std::string reply{};
            reply.reserve(early_hint_count * early_hint.size() + 25);
            for (std::size_t i = 0; i < early_hint_count; ++i) {
                reply.append(early_hint);
            }
            reply.append("HTTP/1.1 100 Continue\r\n\r\n");
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
        }
        if (mode_ == mode_type::known_length) {
            if (input.size() < 6) {
                co_await asio::async_read(socket, asio::dynamic_buffer(input), asio::transfer_exactly(6 - input.size()), asio::use_awaitable);
            }
            body_ = std::move(input);
        } else {
            ruvia::http1_chunked_body_decoder decoder;
            for (;;) {
                const auto decoded = decoder.decode(input);
                if (decoded.failure()) {
                    throw std::runtime_error("invalid upload chunk framing");
                }
                if (const auto* chunk = decoded.body_chunk()) {
                    body_.append(chunk->bytes());
                }
                if (const auto* complete = decoded.complete()) {
                    if (trailers_.append_received(complete->trailers())) {
                        throw std::runtime_error("invalid upload trailers");
                    }
                    break;
                }
                input.erase(0, decoded.consumed_bytes());
                if (decoded.need_more()) {
                    co_await asio::async_read(socket, asio::dynamic_buffer(input),
                        asio::transfer_at_least(1), asio::use_awaitable);
                }
            }
        }
        if (mode_ == mode_type::known_length) {
            asio::steady_timer completed(io_, std::chrono::milliseconds(2));
            co_await completed.async_wait(asio::use_awaitable);
        }
        constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
    }
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer response_release_;
    ruvia::worker_signal done_;
    mode_type mode_;
    std::exception_ptr failure_;
    bool complete_{};
};

enum class cookie_peer_mode { http1,
    http1_replacement,
    http2 };

class cookie_stream_peer final {
public:
    cookie_stream_peer(asio::io_context& io, const ruvia::worker_ref& worker_value,
        cookie_peer_mode mode = cookie_peer_mode::http1)
        : io_(io),
          acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          deadline_(io),
          gate_(io),
          done_(worker_value),
          mode_(mode) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                stop();
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            complete_ = true;
            deadline_.cancel();
            done_.notify();
        });
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    void release_terminal_body() {
        release_requested_ = true;
        gate_.cancel();
    }

    std::array<std::string, 2> requests_{std::string{}, std::string{}};

private:
    void stop() noexcept {
        std::error_code ignored;
        acceptor_.close(ignored);
        gate_.cancel();
        for (auto& socket : sockets_) {
            if (socket) {
                socket->close(ignored);
            }
        }
    }

    asio::awaitable<void> serve() {
#if defined(RUVIA_ENABLE_HTTP2)
        if (mode_ == cookie_peer_mode::http2) {
            co_await serve_http2();
            co_return;
        }
#endif
        sockets_[0].emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        std::string request{};
        co_await asio::async_read_until(*sockets_[0], asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const std::string_view streaming_head = mode_ == cookie_peer_mode::http1_replacement
                                                    ? "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: close\r\nSet-Cookie: sid=old\r\n\r\n"
                                                    : "HTTP/1.1 103 Early Hints\r\nSet-Cookie: informational=ignored; Path=/\r\n\r\n"
                                                      "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nSet-Cookie: session=ready; Path=/\r\n\r\n";
        co_await asio::async_write(*sockets_[0], asio::buffer(streaming_head), asio::use_awaitable);
        for (std::size_t index = 0; index != requests_.size(); ++index) {
            auto& socket = sockets_[index + 1];
            socket.emplace(co_await acceptor_.async_accept(asio::use_awaitable));
            co_await asio::async_read_until(*socket, asio::dynamic_buffer(requests_[index]), "\r\n\r\n", asio::use_awaitable);
            const std::string_view response = mode_ == cookie_peer_mode::http1_replacement && index == 0
                                                  ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\nSet-Cookie: sid=new; Path=/scope\r\n\r\n"
                                                  : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            co_await asio::async_write(*socket, asio::buffer(response), asio::use_awaitable);
            std::error_code ignored;
            socket->close(ignored);
            if (mode_ == cookie_peer_mode::http1_replacement && index == 0) {
                if (!release_requested_) {
                    gate_.expires_at(std::chrono::steady_clock::time_point::max());
                    std::error_code error;
                    co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, error));
                    if (!release_requested_) {
                        throw std::runtime_error("cookie response completion gate expired");
                    }
                }
                constexpr std::string_view body = "x";
                co_await asio::async_write(*sockets_[0], asio::buffer(body), asio::use_awaitable);
                sockets_[0]->close(ignored);
            }
        }
    }

#if defined(RUVIA_ENABLE_HTTP2)
    asio::awaitable<void> serve_http2() {
        sockets_[0].emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        auto& socket = *sockets_[0];
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> streaming_request;
        std::array<char, 16384> input{};
        std::string output;
        std::size_t received = 0;
        for (;;) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            if (received == requests_.size()) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid cookie test HTTP/2 input");
                }
                while (auto event = connection.next_event()) {
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    const auto stream_id = request->stream_id();
                    ruvia::http_response response;
                    if (!streaming_request) {
                        const std::array fields{ruvia::http_header_view{"set-cookie", "informational=ignored; Path=/"}};
                        if (connection.submit_interim_response_head(stream_id,
                                ruvia::http_interim_response_head(ruvia::http_status::early_hints, fields)) !=
                            ruvia::http2_submit_status::accepted) {
                            throw std::runtime_error("cookie test interim head rejected");
                        }
                        response.header("set-cookie", "session=ready; Path=/");
                        if (!std::holds_alternative<ruvia::http_response_stream_commit_plan>(
                                connection.submit_streaming_response_head(stream_id, std::move(response)))) {
                            throw std::runtime_error("cookie test streaming head rejected");
                        }
                        streaming_request.emplace(std::move(*request));
                    } else {
                        if (received == requests_.size()) {
                            throw std::runtime_error("unexpected cookie test request");
                        }
                        const auto built = request->build_request();
                        requests_[received++].assign(built.header("cookie").value_or(std::string_view{}));
                        const auto head = connection.submit_response_head(stream_id, response,
                            ruvia::plan_buffered_http_response_write(built.known_method(), response));
                        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) ||
                            connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
                            throw std::runtime_error("cookie test response rejected");
                        }
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    }

#endif
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer deadline_;
    asio::steady_timer gate_;
    ruvia::worker_signal done_;
    cookie_peer_mode mode_;
    std::array<std::optional<asio::ip::tcp::socket>, 3> sockets_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool release_requested_{false};
};

// Answers one request per connection with the next scripted response head and
// records every request head.
class scripted_head_peer final {
public:
    scripted_head_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, std::initializer_list<std::string_view> heads)
        : acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          deadline_(io),
          done_(worker_value),
          heads_(heads.begin(), heads.end()),
          io_(io) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                std::error_code ignored;
                acceptor_.close(ignored);
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            complete_ = true;
            deadline_.cancel();
            done_.notify();
        });
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::vector<std::string> requests_{};

private:
    asio::awaitable<void> serve() {
        for (const auto& head : heads_) {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
            auto& request = requests_.emplace_back();
            co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer deadline_;
    ruvia::worker_signal done_;
    std::vector<std::string> heads_{};
    asio::io_context& io_;
    std::exception_ptr failure_;
    bool complete_{false};
};

[[nodiscard]] std::string large_gzip_response_body() {
    const std::string body(128 * 1024, 'z');
    auto result_value = ruvia::encode_http_content(ruvia::http_content_coding::gzip, body,
        {.max_encoded_bytes_ = body.size()});
    if (!result_value.has_value()) {
        throw std::runtime_error("could not encode fake HTTP response body");
    }
    return std::string(result_value->data(), result_value->size());
}

class http1_chunked_response_peer final {
public:
    enum class end_mode_type : unsigned char { terminal_gate,
        wait_for_client_close };

    explicit http1_chunked_response_peer(asio::io_context& io, std::string_view encoded,
        end_mode_type end_mode = end_mode_type::terminal_gate, std::string_view long_header = {})
        : io_(io),
          acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          gate_(io),
          encoded_(encoded),
          long_header_(long_header),
          end_mode_(end_mode),
          done_(done_promise_.get_future()) {}

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = failure;
            done_promise_.set_value();
        });
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void release_terminal_chunk() {
        release_requested_ = true;
        gate_.cancel();
    }

    [[nodiscard]] bool observed_client_close() const noexcept {
        return observed_client_close_;
    }

    void wait() {
        if (done_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("HTTP/1 peer did not finish");
        }
        done_.get();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    asio::awaitable<void> serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        std::array<char, 2 * sizeof(std::size_t)> chunk_size{};
        const auto [chunk_end, chunk_error] = std::to_chars(
            chunk_size.data(), chunk_size.data() + chunk_size.size(), encoded_.size(), 16);
        if (chunk_error != std::errc{}) {
            throw std::runtime_error("HTTP/1 peer could not format chunk size");
        }
        std::string response{
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n"
            "Trailer: X-End\r\nConnection: close\r\n"};
        if (!long_header_.empty()) {
            response.append("X-Long: ").append(long_header_).append("\r\n");
        }
        response += "\r\n";
        response.append(chunk_size.data(), chunk_end);
        response.append("\r\n").append(encoded_).append("\r\n");
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        if (end_mode_ == end_mode_type::wait_for_client_close) {
            std::array<char, 1024> input{};
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(input),
                asio::redirect_error(asio::use_awaitable, error))) {
            }
            observed_client_close_ = error == asio::error::eof ||
                                     error == asio::error::connection_reset ||
                                     error == asio::error::operation_aborted;
            co_return;
        }

        if (!release_requested_) {
            gate_.expires_after(std::chrono::seconds(5));
            std::error_code gate_error;
            co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
            if (gate_error != asio::error::operation_aborted) {
                throw std::runtime_error("HTTP/1 terminal-chunk gate expired");
            }
        }

        constexpr std::string_view terminal = "0\r\nX-End: retained\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(terminal), asio::use_awaitable);
        std::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer gate_;
    std::string encoded_{};
    std::string long_header_{};
    end_mode_type end_mode_;
    std::promise<void> done_promise_;
    std::future<void> done_;
    std::exception_ptr failure_;
    bool release_requested_{false};
    bool observed_client_close_{false};
};

template <typename operation_type>
void run_operation(test_worker& worker_value, asio::io_context& io, operation_type&& operation) {
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            co_await operation();
        } catch (...) {
            failure = std::current_exception();
        }
        worker_value.attachment_.stop();
    };
    auto root = worker_value.attachment_.loop().start(run());
    worker_value.attachment_.run();
    std::move(root).get();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

TEST(Ruvia, client_response_body_collectors_return_public_results) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected("\0\xff\xc3", 3);
    loopback_response_server server(io, worker.handle_, {expected});
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
    server.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/collect").send();
        EXPECT_TRUE(!response.stream().complete());
        auto cold_read = response.read();
        auto cold_text = response.stream().text();
        auto collected = co_await response.text();
        EXPECT_TRUE(response.stream().complete());
        EXPECT_EQ(collected.size(), expected.size());
        const std::string_view collected_view = collected.text();
        EXPECT_EQ(collected_view, expected);
        const auto remaining = co_await response.bytes();
        EXPECT_TRUE(remaining.empty());
        EXPECT_TRUE(!(co_await std::move(cold_read)));
        EXPECT_TRUE(!(co_await std::move(cold_text)));
        EXPECT_TRUE(!(co_await response.stream().text()));
        co_await client.shutdown();
        EXPECT_TRUE(response.stream().complete());
        EXPECT_EQ(collected_view, expected);
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, client_response_body_completion_preserves_truncation_after_buffered_bytes) {
    for (const auto& prefix : {std::string{}, std::string("abc")}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        scripted_head_peer peer(io, worker.handle_,
            {std::string("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\n") + prefix});
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            auto response = co_await client.client().get("/truncated").send();
            std::optional<std::error_code> response_error;
            try {
                (void)co_await response.stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                response_error = error.code();
            }
            EXPECT_TRUE(response_error.has_value());
            std::string received{};
            while (received.size() < prefix.size()) {
                EXPECT_TRUE(!response.stream().complete());
                const auto chunk = co_await response.stream().text();
                if (!chunk) {
                    break;
                }
                received.append(*chunk);
            }
            EXPECT_EQ(received, prefix);
            std::optional<std::error_code> completion_error;
            try {
                (void)response.stream().complete();
            } catch (const ruvia::http_client_error& error) {
                completion_error = error.code();
            }
            EXPECT_TRUE(completion_error == response_error);
            std::optional<std::error_code> read_error;
            try {
                (void)co_await response.stream().read();
            } catch (const ruvia::http_client_error& error) {
                read_error = error.code();
            }
            EXPECT_TRUE(read_error == response_error);
            co_await peer.wait();
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

// A response is complete at its declared Content-Length. Bytes beyond that
// boundary belong to no requested response: retain the complete response and
// retire the connection instead of turning the valid message into a failure.
TEST(Ruvia, http_client_preserves_fixed_length_response_before_unsolicited_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(
        io, worker.handle_, {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokunsolicited"});
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/overlong").send();
        const auto body = co_await response.stream().read_all();
        EXPECT_EQ(body.text(), std::string_view("ok"));
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, client_response_body_claims_its_shared_lane_only_when_started) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http1_chunked_response_peer peer(io, large_gzip_response_body());
    peer.start();
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/body-lane").send();
        auto collect = response.stream().read_all();
        auto read = response.stream().read();
        auto text = response.stream().text();
        bool collected = false;
        ruvia::task_scope readers(worker.handle_);
        const auto consume = [&]() -> ruvia::task<void> {
            const auto bytes_value = co_await std::move(collect);
            collected = bytes_value.size() == 128 * 1024;
        };
        readers.spawn(consume());
        bool read_rejected = false;
        bool text_rejected = false;
        bool collect_rejected = false;
        try {
            (void)co_await std::move(read);
        } catch (const std::logic_error&) {
            read_rejected = true;
        }
        try {
            (void)co_await std::move(text);
        } catch (const std::logic_error&) {
            text_rejected = true;
        }
        try {
            (void)co_await response.stream().read_all();
        } catch (const std::logic_error&) {
            collect_rejected = true;
        }
        peer.release_terminal_chunk();
        co_await readers.join();
        EXPECT_TRUE(read_rejected && text_rejected && collect_rejected);
        EXPECT_TRUE(collected);
        EXPECT_TRUE(!(co_await response.stream().text()));
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    peer.wait();
}

namespace {
void exercise_final_head_cookies(ruvia::http_client_versions protocol) {
    for (const auto policy : {ruvia::http_client_received_cookie_policy::retain_and_send,
             ruvia::http_client_received_cookie_policy::ignore}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        const bool http2 = protocol == ruvia::http_client_versions{.http1_ = false};
        cookie_stream_peer peer(io, worker.handle_, http2 ? cookie_peer_mode::http2 : cookie_peer_mode::http1);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.connection_limits_.max_open_ = http2 ? 1 : 2;
        config.versions_ = protocol;
        config.received_cookies_ = policy;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            std::optional<ruvia::http_client_response> streaming;
            streaming.emplace(co_await client.client().get("/stream").send());
            EXPECT_TRUE(!streaming->stream().complete());
            EXPECT_TRUE(streaming->protocol_version() ==
                        (http2 ? ruvia::http_protocol_version::http2 : ruvia::http_protocol_version::http11));
            EXPECT_EQ(streaming->informational_responses().size(), std::size_t{1});
            {
                auto response = co_await client.client().get("/before-abandonment").send();
                EXPECT_TRUE((co_await response.stream().read_all()).empty());
            }
            streaming.reset();
            {
                auto response = co_await client.client().get("/after-abandonment").send();
                EXPECT_TRUE((co_await response.stream().read_all()).empty());
            }
            co_await peer.wait();
            for (const auto& request : peer.requests_) {
                const bool received = request.find("session=ready") != std::string::npos;
                EXPECT_TRUE(received == (policy == ruvia::http_client_received_cookie_policy::retain_and_send));
                EXPECT_TRUE(request.find("informational=ignored") == std::string::npos);
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}
}  // namespace

TEST(Ruvia, http_client_retains_final_head_cookies_before_body_completion_and_after_abandonment) {
    exercise_final_head_cookies(ruvia::http_client_versions{});
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_client_retains_final_head_cookies_before_body_completion_and_after_abandonment) {
    exercise_final_head_cookies(ruvia::http_client_versions{.http1_ = false});
}

#endif

TEST(Ruvia, http1_slow_body_completion_does_not_roll_back_newer_final_head_cookies) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    cookie_stream_peer peer(io, worker.handle_, cookie_peer_mode::http1_replacement);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto slow = co_await client.client().get("/scope/stream").send();
        EXPECT_TRUE(!slow.stream().complete());
        {
            auto update = co_await client.client().get("/scope/update").send();
            EXPECT_TRUE((co_await update.stream().read_all()).empty());
        }
        // The gate opens only after the newer final head has updated the jar.
        peer.release_terminal_body();
        const auto body = co_await slow.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), std::string_view("x"));
        {
            auto check = co_await client.client().get("/scope/check").send();
            EXPECT_TRUE((co_await check.stream().read_all()).empty());
        }
        co_await peer.wait();
        EXPECT_TRUE(peer.requests_[0].find("sid=old") != std::string::npos);
        EXPECT_TRUE(peer.requests_[1].find("sid=new") != std::string::npos);
        EXPECT_TRUE(peer.requests_[1].find("sid=old") == std::string::npos);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 6265 section 5.3 step 11: a Domain cookie replaces the host-only cookie
// with the same name and path, since a host-only cookie's domain is the host.
TEST(Ruvia, http_client_replaces_host_only_cookie_with_same_named_domain_cookie) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\nSet-Cookie: sid=host; Path=/\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n"
            "Set-Cookie: sid=domain; Domain=127.0.0.1; Path=/\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    // Match the IPv4 peer directly without DNS or IPv6 connection fallback.
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* target : {"/host", "/domain", "/check"}) {
            auto response = co_await client.client().get(target).send();
            EXPECT_TRUE((co_await response.stream().read_all()).empty());
            EXPECT_TRUE(response.stream().complete());
        }
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{3});
        if (peer.requests_.size() == 3) {
            EXPECT_TRUE(peer.requests_[1].find("sid=host") != std::string::npos);
            EXPECT_TRUE(peer.requests_[2].find("sid=domain") != std::string::npos);
            EXPECT_TRUE(peer.requests_[2].find("sid=host") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 6265 section 5.2: nameless cookies are ignored; "Domain=." leaves a
// host-only cookie, an invalid Path selects the default-path, and stored values
// keep their internal whitespace on the way back.
TEST(Ruvia, http_client_jar_follows_user_agent_set_cookie_algorithm) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n"
         "Set-Cookie: nameless\r\nSet-Cookie: =emptyname\r\n"
         "Set-Cookie: f=hello world\r\nSet-Cookie: h=1; Domain=.\r\n"
         "Set-Cookie: p=2; Path=/caf\xc3\xa9\r\nSet-Cookie: q=3; Path=relative\r\n"
         "Set-Cookie: d=4; Domain=127.0.0.1; Domain=\r\n"
         "Set-Cookie: part=insecure; Path=/; Partitioned\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* target : {"/set", "/check"}) {
            auto response = co_await client.client().get(target).send();
            EXPECT_TRUE((co_await response.stream().read_all()).empty());
        }
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            const auto& check = peer.requests_[1];
            EXPECT_TRUE(check.find("=nameless") == std::string::npos);
            EXPECT_TRUE(check.find("=emptyname") == std::string::npos);
            EXPECT_TRUE(check.find("f=hello world") != std::string::npos);
            EXPECT_TRUE(check.find("h=1") != std::string::npos);
            EXPECT_TRUE(check.find("q=3") != std::string::npos);
            EXPECT_TRUE(check.find("d=4") != std::string::npos);
            // Path=/café is stored as given and does not match "/check".
            EXPECT_TRUE(check.find("p=2") == std::string::npos);
            EXPECT_TRUE(check.find("part=insecure") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_merges_explicit_cookie_fields_without_losing_other_headers) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string invalid_overridden_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto invalid_overridden = local_http_client_config(invalid_overridden_url);
    invalid_overridden.cookies_ = {{"sid", "bad value"}, {"sid", "stored"}};
    bool rejected_invalid_overridden = false;
    try {
        ruvia::http_client rejected(worker.attachment_.loop(), invalid_overridden);
    } catch (const std::invalid_argument&) {
        rejected_invalid_overridden = true;
    }
    EXPECT_TRUE(rejected_invalid_overridden);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_cookies_ = 2;
    config.max_cookie_bytes_ = 16;
    config.cookies_ = {{"sid", "obsolete-long-value"}, {"mode", "1"}, {"sid", "stored"}};
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        const std::array fields{ruvia::http_header_view{"Cookie", "a=1"},
            ruvia::http_header_view{"X-Test", "present"},
            ruvia::http_header_view{"cOoKiE", "b=2"}};
        auto response = co_await client.client().get("/").headers(fields).send();
        EXPECT_TRUE((co_await response.stream().read_all()).empty());
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{1});
        if (!peer.requests_.empty()) {
            const std::string_view request = peer.requests_.front();
            std::size_t cookie_count = 0;
            bool merged = false;
            bool other_header = false;
            auto line_start = request.find("\r\n") + 2;
            while (line_start < request.size()) {
                const auto line_end = request.find("\r\n", line_start);
                if (line_end == std::string_view::npos || line_end == line_start) {
                    break;
                }
                const auto line = std::string_view(request).substr(line_start, line_end - line_start);
                const auto colon = line.find(':');
                if (colon != std::string_view::npos) {
                    const auto name = line.substr(0, colon);
                    const auto value = line.substr(colon + 1);
                    if (ruvia::http_ascii_equals_ignore_case(name, "cookie")) {
                        ++cookie_count;
                        merged = value == " a=1; b=2; sid=stored; mode=1";
                    } else if (ruvia::http_ascii_equals_ignore_case(name, "x-test")) {
                        other_header = value == " present";
                    }
                }
                line_start = line_end + 2;
            }
            EXPECT_EQ(cookie_count, std::size_t{1});
            EXPECT_TRUE(merged);
            EXPECT_TRUE(other_header);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 section 9.3.8: stored cookies are sensitive and stay out of TRACE.
TEST(Ruvia, http_client_omits_stored_cookies_from_trace) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.cookies_ = {{"sid", "secret"}};
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* method : {"TRACE", "GET"}) {
            auto response = co_await client.client().request(method, "/").send();
            EXPECT_TRUE((co_await response.stream().read_all()).empty());
        }
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            EXPECT_TRUE(peer.requests_[0].find("sid=secret") == std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("sid=secret") != std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 sections 14 and 15.3.7: a range of an encoded representation is not
// decodable alone, so a 206 body is delivered as received.
TEST(Ruvia, http_client_passes_encoded_partial_content_through) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string gzip_prefix("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 206 Partial Content\r\nContent-Encoding: gzip\r\nContent-Range: bytes 0-9/100\r\n"
         "Content-Length: 10\r\nConnection: close\r\n\r\n" +
            gzip_prefix});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/range").send();
        EXPECT_EQ(response.status().value(), std::uint16_t{206});
        const auto bytes_value = co_await response.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()),
            std::string_view(gzip_prefix));
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 §8.4.1 / §12.5.3: content codings are an open registry and a request
// without Accept-Encoding accepts any of them. A stack the client cannot fully
// decode is delivered as received, with Content-Encoding kept.
TEST(Ruvia, http_client_passes_unsupported_content_codings_through) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string opaque("\x01opaque\xff", 8);
    scripted_head_peer peer(io, worker.handle_,
        {std::string("HTTP/1.1 200 OK\r\nContent-Encoding: x-custom\r\nContent-Length: 8\r\nConnection: close\r\n\r\n") + opaque,
            std::string("HTTP/1.1 200 OK\r\nContent-Encoding: gzip, compress\r\nContent-Length: 8\r\nConnection: close\r\n\r\n") +
                opaque});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const std::string_view coding : {std::string_view("x-custom"), std::string_view("gzip, compress")}) {
            auto response = co_await client.client().get("/coded").send();
            EXPECT_EQ(response.status().value(), std::uint16_t{200});
            EXPECT_TRUE(response.header("Content-Encoding") == std::optional<std::string_view>(coding));
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(bytes_value.text(), std::string_view(opaque));
        }
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_strips_decoded_content_encoding_and_rejects_overdeep_known_stacks) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const auto gz_hello = *ruvia::encode_http_content(
        ruvia::http_content_coding::gzip, "hello", {.max_encoded_bytes_ = 1024});
    scripted_head_peer peer(io, worker.handle_,
        {std::move(std::string("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: ").append(std::to_string(gz_hello.size()))) + "\r\nConnection: close\r\n\r\n" + std::string(gz_hello),
            std::move(std::string("HTTP/1.1 200 OK\r\nContent-Encoding: identity, identity, identity, identity, identity, "
                                  "identity, identity, identity, gzip\r\nContent-Length: ")
                    .append(std::to_string(gz_hello.size()))) +
                "\r\nConnection: close\r\n\r\n" + std::string(gz_hello),
            std::move(std::string("HTTP/1.1 200 OK\r\nContent-Encoding: gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip\r\n"
                                  "Content-Length: ")
                    .append(std::to_string(gz_hello.size()))) +
                "\r\nConnection: close\r\n\r\n" + std::string(gz_hello)});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (int i = 0; i < 2; ++i) {
            auto response = co_await client.client().get("/decoded").send();
            EXPECT_EQ(response.status().value(), std::uint16_t{200});
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(bytes_value.text(), std::string_view("hello"));
            EXPECT_TRUE(!response.header("Content-Encoding").has_value());
            EXPECT_TRUE(!response.header("Content-Length").has_value());
        }
        bool rejected_overdeep = false;
        try {
            auto response = co_await client.client().get("/overdeep").send();
            (void)co_await response.stream().read_all();
        } catch (const ruvia::http_client_error& error) {
            rejected_overdeep = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_error);
        }
        EXPECT_TRUE(rejected_overdeep);
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_preserves_large_unsigned_timeout_bounds) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"first", "second"}, std::chrono::milliseconds(10));
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
    auto config = local_http_client_config(config_url);
    const auto large = std::chrono::duration<std::uint64_t, std::milli>{std::numeric_limits<std::uint64_t>::max()};
    config.connect_timeout_ = large;
    config.write_timeout_ = large;
    config.read_timeout_ = large;
    config.request_timeout_ = large;
    config.acquire_timeout_ = large;
    config.idle_timeout_ = large;
    config.http2_ping_interval_ = large;
    config.push_.timeout_ = large;
    ruvia::http_client client(worker.attachment_.loop(), config);
    server.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto first = co_await client.client().get("/large-config").send();
        auto first_body = co_await first.stream().read_all();
        EXPECT_EQ(first_body.text(), std::string_view("first"));
        auto second = co_await client.client().with_options({.timeout_ = large}).get("/large-operation").send();
        auto second_body = co_await second.stream().read_all();
        EXPECT_EQ(second_body.text(), std::string_view("second"));
        co_await server.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_ref_options_override_pool_timeout_and_start_when_operation_runs) {
    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"slow"}, std::chrono::milliseconds(350));
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::milliseconds(50);
        ruvia::http_client client(worker.attachment_.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.client().with_options({.timeout_ = std::chrono::seconds(2)});
            auto cold = handle.get("/pool-timeout-override").send();
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            auto response = co_await std::move(cold);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            EXPECT_EQ(response.status(), ruvia::http_status_code::from_value(200));
            EXPECT_TRUE(elapsed >= std::chrono::milliseconds(50));
            EXPECT_TRUE(elapsed < std::chrono::seconds(2));
            co_await server.wait();
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"late"}, std::chrono::milliseconds(350));
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::seconds(5);
        ruvia::http_client client(worker.attachment_.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto base = client.client().with_options({.timeout_ = std::chrono::seconds(5)});
            auto shortened = base.with_options({.timeout_ = std::chrono::milliseconds(100)});
            auto extended = shortened.with_options({.timeout_ = std::chrono::seconds(5)});
            auto cold = extended.get("/successive-minimum").send();
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            bool timed_out = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::http_client_error& error) {
                timed_out = error.code() == std::make_error_code(std::errc::timed_out);
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            EXPECT_TRUE(timed_out);
            EXPECT_TRUE(elapsed >= std::chrono::milliseconds(50));
            EXPECT_TRUE(elapsed < std::chrono::seconds(2));
            // The deadline may expire before the connection reaches the peer.
            server.stop_accepting();
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // The peer's delayed response is expected to hit the timed-out socket.
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }

    for (const bool stop_base : {true, false}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"cancelled"}, std::chrono::milliseconds(350));
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        ruvia::stop_source base_stop;
        ruvia::stop_source derived_stop;
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            // Stop the in-flight exchange once the peer holds its request.
            server.on_request([&base_stop, &derived_stop, stop_base] {
                (stop_base ? base_stop : derived_stop).request_stop();
            });
            ruvia::stop_group cancellation{base_stop.token(), derived_stop.token()};
            auto base = client.client().with_options({.stop_token_ = base_stop.token()});
            auto copied = base;
            auto derived = copied.with_options({.stop_token_ = cancellation.token()});
            bool cancelled = false;
            try {
                (void)co_await derived.get(stop_base ? "/stop-base" : "/stop-derived").send();
            } catch (const ruvia::http_client_error& error) {
                cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
            }
            EXPECT_TRUE(cancelled);
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // The peer's delayed response is expected to hit the cancelled socket.
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"retiring"}, std::chrono::milliseconds(350));
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            server.on_request([&worker] { worker.attachment_.stop(); });
            bool cancelled = false;
            try {
                (void)co_await client.client().get("/event-loop-stop").send();
            } catch (const ruvia::http_client_error& error) {
                cancelled = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing);
            }
            EXPECT_TRUE(cancelled);
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Retirement closes the in-flight TCP exchange.
            }
        };
        run_operation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"closing"}, std::chrono::milliseconds(350));
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            server.on_request([&client] { client.close(); });
            auto cold = client.client().get("/active-close").send();
            bool closing = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::http_client_error& error) {
                closing = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing);
            }
            EXPECT_TRUE(closing);
            co_await client.shutdown();
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Pool shutdown closes the in-flight TCP exchange before its delayed reply.
            }
        };
        run_operation(worker, io, operation);
    }
}

TEST(Ruvia, http1_full_response_queue_cancellation_and_deadline_finish_without_reading) {
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(io, worker.handle_, {std::string(65536, 'q')});
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto config = local_http_client_config(config_url);
        config.max_response_bytes_ = 1024;
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::stop_source stop;
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.client().with_options({
                .timeout_ = deadline ? std::optional(std::chrono::milliseconds(40)) : std::nullopt,
                .stop_token_ = stop.token(),
            });
            auto response = co_await handle.get("/backpressure").send();
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
            // Keep the response alive without reading or abandoning it. The
            // producer must leave its full-queue wait using the terminal event.
            if (deadline) {
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(80));
            } else {
                stop.request_stop();
            }
            // Ordered worker turns drain cancellation publication and the
            // producer's scheduled wake, without body-reader side effects.
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{0});
            co_await client.shutdown();
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Cancellation can race the peer's final socket write.
            }
        };
        run_operation(worker, io, operation);
    }
}

TEST(Ruvia, http1_full_response_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {std::string(65536, 'q')});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    server.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/backpressure-stop").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        worker.attachment_.stop();
        // The response remains alive across shutdown; its destructor/read()
        // cannot be the event that wakes the producer.
        co_await client.shutdown();
        joined = true;
        try {
            co_await server.wait();
        } catch (const std::system_error&) {
        }
    };
    run_operation(worker, io, operation);
    EXPECT_TRUE(joined);
    EXPECT_TRUE(!worker.handle_.valid());
}

TEST(Ruvia, http1_transfer_gzip_full_queue_cancel_and_deadline_without_reading) {
    const auto encoded = large_gzip_response_body();
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        http1_chunked_response_peer peer(
            io, encoded, http1_chunked_response_peer::end_mode_type::wait_for_client_close);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.max_response_bytes_ = 1024;
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::stop_source stop;
        peer.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.client().with_options({
                .timeout_ = deadline ? std::optional(std::chrono::milliseconds(200)) : std::nullopt,
                .stop_token_ = stop.token(),
            });
            auto response = co_await handle.get("/transfer-gzip-backpressure").send();
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            EXPECT_TRUE(client.client().stats().plaintext_bytes_received_ >= encoded.size());

            if (deadline) {
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(300));
            } else {
                stop.request_stop();
            }
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{0});
            // Keep response alive through join. No body read or response
            // destruction may be what releases the producer's full queue.
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        peer.wait();
        EXPECT_TRUE(peer.observed_client_close());
    }
}

TEST(Ruvia, http1_transfer_gzip_full_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const auto encoded = large_gzip_response_body();
    http1_chunked_response_peer peer(
        io, encoded, http1_chunked_response_peer::end_mode_type::wait_for_client_close);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/transfer-gzip-stop").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::async_asio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
            });
        }
        EXPECT_TRUE(client.client().stats().plaintext_bytes_received_ >= encoded.size());
        worker.attachment_.stop();
        co_await client.shutdown();
        joined = true;
    };
    run_operation(worker, io, operation);
    peer.wait();
    EXPECT_TRUE(joined);
    EXPECT_TRUE(peer.observed_client_close());
    EXPECT_TRUE(!worker.handle_.valid());
}

TEST(Ruvia, http1_transfer_gzip_chunked_streams_before_terminal_chunk_and_bounds_each_decode) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected(128 * 1024, 'z');
    auto encoded_result = ruvia::encode_http_content(ruvia::http_content_coding::gzip, expected,
        {.max_encoded_bytes_ = expected.size()});
    EXPECT_TRUE(encoded_result.has_value());
    if (!encoded_result.has_value()) {
        return;
    }
    http1_chunked_response_peer peer(io, std::string(encoded_result->data(), encoded_result->size()));
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();

    std::size_t decoded_bytes = 0;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/transfer-gzip").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});

        const auto first = co_await response.stream().read();
        EXPECT_TRUE(first.has_value());
        EXPECT_TRUE(first->size() <= config.max_response_bytes_);
        EXPECT_TRUE(!response.stream().complete());
        const auto first_view = std::string_view(
            reinterpret_cast<const char*>(first->data()), first->size());
        decoded_bytes += first->size();

        // Let the producer decode already-buffered compressed bytes while the
        // consumer's borrowed view remains live; only pending storage may grow.
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::async_asio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
            });
        }
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(first->data()), first->size()),
            first_view);
        peer.release_terminal_chunk();

        while (const auto chunk = co_await response.stream().read()) {
            EXPECT_TRUE(chunk->size() <= config.max_response_bytes_);
            decoded_bytes += chunk->size();
        }
        EXPECT_TRUE(response.stream().complete());
        EXPECT_EQ(response.trailer("x-end"), std::optional<std::string_view>("retained"));
        EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    peer.wait();
    EXPECT_EQ(decoded_bytes, expected.size());
    EXPECT_TRUE(decoded_bytes > config.max_response_bytes_);
}

TEST(Ruvia, http1_response_storage_remains_worker_owned_after_client_destruction) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected(128 * 1024, 'z');
    const auto encoded = large_gzip_response_body();
    const std::string long_header(32 * 1024, 'h');
    http1_chunked_response_peer peer(io, encoded,
        http1_chunked_response_peer::end_mode_type::terminal_gate, long_header);
    peer.start();

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        std::optional<ruvia::http_client_response_bytes> retained_body;
        std::string collected{};
        {
            const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
            ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
            response.emplace(co_await client.client().get("/retained-response").send());
            const auto first = co_await response->stream().read();
            EXPECT_TRUE(first.has_value());
            if (first) {
                collected.append(reinterpret_cast<const char*>(first->data()), first->size());
            }
            peer.release_terminal_chunk();
            retained_body.emplace(co_await response->stream().read_all());
            co_await client.shutdown();
        }

        const auto headers = response->headers();
        const auto long_value = response->header("x-long");
        EXPECT_TRUE(long_value.has_value());
        EXPECT_EQ(long_value->size(), long_header.size());
        EXPECT_TRUE(std::ranges::all_of(*long_value, [](char value) { return value == 'h'; }));
        EXPECT_TRUE(!headers.empty());
        const auto text = co_await response->stream().text();
        EXPECT_TRUE(!text.has_value());
        EXPECT_TRUE(!(co_await response->stream().read()).has_value());
        const auto remaining = co_await response->stream().read_all();
        EXPECT_TRUE(remaining.empty());
        const auto retained_view = retained_body->bytes();
        collected.append(reinterpret_cast<const char*>(retained_view.data()), retained_view.size());
        EXPECT_EQ(collected, expected);
        EXPECT_EQ(response->trailer("x-end"), std::optional<std::string_view>("retained"));
        response.reset();
        EXPECT_EQ(retained_body->bytes().size(), retained_view.size());
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(retained_body->bytes().data()),
                      retained_body->bytes().size()),
            std::string_view(expected).substr(expected.size() - retained_view.size()));
        retained_body.reset();
    };
    run_operation(worker, io, operation);
    peer.wait();
}

TEST(Ruvia, client_response_cold_collection_survives_client_and_expires_with_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"cold-body"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto client = std::make_unique<ruvia::http_client>(
            worker.attachment_.loop(), local_http_client_config(client_url));
        std::optional<ruvia::http_client_response> response;
        response.emplace(co_await client->client().get("/cold-body").send());
        auto cold = response->stream().read_all();
        co_await client->shutdown();
        client.reset();

        auto retained = co_await std::move(cold);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(retained.bytes().data()),
                      retained.bytes().size()),
            std::string_view("cold-body"));
        auto expired = response->stream().read_all();
        response.reset();
        bool rejected = false;
        try {
            (void)co_await std::move(expired);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        co_await server.wait();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_sends_heads_beyond_the_retained_buffer_and_rejects_oversized_heads) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"large-head", "small-head"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        const auto body_text = [](const ruvia::http_client_response_bytes& bytes_value) {
            return std::string(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.bytes().size());
        };
        // Larger than the head buffer a connection retains, within the limit.
        const std::string large_value(20 * 1024, 'v');
        const std::array large_headers{ruvia::http_header_view{"X-Large", large_value}};
        auto large = co_await client.client().get("/large").headers(large_headers).send();
        EXPECT_TRUE(large.status() == ruvia::http_status::ok);
        EXPECT_EQ(body_text(co_await large.stream().read_all()), std::string("large-head"));
        auto small = co_await client.client().get("/small").send();
        EXPECT_TRUE(small.status() == ruvia::http_status::ok);
        EXPECT_EQ(body_text(co_await small.stream().read_all()), std::string("small-head"));
        co_await server.wait();

        const std::string oversized_value(ruvia::max_http_header_bytes + 2048, 'o');
        const std::array oversized_headers{ruvia::http_header_view{"X-Oversized", oversized_value}};
        std::optional<std::error_code> rejected;
        try {
            (void)co_await client.client().get("/oversized").headers(oversized_headers).send();
        } catch (const ruvia::http_client_error& error) {
            rejected = error.code();
        }
        EXPECT_TRUE(rejected == ruvia::make_error_code(ruvia::http_client_errc::invalid_request));
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_rejects_connection_options_naming_request_origin_metadata) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"hop-by-hop"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        const std::array hop_by_hop{ruvia::http_header_view{"Connection", "X-Hop, Proxy-Authorization"},
            ruvia::http_header_view{"X-Hop", "1"}};
        auto accepted = co_await client.client().get("/hop").headers(hop_by_hop).send();
        EXPECT_TRUE(accepted.status() == ruvia::http_status::ok);
        (void)co_await accepted.stream().read_all();
        co_await server.wait();

        for (const std::string_view nominated : {"Forwarded", "X-Forwarded-For", "X-Forwarded-Proto", "X-Forwarded-Host"}) {
            const std::array headers{ruvia::http_header_view{"Connection", nominated}};
            std::optional<std::error_code> rejected;
            try {
                (void)co_await client.client().get("/origin").headers(headers).send();
            } catch (const ruvia::http_client_error& error) {
                rejected = error.code();
            }
            EXPECT_TRUE(rejected == ruvia::make_error_code(ruvia::http_client_errc::invalid_request));
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, client_shutdown_keeps_incomplete_response_error_observable) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http1_chunked_response_peer peer(io, large_gzip_response_body(),
        http1_chunked_response_peer::end_mode_type::wait_for_client_close);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        bool running_read_failed = false;
        {
            ruvia::http_client client(worker.attachment_.loop(), config);
            response.emplace(co_await client.client().get("/incomplete-after-close").send());
            asio::steady_timer close_timer(io);
            close_timer.expires_after(std::chrono::milliseconds(50));
            close_timer.async_wait([&client](const std::error_code& error) {
                if (!error) {
                    client.close();
                }
            });
            try {
                (void)co_await response->stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                running_read_failed = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing) ||
                                      error.code() == std::make_error_code(std::errc::operation_canceled) ||
                                      error.code() == ruvia::make_error_code(ruvia::http_client_errc::io_error);
            }
            EXPECT_TRUE(running_read_failed);
            co_await client.shutdown();
        }
        bool retained_response_failed = false;
        try {
            (void)co_await response->stream().read_all();
        } catch (const ruvia::http_client_error& error) {
            retained_response_failed = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing) ||
                                       error.code() == std::make_error_code(std::errc::operation_canceled) ||
                                       error.code() == ruvia::make_error_code(ruvia::http_client_errc::io_error);
        }
        EXPECT_TRUE(retained_response_failed);
        std::optional<std::error_code> read_error;
        try {
            while (co_await response->stream().read()) {
            }
        } catch (const ruvia::http_client_error& error) {
            read_error = error.code();
        }
        EXPECT_TRUE(read_error.has_value());
        std::optional<std::error_code> completion_error;
        try {
            (void)response->stream().complete();
        } catch (const ruvia::http_client_error& error) {
            completion_error = error.code();
        }
        EXPECT_TRUE(completion_error == read_error);
        response.reset();
    };
    run_operation(worker, io, operation);
    peer.wait();
    EXPECT_TRUE(peer.observed_client_close());
}

TEST(Ruvia, client_body_result_uses_pool_budget_after_client_and_worker_teardown) {
    std::optional<ruvia::http_client_response_bytes> retained;
    std::string expected(
        "\0\xff"
        "data",
        6);
    {
        auto& io = ruvia::test::new_test_io_context();
        {
            test_worker worker(io);
            asio::ip::tcp::acceptor acceptor(
                io, {asio::ip::tcp::v4(), std::uint16_t{0}});
            asio::ip::tcp::socket server_socket(io);
            ruvia::worker_signal server_done(worker.handle_);
            std::exception_ptr server_failure;
            const std::string response_wire =
                "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n" +
                expected;
            acceptor.async_accept(server_socket, [&](const std::error_code& error) {
                if (error) {
                    server_failure = std::make_exception_ptr(std::system_error(error));
                    server_done.notify();
                    return;
                }
                asio::async_write(server_socket, asio::buffer(response_wire),
                    [&](const std::error_code& write_error, std::size_t) {
                        if (write_error) {
                            server_failure = std::make_exception_ptr(std::system_error(write_error));
                        }
                        server_done.notify();
                    });
            });
            {
                const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
                ruvia::http_client client(worker.attachment_.loop(),
                    {.base_url_ = client_url},
                    {.result_budget_ = {.max_retained_bytes_ = expected.size()}});
                auto operation = [&]() -> ruvia::task<void> {
                    auto send = client.client().get("/").send();
                    auto response = co_await std::move(send);
                    auto bytes_value = co_await response.stream().read_all();
                    retained.emplace(std::move(bytes_value));
                    co_await server_done.wait();
                    if (server_failure != nullptr) {
                        std::rethrow_exception(server_failure);
                    }
                    EXPECT_EQ(retained->bytes().size(), expected.size());
                    co_await client.shutdown();
                };
                run_operation(worker, io, operation);
            }
        }
    }

    std::atomic_bool contents_matched{false};
    std::thread destroyer([bytes = std::move(*retained), expected = std::move(expected),
                              &contents_matched]() {
        const auto actual = bytes.bytes();
        const auto wanted = std::as_bytes(std::span(expected.data(), expected.size()));
        contents_matched.store(actual.size() == wanted.size() &&
                                   std::equal(actual.begin(), actual.end(), wanted.begin()),
            std::memory_order_release);
    });
    destroyer.join();
    EXPECT_TRUE(contents_matched.load(std::memory_order_acquire));
}

TEST(Ruvia, client_result_budget_rejects_zero_limit_at_client_startup) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    bool rejected_at_client_startup = false;
    try {
        ruvia::http_client client(worker.attachment_.loop(), {.base_url_ = "http://127.0.0.1"},
            {.result_budget_ = {.max_retained_bytes_ = 0}});
    } catch (const std::invalid_argument&) {
        rejected_at_client_startup = true;
    }
    EXPECT_TRUE(rejected_at_client_startup);
}

TEST(Ruvia, http_client_connect_failure_message_is_utf8) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    // Bound but not listening: connections are refused, and no other process
    // can claim the port meanwhile.
    asio::ip::tcp::acceptor unavailable(io);
    unavailable.open(asio::ip::tcp::v4());
    unavailable.bind({asio::ip::tcp::v4(), 0});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(unavailable.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    // Windows retries a refused loopback connect for about two seconds.
    config.connect_timeout_ = std::chrono::seconds(20);
    config.acquire_timeout_ = std::chrono::seconds(20);
    config.request_timeout_ = std::chrono::seconds(20);
    ruvia::http_client client(worker.attachment_.loop(), config);
    std::optional<std::error_code> code;
    std::string message{};
    auto operation = [&]() -> ruvia::task<void> {
        try {
            (void)co_await client.client().get("/").send();
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
            message = error.what();
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_TRUE(code == ruvia::make_error_code(ruvia::http_client_errc::connect_failed));
    EXPECT_TRUE(!message.empty());
    // The OS describes the refusal in the user's language; the text must be
    // UTF-8 whatever the system code page is, so it can travel in a JSON or
    // text/plain; charset=UTF-8 body. The strict JSON parser rejects ill-formed
    // UTF-8, and the message holds no characters that need escaping.
    EXPECT_TRUE(message.find_first_of("\"\\") == std::string::npos);
    const auto decoded = ruvia::from_json<ruvia::string>(std::string("\"") + message + "\"");
    EXPECT_EQ(decoded.view(), std::string_view(message));
}

TEST(Ruvia, http_client_upload_exchange_owns_chunks_and_trailers_and_drives_continue) {
    for (const auto mode : {upload_peer::mode_type::chunked, upload_peer::mode_type::known_length,
             upload_peer::mode_type::continue_value, upload_peer::mode_type::continue_timeout}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        upload_peer peer(io, worker.handle_, mode);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            const bool known = mode == upload_peer::mode_type::known_length;
            const bool expect_continue = mode == upload_peer::mode_type::continue_value || mode == upload_peer::mode_type::continue_timeout;
            auto exchange_value = co_await client.client().request("POST", "/upload").upload({.content_length_ = known ? std::optional<ruvia::http_content_length>{6} : std::nullopt, .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none, .max_chunk_bytes_ = 3, .continue_timeout_ = std::chrono::milliseconds(20)});
            {
                auto cold = exchange_value.body().write("bad");
            }
            {
                auto cold = exchange_value.response();
            }
            bool too_large = false;
            try {
                auto rejected = exchange_value.body().write("four");
            } catch (const std::length_error&) {
                too_large = true;
            }
            EXPECT_TRUE(too_large);
            std::string input{"abc"};
            auto first = exchange_value.body().write(input);
            auto second = exchange_value.body().write("def");
            input.assign("xxx");
            std::string trailer_value{"retained"};
            const std::array<ruvia::http_header_view, 1> fields_value{{{"X-End", trailer_value}}};
            auto ending = exchange_value.body().end(known ? std::span<const ruvia::http_header_view>{} : std::span<const ruvia::http_header_view>(fields_value));
            trailer_value.assign("changed");
            auto receiving = exchange_value.response();
            auto receiving_overlap = exchange_value.response();
            auto receiving_again = exchange_value.response();
            std::optional<ruvia::http_client_response> received;
            bool response_overlap_rejected = false;
            ruvia::task_scope receivers(worker.handle_);
            const auto receive = [&]() -> ruvia::task<void> {
                received.emplace(co_await std::move(receiving));
            };
            const auto reject_response_overlap = [&]() -> ruvia::task<void> {
                try {
                    auto duplicate = co_await std::move(receiving_overlap);
                } catch (const std::logic_error&) {
                    response_overlap_rejected = true;
                }
            };
            receivers.spawn(receive());
            receivers.spawn(reject_response_overlap());
            ruvia::task_scope writers(worker.handle_);
            const auto write_first = [&]() -> ruvia::task<void> { co_await std::move(first); };
            writers.spawn(write_first());
            bool write_overlap_rejected = false;
            bool end_overlap_rejected = false;
            try {
                co_await exchange_value.body().write("bad");
            } catch (const std::logic_error&) {
                write_overlap_rejected = true;
            }
            try {
                co_await exchange_value.body().end();
            } catch (const std::logic_error&) {
                end_overlap_rejected = true;
            }
            co_await writers.join();
            EXPECT_TRUE(write_overlap_rejected && end_overlap_rejected);
            auto moved = std::move(exchange_value);
            co_await std::move(second);
            co_await std::move(ending);
            EXPECT_TRUE(moved.body().complete());
            co_await receivers.join();
            EXPECT_TRUE(response_overlap_rejected);
            bool transferred_twice = false;
            try {
                auto duplicate = co_await std::move(receiving_again);
            } catch (const std::logic_error&) {
                transferred_twice = true;
            }
            EXPECT_TRUE(transferred_twice);
            auto& response = *received;
            auto bytes_value = co_await response.stream().read_all(16);
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
            const auto informational = response.informational_responses();
            EXPECT_EQ(informational.size(), mode == upload_peer::mode_type::continue_value
                                                ? upload_peer::early_hint_count + 1
                                                : std::size_t{0});
            if (mode == upload_peer::mode_type::continue_value &&
                informational.size() == upload_peer::early_hint_count + 1) {
                for (std::size_t i = 0; i < upload_peer::early_hint_count; ++i) {
                    EXPECT_EQ(informational[i].status().value(), std::uint16_t{103});
                    const auto fields = informational[i].headers();
                    EXPECT_EQ(fields.size(), std::size_t{1});
                    if (fields.size() == 1) {
                        EXPECT_EQ(fields[0].value(), "</asset>; rel=preload");
                    }
                }
                EXPECT_EQ(informational.back().status().value(), std::uint16_t{100});
            }
            co_await peer.wait();
            EXPECT_EQ(peer.body_, "abcdef");
            EXPECT_TRUE(peer.trailers_.field("x-end") == (known ? std::nullopt : std::optional<std::string_view>("retained")));
            EXPECT_TRUE((peer.head_.find("Expect: 100-continue") != std::string::npos) == expect_continue);
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

TEST(Ruvia, http1_small_upload_completes_with_a_bounded_response_budget) {
    for (const auto mode : {upload_peer::mode_type::known_length, upload_peer::mode_type::chunked}) {
        const bool known = mode == upload_peer::mode_type::known_length;
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        upload_peer peer(io, worker.handle_, mode);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.max_response_bytes_ = 1024;
        // Completing a six-byte upload (with or without trailers) must not
        // require a header-section-sized buffer from the response budget.
        ruvia::http_client client(worker.attachment_.loop(), config,
            {.result_budget_ = {.max_retained_bytes_ = 4096, .max_in_flight_bytes_ = 32 * 1024}});
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            std::exception_ptr failure;
            try {
                auto exchange = co_await client.client().post("/upload").upload(
                    {.content_length_ = known ? std::optional<ruvia::http_content_length>{6} : std::nullopt,
                        .max_chunk_bytes_ = 3});
                co_await exchange.body().write("abc");
                co_await exchange.body().write("def");
                const std::array<ruvia::http_header_view, 1> trailers{{{"X-End", "retained"}}};
                co_await exchange.body().end(known ? std::span<const ruvia::http_header_view>{}
                                                   : std::span<const ruvia::http_header_view>(trailers));
                EXPECT_TRUE(exchange.body().complete());
                auto response = co_await exchange.response();
                EXPECT_TRUE(response.status() == ruvia::http_status::ok);
                auto bytes = co_await response.stream().read_all(16);
                EXPECT_EQ(
                    std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), "ok");
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (failure) {
                peer.stop();
            }
            try {
                co_await peer.wait();
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
            if (failure) {
                std::rethrow_exception(failure);
            }
            EXPECT_EQ(peer.body_, "abcdef");
            EXPECT_TRUE(peer.trailers_.field("x-end") == (known ? std::nullopt : std::optional<std::string_view>("retained")));
        };
        run_operation(worker, io, operation);
    }
}

TEST(Ruvia, http_client_tunnel_claims_output_when_started_and_retires_after_abort) {
    for (const bool aborting : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::tcp::v4(), 0});
        ruvia::worker_signal peer_done(worker.handle_);
        std::exception_ptr peer_failure;
        std::string received{};
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf head;
            co_await asio::async_read_until(socket, head, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view accepted = "HTTP/1.1 200 Connection Established\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(accepted), asio::use_awaitable);
            if (!aborting) {
                co_await asio::async_read(socket, asio::dynamic_buffer(received), asio::transfer_exactly(6), asio::use_awaitable);
                co_await asio::async_write(socket, asio::buffer(received), asio::use_awaitable);
            }
            std::array<char, 64> discarded;
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(discarded), asio::redirect_error(asio::use_awaitable, error))) {
            }
            if (error != asio::error::eof && error != asio::error::connection_reset) {
                throw std::system_error(error);
            }
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            auto result = co_await client.client().connect("target.test:443").open();
            auto* tunnel = result.tunnel();
            EXPECT_TRUE(tunnel != nullptr);
            if (tunnel == nullptr) {
                co_return;
            }
            unsigned datagram_adapters_rejected = 0;
            try {
                auto datagrams = std::move(*tunnel).datagrams();
                static_cast<void>(datagrams);
            } catch (const std::logic_error&) {
                ++datagram_adapters_rejected;
            }
            try {
                auto udp = std::move(*tunnel).udp();
                static_cast<void>(udp);
            } catch (const std::logic_error&) {
                ++datagram_adapters_rejected;
            }
            EXPECT_EQ(datagram_adapters_rejected, 2U);
            EXPECT_TRUE(tunnel->status() == ruvia::http_status::ok);
            std::string input{"abc"};
            auto first = tunnel->write(input);
            auto second = tunnel->write("def");
            auto ending = tunnel->finish();
            auto reading = tunnel->read();
            auto reading_again = tunnel->read();
            input.assign("changed");
            bool first_cancelled = false;
            ruvia::task_scope writers(worker.handle_);
            const auto send_first = [&]() -> ruvia::task<void> {
                try {
                    co_await std::move(first);
                } catch (const ruvia::http_client_error&) {
                    first_cancelled = true;
                }
            };
            writers.spawn(send_first());
            if (aborting) {
                tunnel->abort();
                co_await writers.join();
                EXPECT_TRUE(first_cancelled);
                unsigned cancelled = 0;
                try {
                    co_await std::move(second);
                } catch (const ruvia::http_client_error&) {
                    ++cancelled;
                }
                try {
                    co_await std::move(ending);
                } catch (const ruvia::http_client_error&) {
                    ++cancelled;
                }
                EXPECT_EQ(cancelled, 2U);
            } else {
                unsigned rejected = 0;
                try {
                    co_await tunnel->write("bad");
                } catch (const std::logic_error&) {
                    ++rejected;
                }
                try {
                    co_await tunnel->finish();
                } catch (const std::logic_error&) {
                    ++rejected;
                }
                co_await writers.join();
                EXPECT_EQ(rejected, 2U);
                EXPECT_TRUE(!first_cancelled);
                co_await std::move(second);
                co_await std::move(ending);
                std::string echoed{};
                if (const auto bytes_value = co_await std::move(reading)) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                if (const auto bytes_value = co_await std::move(reading_again)) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                while (const auto bytes_value = co_await tunnel->read()) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                EXPECT_EQ(echoed, "abcdef");
                co_await tunnel->finish();
            }
            co_await peer_done.wait();
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        if (!aborting) {
            EXPECT_EQ(received, "abcdef");
        }
    }
}

TEST(Ruvia, http_client_upload_exchange_preserves_early_final_response_and_stops_upload) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    upload_peer peer(io, worker.handle_, upload_peer::mode_type::early_final);
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    std::optional<ruvia::http_client> client(std::in_place, worker.attachment_.loop(), local_http_client_config(client_url));
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto exchange_value = co_await client->client().request("POST", "/upload").upload({.expectation_ = ruvia::http_client_request_expectation::continue_value});
        auto response = co_await exchange_value.response();
        EXPECT_EQ(response.status().value(), std::uint16_t{413});
        bool cancelled = false;
        try {
            co_await exchange_value.body().write("rejected");
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(cancelled);
        auto bytes_value = co_await response.stream().read_all(16);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "no");
        co_await peer.wait();
        co_await client->shutdown();
        client.reset();
        // The response domain keeps the exchange's output signals alive after
        // client destruction, including the stopped, unfinished upload path.
        EXPECT_EQ(response.status().value(), std::uint16_t{413});
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "no");
    };
    run_operation(worker, io, operation);
}

namespace {
void exercise_early_response_during_upload(upload_peer::mode_type mode) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    upload_peer peer(io, worker.handle_, mode);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        constexpr std::size_t upload_size = 8 * 1024 * 1024;
        auto exchange = co_await client.client().post("/upload").upload(
            {.max_chunk_bytes_ = upload_size});
        std::string upload(upload_size, 'u');
        bool upload_failed = false;
        ruvia::task_scope writers(worker.handle_);
        const auto write = [&]() -> ruvia::task<void> {
            try {
                co_await exchange.body().write(upload);
            } catch (const ruvia::http_client_error&) {
                upload_failed = true;
            }
        };
        writers.spawn(write());
        auto response = co_await exchange.response();
        EXPECT_EQ(response.status().value(), std::uint16_t{413});
        co_await writers.join();
        EXPECT_TRUE(upload_failed);
        if (mode == upload_peer::mode_type::early_final_while_uploading) {
            // Let the transport write observe the peer close before resuming
            // response body consumption.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
        }
        std::size_t response_bytes = 0;
        while (const auto chunk = co_await response.stream().read()) {
            response_bytes += chunk->size();
            EXPECT_TRUE(std::all_of(
                chunk->begin(), chunk->end(), [](std::byte value) { return value == std::byte{'r'}; }));
        }
        EXPECT_EQ(response_bytes, upload_peer::rejected_body_size);
        EXPECT_TRUE(response.stream().complete());
        peer.release_response();
        co_await peer.wait();
        EXPECT_TRUE(client.client().stats().plaintext_bytes_sent_ >=
                    peer.head_.size() + peer.uploaded_bytes_);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}
}  // namespace

TEST(Ruvia, http_client_early_response_stops_upload_and_preserves_body) {
    exercise_early_response_during_upload(upload_peer::mode_type::early_final_orderly_while_uploading);
}

#if !defined(_WIN32)
TEST(Ruvia, http_client_upload_write_failure_preserves_complete_early_response) {
    exercise_early_response_during_upload(upload_peer::mode_type::early_final_while_uploading);
}
#endif

namespace {
void exercise_tls_response_retirement(bool join_before_reuse) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const std::string large(262163, 'b');
    const std::array<std::string_view, 2> expected{"fresh connection body", "healthy reused body"};
    unsigned connections = 0;
    unsigned requests = 0;
    bool cancelled_socket_closed = false;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(large.size()))) + "\r\n\r\n" + large;
            std::error_code error;
            co_await asio::async_write(stream, asio::buffer(response), asio::redirect_error(asio::use_awaitable, error));
            std::array<char, 1024> bytes_value{};
            while (co_await stream.async_read_some(asio::buffer(bytes_value), asio::redirect_error(asio::use_awaitable, error))) {
            }
            cancelled_socket_closed = error == asio::error::eof || error == asio::error::connection_reset || error == asio::ssl::error::stream_truncated;
        }
        asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
        ++connections;
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        for (const auto body : expected) {
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.max_response_bytes_ = 8192;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::stop_source cancellation;
        {
            auto response = co_await client.client().with_options({.stop_token_ = cancellation.token()}).get("/partial").send();
            const auto first = co_await response.stream().text();
            EXPECT_TRUE(first.has_value());
            EXPECT_TRUE(!first->empty());
            EXPECT_TRUE(first->size() < large.size());
            EXPECT_TRUE(std::ranges::all_of(*first, [](char value) { return value == 'b'; }));
            cancellation.request_stop();
            // Cancellation itself must not invalidate a previously returned borrow.
            EXPECT_TRUE(std::ranges::all_of(*first, [](char value) { return value == 'b'; }));
        }
        if (join_before_reuse) {
            while (client.client().stats().in_flight_requests_ != 0) {
                (void)co_await ruvia::async_asio([&io](auto handler) {
                    asio::post(io, [handler = std::move(handler)]() mutable { handler(std::error_code{}); });
                });
            }
            EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        }
        for (const auto body : expected) {
            auto response = co_await client.client().get("/next?exact=body").send();
            EXPECT_TRUE(response.protocol_version() == ruvia::http_protocol_version::http11);
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), body);
            EXPECT_TRUE(!(co_await response.stream().read()));
        }
        co_await peer_done.wait();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_TRUE(cancelled_socket_closed);
    EXPECT_EQ(connections, 2U);
    EXPECT_EQ(requests, 3U);
}
}  // namespace

TEST(Ruvia, http1_tls_partial_response_cancellation_retires_transport_and_preserves_healthy_reuse) {
    exercise_tls_response_retirement(true);
}

TEST(Ruvia, http1_tls_queued_reconnect_joins_cancelled_producer_before_replacing_transport) {
    exercise_tls_response_retirement(false);
}

namespace {
// A failed client operation must not leave the peer's accept pending, which
// would keep the I/O context running.
struct acceptor_closer final {
    asio::ip::tcp::acceptor& acceptor_;
    ~acceptor_closer() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }
};
}  // namespace

// RFC 9112 §9.8: only close_notify ends a close-delimited body over TLS; a TCP
// close without it may be an attacker truncating the response.
TEST(Ruvia, http1_tls_close_delimited_response_requires_close_notify) {
    for (const bool close_notify : {true, false}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            std::error_code ignored;
            if (close_notify) {
                co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
            } else {
                // A FIN rather than a close: unread client bytes would turn it into a reset.
                stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
                std::array<char, 256> discarded;
                while (co_await stream.next_layer().async_read_some(
                    asio::buffer(discarded), asio::redirect_error(asio::use_awaitable, ignored))) {
                }
            }
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            auto response = co_await client.client().get("/close-delimited").send();
            std::optional<std::string> body;
            bool truncated = false;
            try {
                const auto bytes_value = co_await response.stream().read_all();
                body.emplace(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size());
            } catch (const ruvia::http_client_error& error) {
                truncated = error.code() == ruvia::make_error_code(ruvia::http_client_errc::io_error);
            }
            if (close_notify) {
                EXPECT_TRUE(body == std::optional<std::string>(std::string("partial")));
            } else {
                EXPECT_TRUE(!body.has_value());
                EXPECT_TRUE(truncated);
            }
            co_await client.shutdown();
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
        };
        run_operation(worker, io, operation);
    }
}

// RFC 9110 §4.3.4 and RFC 9525 §6.3: a DNS name is authenticated only by a
// DNS-ID subjectAltName. A certificate naming the host only in its subject
// Common Name, or through a wildcard that is not a whole left-most label,
// fails the handshake; a whole-label wildcard is accepted.
TEST(Ruvia, http_client_tls_matches_dns_names_only_against_dns_id_subject_alt_names) {
    struct identity_case final {
        const char* common_name_;
        std::string subject_alt_name_{};
        const char* server_name_;
        bool accepted_;
    };
    const std::array cases{
        identity_case{"localhost", std::string(), "localhost", false},
        identity_case{"www.example.com", std::string("DNS:w*.example.com"), "www.example.com", false},
        identity_case{"www.example.com", std::string("DNS:*.example.com"), "www.example.com", true},
    };
    for (const auto& identity_value : cases) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity(identity_value.common_name_, identity_value.subject_alt_name_);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        bool peer_finished = false;
        bool peer_handshake_completed = false;
        ruvia::worker_signal peer_done(worker.handle_);
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            std::error_code error;
            co_await stream.async_handshake(
                asio::ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
            if (error) {
                co_return;
            }
            peer_handshake_completed = true;
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, error));
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr) {
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.tls_.server_name_ = identity_value.server_name_;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            std::optional<std::error_code> failure;
            try {
                auto response = co_await client.client().get("/identity").send();
                EXPECT_EQ(response.status().value(), std::uint16_t{200});
                (void)co_await response.stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                failure = error.code();
            }
            EXPECT_EQ(!failure.has_value(), identity_value.accepted_);
            co_await client.shutdown();
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            EXPECT_EQ(peer_handshake_completed, identity_value.accepted_);
        };
        run_operation(worker, io, operation);
    }
}

namespace tls_record_budget_tests {

struct tunnel_peer final {
    tunnel_peer(asio::io_context& io, const ruvia::worker_ref& worker, asio::ssl::context& tls)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          done_(worker),
          tls_(tls) {}

    asio::awaitable<void> serve() {
        asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor_.async_accept(asio::use_awaitable), tls_);
        std::error_code error;
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        asio::streambuf request;
        co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return;
        }
        constexpr std::string_view head = "HTTP/1.1 200 Connection Established\r\n\r\n";
        co_await asio::async_write(stream, asio::buffer(head), asio::use_awaitable);
        std::array<char, 128> bytes{};
        for (;;) {
            const auto count = co_await stream.async_read_some(asio::buffer(bytes), asio::redirect_error(asio::use_awaitable, error));
            received_.append(bytes.data(), count);
            if (error) {
                co_return;
            }
        }
    }

    ruvia::task<void> exercise(ruvia::http_client& client, std::uint64_t limit) {
        const acceptor_closer closer{acceptor_};
        unsigned completed = 0;
        bool failed = false;
        try {
            auto result = co_await client.client().connect("target.test:443").open();
            auto tunnel = std::move(*result.tunnel());
            for (; completed != 64; ++completed) {
                co_await tunnel.write("x");
            }
        } catch (const ruvia::http_client_error&) {
            failed = true;
        }
        EXPECT_TRUE(failed);
        EXPECT_TRUE(completed < 64);
        EXPECT_EQ(completed != 0, limit != 1);
        co_await client.shutdown();
        while (!finished_) {
            co_await done_.wait();
        }
        EXPECT_TRUE(received_.size() <= limit);
        EXPECT_TRUE(std::ranges::all_of(received_, [](char byte) { return byte == 'x'; }));
    }

    asio::ip::tcp::acceptor acceptor_;
    ruvia::worker_signal done_;
    asio::ssl::context& tls_;
    bool finished_{false};
    std::string received_{};
};

}  // namespace tls_record_budget_tests

TEST(Ruvia, http_client_tls_record_budget_covers_handshake_and_replaced_connect_bio) {
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        for (const std::uint64_t limit : {std::uint64_t{1}, std::uint64_t{32}}) {
            auto& io = ruvia::test::new_test_io_context();
            test_worker worker(io);
            ruvia::test::tls_identity identity("localhost");
            EXPECT_TRUE(SSL_CTX_set_min_proto_version(identity.context_.native_handle(), version) == 1);
            EXPECT_TRUE(SSL_CTX_set_max_proto_version(identity.context_.native_handle(), version) == 1);
            tls_record_budget_tests::tunnel_peer peer(io, worker.handle_, identity.context_);
            asio::co_spawn(io, peer.serve(), [&](std::exception_ptr) { peer.finished_ = true; peer.done_.notify(); });
            const std::string config_url = std::move(std::string("https://127.0.0.1:").append(std::to_string(peer.acceptor_.local_endpoint().port())));
            auto config = local_http_client_config(config_url);
            config.tls_.ca_file_ = identity.ca_file_;
            config.tls_.server_name_ = "localhost";
            config.tls_.max_outgoing_records_ = limit;
            config.request_timeout_ = std::chrono::seconds(3);
            ruvia::http_client client(worker.attachment_.loop(), config);
            run_operation(worker, io, [&] { return peer.exercise(client, limit); });
        }
    }
}

// RFC 9113 §9.2.1: renegotiation is disabled. A TLS 1.2 server's HelloRequest
// is answered with a no_renegotiation alert, which ends the server's attempt,
// while the response already sent is still delivered.
TEST(Ruvia, http_client_tls_refuses_server_initiated_renegotiation) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    EXPECT_EQ(SSL_CTX_set_max_proto_version(identity.context_.native_handle(), TLS1_2_VERSION), 1);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    bool peer_finished = false;
    bool renegotiation_refused = false;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        asio::ssl::stream<asio::ip::tcp::socket> stream(
            co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
        // Sends HelloRequest; the client's reply arrives with later reads.
        if (SSL_renegotiate(stream.native_handle()) != 1) {
            throw std::runtime_error("cannot request TLS renegotiation");
        }
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        std::array<char, 256> bytes_value{};
        std::error_code error;
        while (co_await stream.async_read_some(asio::buffer(bytes_value), asio::redirect_error(asio::use_awaitable, error))) {
        }
        renegotiation_refused = error.category() == asio::error::get_ssl_category() &&
                                ERR_GET_REASON(static_cast<unsigned long>(error.value())) == SSL_R_NO_RENEGOTIATION;
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto response = co_await client.client().get("/renegotiate").send();
        const auto bytes_value = co_await response.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
        co_await client.shutdown();
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
    };
    run_operation(worker, io, operation);
    EXPECT_TRUE(renegotiation_refused);
}

// RFC 9112 §9.8 / RFC 9846 §6.1: the client sends close_notify before it
// closes a TLS connection: when a response ends the connection, when shutdown
// retires an idle keep-alive connection, and when shutdown ends an HTTP/2
// session whose reader is still waiting for input.
TEST(Ruvia, tls_client_sends_close_notify_before_closing) {
    enum class peer_mode : std::uint8_t { http1_close,
        http1_keep_alive,
        http2 };
    for (const auto mode : {
             peer_mode::http1_close,
             peer_mode::http1_keep_alive,
#if defined(RUVIA_ENABLE_HTTP2)
             peer_mode::http2,
#endif
         }) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        if (mode == peer_mode::http2) {
            SSL_CTX_set_alpn_select_cb(
                identity.context_.native_handle(),
                [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                    unsigned int inlen, void*) -> int {
                    static constexpr unsigned char h2[] = {2, 'h', '2'};
                    return SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, h2, sizeof(h2), in,
                               inlen) == OPENSSL_NPN_NEGOTIATED
                               ? SSL_TLSEXT_ERR_OK
                               : SSL_TLSEXT_ERR_ALERT_FATAL;
                },
                nullptr);
        }
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        // eof is the client's close_notify; stream_truncated a bare TCP close.
        std::error_code client_end;
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            std::array<char, 16384> input{};
#if defined(RUVIA_ENABLE_HTTP2)
            if (mode == peer_mode::http2) {
                std::error_code write_error;
                auto connection = ruvia::http2_connection::server();
                std::string output;
                while (!client_end) {
                    if (connection.wants_write()) {
                        ruvia::test::drain_output(connection, output);
                        // A failed write leaves the client's end to the next read.
                        (void)co_await asio::async_write(stream, asio::buffer(output),
                            asio::redirect_error(asio::use_awaitable, write_error));
                    }
                    const auto count = co_await stream.async_read_some(
                        asio::buffer(input), asio::redirect_error(asio::use_awaitable, client_end));
                    (void)connection.feed(std::string_view(input.data(), count));
                    while (auto event = connection.next_event()) {
                        if (auto* request = event->request_head()) {
                            const auto stream_id = request->stream_id();
                            const auto built = request->build_request();
                            ruvia::http_response response;
                            response.body("ok");
                            (void)connection.submit_response_head(stream_id, response,
                                ruvia::plan_buffered_http_response_write(built.known_method(), response));
                            (void)connection.submit_data(
                                stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535);
                            (void)connection.release(std::move(*request));
                        }
                    }
                }
                co_return;
            }
#endif
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            const std::string_view response = mode == peer_mode::http1_close
                                                  ? "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok"
                                                  : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            while (!client_end) {
                (void)co_await stream.async_read_some(
                    asio::buffer(input), asio::redirect_error(asio::use_awaitable, client_end));
            }
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.request_timeout_ = std::chrono::seconds(3);
        if (mode == peer_mode::http2) {
            config.versions_ = ruvia::http_client_versions{.http1_ = false};
        }
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            auto response = co_await client.client().get("/close-notify").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(bytes_value.size(), std::size_t{2});
            co_await client.shutdown();
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            EXPECT_TRUE(client_end == asio::error::eof);
        };
        run_operation(worker, io, operation);
    }
}

// A response on a connection that is not reused completes even when trailing
// bytes arrive with it: they are discarded with the connection, not parsed.
TEST(Ruvia, http1_response_on_a_closing_connection_discards_trailing_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhelloXjunk",
            "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\n\r\ndeniedHTTP/1.1 200 OK\r\n\r\n"});
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        {
            auto response = co_await client.client().get("/closing").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(
                std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "hello");
        }
        {
            auto result = co_await client.client().connect("target.test:443").open();
            auto* response = result.response();
            EXPECT_TRUE(response != nullptr);
            if (response != nullptr) {
                EXPECT_EQ(response->status().value(), std::uint16_t{407});
                const auto bytes_value = co_await response->stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()),
                              bytes_value.size()),
                    "denied");
            }
        }
        co_await peer.wait();
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_replaces_a_keep_alive_connection_the_peer_closed_while_idle) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    unsigned connections = 0;
    bool idle_closed = false;
    ruvia::worker_signal idle_closed_signal(worker.handle_);
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        for (const std::string_view body : {std::string_view("first"), std::string_view("second")}) {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ++connections;
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response =
                std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            if (!idle_closed) {
                // A keep-alive idle timeout: the peer closes without a request.
                std::error_code ignored;
                socket.close(ignored);
                idle_closed = true;
                idle_closed_signal.notify();
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
            auto response = co_await client.client().get("/idle").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
            while (!idle_closed) {
                co_await idle_closed_signal.wait();
            }
            // Let the peer's FIN reach the idle pooled socket.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.failed_requests_, std::size_t{0});
        EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

TEST(Ruvia, http_client_retries_only_idempotent_requests_after_a_reused_connection_fails_before_its_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    unsigned connections = 0;
    unsigned retried_requests = 0;
    unsigned non_idempotent_requests = 0;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto read_request = [](asio::ip::tcp::socket& socket) -> asio::awaitable<void> {
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
    };
    const auto answer = [](asio::ip::tcp::socket& socket, std::string_view body,
                            std::string_view connection) -> asio::awaitable<void> {
        const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\nConnection: " +
                                     std::string(connection) + "\r\n\r\n" + std::string(body);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    };
    const auto serve = [&]() -> asio::awaitable<void> {
        auto first = co_await acceptor.async_accept(asio::use_awaitable);
        ++connections;
        co_await read_request(first);
        co_await answer(first, "prime", "keep-alive");
        co_await read_request(first);
        ++retried_requests;
        std::error_code ignored;
        first.close(ignored);

        std::error_code accept_error;
        auto second = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, accept_error));
        if (accept_error == asio::error::operation_aborted) {
            co_return;
        }
        if (accept_error) {
            throw std::system_error(accept_error);
        }
        ++connections;
        co_await read_request(second);
        ++retried_requests;
        co_await answer(second, "retried", "keep-alive");
        co_await read_request(second);
        ++non_idempotent_requests;
        acceptor.close(ignored);
        second.close(ignored);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto first = co_await client.client().get("/prime").send();
        const auto first_body = co_await first.stream().read_all();
        EXPECT_EQ(std::string_view(
                      reinterpret_cast<const char*>(first_body.bytes().data()), first_body.size()),
            "prime");

        std::optional<std::string> retry_body;
        try {
            auto response = co_await client.client().get("/retry").send();
            const auto bytes_value = co_await response.stream().read_all();
            retry_body.emplace(
                reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size());
        } catch (const ruvia::http_client_error&) {
            std::error_code ignored;
            acceptor.close(ignored);
        }
        std::optional<std::error_code> post_failure;
        if (retry_body.has_value()) {
            try {
                (void)co_await client.client().request("POST", "/post").send();
            } catch (const ruvia::http_client_error& error) {
                post_failure = error.code();
            }
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(retry_body == std::optional<std::string>(std::string("retried")));
        EXPECT_TRUE(post_failure == ruvia::make_error_code(ruvia::http_client_errc::io_error));
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.completed_requests_, std::size_t{2});
        EXPECT_EQ(stats.failed_requests_, std::size_t{1});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
    EXPECT_EQ(retried_requests, 2U);
    EXPECT_EQ(non_idempotent_requests, 1U);
}

// The idle probe never decrypts: any record a TLS peer sends while the pooled
// connection idles, a TLS 1.3 session ticket as much as a close_notify,
// retires the connection, and the next request uses a fresh one.
TEST(Ruvia, http_client_tls_idle_probe_retires_a_connection_with_unread_peer_records) {
    for (const bool peer_closes : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        unsigned connections = 0;
        bool idle_sent = false;
        ruvia::worker_signal idle_sent_signal(worker.handle_);
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto answer = [](asio::ssl::stream<asio::ip::tcp::socket>& stream,
                                std::string_view body) -> asio::awaitable<void> {
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response =
                std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        };
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> first(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await first.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await answer(first, "first");
            if (peer_closes) {
                // Starting the shutdown writes close_notify at once; it then
                // waits for the client's reply, which ends when the client
                // drops the retired connection.
                idle_sent = true;
                idle_sent_signal.notify();
                std::error_code ignored;
                co_await first.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
            } else {
                if (SSL_new_session_ticket(first.native_handle()) != 1) {
                    throw std::runtime_error("TLS 1.3 was not negotiated");
                }
                // A post-handshake handshake step sends the ticket immediately.
                co_await first.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
                idle_sent = true;
                idle_sent_signal.notify();
            }
            // No FIN precedes the client's probe: only the idle record retires
            // the first connection.
            asio::ssl::stream<asio::ip::tcp::socket> second(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await second.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await answer(second, "second");
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.tls_.ca_file_ = identity.ca_file_;
        config.connection_limits_.max_open_ = 1;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
                auto response = co_await client.client().get("/idle").send();
                const auto bytes_value = co_await response.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
                while (!idle_sent) {
                    co_await idle_sent_signal.wait();
                }
                // Let the idle record reach the pooled socket.
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            }
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.failed_requests_, std::size_t{0});
            EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{1});
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        EXPECT_EQ(connections, 2U);
    }
}

TEST(Ruvia, http_client_abandoned_queued_request_is_never_written) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::vector<std::string> request_lines{};
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::string buffered{};
        for (unsigned index = 0; index != 2; ++index) {
            const auto head_bytes =
                co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
            request_lines.emplace_back(std::string_view(buffered).substr(0, buffered.find("\r\n")));
            buffered.erase(0, head_bytes);
            if (!request_lines.back().starts_with("GET ")) {
                co_return;
            }
            if (index == 0) {
                // Keep the only slot leased while the queued request is abandoned.
                asio::steady_timer delay(socket.get_executor(), std::chrono::milliseconds(100));
                co_await delay.async_wait(asio::use_awaitable);
            }
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        ruvia::task_scope first(worker.handle_);
        const auto send_first = [&]() -> ruvia::task<void> {
            auto response = co_await client.client().get("/first").send();
            (void)co_await response.stream().read_all();
        };
        first.spawn(send_first());
        {
            auto abandoned = co_await client.client().request("POST", "/abandoned").upload({});
            // Let the request driver queue behind the leased slot.
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
        }
        co_await first.join();
        auto response = co_await client.client().get("/third").send();
        const auto bytes_value = co_await response.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(request_lines.size(), std::size_t{2});
    if (request_lines.size() == 2) {
        EXPECT_EQ(request_lines[0], "GET /first HTTP/1.1");
        EXPECT_EQ(request_lines[1], "GET /third HTTP/1.1");
    }
}

// Dropping an exchange whose connection is still being established cancels
// that connect: the slot frees at once instead of after connect_timeout_.
TEST(Ruvia, http_client_abandoned_request_cancels_its_connect_in_progress) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    // Accepts TCP but never answers the TLS ClientHello, so the client's
    // connect stays in progress until its connect_timeout_ or a cancellation.
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string client_url = std::move(std::string("https://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    ruvia::http_client_config config{.base_url_ = client_url,
        .connection_limits_ = {.max_open_ = 1},
        .connect_timeout_ = std::chrono::seconds(5),
        .request_timeout_ = std::chrono::seconds(10)};
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            auto abandoned = co_await client.client().request("POST", "/abandoned").upload({});
            // Let the driver reach the TLS handshake.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.in_flight_requests_ + stats.buffered_requests_, std::size_t{1});
        }
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            const auto stats = client.client().stats();
            if (stats.in_flight_requests_ + stats.buffered_requests_ == 0) {
                break;
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
                throw std::runtime_error("abandoned connect kept its pool slot leased");
            }
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(10));
        }
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 §7.5 permits the final response to finish before its request. A
// persistent final response does not cancel content that was already released.
TEST(Ruvia, http_client_finishes_upload_after_an_early_persistent_response_before_reusing_connection) {
    enum class release_mode { immediate,
        continued,
        timed_out,
        submillisecond_timeout };
    for (const auto mode : {release_mode::immediate, release_mode::continued, release_mode::timed_out, release_mode::submillisecond_timeout}) {
        const bool expect_continue = mode != release_mode::immediate;
        const bool timeout_release = mode == release_mode::timed_out || mode == release_mode::submillisecond_timeout;
        for (const bool known_length : {false, true}) {
            auto& io = ruvia::test::new_test_io_context();
            test_worker worker(io);
            asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
            std::exception_ptr peer_failure;
            bool peer_finished = false;
            ruvia::worker_signal peer_done(worker.handle_);
            std::string request_body{};
            ruvia::http_request_trailers request_trailers;
            std::string next_request{};
            const auto serve = [&]() -> asio::awaitable<void> {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                std::string buffered{};
                const auto head_bytes =
                    co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                buffered.erase(0, head_bytes);
                if (timeout_release) {
                    if (buffered.empty()) {
                        co_await asio::async_read(socket, asio::dynamic_buffer(buffered),
                            asio::transfer_at_least(1), asio::use_awaitable);
                    }
                }
                std::string response{};
                if (expect_continue && !timeout_release) {
                    response += "HTTP/1.1 100 Continue\r\n\r\n";
                }
                response += "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
                if (known_length) {
                    if (buffered.size() < 3) {
                        co_await asio::async_read(socket, asio::dynamic_buffer(buffered),
                            asio::transfer_exactly(3 - buffered.size()), asio::use_awaitable);
                    }
                    request_body.assign(buffered.data(), 3);
                    buffered.erase(0, 3);
                } else {
                    ruvia::http1_chunked_body_decoder decoder;
                    for (;;) {
                        const auto decoded = decoder.decode(buffered);
                        if (decoded.failure()) {
                            throw std::runtime_error("invalid upload chunk framing");
                        }
                        if (const auto* chunk = decoded.body_chunk()) {
                            request_body.append(chunk->bytes());
                        }
                        const bool complete = decoded.complete() != nullptr;
                        if (complete && request_trailers.append_received(decoded.complete()->trailers())) {
                            throw std::runtime_error("invalid upload trailers");
                        }
                        buffered.erase(0, decoded.consumed_bytes());
                        if (complete) {
                            break;
                        }
                        if (decoded.need_more()) {
                            co_await asio::async_read(socket, asio::dynamic_buffer(buffered),
                                asio::transfer_at_least(1), asio::use_awaitable);
                        }
                    }
                }
                const auto next_head_bytes =
                    co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                next_request.assign(buffered.data(), next_head_bytes);
                constexpr std::string_view final_response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
                co_await asio::async_write(socket, asio::buffer(final_response), asio::use_awaitable);
            };
            asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
                peer_failure = failure;
                peer_finished = true;
                peer_done.notify();
            });
            const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
            auto config = local_http_client_config(config_url);
            config.connection_limits_.max_open_ = 1;
            config.request_timeout_ = std::chrono::seconds(3);
            ruvia::http_client client(worker.attachment_.loop(), config);
            auto operation = [&]() -> ruvia::task<void> {
                const acceptor_closer closer{acceptor};
                ruvia::http_client_upload_config upload{
                    .content_length_ = known_length ? std::optional<ruvia::http_content_length>{3} : std::nullopt,
                    .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none,
                    .continue_timeout_ = std::chrono::duration<std::uint64_t, std::milli>{std::numeric_limits<std::uint64_t>::max()}};
                if (timeout_release) {
                    upload.continue_timeout_ = mode == release_mode::submillisecond_timeout
                                                   ? std::chrono::microseconds(1)
                                                   : std::chrono::microseconds(20000);
                }
                auto exchange = co_await client.client().request("POST", "/early").upload(upload);
                if (timeout_release) {
                    co_await exchange.body().write("a");
                }
                auto response = co_await exchange.response();
                EXPECT_EQ(response.status().value(), std::uint16_t{200});
                const auto early_body = co_await response.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(early_body.bytes().data()), early_body.size()), "ok");
                EXPECT_TRUE(response.stream().complete());
                EXPECT_TRUE(!exchange.body().complete());
                EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
                co_await exchange.body().write(timeout_release ? "bc" : "abc");
                const ruvia::http_header_view trailers[] = {{"X-End", "retained"}};
                co_await exchange.body().end(known_length ? std::span<const ruvia::http_header_view>{}
                                                          : std::span<const ruvia::http_header_view>(trailers));
                auto next = co_await client.client().get("/next").send();
                const auto next_body = co_await next.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
                while (!peer_finished) {
                    co_await peer_done.wait();
                }
                if (peer_failure) {
                    std::rethrow_exception(peer_failure);
                }
                co_await client.shutdown();
            };
            run_operation(worker, io, operation);
            EXPECT_EQ(request_body, "abc");
            EXPECT_TRUE(request_trailers.field("x-end") ==
                        (known_length ? std::nullopt : std::optional<std::string_view>("retained")));
            ruvia::http1_server_request_parse_state parsed;
            parsed.parse_head(next_request);
            EXPECT_TRUE(parsed.head_ready());
            EXPECT_EQ(parsed.request().method(), "GET");
            EXPECT_EQ(parsed.request().target(), "/next");
        }
    }
}

TEST(Ruvia, http_client_abandoned_upload_cancels_its_retained_incomplete_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view head = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
        std::array<char, 1> input;
        std::error_code error;
        const auto bytes = co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        if (bytes != 0 || (error != asio::error::eof && error != asio::error::connection_reset)) {
            throw std::runtime_error("abandoned HTTP/1 upload did not close");
        }
        auto next_socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf next_request;
        co_await asio::async_read_until(next_socket, next_request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view next_response =
            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
        co_await asio::async_write(next_socket, asio::buffer(next_response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    // A broken abandonment path fails as timeout rather than hanging the suite.
    config.request_timeout_ = std::chrono::seconds(1);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        std::optional<ruvia::http_client_response> response;
        {
            auto exchange = co_await client.client().post("/abandoned").upload({.content_length_ = 6});
            response.emplace(co_await exchange.response());
            EXPECT_TRUE(!response->stream().complete());
        }
        bool cancelled = false;
        try {
            (void)co_await response->stream().read_all();
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(cancelled);
        auto next = co_await client.client().get("/next").send();
        const auto next_body = co_await next.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_bounds_unfinished_upload_after_response_body_completion) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    bool first_closed = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 256> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
        if (error != asio::error::eof && error != asio::error::connection_reset) {
            throw std::system_error(error);
        }
        first_closed = true;
        peer_done.notify();
        auto next_socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf next_request;
        co_await asio::async_read_until(next_socket, next_request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view next_response =
            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
        co_await asio::async_write(next_socket, asio::buffer(next_response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        first_closed = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::milliseconds(100);
    config.connection_limits_.max_open_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.client().request("POST", "/unfinished").upload({});
        auto response = co_await exchange.response();
        (void)co_await response.stream().read_all();
        while (!first_closed) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        bool stopped = false;
        try {
            co_await exchange.body().write("late");
        } catch (const ruvia::http_client_error& error) {
            stopped = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(stopped);
        auto next = co_await client.client().get("/after-timeout").send();
        const auto next_body = co_await next.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
        EXPECT_TRUE(!(co_await response.stream().text()).has_value());
        EXPECT_TRUE(response.stream().complete());
    };
    run_operation(worker, io, operation);
}

// A transport failure while the response body is still arriving is the real
// cause of the exchange failure, even though the writer sees it first and its
// cleanup cancels the receiver's read; it is reported as such, not as a
// cancellation. The response body is left unread so the receiver sits in the
// backpressure wait without a read in flight and only the upload observes the reset.
TEST(Ruvia, http_client_reports_upload_transport_failure_during_response_body) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n0123456789012345678901234567890123456789";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 4096> input{};
        std::size_t received = 0;
        while (received < 64 * 1024) {
            received += co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
        }
        // SO_LINGER 0 turns close() into a reset the client's next write observes.
        socket.set_option(asio::socket_base::linger(true, 0));
        socket.close();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 16;
    config.connection_limits_.max_open_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.client().request("POST", "/reset").upload({});
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        const std::string chunk(16 * 1024, 'u');
        std::optional<std::error_code> upload_error;
        for (int i = 0; i < 1024 && !upload_error.has_value(); ++i) {
            try {
                co_await exchange.body().write(chunk);
            } catch (const ruvia::http_client_error& error) {
                upload_error = error.code();
            }
        }
        EXPECT_TRUE(upload_error.has_value());
        if (upload_error.has_value()) {
            EXPECT_TRUE(*upload_error == ruvia::make_error_code(ruvia::http_client_errc::io_error));
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_capsule_client_retains_payloads_across_reads_and_rejects_truncated_frames) {
    for (const bool truncated : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::tcp::v4(), 0});
        ruvia::worker_signal peer_done(worker.handle_);
        std::exception_ptr peer_failure;
        std::string first_payload(128 * 1024 + 17, 'a');
        first_payload[4095] = '\0';
        first_payload[8192] = '\xff';
        const std::string second_payload(4096, 'b');
        std::string wire{"HTTP/1.1 200 Connection Established\r\n\r\n"};
        const auto append_capsule = [&](std::uint64_t type, std::string_view payload) {
            std::array<char, 16> header;
            const auto encoded = ruvia::encode_http_capsule_header(header, type, payload.size());
            wire.append(header.data(), std::get<std::size_t>(encoded));
            wire.append(payload);
        };
        append_capsule(17, first_payload);
        append_capsule(18, second_payload);
        if (truncated) {
            wire.pop_back();
        } else {
            append_capsule(19, {});
        }
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf head;
            co_await asio::async_read_until(socket, head, "\r\n\r\n", asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_done.notify();
        });
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        auto operation = [&]() -> ruvia::task<void> {
            auto result = co_await client.client().connect("target.test:443").open();
            if (result.tunnel() == nullptr) {
                throw std::runtime_error("expected an accepted tunnel");
            }
            std::optional<ruvia::http_capsule> retained;
            {
                auto stream = std::move(*result.tunnel()).capsules();
                retained = co_await stream.read();
                EXPECT_TRUE(retained && retained->type() == 17 && retained->payload() == first_payload);
                bool rejected = false;
                try {
                    auto second = co_await stream.read();
                    EXPECT_TRUE(second && second->type() == 18 && second->payload() == second_payload);
                    auto empty = co_await stream.read();
                    EXPECT_TRUE(empty && empty->type() == 19 && empty->payload().empty());
                    EXPECT_TRUE(!(co_await stream.read()));
                    co_await stream.finish();
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                EXPECT_EQ(rejected, truncated);
            }
            EXPECT_TRUE(retained && retained->payload() == first_payload);
            retained.reset();
            co_await client.shutdown();
            co_await peer_done.wait();
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
        };
        run_operation(worker, io, operation);
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_client_uses_available_stream_capacity_across_connections) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::array<std::optional<asio::ip::tcp::socket>, 2> sockets;
    std::array<ruvia::http2_connection, 2> connections{
        ruvia::http2_connection::server(), ruvia::http2_connection::server()};
    ruvia::worker_signal peer_done(worker.handle_);
    asio::steady_timer deadline(io);
    std::exception_ptr peer_failure;
    bool peer_finished = false;

    const auto flush = [&](std::size_t index, std::optional<std::uint32_t> stream_limit = std::nullopt)
        -> asio::awaitable<void> {
        if (stream_limit && connections[index].submit_max_concurrent_streams(*stream_limit) !=
                                ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 capacity settings rejected");
        }
        std::string output;
        ruvia::test::drain_output(connections[index], output);
        if (!output.empty()) {
            co_await asio::async_write(*sockets[index], asio::buffer(output), asio::use_awaitable);
        }
    };
    const auto read_request = [&](std::size_t index) -> asio::awaitable<ruvia::http2_request_head_event> {
        std::array<char, 16384> input{};
        for (;;) {
            while (auto event = connections[index].next_event()) {
                if (auto* request = event->request_head()) {
                    co_return std::move(*request);
                }
            }
            const auto count = co_await sockets[index]->async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto status = connections[index].feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 capacity test input");
                }
                while (auto event = connections[index].next_event()) {
                    if (auto* request = event->request_head()) {
                        co_return std::move(*request);
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    const auto respond = [&](std::size_t index, ruvia::http2_request_head_event&& request) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        const auto submitted = connections[index].submit_response_head(
            stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(submitted) ||
            connections[index].release(std::move(request)) !=
                ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("HTTP/2 capacity test response rejected");
        }
    };
    const auto serve = [&]() -> asio::awaitable<void> {
        // Concurrent requests of one route share the first connection's
        // negotiation; its single stream slot then sends the next request to
        // a second connection, which has two.
        sockets[0].emplace(co_await acceptor.async_accept(asio::use_awaitable));
        co_await flush(0, 1);
        sockets[1].emplace(co_await acceptor.async_accept(asio::use_awaitable));
        co_await flush(1, 2);

        auto first_warm = co_await read_request(0);
        auto second_warm = co_await read_request(1);
        respond(1, std::move(second_warm));
        co_await flush(1);
        respond(0, std::move(first_warm));
        co_await flush(0);

        auto saturated = co_await read_request(0);
        auto available = co_await read_request(1);
        auto rerouted = co_await read_request(1);
        if (rerouted.build_request().target() != "/probe") {
            throw std::runtime_error("HTTP/2 request was not routed to available stream capacity");
        }
        respond(0, std::move(saturated));
        respond(1, std::move(available));
        respond(1, std::move(rerouted));
        co_await flush(0);
        co_await flush(1);
        for (auto& socket : sockets) {
            std::error_code ignored;
            socket->close(ignored);
        }
    };
    deadline.expires_after(std::chrono::seconds(3));
    deadline.async_wait([&](const std::error_code& error) {
        if (error) {
            return;
        }
        std::error_code ignored;
        acceptor.close(ignored);
        for (auto& socket : sockets) {
            if (socket) {
                socket->close(ignored);
            }
        }
    });
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        deadline.cancel();
        peer_done.notify();
    });

    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.max_concurrent_http2_streams_per_connection_ = 2;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(2);
    ruvia::http_client client(worker.attachment_.loop(), config);
    const auto send = [&](ruvia::http_client& client_value, std::string_view target) -> ruvia::task<void> {
        auto response = co_await client_value.client().get(target).send();
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        EXPECT_TRUE((co_await response.stream().read_all()).empty());
    };
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            ruvia::task_scope warm(worker.handle_);
            warm.spawn(send(client, "/warm-first"));
            warm.spawn(send(client, "/warm-second"));
            co_await warm.join();
        }
        {
            ruvia::task_scope requests(worker.handle_);
            requests.spawn(send(client, "/hold-saturated"));
            requests.spawn(send(client, "/hold-available"));
            requests.spawn(send(client, "/probe"));
            co_await requests.join();
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http2_cold_start_cancelled_waiter_does_not_close_active_session) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_done(worker.handle_);
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::stop_source cancel_second;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        // Cancel the second queued request while the first is completing the
        // HTTP/2 connection preface, so the second wakes from http1_scheduler_
        // after protocol_ is already http2.
        cancel_second.request_stop();
        auto connection = ruvia::http2_connection::server();
        std::array<char, 16384> input{};
        std::string output;
        bool answered = false;
        for (;;) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            if (answered && !connection.wants_write()) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 input");
                }
                while (auto event = connection.next_event()) {
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    const auto stream_id = request->stream_id();
                    const auto built = request->build_request();
                    ruvia::http_response response;
                    response.body("first-ok");
                    const auto head = connection.submit_response_head(
                        stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
                    if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) ||
                        connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ !=
                            ruvia::http2_data_submit_status::complete ||
                        connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
                        throw std::runtime_error("HTTP/2 response submission failed");
                    }
                    answered = true;
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    ruvia::http_client client(worker.attachment_.loop(), config);
    const auto send_first = [](ruvia::http_client& c, bool& ok_out) -> ruvia::task<void> {
        auto response = co_await c.client().get("/first").send();
        const auto body = co_await response.stream().read_all();
        ok_out = response.status() == ruvia::http_status::ok &&
                 std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()) == "first-ok";
    };
    const auto send_second = [](ruvia::http_client& c, ruvia::stop_token token,
                                 bool& cancelled_out) -> ruvia::task<void> {
        try {
            (void)co_await c.client().with_options({.stop_token_ = std::move(token)}).get("/second").send();
        } catch (const ruvia::http_client_error& error) {
            cancelled_out = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
    };
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        ruvia::task_scope tasks(worker.handle_);
        bool first_ok = false;
        bool second_cancelled = false;
        tasks.spawn(send_first(client, first_ok));
        tasks.spawn(send_second(client, cancel_second.token(), second_cancelled));
        co_await tasks.join();
        EXPECT_TRUE(first_ok);
        EXPECT_TRUE(second_cancelled);
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_alpn_renegotiates_after_http2_disconnect_and_closes_on_mismatch) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    std::atomic<bool> offer_h2{true};
    SSL_CTX_set_alpn_select_cb(
        identity.context_.native_handle(),
        [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
            void* arg) -> int {
            const bool want_h2 = static_cast<std::atomic<bool>*>(arg)->load();
            const std::string_view target = want_h2 ? "h2" : "http/1.1";
            unsigned int pos = 0;
            while (pos < inlen) {
                const unsigned int len = in[pos++];
                if (pos + len <= inlen &&
                    std::string_view(reinterpret_cast<const char*>(in + pos), len) == target) {
                    *out = in + pos;
                    *outlen = static_cast<unsigned char>(len);
                    return SSL_TLSEXT_ERR_OK;
                }
                pos += len;
            }
            return SSL_TLSEXT_ERR_NOACK;
        },
        &offer_h2);

    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    ruvia::worker_signal peer_done(worker.handle_);
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    bool mismatch_socket_closed = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        // 1st connection: negotiate h2, serve one request, then close.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            auto connection = ruvia::http2_connection::server();
            std::array<char, 16384> input{};
            std::string output;
            bool answered = false;
            for (;;) {
                if (connection.wants_write()) {
                    ruvia::test::drain_output(connection, output);
                    co_await asio::async_write(stream, asio::buffer(output), asio::use_awaitable);
                }
                // The drain ends with the final GOAWAY sent after the client's
                // PING ACK, or earlier when the retiring client closes first.
                if (answered && connection.drain_final() && !connection.wants_write()) {
                    break;
                }
                std::error_code read_error;
                const auto count = co_await stream.async_read_some(
                    asio::buffer(input), asio::redirect_error(asio::use_awaitable, read_error));
                if (read_error && answered) {
                    break;
                }
                if (read_error) {
                    throw std::system_error(read_error);
                }
                (void)connection.feed(std::string_view(input.data(), count));
                while (auto event = connection.next_event()) {
                    if (auto* request = event->request_head()) {
                        const auto stream_id = request->stream_id();
                        const auto built = request->build_request();
                        ruvia::http_response response;
                        response.body("h2-ok");
                        (void)connection.submit_response_head(stream_id, response,
                            ruvia::plan_buffered_http_response_write(built.known_method(), response));
                        (void)connection.submit_data(
                            stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535);
                        (void)connection.release(std::move(*request));
                        // GOAWAY precedes the close so the client retires this
                        // session deterministically: seen before the next send it
                        // opens a new connection; the PING round trip then
                        // advertises last_stream_id = 1 before the close.
                        connection.begin_drain();
                        answered = true;
                    }
                }
            }
            offer_h2.store(false);
            std::error_code ignored;
            stream.lowest_layer().close(ignored);
        }
        // 2nd connection: negotiate http/1.1 for the negotiate client.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nh1-ok";
            co_await asio::async_write(stream, asio::buffer(reply), asio::use_awaitable);
        }
        // 3rd connection: http2_only client rejects http/1.1 ALPN and closes socket.
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            std::array<char, 16> buf{};
            std::error_code read_ec;
            (void)co_await stream.next_layer().async_read_some(
                asio::buffer(buf), asio::redirect_error(asio::use_awaitable, read_ec));
            mismatch_socket_closed =
                read_ec == asio::error::eof || read_ec == asio::error::connection_reset;
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{};
    ruvia::http_client negotiate_client(worker.attachment_.loop(), config);

    auto h2_only_config = config;
    h2_only_config.versions_ = ruvia::http_client_versions{.http1_ = false};
    ruvia::http_client h2_only_client(worker.attachment_.loop(), h2_only_config);

    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            auto first = co_await negotiate_client.client().get("/first").send();
            EXPECT_TRUE(first.protocol_version() == ruvia::http_protocol_version::http2);
            const auto first_body = co_await first.stream().read_all();
            EXPECT_EQ(
                std::string_view(reinterpret_cast<const char*>(first_body.bytes().data()), first_body.size()), "h2-ok");
        }
        {
            auto second = co_await negotiate_client.client().get("/second").send();
            EXPECT_TRUE(second.protocol_version() == ruvia::http_protocol_version::http11);
            const auto second_body = co_await second.stream().read_all();
            EXPECT_EQ(
                std::string_view(reinterpret_cast<const char*>(second_body.bytes().data()), second_body.size()), "h1-ok");
        }
        bool protocol_unavailable = false;
        try {
            (void)co_await h2_only_client.client().get("/mismatch").send();
        } catch (const ruvia::http_client_error& error) {
            protocol_unavailable = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_unavailable);
        }
        EXPECT_TRUE(protocol_unavailable);
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(mismatch_socket_closed);
        co_await negotiate_client.shutdown();
        co_await h2_only_client.shutdown();
    };
    run_operation(worker, io, operation);
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), nullptr, nullptr);
}

#endif

// RFC 6265 section 5.4: stored and configured cookies belong to the fixed
// origin. A CONNECT-UDP tunnel to another authority carries none of them, and
// that authority's Set-Cookie does not enter the origin's jar.
TEST(Ruvia, http_client_scopes_cookies_to_the_origin_authority_for_tunnels) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 407 Proxy Authentication Required\r\nSet-Cookie: tunnel=other\r\nContent-Length: 0\r\n"
         "Connection: close\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.cookies_ = {{"sid", "secret"}};
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        const std::array fields{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
        auto result = co_await client.client().connect("proxy.test:8443").protocol("connect-udp").target("/.well-known/masque/udp/target.test/53/").headers(fields).open();
        EXPECT_TRUE(result.tunnel() == nullptr);
        if (auto* rejected = result.response()) {
            EXPECT_EQ(rejected->status().value(), std::uint16_t{407});
            EXPECT_TRUE((co_await rejected->stream().read_all()).empty());
        }
        auto origin = co_await client.client().get("/").send();
        EXPECT_TRUE((co_await origin.stream().read_all()).empty());
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            EXPECT_TRUE(peer.requests_[0].find("secret") == std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("sid=secret") != std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("tunnel=other") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// read_timeout_ bounds each response read rather than the exchange: a peer
// that stops sending the head, or the body after its head, fails the response
// with timeout long before request_timeout_ and loses the connection.
TEST(Ruvia, http1_client_read_timeout_bounds_a_stalled_response_head_and_body) {
    for (const bool head_sent : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        bool client_closed = false;
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            if (head_sent) {
                const std::string partial("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab");
                co_await asio::async_write(socket, asio::buffer(partial), asio::use_awaitable);
            }
            // Stall until the client abandons the read and closes.
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            client_closed = true;
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.read_timeout_ = std::chrono::milliseconds(100);
        config.request_timeout_ = std::chrono::seconds(20);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                auto response = co_await client.client().get("/stall").send();
                EXPECT_TRUE(head_sent);
                (void)co_await response.stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            EXPECT_TRUE(client_closed);
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

// idle_timeout_ closes a pooled keep-alive connection that no exchange reused,
// without a request; the next request opens a new connection.
TEST(Ruvia, http1_client_idle_timeout_closes_an_unused_pooled_connection) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::optional<asio::ip::tcp::socket> idle_socket;
    asio::steady_timer guard(io);
    unsigned connections = 0;
    bool idle_closed = false;
    std::chrono::steady_clock::duration idle_for{};
    ruvia::worker_signal idle_closed_signal(worker.handle_);
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        for (const std::string_view body : {std::string_view("first"), std::string_view("second")}) {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ++connections;
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response =
                std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            if (idle_closed) {
                continue;
            }
            // Keep the connection open; only the client's idle bound ends it.
            const auto responded = std::chrono::steady_clock::now();
            idle_socket.emplace(std::move(socket));
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await idle_socket->async_read_some(
                    asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            idle_for = std::chrono::steady_clock::now() - responded;
            idle_closed = true;
            idle_closed_signal.notify();
        }
    };
    guard.expires_after(std::chrono::seconds(10));
    guard.async_wait([&](const std::error_code& error) {
        if (!error && idle_socket) {
            std::error_code ignored;
            idle_socket->close(ignored);
        }
    });
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        guard.cancel();
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.idle_timeout_ = std::chrono::milliseconds(100);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
            auto response = co_await client.client().get("/idle").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
            while (!idle_closed) {
                co_await idle_closed_signal.wait();
            }
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(idle_for < std::chrono::seconds(5));
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.failed_requests_, std::size_t{0});
        EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

namespace {
#if defined(RUVIA_ENABLE_HTTP2)
// One prior-knowledge HTTP/2 connection served by the sans-I/O core, which
// answers PINGs as it reads. The mode decides each request's answer.
class http2_liveness_peer final {
public:
    enum class mode_type : std::uint8_t {
        // The first request never gets a response; later ones get "ok".
        stall_first_request,
        answer,
        // The request is answered after the client sent two liveness PINGs.
        answer_after_pings,
        // After the request the peer reads but neither processes nor writes.
        silent_after_request,
    };

    http2_liveness_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, mode_type mode)
        : acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          guard_(io),
          done_(worker_value),
          io_(io),
          mode_(mode) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        guard_.expires_after(std::chrono::seconds(10));
        guard_.async_wait([this](const std::error_code& error) {
            if (error) {
                return;
            }
            std::error_code ignored;
            acceptor_.close(ignored);
            if (socket_) {
                socket_->close(ignored);
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            finished_ = true;
            guard_.cancel();
            done_.notify();
        });
    }

    // Completes once the client closed the connection.
    [[nodiscard]] ruvia::task<void> wait() {
        while (!finished_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    // From the last response to the client's close.
    std::chrono::steady_clock::duration idle_for_{};
    // The client sent GOAWAY before it closed (RFC 9113 section 6.8).
    bool goaway_received_{false};

private:
    // A liveness PING the client sent: length 8, type PING, no ACK flag.
    [[nodiscard]] static std::size_t count_pings(std::string_view bytes) noexcept {
        static constexpr std::string_view ping_header("\0\0\x08\x06\0", 5);
        std::size_t count = 0;
        for (auto at = bytes.find(ping_header); at != std::string_view::npos; at = bytes.find(ping_header, at + 1)) {
            ++count;
        }
        return count;
    }

    void respond(ruvia::http2_connection& connection, ruvia::http2_request_head_event&& request) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        response.body("ok");
        const auto head = connection.submit_response_head(
            stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) ||
            connection.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ !=
                ruvia::http2_data_submit_status::complete ||
            connection.release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("HTTP/2 liveness peer response rejected");
        }
        responded_ = std::chrono::steady_clock::now();
    }

    asio::awaitable<void> serve() {
        socket_.emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        auto connection = ruvia::http2_connection::server();
        std::array<char, 16384> input{};
        std::string output;
        std::vector<ruvia::http2_request_head_event> stalled{};
        std::optional<ruvia::http2_request_head_event> pending;
        std::size_t requests = 0;
        std::size_t pings = 0;
        bool silent = false;
        for (;;) {
            std::error_code error;
            if (!silent && connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                // The core answers a client GOAWAY with its own, which may
                // reach a client that already closed.
                co_await asio::async_write(
                    *socket_, asio::buffer(output), asio::redirect_error(asio::use_awaitable, error));
            }
            std::size_t count = 0;
            if (!error) {
                count = co_await socket_->async_read_some(
                    asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            if (error) {
                idle_for_ = std::chrono::steady_clock::now() - responded_;
                co_return;
            }
            if (silent) {
                continue;
            }
            const auto bytes = std::string_view(input.data(), count);
            if (pending) {
                pings += count_pings(bytes);
            }
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid HTTP/2 liveness peer input");
                }
                while (auto event = connection.next_event()) {
                    if (event->goaway() != nullptr) {
                        goaway_received_ = true;
                    }
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    ++requests;
                    switch (mode_) {
                        case mode_type::stall_first_request:
                            if (requests == 1) {
                                stalled.push_back(std::move(*request));
                            } else {
                                respond(connection, std::move(*request));
                            }
                            break;
                        case mode_type::answer:
                            respond(connection, std::move(*request));
                            break;
                        case mode_type::answer_after_pings:
                            pending.emplace(std::move(*request));
                            break;
                        case mode_type::silent_after_request:
                            stalled.push_back(std::move(*request));
                            silent = true;
                            break;
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
            if (pending && pings >= 2) {
                respond(connection, std::move(*pending));
                pending.reset();
            }
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer guard_;
    ruvia::worker_signal done_;
    asio::io_context& io_;
    mode_type mode_;
    std::optional<asio::ip::tcp::socket> socket_;
    std::chrono::steady_clock::time_point responded_{std::chrono::steady_clock::now()};
    std::exception_ptr failure_;
    bool finished_{false};
};

[[nodiscard]] ruvia::http_client_config local_http2_client_config(std::string_view base_url) {
    auto config = local_http_client_config(base_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(20);
    return config;
}

#endif

[[nodiscard]] std::string_view response_text(const ruvia::http_client_response_bytes& bytes_value) noexcept {
    return {reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()};
}
}  // namespace

#if defined(RUVIA_ENABLE_HTTP2)
// The HTTP/2 read inactivity bound resets only the stalled stream: the next
// request reuses the same session.
TEST(Ruvia, http2_client_read_timeout_resets_the_stalled_stream_and_keeps_the_session) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::stall_first_request);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::error_code> code;
        try {
            (void)co_await client.client().get("/stall").send();
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
        }
        EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
        // The peer accepts one connection only.
        auto response = co_await client.client().get("/ok").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        co_await client.shutdown();
        co_await peer.wait();
    };
    run_operation(worker, io, operation);
}

// idle_timeout_ closes an HTTP/2 connection once its last stream is gone,
// with GOAWAY before the close (RFC 9113 section 6.8).
TEST(Ruvia, http2_client_idle_timeout_closes_a_connection_without_streams) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::answer);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.idle_timeout_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/ok").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        // Only the idle bound closes the connection before the guard does.
        co_await peer.wait();
        EXPECT_TRUE(peer.idle_for_ < std::chrono::seconds(5));
        EXPECT_TRUE(peer.goaway_received_);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// http2_ping_interval_: a connection that stays silent for an interval gets a
// PING, and one that stays silent for another fails with timeout.
TEST(Ruvia, http2_client_ping_fails_a_silent_connection) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::silent_after_request);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::nullopt;
    config.http2_ping_interval_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::error_code> code;
        try {
            (void)co_await client.client().get("/silent").send();
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
        }
        EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
        EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
        // The failed session closed its transport.
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// A peer that acknowledges the liveness PINGs keeps a quiet connection alive
// beyond two intervals.
TEST(Ruvia, http2_client_ping_ack_keeps_a_quiet_connection_alive) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_liveness_peer peer(io, worker.handle_, http2_liveness_peer::mode_type::answer_after_pings);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http2_client_config(config_url);
    config.read_timeout_ = std::nullopt;
    config.http2_ping_interval_ = std::chrono::milliseconds(100);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/slow").send();
        EXPECT_EQ(response_text(co_await response.stream().read_all()), "ok");
        co_await client.shutdown();
        co_await peer.wait();
    };
    run_operation(worker, io, operation);
}

#endif

namespace http_client_pool_test {

// Completion, progress and gating of one scripted origin coroutine, observed
// from the client's worker. Its guard closes the acceptor and every connection
// the script registered, so a client that stops early cannot leave it pending.
class scripted_origin final {
public:
    scripted_origin(asio::io_context& io, const ruvia::worker_ref& worker_value)
        : io_(io),
          acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          guard_(io),
          gate_(io),
          progress_(worker_value) {
        gate_.expires_at(asio::steady_timer::time_point::max());
    }
    scripted_origin(asio::io_context&, ruvia::worker_ref&&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    [[nodiscard]] asio::ip::tcp::acceptor& acceptor() noexcept {
        return acceptor_;
    }

    void start(asio::awaitable<void> script) {
        guard_.expires_after(std::chrono::seconds(10));
        guard_.async_wait([this](const std::error_code& error) {
            if (error || finished_) {
                return;
            }
            std::error_code ignored;
            acceptor_.close(ignored);
            for (auto* socket : sockets_) {
                socket->close(ignored);
            }
        });
        asio::co_spawn(io_, std::move(script), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            finished_ = true;
            guard_.cancel();
            progress_.notify();
        });
    }

    // Script side.
    void watch(asio::ip::tcp::socket::lowest_layer_type& socket) {
        sockets_.push_back(&socket);
    }
    void advance() noexcept {
        ++step_;
        progress_.notify();
    }
    [[nodiscard]] asio::awaitable<void> wait_for_release() {
        std::error_code ignored;
        co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        gate_.expires_at(asio::steady_timer::time_point::max());
    }

    // Client side.
    void release() {
        gate_.expires_at(asio::steady_timer::time_point::min());
    }
    [[nodiscard]] ruvia::task<void> reach(std::size_t step) {
        while (step_ < step && !finished_) {
            co_await progress_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (step_ < step) {
            throw std::runtime_error("scripted origin ended early");
        }
    }
    [[nodiscard]] ruvia::task<void> join() {
        while (!finished_) {
            co_await progress_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    void stop_accepting() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }

private:
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer guard_;
    asio::steady_timer gate_;
    ruvia::worker_signal progress_;
    std::vector<asio::ip::tcp::socket::lowest_layer_type*> sockets_{};
    std::exception_ptr failure_;
    std::size_t step_{0};
    bool finished_{false};
};

#if defined(RUVIA_ENABLE_HTTP2)
// One server connection of a scripted HTTP/2 origin over TCP or TLS, driven by
// the sans-I/O core, which answers SETTINGS and PING itself. Request heads
// queue in arrival order.
template <typename stream_type>
class http2_origin_connection final {
public:
    http2_origin_connection(scripted_origin& origin, stream_type stream)
        : stream_(std::move(stream)) {
        origin.watch(stream_.lowest_layer());
    }

    [[nodiscard]] ruvia::http2_connection& connection() noexcept {
        return connection_;
    }
    [[nodiscard]] bool goaway_received() const noexcept {
        return goaway_received_;
    }

    // Submit capacity settings through the protocol engine so its ACK accounting
    // stays synchronized with the peer, then write every pending frame.
    [[nodiscard]] asio::awaitable<bool> flush(std::optional<std::uint8_t> stream_limit = std::nullopt) {
        if (stream_limit && connection_.submit_max_concurrent_streams(*stream_limit) !=
                                ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 scripted origin settings rejected");
        }
        ruvia::test::drain_output(connection_, output_);
        if (output_.empty()) {
            co_return true;
        }
        std::error_code error;
        co_await asio::async_write(stream_, asio::buffer(output_), asio::redirect_error(asio::use_awaitable, error));
        co_return !error;
    }
    [[nodiscard]] asio::awaitable<ruvia::http2_request_head_event> next_request() {
        while (requests_.empty()) {
            if (!co_await read()) {
                throw std::runtime_error("client ended the scripted HTTP/2 connection before a request");
            }
        }
        auto request = std::move(requests_.front());
        requests_.pop_front();
        co_return request;
    }
    // Reads until the drain this side started is final and written.
    [[nodiscard]] asio::awaitable<void> finish_drain() {
        while (!connection_.drain_final() || connection_.wants_write()) {
            if (!co_await read()) {
                throw std::runtime_error("client ended the scripted HTTP/2 connection during its drain");
            }
        }
    }
    [[nodiscard]] asio::awaitable<void> read_until_end() {
        while (co_await read()) {
        }
    }
    void respond(ruvia::http2_request_head_event&& request, std::string_view body) {
        const auto stream_id = request.stream_id();
        const auto built = request.build_request();
        ruvia::http_response response;
        response.body(body);
        const auto head = connection_.submit_response_head(
            stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) ||
            connection_.submit_data(stream_id, response.body_bytes(), ruvia::http2_end_stream::end_stream, 65535).status_ !=
                ruvia::http2_data_submit_status::complete ||
            connection_.release(std::move(request)) != ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("scripted HTTP/2 response rejected");
        }
    }
    void refuse(ruvia::http2_request_head_event&& request, ruvia::http2_error_code error) {
        if (connection_.submit_reset(request.stream_id(), error) != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("scripted HTTP/2 reset rejected");
        }
        (void)connection_.release(std::move(request));
    }
    // An abrupt end of the connection, without GOAWAY.
    void close() {
        std::error_code ignored;
        stream_.lowest_layer().close(ignored);
    }
    // Sends FIN; reading continues until the client closes.
    void end_output() {
        std::error_code ignored;
        stream_.lowest_layer().shutdown(asio::socket_base::shutdown_send, ignored);
    }

private:
    // One read: feeds the core, queues request heads and writes its answers.
    [[nodiscard]] asio::awaitable<bool> read() {
        if (!co_await flush()) {
            co_return false;
        }
        std::error_code error;
        const auto count = co_await stream_.async_read_some(
            asio::buffer(input_), asio::redirect_error(asio::use_awaitable, error));
        if (error) {
            co_return false;
        }
        const auto bytes = std::string_view(input_.data(), count);
        for (;;) {
            const auto status = connection_.feed(bytes);
            if (status == ruvia::http2_feed_result::protocol_failure) {
                throw std::runtime_error("invalid client HTTP/2 input");
            }
            while (auto event = connection_.next_event()) {
                if (event->goaway() != nullptr) {
                    goaway_received_ = true;
                }
                if (auto* request = event->request_head()) {
                    requests_.push_back(std::move(*request));
                }
            }
            if (status != ruvia::http2_feed_result::events_pending) {
                break;
            }
        }
        co_return co_await flush();
    }

    stream_type stream_;
    ruvia::http2_connection connection_{ruvia::http2_connection::server()};
    std::array<char, 16384> input_{};
    std::string output_;
    std::deque<ruvia::http2_request_head_event> requests_{};
    bool goaway_received_{false};
};

using tcp_origin_connection = http2_origin_connection<asio::ip::tcp::socket>;

[[nodiscard]] ruvia::http_client_config http2_client_config(std::string_view base_url) {
    auto config = local_http_client_config(base_url);
    config.connection_limits_.max_open_ = 1;
    config.versions_ = ruvia::http_client_versions{.http1_ = false};
    config.request_timeout_ = std::chrono::seconds(5);
    return config;
}

#endif

[[nodiscard]] ruvia::task<void> send_and_discard(ruvia::http_client& client, std::string_view target) {
    try {
        auto response = co_await client.client().get(target).send();
        (void)co_await response.stream().read_all();
    } catch (const ruvia::http_client_error&) {
        // Ended by the client's shutdown.
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
[[nodiscard]] ruvia::task<void> send_and_keep(
    ruvia::http_client& client, std::string_view target, std::optional<std::string>& body, bool& done,
    ruvia::worker_signal& done_signal) {
    try {
        auto response = co_await client.client().get(target).send();
        const auto bytes_value = co_await response.stream().read_all();
        body.emplace(response_text(bytes_value));
    } catch (const ruvia::http_client_error&) {
        // `body` stays unset; the caller's check reports it.
    }
    done = true;
    done_signal.notify();
}

#endif

// Runs `operation`, then always shuts the client down and joins the requests
// it spawned into `requests` and `origin`, so a failed check cannot leave the
// origin, the client or a request pending.
template <typename operation_type>
[[nodiscard]] ruvia::task<void> run_against(ruvia::http_client& client, scripted_origin& origin,
    ruvia::task_scope& requests, operation_type&& operation) {
    std::exception_ptr failure;
    try {
        co_await operation();
    } catch (...) {
        failure = std::current_exception();
    }
    origin.stop_accepting();
    co_await client.shutdown();
    try {
        co_await requests.join();
        co_await origin.join();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace http_client_pool_test

#if defined(RUVIA_ENABLE_HTTP2)
// A draining HTTP/2 session never holds a new request beyond acquire_timeout_,
// even without a request timeout: the wait for the slot's reconnect is a
// connection acquisition (Pingora never reuses a shutting-down connection).
TEST(Ruvia, http2_client_draining_session_bounds_a_new_request_by_acquire_timeout) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection first(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await first.flush();
        // The held request stays open across the drain.
        [[maybe_unused]] const auto held = co_await first.next_request();
        first.connection().begin_drain();
        co_await first.finish_drain();
        origin.advance();
        co_await first.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.request_timeout_ = std::nullopt;
    config.acquire_timeout_ = std::chrono::milliseconds(200);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_discard(client, "/hold"));
            co_await origin.reach(1);
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                (void)co_await client.client().with_options({.timeout_ = std::chrono::seconds(5)}).get("/next").send();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
        });
    };
    run_operation(worker, io, operation);
}

// A request routed to a draining HTTP/2 session uses a free, unconnected slot
// instead of waiting for the drain. With two connections of two streams each,
// leases alternate between the slots: /b and /c complete in that order, so
// /next takes the lease of the draining slot.
TEST(Ruvia, http2_client_draining_session_yields_to_a_free_slot) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection draining(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await draining.flush();
        [[maybe_unused]] const auto held = co_await draining.next_request();
        origin.advance();
        // Requests share the session while its two streams last.
        auto c = co_await draining.next_request();
        origin.advance();
        tcp_origin_connection closing(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await closing.flush();
        auto b = co_await closing.next_request();
        origin.advance();
        closing.respond(std::move(b), "b");
        (void)co_await closing.flush();
        // The peer ends this connection: its slot becomes free and unconnected.
        closing.end_output();
        co_await origin.wait_for_release();
        draining.respond(std::move(c), "c");
        (void)co_await draining.flush();
        draining.connection().begin_drain();
        co_await draining.finish_drain();
        origin.advance();
        tcp_origin_connection fresh(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await fresh.flush();
        fresh.respond(co_await fresh.next_request(), "next");
        (void)co_await fresh.flush();
        co_await closing.read_until_end();
        co_await fresh.read_until_end();
        co_await draining.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.max_concurrent_http2_streams_per_connection_ = 2;
    config.acquire_timeout_ = std::chrono::milliseconds(300);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    std::optional<std::string> b_body;
    std::optional<std::string> c_body;
    bool b_done = false;
    bool c_done = false;
    ruvia::worker_signal done_signal(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_discard(client, "/hold"));
            co_await origin.reach(1);
            requests.spawn(send_and_keep(client, "/c", c_body, c_done, done_signal));
            co_await origin.reach(2);
            requests.spawn(send_and_keep(client, "/b", b_body, b_done, done_signal));
            co_await origin.reach(3);
            while (!b_done) {
                co_await done_signal.wait();
            }
            // Its driver releases the lease after the body completed.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(50));
            origin.release();
            while (!c_done) {
                co_await done_signal.wait();
            }
            co_await origin.reach(4);
            // The ended connection's session retires meanwhile.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            EXPECT_TRUE(b_body == std::optional<std::string>(std::string("b")));
            EXPECT_TRUE(c_body == std::optional<std::string>(std::string("c")));
            auto next = co_await client.client().get("/next").send();
            EXPECT_EQ(response_text(co_await next.stream().read_all()), "next");
        });
    };
    run_operation(worker, io, operation);
}

// An idempotent request whose reused HTTP/2 connection fails at the transport
// after its head was sent is retried once on a new connection, like an HTTP/1
// exchange (Pingora RetryType::ReusedOnly); a non-idempotent one is not.
TEST(Ruvia, http2_client_retries_only_idempotent_requests_after_a_reused_connection_fails) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    unsigned connections = 0;
    const auto script = [&]() -> asio::awaitable<void> {
        {
            tcp_origin_connection first(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
            ++connections;
            (void)co_await first.flush();
            first.respond(co_await first.next_request(), "prime");
            (void)co_await first.flush();
            (void)co_await first.next_request();
            // No GOAWAY: an L4 middlebox reset, for example.
            first.close();
        }
        tcp_origin_connection second(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        ++connections;
        (void)co_await second.flush();
        second.respond(co_await second.next_request(), "retried");
        (void)co_await second.flush();
        (void)co_await second.next_request();
        second.close();
        std::error_code accept_error;
        [[maybe_unused]] auto extra =
            co_await origin.acceptor().async_accept(asio::redirect_error(asio::use_awaitable, accept_error));
        if (!accept_error) {
            ++connections;
        }
    };
    origin.start(script());
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    ruvia::http_client client(worker.attachment_.loop(), http2_client_config(client_url));
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            auto prime = co_await client.client().get("/prime").send();
            EXPECT_EQ(response_text(co_await prime.stream().read_all()), "prime");
            auto retried = co_await client.client().get("/retry").send();
            EXPECT_EQ(response_text(co_await retried.stream().read_all()), "retried");
            std::optional<std::error_code> post_failure;
            try {
                (void)co_await client.client().request("POST", "/post").send();
            } catch (const ruvia::http_client_error& error) {
                post_failure = error.code();
            }
            EXPECT_TRUE(post_failure == ruvia::make_error_code(ruvia::http_client_errc::io_error));
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.completed_requests_, std::size_t{2});
            EXPECT_EQ(stats.failed_requests_, std::size_t{1});
        });
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

// A peer SETTINGS_MAX_CONCURRENT_STREAMS below the local limit parks a request
// for stream capacity at most acquire_timeout_, even without a request
// timeout; a parked request proceeds once a stream closes.
TEST(Ruvia, http2_client_stream_capacity_wait_is_an_acquisition) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_origin origin(io, worker.handle_);
    const auto script = [&]() -> asio::awaitable<void> {
        tcp_origin_connection only(origin, co_await origin.acceptor().async_accept(asio::use_awaitable));
        (void)co_await only.flush(std::uint8_t{1});
        auto held = co_await only.next_request();
        origin.advance();
        co_await origin.wait_for_release();
        only.respond(std::move(held), "held");
        (void)co_await only.flush();
        only.respond(co_await only.next_request(), "after");
        (void)co_await only.flush();
        co_await only.read_until_end();
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
    auto config = http2_client_config(config_url);
    config.request_timeout_ = std::nullopt;
    config.acquire_timeout_ = std::chrono::milliseconds(500);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    std::optional<std::string> held_body;
    std::optional<std::string> after_body;
    bool held_done = false;
    bool after_done = false;
    ruvia::worker_signal done_signal(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            requests.spawn(send_and_keep(client, "/held", held_body, held_done, done_signal));
            co_await origin.reach(1);
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                (void)co_await client.client().with_options({.timeout_ = std::chrono::seconds(5)}).get("/blocked").send();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
            requests.spawn(send_and_keep(client, "/after", after_body, after_done, done_signal));
            // Parked for capacity before the held stream closes.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(50));
            origin.release();
            while (!held_done || !after_done) {
                co_await done_signal.wait();
            }
            EXPECT_TRUE(held_body == std::optional<std::string>(std::string("held")));
            EXPECT_TRUE(after_body == std::optional<std::string>(std::string("after")));
        });
    };
    run_operation(worker, io, operation);
}

#endif

// Pingora's idle_poll: a pooled HTTP/1 connection closes as soon as its peer
// ends it or sends unsolicited bytes, even without idle_timeout_.
TEST(Ruvia, http1_client_closes_an_idle_connection_its_peer_ended_or_wrote_to) {
    for (const bool peer_writes : {false, true}) {
        using namespace http_client_pool_test;
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        scripted_origin origin(io, worker.handle_);
        std::optional<std::chrono::steady_clock::duration> closed_after;
        const auto script = [&]() -> asio::awaitable<void> {
            auto socket = co_await origin.acceptor().async_accept(asio::use_awaitable);
            origin.watch(socket.lowest_layer());
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst";
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            origin.advance();
            // Lets the client pool the connection first.
            asio::steady_timer pause(socket.get_executor(), std::chrono::milliseconds(50));
            co_await pause.async_wait(asio::use_awaitable);
            const auto acted = std::chrono::steady_clock::now();
            if (peer_writes) {
                co_await asio::async_write(socket, asio::buffer(std::string_view("X")), asio::use_awaitable);
            } else {
                socket.shutdown(asio::socket_base::shutdown_send);
            }
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
                closed_after = std::chrono::steady_clock::now() - acted;
            }
        };
        origin.start(script());
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
        auto config = local_http_client_config(config_url);
        config.connection_limits_.max_open_ = 1;
        config.idle_timeout_ = std::nullopt;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::task_scope requests(worker.handle_);
        auto operation = [&]() -> ruvia::task<void> {
            co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
                auto response = co_await client.client().get("/first").send();
                EXPECT_EQ(response_text(co_await response.stream().read_all()), "first");
                // The origin ends only once the client closed the connection;
                // its guard would close it after 10 seconds.
                co_await origin.join();
                EXPECT_TRUE(closed_after.has_value() && *closed_after < std::chrono::seconds(2));
                EXPECT_EQ(client.client().stats().idle_connections_with_pending_data_,
                    peer_writes ? std::size_t{1} : std::size_t{0});
            });
        };
        run_operation(worker, io, operation);
    }
}

#if defined(RUVIA_ENABLE_HTTP2)
// RFC 9113 section 7: RST_STREAM(HTTP_1_1_REQUIRED) before any response head
// makes a negotiate client retry the request over HTTP/1.1, and every later
// connection offers only http/1.1 (Pingora's prefer_h1).
TEST(Ruvia, http_client_retries_over_http11_after_http11_required) {
    using namespace http_client_pool_test;
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    // Whether each handshake offered h2; the origin selects it when offered.
    std::vector<bool> offered_h2;
    SSL_CTX_set_alpn_select_cb(
        identity.context_.native_handle(),
        [](SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
            void* arg) -> int {
            bool h2 = false;
            const unsigned char* http11 = nullptr;
            for (unsigned int pos = 0; pos < inlen;) {
                const unsigned int len = in[pos++];
                if (pos + len > inlen) {
                    break;
                }
                const std::string_view protocol(reinterpret_cast<const char*>(in + pos), len);
                if (protocol == "h2") {
                    h2 = true;
                    *out = in + pos;
                    *outlen = static_cast<unsigned char>(len);
                } else if (protocol == "http/1.1") {
                    http11 = in + pos;
                }
                pos += len;
            }
            static_cast<std::vector<bool>*>(arg)->push_back(h2);
            if (h2) {
                return SSL_TLSEXT_ERR_OK;
            }
            if (http11 != nullptr) {
                *out = http11;
                *outlen = 8;
                return SSL_TLSEXT_ERR_OK;
            }
            return SSL_TLSEXT_ERR_NOACK;
        },
        &offered_h2);
    scripted_origin origin(io, worker.handle_);
    using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;
    const auto script = [&]() -> asio::awaitable<void> {
        {
            tls_stream stream(co_await origin.acceptor().async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            http2_origin_connection<tls_stream> refusing(origin, std::move(stream));
            (void)co_await refusing.flush();
            refusing.refuse(co_await refusing.next_request(), ruvia::http2_error_code::http11_required);
            (void)co_await refusing.flush();
            co_await refusing.read_until_end();
        }
        tls_stream stream(co_await origin.acceptor().async_accept(asio::use_awaitable), identity.context_);
        origin.watch(stream.lowest_layer());
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        for (const std::string_view body : {std::string_view("h1-ok"), std::string_view("h1-again")}) {
            std::string request{};
            co_await asio::async_read_until(stream, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            const std::string response =
                std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
        std::array<char, 64> input{};
        std::error_code error;
        while (!error) {
            (void)co_await stream.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        }
    };
    origin.start(script());
    const std::string config_url = std::move(std::string("https://localhost:").append(std::to_string(origin.port())));
    auto config = local_http_client_config(config_url);
    config.tls_.ca_file_ = identity.ca_file_;
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(5);
    ruvia::http_client client(worker.attachment_.loop(), config);
    ruvia::task_scope requests(worker.handle_);
    auto operation = [&]() -> ruvia::task<void> {
        co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
            auto first = co_await client.client().get("/first").send();
            EXPECT_TRUE(first.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_EQ(response_text(co_await first.stream().read_all()), "h1-ok");
            auto second = co_await client.client().get("/second").send();
            EXPECT_TRUE(second.protocol_version() == ruvia::http_protocol_version::http11);
            EXPECT_EQ(response_text(co_await second.stream().read_all()), "h1-again");
        });
    };
    run_operation(worker, io, operation);
    SSL_CTX_set_alpn_select_cb(identity.context_.native_handle(), nullptr, nullptr);
    EXPECT_TRUE(offered_h2 == std::vector<bool>({true, false}));
}
#endif
