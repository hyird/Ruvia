#include "ruvia/core/config_validation.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <openssl/ssl.h>

#include "ruvia/web/app.h"
#include "ruvia/web/http_client_types.h"

#include "app/app_listener_options.h"
#include "app/app_state.h"
#include "client/client_transport.h"
#include "client/http_client_config_storage.h"
#include "client/websocket_client_config_storage.h"
#include "db/db_config_storage.h"
#include "failing_memory_resource.h"
#include "memory_resource_fixture.h"
#include "redis/redis_config_storage.h"
#include "server/http_server_listener.h"
#include "server/http_server_options.h"
#include "server/http_server_options_validation.h"
#include "test_harness.h"
#include "tls/tls_host.h"

namespace {

using ruvia::test::counting_memory_resource;

using ruvia::ensure_config_host;
using ruvia::ensure_non_zero_port;
using ruvia::ensure_positive_duration;
using ruvia::ensure_positive_size;
using ruvia::is_valid_config_host;
using ruvia::separated_port_host_rules;
using ruvia::detail::client_port_text_buffer_type;
using ruvia::detail::client_transport_config_view;
using ruvia::detail::format_client_port;
using ruvia::detail::http_server_listener_definition;
using ruvia::detail::http_server_options;
using ruvia::detail::is_valid_sni_host;
using ruvia::detail::make_client_transport_config_view;
using ruvia::detail::normalize_alt_svc_advertisement;
using ruvia::detail::validate_client_origin_host;
using ruvia::detail::validate_client_transport_config;
using ruvia::detail::validate_http_server_configuration;
using ruvia::detail::validate_http_server_listener;

http_server_listener_definition http3_tls_listener(
    std::uint16_t port, std::chrono::milliseconds handshake_timeout = std::chrono::seconds(10),
    std::chrono::milliseconds drain_timeout = std::chrono::seconds(30)) {
    http_server_listener_definition::tls_type tls;
    tls.identity_.certificate_chain_file_ = "cert.pem";
    tls.identity_.private_key_file_ = "key.pem";
    return http_server_listener_definition(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), std::move(tls),
        ruvia::http3_listen_config{.mode_ = ruvia::http3_mode::enabled,
            .handshake_timeout_ = handshake_timeout,
            .drain_timeout_ = drain_timeout});
}

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

