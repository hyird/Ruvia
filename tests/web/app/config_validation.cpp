
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/app.h"

#include "test_harness.h"

namespace {

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

RUVIA_TEST(app_http_client_rejects_invalid_hosts_with_distinct_messages) {
    const auto register_client = [](ruvia::http_client_config config) {
        return caught_message([&config] {
            ruvia::app().http_client({.alias_ = "config-validation", .config_ = std::move(config)});
        });
    };
    const auto with_host = [](std::string_view host) {
        ruvia::http_client_config config;
        config.host_ = host;
        return config;
    };

    RUVIA_CHECK_EQ(register_client(with_host("")), std::string("http client host must not be empty"));
    for (const std::string_view host : {std::string_view("has space"), std::string_view("a/b"),
             std::string_view("a\\b"), std::string_view("a\rb", 3), std::string_view("a\x7f"
                                                                                     "b",
                                                                        3),
             std::string_view("[::1]"), std::string_view("host:80")}) {
        RUVIA_CHECK_EQ(register_client(with_host(host)), std::string("http client host is invalid"));
    }

    auto tls_config = with_host("example.com");
    tls_config.tls_.server_name_ = "";
    RUVIA_CHECK_EQ(register_client(std::move(tls_config)), std::string("TLS server name must not be empty"));
    tls_config = with_host("example.com");
    tls_config.tls_.server_name_ = "host:80";
    RUVIA_CHECK_EQ(register_client(std::move(tls_config)), std::string("TLS server name is invalid"));
    // The override replaces the origin host for SNI and peer verification, so
    // it must be an IP literal or an ASCII DNS name.
    for (const std::string_view server_name : {std::string_view("a::b::c"), std::string_view("b\xc3\xbc"
                                                                                             "cher.example"),
             std::string_view("-leading.example")}) {
        tls_config = with_host("example.com");
        tls_config.tls_.server_name_ = std::string(server_name);
        RUVIA_CHECK_EQ(register_client(std::move(tls_config)), std::string("TLS server name is invalid"));
    }
    std::string long_name;
    for (unsigned label = 0; label != 5; ++label) {
        long_name.append(std::string(60, 'a')).push_back('.');
    }
    long_name.append("example");
    tls_config = with_host("example.com");
    tls_config.tls_.server_name_ = long_name;
    RUVIA_CHECK_EQ(register_client(std::move(tls_config)), std::string("TLS server name is invalid"));
}

RUVIA_TEST(client_tls_rejects_unsafe_outgoing_record_limits) {
    const auto rejects = [](std::uint64_t limit) {
        ruvia::http_client_config config;
        config.host_ = "example.com";
        config.tls_.max_outgoing_records_ = limit;
        return throws_invalid([&config] {
            ruvia::app().http_client({.alias_ = "tls-record-limit", .config_ = std::move(config)});
        });
    };
    RUVIA_CHECK(rejects(0));
    RUVIA_CHECK(rejects((std::uint64_t{1} << 23) + 1));
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
    disabled_refresh.refresh_interval_ = std::chrono::milliseconds::zero();
    RUVIA_CHECK(throws_invalid(
        [&disabled_refresh] { ruvia::app().document_root(std::move(disabled_refresh)); }));
}

RUVIA_TEST(app_compression_rejects_invalid_thresholds_at_configuration) {
    RUVIA_CHECK(
        throws_invalid([] { ruvia::app().compression({.min_bytes_ = 1024, .sync_bytes_ = 512}); }));
    RUVIA_CHECK(throws_invalid(
        [] { ruvia::app().compression({.min_bytes_ = 1024, .sync_bytes_ = 2048, .max_bytes_ = 1024}); }));
}
