#include "ruvia/core/config_validation.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/app.h"

#include "test_harness.h"

namespace {

using ruvia::ensure_config_host;
using ruvia::ensure_non_zero_port;
using ruvia::ensure_positive_duration;
using ruvia::ensure_positive_size;
using ruvia::is_valid_config_host;
using ruvia::separated_port_host_rules;

// Returns the invalid_argument message a call throws, or empty if it does not.
template <typename fn_type>
std::string caught_message(fn_type&& fn) {
    try {
        fn();
        return {};
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
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

RUVIA_TEST(app_http3_mode_normalizes_against_https_and_validates_effective_config) {
    auto& app = ruvia::app();
    app.server({});
    app.listen({.address_ = "127.0.0.1", .http_ = 8080});
    app.server({.max_connections_per_worker_ = std::nullopt});

    // Automatic mode remains off for HTTP-only listeners, even without an H3
    // connection cap.
    RUVIA_CHECK(!throws_invalid([&] {
        app.listen({.address_ = "127.0.0.1", .http_ = 8081});
    }));

    const auto https_config = [](ruvia::http3_listen_config http3 = {}) {
        return ruvia::listen_config{
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
            .http3_ = http3,
        };
    };

    // Automatic mode enables H3 on HTTPS, so the existing finite-capacity
    // requirement still applies.
    RUVIA_CHECK(throws_invalid([&] { app.listen(https_config()); }));
    RUVIA_CHECK(throws_invalid([&] {
        app.listen({.address_ = "127.0.0.1",
            .http_ = 8082,
            .http3_ = {.mode_ = ruvia::http3_mode::enabled}});
    }));

    // Disabled mode suppresses H3 even on HTTPS and does not validate unused
    // H3 timeout values.
    RUVIA_CHECK(!throws_invalid([&] {
        app.listen(https_config({.mode_ = ruvia::http3_mode::disabled,
            .handshake_timeout_ = std::chrono::milliseconds::zero(),
            .drain_timeout_ = std::chrono::milliseconds::zero()}));
    }));
    RUVIA_CHECK(throws_invalid([&] {
        app.listen(https_config({.mode_ = ruvia::http3_mode::enabled}));
    }));

    app.server({});
    RUVIA_CHECK(!throws_invalid([&] {
        app.listen(https_config({.handshake_timeout_ = std::chrono::milliseconds{25},
            .drain_timeout_ = std::chrono::milliseconds{50}}));
    }));
    RUVIA_CHECK(!throws_invalid([&] {
        app.listen(https_config({.mode_ = ruvia::http3_mode::enabled,
            .handshake_timeout_ = std::chrono::milliseconds{25},
            .drain_timeout_ = std::chrono::milliseconds{50}}));
    }));
    RUVIA_CHECK(throws_invalid([&] {
        app.listen(https_config({.handshake_timeout_ = std::chrono::milliseconds::zero()}));
    }));
    RUVIA_CHECK(throws_invalid([&] {
        app.listen(https_config({.drain_timeout_ = std::chrono::milliseconds::zero()}));
    }));

    RUVIA_CHECK(throws_invalid([&] {
        app.listen({.address_ = "127.0.0.1",
            .http_ = 8083,
            .http3_ = {.mode_ = static_cast<ruvia::http3_mode>(0xFF)}});
    }));

    app.server({});
    app.listen({.address_ = "127.0.0.1", .http_ = 8080});
}

RUVIA_TEST(config_host_validation_default_rules) {
    RUVIA_CHECK(is_valid_config_host("localhost"));
    RUVIA_CHECK(is_valid_config_host("0.0.0.0"));
    RUVIA_CHECK(is_valid_config_host("example.com"));
    RUVIA_CHECK(is_valid_config_host("::1"));
    RUVIA_CHECK(!is_valid_config_host(""));                           // empty
    RUVIA_CHECK(!is_valid_config_host("has space"));                  // control/space bytes
    RUVIA_CHECK(!is_valid_config_host("a/b"));                        // '/'
    RUVIA_CHECK(!is_valid_config_host("a\\b"));                       // '\\'
    RUVIA_CHECK(!is_valid_config_host(std::string_view("a\rb", 3)));  // CR
    RUVIA_CHECK(
        !is_valid_config_host(std::string_view("a\x7f"
                                               "b",
            3)));  // DEL
}

RUVIA_TEST(config_host_validation_separated_port_rules) {
    // For a "host:port" style listen address, brackets and a single colon are
    // disallowed (the colon separates the port).
    RUVIA_CHECK(!is_valid_config_host("[::1]", separated_port_host_rules));    // brackets rejected
    RUVIA_CHECK(!is_valid_config_host("host:80", separated_port_host_rules));  // single colon rejected
    RUVIA_CHECK(is_valid_config_host("host", separated_port_host_rules));      // bare host is fine
    RUVIA_CHECK(is_valid_config_host("::1", separated_port_host_rules));       // two colons is not "single"
}

RUVIA_TEST(config_size_port_duration_guards) {
    RUVIA_CHECK(throws_invalid([] { ensure_positive_size(0, "size"); }));
    RUVIA_CHECK(!throws_invalid([] { ensure_positive_size(1, "size"); }));

    RUVIA_CHECK(throws_invalid([] { ensure_non_zero_port(0, "port"); }));
    RUVIA_CHECK(!throws_invalid([] { ensure_non_zero_port(8080, "port"); }));

    using namespace std::chrono;
    // Positive means strictly greater than zero.
    RUVIA_CHECK(throws_invalid([] { ensure_positive_duration(seconds(0), "d"); }));
    RUVIA_CHECK(!throws_invalid([] { ensure_positive_duration(milliseconds(1), "d"); }));
}

RUVIA_TEST(config_ensure_host_throws_distinct_messages) {
    // An empty host reports the empty message; an invalid host reports the
    // invalid message; a valid host does not throw.
    RUVIA_CHECK_EQ(caught_message([] { ensure_config_host("", "was-empty", "was-invalid"); }),
        std::string("was-empty"));
    RUVIA_CHECK_EQ(caught_message([] { ensure_config_host("bad host", "was-empty", "was-invalid"); }),
        std::string("was-invalid"));
    RUVIA_CHECK(
        caught_message([] { ensure_config_host("example.com", "was-empty", "was-invalid"); }).empty());
}

RUVIA_TEST(app_document_root_rejects_invalid_static_options_at_configuration) {
    ruvia::document_root_config config;
    config.root_ = "public";
    config.static_options_.cache_control_ = " private";

    RUVIA_CHECK(throws_invalid([&config] { ruvia::app().document_root(std::move(config)); }));
}

RUVIA_TEST(app_document_root_rejects_disabled_refresh) {
    ruvia::document_root_config disabled_refresh;
    disabled_refresh.root_ = "public";
    disabled_refresh.runtime_.refresh_interval_ = std::chrono::milliseconds::zero();
    RUVIA_CHECK(throws_invalid(
        [&disabled_refresh] { ruvia::app().document_root(std::move(disabled_refresh)); }));
}

RUVIA_TEST(app_compression_rejects_invalid_thresholds_at_configuration) {
    RUVIA_CHECK(
        throws_invalid([] { ruvia::app().compression({.min_bytes_ = 1024, .sync_bytes_ = 512}); }));
    RUVIA_CHECK(throws_invalid(
        [] { ruvia::app().compression({.min_bytes_ = 1024, .sync_bytes_ = 2048, .max_bytes_ = 1024}); }));
}