template <typename storage_type, typename config_type, typename verify_type>
void verify_config_rebinding(ruvia::testing::test_context& ruvia_ctx, const config_type& config, verify_type&& verify) {
    counting_memory_resource source_resource;
    std::optional<storage_type> source_value(std::in_place, config, &source_resource);
    const auto source_allocations = source_resource.allocation_count();
    bool succeeded = false;
    std::size_t failures = 0;
    for (std::size_t allowance = 0; allowance != 64 && !succeeded; ++allowance) {
        failing_memory_resource target_resource;
        std::optional<storage_type> rebound;
        target_resource.fail_after(allowance);
        try {
            rebound.emplace(*source_value, &target_resource);
            succeeded = true;
        } catch (const std::bad_alloc&) {
            ++failures;
        }
        target_resource.allow_allocations();
        RUVIA_CHECK_EQ(source_resource.allocation_count(), source_allocations);
        if (succeeded) {
            source_value.reset();
            RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
            verify(*rebound, &target_resource);
        } else {
            verify(*source_value, &source_resource);
        }
        rebound.reset();
        RUVIA_CHECK_EQ(target_resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(succeeded);
    RUVIA_CHECK(failures >= 2);
}

}  // namespace

RUVIA_TEST(app_state_has_no_implicit_listener) {
    const ruvia::detail::app_state state;
    RUVIA_CHECK(state.listeners_.empty());
}

RUVIA_TEST(server_configuration_rejects_empty_listeners) {
    RUVIA_CHECK_EQ(caught_message([] {
        (void)validate_http_server_configuration(
            std::span<const http_server_listener_definition>{}, http_server_options{});
    }),
        std::string("HTTP server worker requires at least one listener"));
}

RUVIA_TEST(http3_listen_config_defaults_to_automatic_and_accepts_designated_values) {
    const ruvia::listen_config defaults{};
    RUVIA_CHECK_EQ(defaults.http3_.mode_, ruvia::http3_mode::automatic);
    RUVIA_CHECK_EQ(defaults.http3_.handshake_timeout_, std::chrono::seconds{10});
    RUVIA_CHECK_EQ(defaults.http3_.drain_timeout_, std::chrono::seconds{30});

    const ruvia::listen_config configured{.https_ = 8443,
        .http3_ = ruvia::http3_listen_config{
            .mode_ = ruvia::http3_mode::enabled,
            .handshake_timeout_ = std::chrono::milliseconds{25},
            .drain_timeout_ = std::chrono::milliseconds{50},
        }};
    RUVIA_CHECK_EQ(configured.http3_.mode_, ruvia::http3_mode::enabled);
    RUVIA_CHECK_EQ(configured.http3_.handshake_timeout_, std::chrono::milliseconds{25});
    RUVIA_CHECK_EQ(configured.http3_.drain_timeout_, std::chrono::milliseconds{50});
}

RUVIA_TEST(alt_svc_advertisement_normalizes_active_disabled_clear_and_override_modes) {
    std::pmr::monotonic_buffer_resource resource;

    const auto automatic = normalize_alt_svc_advertisement(
        ruvia::alt_svc_config{}, std::uint16_t{443}, &resource);
    RUVIA_CHECK_EQ(automatic, "h3=\":443\"; ma=86400");

    const auto custom_parameters = normalize_alt_svc_advertisement(
        {.max_age_ = std::chrono::seconds{300},
            .persist_ = true,
            .advertised_port_ = std::uint16_t{9443}},
        std::uint16_t{8443}, &resource);
    RUVIA_CHECK_EQ(custom_parameters, "h3=\":9443\"; ma=300; persist=1");

    RUVIA_CHECK(normalize_alt_svc_advertisement(
        ruvia::alt_svc_config{}, std::nullopt, &resource)
            .empty());
    RUVIA_CHECK(normalize_alt_svc_advertisement(
        {.mode_ = ruvia::alt_svc_mode::disabled}, std::uint16_t{443}, &resource)
            .empty());
    RUVIA_CHECK_EQ(normalize_alt_svc_advertisement(
                       {.mode_ = ruvia::alt_svc_mode::clear}, std::nullopt, &resource),
        "clear");
    RUVIA_CHECK(throws_invalid([&] {
        (void)normalize_alt_svc_advertisement(
            {.max_age_ = std::chrono::seconds{-1}}, std::uint16_t{443}, &resource);
    }));
    RUVIA_CHECK(throws_invalid([&] {
        (void)normalize_alt_svc_advertisement(
            {.advertised_port_ = std::uint16_t{0}}, std::uint16_t{443}, &resource);
    }));
    RUVIA_CHECK(throws_invalid([&] {
        (void)normalize_alt_svc_advertisement(
            {.mode_ = static_cast<ruvia::alt_svc_mode>(0xFF)}, std::uint16_t{443}, &resource);
    }));
}

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

RUVIA_TEST(http3_listener_uses_one_tls_endpoint_and_rejects_duplicates) {
    auto listener_value = http3_tls_listener(8443);
    RUVIA_CHECK(std::holds_alternative<http_server_listener_definition::tls_type>(listener_value.transport_));
    RUVIA_CHECK(listener_value.http3_.has_value());
    RUVIA_CHECK_EQ(listener_value.endpoint_.port(), std::uint16_t{8443});
    RUVIA_CHECK(!throws_invalid([&] { validate_http_server_listener(listener_value); }));

    const std::array single{std::move(listener_value)};
    const auto validated = validate_http_server_configuration(single, http_server_options{});
    RUVIA_CHECK_EQ(validated.listeners().size(), std::size_t{1});
    RUVIA_CHECK_EQ(validated.listeners().front().endpoint_.port(), std::uint16_t{8443});

    const std::array duplicate{http3_tls_listener(8443), http3_tls_listener(9443)};
    RUVIA_CHECK(throws_invalid([&] {
        (void)validate_http_server_configuration(duplicate, http_server_options{});
    }));

    http_server_listener_definition plain(asio::ip::tcp::endpoint(
                                              asio::ip::make_address("127.0.0.1"), 8443),
        http_server_listener_definition::plain_http_type{}, ruvia::http3_listen_config{});
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(plain); }));
    RUVIA_CHECK(throws_invalid([&] {
        validate_http_server_listener(http3_tls_listener(8443, std::chrono::milliseconds::zero()));
    }));
    RUVIA_CHECK(throws_invalid([&] {
        validate_http_server_listener(http3_tls_listener(
            8443, std::chrono::seconds(10), std::chrono::milliseconds::zero()));
    }));
}

