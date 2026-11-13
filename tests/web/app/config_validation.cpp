#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"
#include "ruvia/web/streaming.h"

#include "http_server_fixture.h"
#include "worker_task_fixture.h"

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