RUVIA_TEST(http3_server_limits_bound_every_downstream_capacity) {
    const std::array listeners{http3_tls_listener(8443)};
    const auto rejects = [&listeners](http_server_options options) {
        return throws_invalid([&] {
            (void)validate_http_server_configuration(listeners, std::move(options));
        });
    };

    auto options = http_server_options{};
    options.max_connections_.reset();
    RUVIA_CHECK(rejects(std::move(options)));

    options = http_server_options{};
    options.max_connections_ = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(rejects(std::move(options)));

    options = http_server_options{};
    options.worker_queue_capacity_ = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(rejects(std::move(options)));

    options = http_server_options{};
    options.max_requests_per_connection_.reset();
    RUVIA_CHECK(rejects(std::move(options)));

    options = http_server_options{};
    constexpr auto max_power_of_two =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    options.max_requests_per_connection_ = max_power_of_two / 2 + 1;
    RUVIA_CHECK(rejects(std::move(options)));

    options = http_server_options{};
    options.max_requests_per_connection_ = 1000;
    RUVIA_CHECK(!rejects(std::move(options)));
    RUVIA_CHECK_EQ(ruvia::detail::http3_worker_tracked_stream_capacity(1000), std::size_t{1192});
    RUVIA_CHECK_EQ(ruvia::detail::http3_transport_lifetime_stream_capacity(1000), std::size_t{1128});

    options = http_server_options{};
    options.max_requests_per_connection_ = std::numeric_limits<std::size_t>::max();
    RUVIA_CHECK(rejects(std::move(options)));

    const auto max_connection_capacity =
        static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    RUVIA_CHECK(throws_invalid([max_connection_capacity] {
        ruvia::detail::validate_http3_server_limits(
            2, ruvia::http3_listen_config{}, 1000, max_connection_capacity / 2 + 1);
    }));
}

RUVIA_TEST(http3_capacity_settings_are_independent_and_validate_aggregate_storage) {
    auto listener_value = http3_tls_listener(8443);
    listener_value.http3_->stream_buffer_capacity_ = 3;
    listener_value.http3_->datagram_input_capacity_ = 5;
    listener_value.http3_->datagram_output_capacity_ = 2;
    auto options = http_server_options{};
    options.worker_queue_capacity_ = 1;
    const std::array listeners{listener_value};
    (void)validate_http_server_configuration(listeners, std::move(options));
    const auto capacity = ruvia::detail::normalize_http3_capacity(*listener_value.http3_, 4);
    RUVIA_CHECK_EQ(capacity.stream_slots_, 3u);
    RUVIA_CHECK_EQ(capacity.input_slots_, std::size_t{5});
    RUVIA_CHECK_EQ(capacity.output_credits_, std::size_t{2});
    RUVIA_CHECK_EQ(capacity.packet_slots_, std::size_t{29});

    for (auto member : {&ruvia::http3_listen_config::stream_buffer_capacity_,
             &ruvia::http3_listen_config::datagram_input_capacity_,
             &ruvia::http3_listen_config::datagram_output_capacity_}) {
        auto invalid = *listener_value.http3_;
        invalid.*member = 0;
        RUVIA_CHECK(throws_invalid([&] { (void)ruvia::detail::normalize_http3_capacity(invalid, 1); }));
        invalid.*member = std::numeric_limits<std::size_t>::max();
        RUVIA_CHECK(throws_invalid([&] { (void)ruvia::detail::normalize_http3_capacity(invalid, 1); }));
    }
    auto invalid = *listener_value.http3_;
    invalid.stream_buffer_capacity_ = std::numeric_limits<std::uint32_t>::max();
    RUVIA_CHECK(throws_invalid([&] { (void)ruvia::detail::normalize_http3_capacity(invalid, 1); }));
    invalid = *listener_value.http3_;
    invalid.datagram_input_capacity_ = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) /
                                       ruvia::detail::http3_capacity::packet_bytes / 2;
    RUVIA_CHECK(throws_invalid([&] { (void)ruvia::detail::normalize_http3_capacity(invalid, 3); }));
    if constexpr (sizeof(std::size_t) > sizeof(std::uint32_t)) {
        options = http_server_options{};
        options.worker_queue_capacity_ = std::numeric_limits<std::uint32_t>::max();
        (void)validate_http_server_configuration(listeners, std::move(options));
    }
}

RUVIA_TEST(client_ip_classification_uses_complete_literal_views) {
    for (const std::string_view host : {"127.0.0.1", "0.0.0.0", "255.255.255.255",
             "::", "::1", "2001:db8::1234", "::ffff:192.0.2.1",
             "1:2:3:4:5:6:7:8"}) {
        RUVIA_CHECK(ruvia::detail::is_client_ip_address(host));
    }
    for (const std::string_view host : {"", "localhost", "api.example.test",
             "256.1.2.3", "127.1", "01.2.3.4", "127.0.0.1.example", "[::1]",
             "fe80::1%eth0", "1:2:3:4:5:6:7:8:9", "2001:db8::bad::1"}) {
        RUVIA_CHECK(!ruvia::detail::is_client_ip_address(host));
    }
    constexpr std::string_view bounded = "2001:db8::1/trailing";
    RUVIA_CHECK(ruvia::detail::is_client_ip_address(bounded.substr(0, bounded.find('/'))));
    RUVIA_CHECK(!ruvia::detail::is_client_ip_address(bounded));
    constexpr char embedded_null[] = "127.0.0.1\0suffix";
    RUVIA_CHECK(!ruvia::detail::is_client_ip_address(
        std::string_view(embedded_null, sizeof(embedded_null) - 1)));
    RUVIA_CHECK(!ruvia::detail::is_client_ip_address(std::string(253, 'a')));
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

RUVIA_TEST(sni_host_validation_accepts_dns_name_only) {
    RUVIA_CHECK(is_valid_sni_host("localhost"));
    RUVIA_CHECK(is_valid_sni_host("Example.com"));
    RUVIA_CHECK(is_valid_sni_host("xn--bcher-kva.example"));
    RUVIA_CHECK(!is_valid_sni_host(""));
    RUVIA_CHECK(!is_valid_sni_host("example.com."));
    RUVIA_CHECK(!is_valid_sni_host("example.com:443"));
    RUVIA_CHECK(!is_valid_sni_host("[::1]"));
    RUVIA_CHECK(!is_valid_sni_host("127.0.0.1"));
    RUVIA_CHECK(!is_valid_sni_host("bad host"));
    RUVIA_CHECK(!is_valid_sni_host("-bad.example"));
    RUVIA_CHECK(!is_valid_sni_host("bad-.example"));
    RUVIA_CHECK(!is_valid_sni_host("bad..example"));
    const auto label = std::string(63, 'a');
    const auto longest = label + "." + label + "." + label + "." + std::string(61, 'b');
    RUVIA_CHECK(is_valid_sni_host(longest));
    RUVIA_CHECK(!is_valid_sni_host(longest + "b"));
}

RUVIA_TEST(sni_host_validation_uses_distinct_empty_and_invalid_messages) {
    RUVIA_CHECK_EQ(caught_message(
                       [] { ruvia::detail::ensure_sni_host("", "was-empty", "was-invalid"); }),
        std::string("was-empty"));
    RUVIA_CHECK_EQ(caught_message([] {
        ruvia::detail::ensure_sni_host("bad host", "was-empty", "was-invalid");
    }),
        std::string("was-invalid"));
    RUVIA_CHECK(caught_message([] {
        ruvia::detail::ensure_sni_host(
            "example.com", "was-empty", "was-invalid");
    })
            .empty());
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

RUVIA_TEST(client_transport_uri_host_preserves_network_host) {
    counting_memory_resource resource;
    for (const auto& [host, expected] : {
             std::pair{"example.com", "example.com"},
             std::pair{"192.0.2.1", "192.0.2.1"},
             std::pair{"::1", "[::1]"},
             std::pair{"2001:db8::1", "[2001:db8::1]"}}) {
        std::string network_host(host);
        const auto wire_host = ruvia::detail::client_uri_host(network_host, &resource);
        RUVIA_CHECK_EQ(std::string_view(wire_host), std::string_view(expected));
        RUVIA_CHECK_EQ(network_host, std::string(host));
        RUVIA_CHECK(wire_host.get_allocator().resource() == &resource);
    }
}

RUVIA_TEST(client_transport_validation_uses_one_host_and_policy_contract) {
    RUVIA_CHECK(!throws_invalid(
        [] { validate_client_origin_host("example.com", "host is empty", "host is invalid"); }));
    RUVIA_CHECK(!throws_invalid(
        [] { validate_client_origin_host("::1", "host is empty", "host is invalid"); }));
    RUVIA_CHECK(throws_invalid(
        [] { validate_client_origin_host("host:443", "host is empty", "host is invalid"); }));
    RUVIA_CHECK(throws_invalid(
        [] { validate_client_origin_host("[::1]", "host is empty", "host is invalid"); }));

    ruvia::http_client_config config;
    RUVIA_CHECK(!throws_invalid(
        [&config] { validate_client_transport_config(make_client_transport_config_view(config)); }));

    config.tcp_no_delay_ = std::bit_cast<ruvia::tcp_no_delay_policy>(std::uint8_t{255});
    RUVIA_CHECK(throws_invalid(
        [&config] { validate_client_transport_config(make_client_transport_config_view(config)); }));
    config.tcp_no_delay_ = ruvia::tcp_no_delay_policy::enable;
    config.certificate_chain_file_ = "client.pem";
    RUVIA_CHECK(throws_invalid(
        [&config] { validate_client_transport_config(make_client_transport_config_view(config)); }));
}

RUVIA_TEST(client_origin_accepts_network_names_and_rejects_uri_only_names) {
    for (const auto host : {"localhost", "api.example.com", "api.example.com.",
             "xn--bcher-kva.example", "192.0.2.1", "::1", "::ffff:192.0.2.1"}) {
        RUVIA_CHECK(!throws_invalid(
            [host] { validate_client_origin_host(host, "empty", "invalid"); }));
    }
    for (const auto host : {"foo%2Ebar", "%C3%BC.example", "bad_name.example",
             "-bad.example", "bad-.example", "bad..example", ".", "example.com..",
             "[::1]", "fe80::1%eth0", "[v1.example]", "example.com:443"}) {
        RUVIA_CHECK(throws_invalid(
            [host] { validate_client_origin_host(host, "empty", "invalid"); }));
    }
    const auto label = std::string(63, 'a');
    const auto longest = label + "." + label + "." + label + "." + std::string(61, 'b');
    for (const auto& host : {label + ".example", longest, longest + "."}) {
        RUVIA_CHECK(!throws_invalid(
            [&host] { validate_client_origin_host(host, "empty", "invalid"); }));
    }
    for (const auto& host : {std::string(64, 'a') + ".example", longest + "b"}) {
        RUVIA_CHECK(throws_invalid(
            [&host] { validate_client_origin_host(host, "empty", "invalid"); }));
    }
}

RUVIA_TEST(client_tls_stream_inherits_verification_and_protocol_policy) {
    asio::io_context loop;
    for (const auto policy : {ruvia::tls_peer_verification_policy::verify,
             ruvia::tls_peer_verification_policy::skip_verification}) {
        asio::ssl::context tls(asio::ssl::context::tls_client);
        ruvia::detail::configure_client_tls_context(*tls.native_handle(), {.tls_peer_verification_ = policy});
        asio::ssl::stream<asio::ip::tcp::socket> stream(loop, tls);
        RUVIA_CHECK_EQ(SSL_get_verify_mode(stream.native_handle()),
            policy == ruvia::tls_peer_verification_policy::verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE);
        RUVIA_CHECK_EQ(SSL_get_min_proto_version(stream.native_handle()), TLS1_2_VERSION);
    }
}

RUVIA_TEST(client_tls_sni_uses_dns_identity_without_changing_network_host) {
    asio::io_context loop;
    asio::ssl::context tls(asio::ssl::context::tls_client);
    for (const auto& [host, expected] : {
             std::pair{"example.com", "example.com"},
             std::pair{"example.com.", "example.com"},
             std::pair{"127.0.0.1", ""},
             std::pair{"2001:db8::1", ""}}) {
        asio::ssl::stream<asio::ip::tcp::socket> stream(loop, tls);
        std::pmr::string network_host(host);
        RUVIA_CHECK(ruvia::detail::prepare_client_tls_stream(stream, network_host,
                        {.tls_peer_verification_ = ruvia::tls_peer_verification_policy::verify},
                        ruvia::detail::client_alpn_mode::http11) ==
                    ruvia::detail::client_tls_setup_error::none);
        const auto* name = SSL_get_servername(stream.native_handle(), TLSEXT_NAMETYPE_host_name);
        RUVIA_CHECK_EQ(name ? std::string_view(name) : std::string_view{},
            std::string_view(expected));
        RUVIA_CHECK_EQ(std::string_view(network_host), std::string_view(host));
    }
}

RUVIA_TEST(client_transport_formats_the_complete_port_domain) {
    client_port_text_buffer_type buffer{};
    RUVIA_CHECK_EQ(format_client_port(1, buffer), std::string_view("1"));
    RUVIA_CHECK_EQ(format_client_port(443, buffer), std::string_view("443"));
    RUVIA_CHECK_EQ(format_client_port(65'535, buffer), std::string_view("65535"));
}

RUVIA_TEST(client_transport_storage_owns_normalized_strings) {
    std::pmr::unsynchronized_pool_resource resource;
    std::optional<ruvia::detail::client_transport_config_storage> storage;
    {
        ruvia::http_client_config config;
        config.ca_file_ = std::string(80, 'c');
        config.certificate_chain_file_ = std::string(80, 'x');
        config.private_key_file_ = std::string(80, 'k');
        config.private_key_password_ = std::string(80, 'p');
        storage.emplace(make_client_transport_config_view(config), &resource);
    }

    const auto view = storage->view();
    const std::string expected_ca_file(80, 'c');
    const std::string expected_certificate(80, 'x');
    const std::string expected_private_key(80, 'k');
    const std::string expected_password(80, 'p');
    RUVIA_CHECK_EQ(view.ca_file_, std::string_view(expected_ca_file));
    RUVIA_CHECK_EQ(view.certificate_chain_file_, std::string_view(expected_certificate));
    RUVIA_CHECK_EQ(view.private_key_file_, std::string_view(expected_private_key));
    RUVIA_CHECK_EQ(view.private_key_password_, std::string_view(expected_password));
}

RUVIA_TEST(http_client_config_rebinding_preserves_normalized_origin_and_owned_fields) {
    for (const auto scheme : {ruvia::http_scheme::http, ruvia::http_scheme::https}) {
        for (const bool explicit_port : {false, true}) {
            ruvia::http_client_config config;
            config.host_ = std::string(40, 'h') + ".example";
            config.scheme_ = scheme;
            if (explicit_port) {
                config.port_ = 8443;
            }
            config.ca_file_ = std::string(80, 'c');
            config.certificate_chain_file_ = std::string(80, 'x');
            config.private_key_file_ = std::string(80, 'k');
            config.private_key_password_ = std::string(80, 'p');
            config.user_agent_ = std::string(80, 'a');
            config.cookies_.emplace_back(std::string(80, 'n'), std::string(80, 'v'));
            config.qpack_.max_table_capacity_ = 8192;
            config.qpack_.max_blocked_streams_ = 7;
            verify_config_rebinding<ruvia::detail::http_client_config_storage>(ruvia_ctx, config,
                [&](const auto& storage, std::pmr::memory_resource* resource) {
                    RUVIA_CHECK_EQ(std::string_view(storage.host_), std::string_view(config.host_));
                    RUVIA_CHECK(storage.host_.get_allocator().resource() == resource);
                    RUVIA_CHECK(storage.scheme_ == scheme);
                    RUVIA_CHECK_EQ(storage.port_, explicit_port ? 8443 : (scheme == ruvia::http_scheme::https ? 443 : 80));
                    RUVIA_CHECK_EQ(storage.http3_qpack_.max_table_capacity_, config.qpack_.max_table_capacity_);
                    RUVIA_CHECK_EQ(storage.http3_qpack_.max_blocked_streams_, config.qpack_.max_blocked_streams_);
                    RUVIA_CHECK_EQ(std::string_view(storage.user_agent_), std::string_view(config.user_agent_));
                    RUVIA_CHECK(storage.user_agent_.get_allocator().resource() == resource);
                    RUVIA_CHECK_EQ(storage.cookies_.size(), std::size_t{1});
                    RUVIA_CHECK(storage.cookies_.get_allocator().resource() == resource);
                    RUVIA_CHECK(storage.cookies_.front().first.get_allocator().resource() == resource);
                    RUVIA_CHECK(storage.cookies_.front().second.get_allocator().resource() == resource);
                    RUVIA_CHECK_EQ(std::string_view(storage.cookies_.front().first), std::string_view(config.cookies_.front().first));
                    RUVIA_CHECK_EQ(std::string_view(storage.cookies_.front().second), std::string_view(config.cookies_.front().second));
                    const auto transport = storage.transport_.view();
                    RUVIA_CHECK_EQ(transport.ca_file_, std::string_view(config.ca_file_));
                    RUVIA_CHECK_EQ(transport.certificate_chain_file_, std::string_view(config.certificate_chain_file_));
                    RUVIA_CHECK_EQ(transport.private_key_file_, std::string_view(config.private_key_file_));
                    RUVIA_CHECK_EQ(transport.private_key_password_, std::string_view(config.private_key_password_));
                });
        }
    }
}

RUVIA_TEST(redis_config_rebinding_preserves_tls_credentials_and_limits) {
    ruvia::redis_config config;
    config.host_ = std::string(40, 'h') + ".example";
    config.username_ = std::string(80, 'u');
    config.password_ = std::string(80, 'p');
    config.tls_.ca_file_ = std::string(80, 'c');
    config.tls_.certificate_file_ = std::string(80, 'x');
    config.tls_.private_key_file_ = std::string(80, 'k');
    config.tls_.server_name_ = config.host_;
    config.database_ = 2;
    config.pool_size_per_worker_ = 7;
    config.blocking_pool_size_per_worker_ = 3;
    config.command_timeout_ = std::nullopt;
    config.max_reply_bytes_ = std::nullopt;
    config.max_array_depth_ = 17;
    verify_config_rebinding<ruvia::detail::redis_config_storage>(ruvia_ctx, config,
        [&](const auto& storage, std::pmr::memory_resource* resource) {
            RUVIA_CHECK_EQ(std::string_view(storage.host_), std::string_view(config.host_));
            RUVIA_CHECK(storage.host_.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.username_), std::string_view(config.username_));
            RUVIA_CHECK(storage.username_.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.password_), std::string_view(config.password_));
            RUVIA_CHECK(storage.password_.get_allocator().resource() == resource);
            RUVIA_CHECK(storage.tls_.mode_ == config.tls_.mode_);
            RUVIA_CHECK_EQ(std::string_view(storage.tls_.ca_file_), std::string_view(config.tls_.ca_file_));
            RUVIA_CHECK(storage.tls_.ca_file_.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.tls_.certificate_file_), std::string_view(config.tls_.certificate_file_));
            RUVIA_CHECK(storage.tls_.certificate_file_.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.tls_.private_key_file_), std::string_view(config.tls_.private_key_file_));
            RUVIA_CHECK(storage.tls_.private_key_file_.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(std::string_view(storage.tls_.server_name_), std::string_view(config.tls_.server_name_));
            RUVIA_CHECK(storage.tls_.server_name_.get_allocator().resource() == resource);
            RUVIA_CHECK_EQ(storage.database_, config.database_);
            RUVIA_CHECK_EQ(storage.pool_size_per_worker_, config.pool_size_per_worker_);
            RUVIA_CHECK_EQ(storage.blocking_pool_size_per_worker_, config.blocking_pool_size_per_worker_);
            RUVIA_CHECK(!storage.command_timeout_);
            RUVIA_CHECK(!storage.max_reply_bytes_);
            RUVIA_CHECK_EQ(storage.max_array_depth_, config.max_array_depth_);
        });
}

RUVIA_TEST(http_client_config_is_validated_before_pmr_normalization) {
    ruvia::http_client_config config;
    config.host_ = "example.com";
    config.ca_file_ = std::string(128, 'c');
    config.connect_timeout_ = std::chrono::milliseconds::zero();
    counting_memory_resource resource;

    RUVIA_CHECK(
        throws_invalid([&] { (void)ruvia::detail::http_client_config_storage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
}

RUVIA_TEST(http_client_config_rejects_overflowing_scheduler_capacity) {
    ruvia::http_client_config config;
    config.host_ = "example.com";
    config.connection_count_ = 2;
    config.max_concurrent_http2_streams_per_connection_ = std::numeric_limits<std::size_t>::max();

    RUVIA_CHECK(throws_invalid([&] {
        std::pmr::unsynchronized_pool_resource resource;
        (void)ruvia::detail::http_client_config_storage(config, &resource);
    }));
}

RUVIA_TEST(http3_client_config_requires_https_and_bounded_response_storage) {
    ruvia::http_client_config config;
    config.host_ = "example.com";
    config.protocol_ = ruvia::http_client_protocol::http3_only;

    std::pmr::unsynchronized_pool_resource resource;
    RUVIA_CHECK(!throws_invalid(
        [&] { (void)ruvia::detail::http_client_config_storage(config, &resource); }));

    config.http3_early_data_ = true;
    RUVIA_CHECK(!throws_invalid(
        [&] { (void)ruvia::detail::http_client_config_storage(config, &resource); }));
    config.protocol_ = ruvia::http_client_protocol::negotiate;
    config.scheme_ = ruvia::http_scheme::http;
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::http_client_config_storage(config, &resource); }));
    config.protocol_ = ruvia::http_client_protocol::http3_only;
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::http_client_config_storage(config, &resource); }));

    config.scheme_ = ruvia::http_scheme::https;
    config.http3_early_data_ = false;
    config.max_response_bytes_ = std::size_t{64} * 1024 * 1024 + 1;
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::http_client_config_storage(config, &resource); }));
}

RUVIA_TEST(websocket_client_config_rejects_invalid_startup_config) {
    ruvia::websocket_client_config config;
    config.host_ = "example.com";
    config.ca_file_ = std::string(128, 'c');
    config.connect_timeout_ = std::chrono::milliseconds::zero();
    counting_memory_resource resource;

    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::websocket_client_config_storage(config, &resource); }));

    config.connect_timeout_ = std::chrono::milliseconds{5000};
    config.target_ = "/events#fragment";
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::websocket_client_config_storage(config, &resource); }));

    config.target_ = "/";
    config.subprotocols_ = {"chat", "chat"};
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::websocket_client_config_storage(config, &resource); }));

    config.subprotocols_ = {"bad token"};
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::websocket_client_config_storage(config, &resource); }));

    config.subprotocols_ = {""};
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::websocket_client_config_storage(config, &resource); }));

    config.subprotocols_.clear();
    config.heartbeat_.pong_timeout_ = std::chrono::milliseconds{1000};
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::websocket_client_config_storage(config, &resource); }));

    config.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds{1000},
        .pong_timeout_ = std::chrono::milliseconds::zero()};
    RUVIA_CHECK(throws_invalid(
        [&] { (void)ruvia::detail::websocket_client_config_storage(config, &resource); }));
}

RUVIA_TEST(websocket_client_config_storage_owns_normalized_strings) {
    std::pmr::unsynchronized_pool_resource resource;
    std::optional<ruvia::detail::websocket_client_config_storage> storage;
    ruvia::websocket_client_config config;
    {
        config.host_ = std::string(40, 'h') + "." + std::string(40, 'h');
        config.target_ = "/" + std::string(80, 't');
        config.headers_.emplace_back("X-Test", std::string(80, 'v'));
        config.subprotocols_ = {"chat", "superchat"};
        config.ca_file_ = std::string(80, 'c');
        config.user_agent_ = std::string(80, 'u');
        storage.emplace(config, &resource);
    }

    RUVIA_CHECK(storage->host_.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->target_.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->headers_.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->headers_.front().name_.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->headers_.front().value_.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->subprotocols_.get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->subprotocols_.front().get_allocator().resource() == &resource);
    RUVIA_CHECK(storage->user_agent_.get_allocator().resource() == &resource);
    RUVIA_CHECK_EQ(std::string(storage->host_), std::string(40, 'h') + "." + std::string(40, 'h'));
    RUVIA_CHECK_EQ(std::string(storage->target_), "/" + std::string(80, 't'));
    RUVIA_CHECK_EQ(std::string(storage->headers_.front().name_), "X-Test");
    RUVIA_CHECK_EQ(std::string(storage->headers_.front().value_), std::string(80, 'v'));
    RUVIA_CHECK_EQ(storage->subprotocols_.size(), std::size_t{2});
    RUVIA_CHECK_EQ(std::string(storage->subprotocols_[0]), "chat");
    RUVIA_CHECK_EQ(std::string(storage->subprotocols_[1]), "superchat");
    RUVIA_CHECK(!storage->heartbeat_.ping_interval_.has_value());
    RUVIA_CHECK(!storage->heartbeat_.pong_timeout_.has_value());
    RUVIA_CHECK_EQ(std::string(storage->user_agent_), std::string(80, 'u'));

    config.heartbeat_ = {.ping_interval_ = std::chrono::milliseconds{1000}};
    storage.emplace(config, &resource);
    RUVIA_CHECK_EQ(storage->heartbeat_.ping_interval_->count(), std::int64_t{1000});
    RUVIA_CHECK_EQ(storage->heartbeat_.pong_timeout_->count(), std::int64_t{1000});
}

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
RUVIA_TEST(database_config_is_validated_before_pmr_normalization) {
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::db_config config{.driver_ = ruvia::db_driver::mariadb};
#else
    ruvia::db_config config{.driver_ = ruvia::db_driver::postgresql};
#endif
    config.username_ = std::string(128, 'u');
    config.connect_timeout_ = std::chrono::milliseconds::zero();
    counting_memory_resource resource;

    RUVIA_CHECK(throws_invalid([&] { (void)ruvia::detail::db_config_storage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
}
#endif

RUVIA_TEST(redis_config_is_validated_before_pmr_normalization) {
    ruvia::redis_config config;
    config.username_ = std::string(128, 'u');
    config.connect_timeout_ = std::chrono::milliseconds::zero();
    counting_memory_resource resource;

    RUVIA_CHECK(throws_invalid([&] { (void)ruvia::detail::redis_config_storage(config, &resource); }));
    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{0});
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

RUVIA_TEST(integration_config_copies_public_strings_into_internal_pmr_storage) {
    std::pmr::unsynchronized_pool_resource target_resource;
#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
    std::optional<ruvia::detail::db_config_storage> database;
#endif
    std::optional<ruvia::detail::redis_config_storage> redis;
    {
#ifdef RUVIA_ENABLE_MARIADB
        auto source_value = ruvia::db_config{.driver_ = ruvia::db_driver::mariadb};
#elif defined(RUVIA_ENABLE_POSTGRESQL)
        auto source_value = ruvia::db_config{.driver_ = ruvia::db_driver::postgresql};
#endif
#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
#ifdef RUVIA_ENABLE_MARIADB
        // MariaDB's default verify_identity mode requires an IP-literal host.
        source_value.host_ = "127.0.0.1";
#else
        source_value.host_ = std::string(40, 'h') + "." + std::string(39, 'h');
#endif
        source_value.username_ = std::string(80, 'u');
        source_value.password_ = std::string(80, 'p');
        source_value.database_ = std::string(80, 'd');
        database.emplace(source_value, &target_resource);
        RUVIA_CHECK(database->host_.get_allocator().resource() == &target_resource);
        RUVIA_CHECK(database->username_.get_allocator().resource() == &target_resource);
        RUVIA_CHECK(database->password_.get_allocator().resource() == &target_resource);
        RUVIA_CHECK(database->database_.get_allocator().resource() == &target_resource);
#endif

        ruvia::redis_config source_redis{
            .host_ = std::string(80, 'r'),
            .port_ = 6379,
            .username_ = std::string(80, 'x'),
            .password_ = std::string(80, 'y'),
        };
        redis.emplace(source_redis, &target_resource);
        RUVIA_CHECK(redis->host_.get_allocator().resource() == &target_resource);
        RUVIA_CHECK(redis->username_.get_allocator().resource() == &target_resource);
        RUVIA_CHECK(redis->password_.get_allocator().resource() == &target_resource);
    }

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
#ifdef RUVIA_ENABLE_MARIADB
    RUVIA_CHECK_EQ(std::string(database->host_), "127.0.0.1");
#else
    RUVIA_CHECK_EQ(std::string(database->host_), std::string(40, 'h') + "." + std::string(39, 'h'));
#endif
#endif
    RUVIA_CHECK_EQ(std::string(redis->host_), std::string(80, 'r'));
}
