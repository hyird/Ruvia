#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/memory/process_resource.h"
#include "ruvia/web/app.h"
#include "ruvia/web/static_files.h"

#include "app/app_state.h"
#include "http/cors_options.h"
#include "server/http_server_options_validation.h"
#include "test_harness.h"

namespace {

class releasable_memory_resource final : public std::pmr::memory_resource {
public:
    void release() noexcept {
        released_ = true;
    }

    [[nodiscard]] bool deallocated_after_release() const noexcept {
        return deallocated_after_release_;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        deallocated_after_release_ = deallocated_after_release_ || released_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool released_{false};
    bool deallocated_after_release_{false};
};

using ruvia::detail::http_server_listener_definition;
using ruvia::detail::http_server_options;
using ruvia::detail::validate_http_server_configuration;
using ruvia::detail::validate_http_server_listener;
using ruvia::detail::validate_http_server_options;
using ruvia::detail::validated_http_server_configuration;

template <typename transport_type>
http_server_listener_definition make_listener(transport_type&& transport) {
    return http_server_listener_definition(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 8080),
        std::forward<transport_type>(transport));
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

ruvia::tls_config tls_config(std::filesystem::path certificate_chain_file,
    std::filesystem::path private_key_file, std::string private_key_password = {}) {
    return {
        .certificate_chain_file_ = std::move(certificate_chain_file),
        .private_key_file_ = std::move(private_key_file),
        .private_key_password_ = std::move(private_key_password),
    };
}

}  // namespace

RUVIA_TEST(validate_server_options_accepts_defaults) {
    RUVIA_CHECK(!http_server_options{}.max_stream_body_bytes_.has_value());
    RUVIA_CHECK(!http_server_options{}.compression_.has_value());
    RUVIA_CHECK_EQ(ruvia::compression_config{}.min_bytes_, std::size_t{1024});
    RUVIA_CHECK_EQ(ruvia::compression_config{}.sync_bytes_, std::size_t{64} * 1024);
    RUVIA_CHECK_EQ(ruvia::compression_config{}.max_bytes_, std::size_t{64} * 1024 * 1024);
    RUVIA_CHECK_EQ(
        http_server_options{}.rate_limit_capacity_per_worker_, ruvia::default_rate_limit_capacity_per_worker);
    // An unconfigured server is bounded by default against connection floods.
    const auto default_max_connections = http_server_options{}.max_connections_;
    RUVIA_CHECK(default_max_connections.has_value());
    RUVIA_CHECK_EQ(default_max_connections.value_or(0), std::size_t{1024});
    RUVIA_CHECK(!throws_invalid([] { validate_http_server_options(http_server_options{}); }));
}

RUVIA_TEST(server_configuration_owns_listener_inputs_until_retirement) {
    releasable_memory_resource caller_resource;
    std::optional<validated_http_server_configuration> configuration;
    const std::string certificate(128, 'c');
    const std::string key(128, 'k');
    {
        http_server_listener_definition::tls_type tls(&caller_resource);
        tls.identity_.certificate_chain_file_ = certificate;
        tls.identity_.private_key_file_ = key;
        tls.http3_early_data_ = true;
        auto& sni = tls.sni_identities_.emplace_back(&caller_resource);
        sni.host_ = "api.example.test";
        sni.identity_.certificate_chain_file_ = certificate;
        sni.identity_.private_key_file_ = key;
        const std::array listeners{make_listener(std::move(tls))};
        configuration.emplace(validate_http_server_configuration(listeners, http_server_options{}));
    }
    caller_resource.release();
    const auto& listener_value = configuration->listeners().front();
    const auto& tls = std::get<http_server_listener_definition::tls_type>(listener_value.transport_);
    RUVIA_CHECK_EQ(tls.identity_.certificate_chain_file_, std::string_view(certificate));
    RUVIA_CHECK_EQ(tls.identity_.private_key_file_, std::string_view(key));
    RUVIA_CHECK(tls.http3_early_data_);
    RUVIA_CHECK_EQ(tls.sni_identities_.front().host_, std::string_view("api.example.test"));
    {
        ruvia::detail::http_server_session_config session(listener_value, std::pmr::get_default_resource());
        RUVIA_CHECK(session.tls()->http3_early_data_);
    }
    configuration.reset();
    RUVIA_CHECK(!caller_resource.deallocated_after_release());
}

RUVIA_TEST(server_limits_reject_zero_client_result_budgets_at_each_entry) {
    for (const bool retained : {false, true}) {
        ruvia::server_config config;
        auto& limit = retained ? config.http_client_result_budget_.max_retained_bytes_
                               : config.http_client_result_budget_.max_in_flight_bytes_;
        limit = 0;
        RUVIA_CHECK(throws_invalid([&] {
            (void)ruvia::detail::normalize_server_options(config, http_server_options{});
        }));
        http_server_options options;
        options.http_client_result_budget_ = config.http_client_result_budget_;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
}

RUVIA_TEST(app_enables_a_bounded_blocking_pool_by_default) {
    ruvia::detail::app_state state;
    RUVIA_CHECK(state.blocking_pool_.has_value());
    if (state.blocking_pool_.has_value()) {
        RUVIA_CHECK_EQ(state.blocking_pool_->thread_count_, std::size_t{0});
        RUVIA_CHECK_EQ(state.blocking_pool_->queue_capacity_, std::size_t{0});
    }
}

RUVIA_TEST(validate_server_options_rejects_invalid_compression_thresholds) {
    http_server_options options;
    options.compression_ = ruvia::compression_config{.min_bytes_ = 0};
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));

    options.compression_ = ruvia::compression_config{.min_bytes_ = 1024, .sync_bytes_ = 512};
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));

    options.compression_ =
        ruvia::compression_config{.min_bytes_ = 1024, .sync_bytes_ = 4096, .max_bytes_ = 2048};
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));

    options.compression_ = ruvia::compression_config{};
    RUVIA_CHECK(!throws_invalid([&] { validate_http_server_options(options); }));
}

RUVIA_TEST(validate_server_options_owns_document_root_runtime_policy) {
    namespace fs = std::filesystem;
    http_server_options options;
    RUVIA_CHECK(options.document_root_.root() == nullptr);
    RUVIA_CHECK(options.document_root_.refresh_options() == nullptr);
    RUVIA_CHECK(!throws_invalid([&] { validate_http_server_options(options); }));

    const auto dir = fs::temp_directory_path() / "ruvia_server_options_document_root";
    fs::remove_all(dir);
    fs::create_directories(dir);
    ruvia::static_root root(dir);
    options.document_root_ = http_server_options::document_root_type::standalone(root);
    RUVIA_CHECK(!throws_invalid([&] { validate_http_server_options(options); }));

    options.document_root_ = http_server_options::document_root_type::refreshing(
        root, {.refresh_interval_ = std::chrono::milliseconds(1)});
    // An application-managed refreshing root needs the blocking pool that performs
    // directory scans off the event loop.
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));

    ruvia::blocking_pool pool(ruvia::blocking_pool_options{.thread_count_ = 1});
    options.blocking_pool_ = &pool;
    options.document_root_ = http_server_options::document_root_type::refreshing(
        root, {.refresh_interval_ = std::chrono::milliseconds::zero()});
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));

    options.document_root_ = http_server_options::document_root_type::refreshing(
        root, {.refresh_interval_ = std::chrono::milliseconds(1)});
    RUVIA_CHECK(!throws_invalid([&] { validate_http_server_options(options); }));

    options.document_root_ = http_server_options::document_root_type::refreshing(
        root, {.refresh_interval_ = std::chrono::milliseconds(1)}, {.gzip_ = true, .min_bytes_ = 0});
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));

    options.document_root_ = http_server_options::document_root_type::refreshing(root,
        {.refresh_interval_ = std::chrono::milliseconds(1)},
        {.gzip_ = true, .min_bytes_ = 1024, .max_bytes_ = 512});
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));

    fs::remove_all(dir);
}

RUVIA_TEST(validate_server_options_rejects_configured_nonpositive_timeout) {
    // Every connection timeout feeds the same positive optional fold. Each one bounds
    // how long a slow client can hold a connection (a slowloris defense), so a
    // nonpositive value in ANY of them must be rejected -- checking only idle_timeout
    // would miss a field dropped from the fold call.
    using std::chrono::milliseconds;
    {
        http_server_options options;
        options.idle_timeout_ = milliseconds(0);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.request_header_timeout_ = milliseconds(0);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.request_body_timeout_ = milliseconds(0);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.write_timeout_ = milliseconds(0);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.idle_timeout_ = milliseconds(-1);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.request_header_timeout_ = milliseconds(-1);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.request_body_timeout_ = milliseconds(-1);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.write_timeout_ = milliseconds(-1);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
}

RUVIA_TEST(validate_server_options_rejects_nonpositive_limits) {
    {
        http_server_options options;
        options.worker_queue_capacity_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.rate_limit_capacity_per_worker_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.rate_limit_capacity_per_worker_ = 3;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.memory_config_.request_initial_buffer_bytes_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.max_connections_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.max_requests_per_connection_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.max_stream_body_bytes_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.max_buffered_body_bytes_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.max_websocket_message_bytes_ = 0;
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
    {
        http_server_options options;
        options.scan_interval_ = std::chrono::milliseconds(0);
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_options(options); }));
    }
}

RUVIA_TEST(validate_server_options_enforces_tls_material) {
    // TLS on but no certificate / key files is rejected.
    auto missing = make_listener(http_server_listener_definition::tls_type{});
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(missing); }));

    // With both files present it is accepted.
    http_server_listener_definition::tls_type tls;
    tls.identity_.certificate_chain_file_ = "cert.pem";
    tls.identity_.private_key_file_ = "key.pem";
    auto configured = make_listener(std::move(tls));
    RUVIA_CHECK(!throws_invalid([&] { validate_http_server_listener(configured); }));
}

RUVIA_TEST(http3_early_data_is_opt_in_and_rejects_client_certificate_policy) {
    const auto valid_tls = [] {
        http_server_listener_definition::tls_type tls;
        tls.identity_.certificate_chain_file_ = "cert.pem";
        tls.identity_.private_key_file_ = "key.pem";
        return tls;
    };
    {
        auto tls = valid_tls();
        RUVIA_CHECK(!tls.http3_early_data_);
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(!throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
    {
        auto tls = valid_tls();
        tls.http3_early_data_ = true;
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(!throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
    {
        auto tls = valid_tls();
        tls.http3_early_data_ = true;
        tls.client_certificates_.emplace(std::pmr::get_default_resource(),
            ruvia::tls_client_certificate_requirement::optional);
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
}

RUVIA_TEST(validate_server_options_enforces_nested_tls_material) {
    const auto valid_tls = [] {
        http_server_listener_definition::tls_type tls;
        tls.identity_.certificate_chain_file_ = "cert.pem";
        tls.identity_.private_key_file_ = "key.pem";
        return tls;
    };

    {
        auto tls = valid_tls();
        tls.client_certificates_.emplace(
            std::pmr::get_default_resource(), ruvia::tls_client_certificate_requirement::optional);
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
    {
        auto tls = valid_tls();
        auto& sni = tls.sni_identities_.emplace_back();
        sni.identity_.certificate_chain_file_ = "sni-cert.pem";
        sni.identity_.private_key_file_ = "sni-key.pem";
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
    {
        auto tls = valid_tls();
        auto& sni = tls.sni_identities_.emplace_back();
        sni.host_ = "example.com:443";
        sni.identity_.certificate_chain_file_ = "sni-cert.pem";
        sni.identity_.private_key_file_ = "sni-key.pem";
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
    {
        auto tls = valid_tls();
        auto& sni = tls.sni_identities_.emplace_back();
        sni.host_ = "example.com";
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
    {
        auto tls = valid_tls();
        for (const auto* host : {"Example.com", "example.COM"}) {
            auto& sni = tls.sni_identities_.emplace_back();
            sni.host_ = host;
            sni.identity_.certificate_chain_file_ = "sni-cert.pem";
            sni.identity_.private_key_file_ = "sni-key.pem";
        }
        auto listener_value = make_listener(std::move(tls));
        RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(listener_value); }));
    }
}

RUVIA_TEST(validated_server_configuration_requires_complete_validation) {
    RUVIA_CHECK(throws_invalid([] { (void)validate_http_server_configuration({}, {}); }));
    const std::array listeners{
        make_listener(http_server_listener_definition::plain_http_type{}),
    };

    http_server_options invalid_options;
    invalid_options.max_buffered_body_bytes_ = 0;
    RUVIA_CHECK(throws_invalid([&listeners, &invalid_options] {
        (void)validate_http_server_configuration(listeners, std::move(invalid_options));
    }));

    const auto configuration = validate_http_server_configuration(listeners, http_server_options{});
    RUVIA_CHECK_EQ(configuration.listeners().size(), std::size_t{1});
    RUVIA_CHECK(configuration.options().max_buffered_body_bytes_ > 0);
}

RUVIA_TEST(web_internal_config_defaults_use_the_process_resource) {
    auto* const expected = ruvia::detail::process_resource();

    const ruvia::detail::cors_options cors;
    RUVIA_CHECK(cors.origin_.get_allocator().resource() == expected);
    RUVIA_CHECK(cors.request_headers_.get_allocator().resource() == expected);
    RUVIA_CHECK(cors.expose_headers_.get_allocator().resource() == expected);

    const http_server_listener_definition::tls_type tls;
    RUVIA_CHECK(tls.identity_.certificate_chain_file_.get_allocator().resource() == expected);
    RUVIA_CHECK(tls.identity_.private_key_file_.get_allocator().resource() == expected);
    RUVIA_CHECK(tls.identity_.private_key_password_.get_allocator().resource() == expected);
    RUVIA_CHECK(tls.sni_identities_.get_allocator().resource() == expected);

    const http_server_listener_definition::tls_client_certificate_policy_type client_certificates;
    RUVIA_CHECK(client_certificates.verify_file_.get_allocator().resource() == expected);

    const http_server_listener_definition::tls_type::sni_identity_type sni;
    RUVIA_CHECK(sni.host_.get_allocator().resource() == expected);
    RUVIA_CHECK(sni.identity_.certificate_chain_file_.get_allocator().resource() == expected);
}

RUVIA_TEST(validate_server_options_requires_redirect_https_port) {
    auto listener_value = make_listener(http_server_listener_definition::redirect_http_to_https_type{0});
    RUVIA_CHECK(throws_invalid([&] { validate_http_server_listener(listener_value); }));
}

RUVIA_TEST(listener_config_rejects_invalid_listener_and_tls_states_at_construction) {
    RUVIA_CHECK(throws_invalid([] { ruvia::app().listen({}); }));
    RUVIA_CHECK(throws_invalid([] { ruvia::app().listen({.address_ = {}, .http_ = 8080}); }));
    RUVIA_CHECK(throws_invalid([] { ruvia::app().listen({.address_ = "localhost", .http_ = 8080}); }));
    RUVIA_CHECK(throws_invalid([] { ruvia::app().listen({.address_ = std::string("127.0.0.1\0bad", 13), .http_ = 8080}); }));
    RUVIA_CHECK(throws_invalid([] { ruvia::app().listen({.address_ = "127.0.0.1", .http_ = 0}); }));
    RUVIA_CHECK(throws_invalid(
        [] { ruvia::app().listen({.address_ = "127.0.0.1", .http_ = 8080, .https_ = 8080}); }));
    RUVIA_CHECK(throws_invalid([] {
        ruvia::app().listen({.address_ = "127.0.0.1", .http_ = 8080, .auto_https_redirect_ = true});
    }));
    RUVIA_CHECK(
        throws_invalid([] { ruvia::app().listen({.address_ = "127.0.0.1", .https_ = 8443}); }));
    RUVIA_CHECK(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ = {.certificate_chain_file_ = "cert.pem"},
        });
    }));
    RUVIA_CHECK(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .http_ = 8080,
            .tls_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
        });
    }));
    RUVIA_CHECK(throws_invalid([] {
        ruvia::app().listen({
            .address_ = "127.0.0.1",
            .https_ = 8443,
            .tls_ =
                {
                    .certificate_chain_file_ = "cert.pem",
                    .private_key_file_ = "key.pem",
                    .client_certificates_ =
                        {
                            .requirement_ = ruvia::tls_client_certificate_requirement::required,
                        },
                },
        });
    }));
    RUVIA_CHECK(throws_invalid([] {
        ruvia::app().server(
            {.process_signal_handlers_ = static_cast<ruvia::process_signal_handler_policy>(0xFF)});
    }));
}

RUVIA_TEST(tls_config_rejects_empty_or_duplicate_sni_identity) {
    const auto rejects = [](std::vector<ruvia::tls_sni_config> sni) {
        return throws_invalid([&] {
            ruvia::app().listen({
                .address_ = "127.0.0.1",
                .https_ = 8443,
                .tls_ =
                    {
                        .certificate_chain_file_ = "cert.pem",
                        .private_key_file_ = "key.pem",
                        .sni_ = std::move(sni),
                    },
            });
        });
    };
    RUVIA_CHECK(rejects(
        {{.host_ = {}, .certificate_chain_file_ = "other.pem", .private_key_file_ = "other.key"}}));
    RUVIA_CHECK(rejects({{.host_ = "example.com:443",
        .certificate_chain_file_ = "other.pem",
        .private_key_file_ = "other.key"}}));
    RUVIA_CHECK(rejects({{.host_ = "127.0.0.1",
        .certificate_chain_file_ = "other.pem",
        .private_key_file_ = "other.key"}}));
    RUVIA_CHECK(rejects({
        {.host_ = "Example.com", .certificate_chain_file_ = "other.pem", .private_key_file_ = "other.key"},
        {.host_ = "example.COM", .certificate_chain_file_ = "third.pem", .private_key_file_ = "third.key"},
    }));
}

RUVIA_TEST(tls_identity_owns_password_independently_of_caller_resource) {
    releasable_memory_resource caller_resource;
    const std::string expected(80, 's');
    std::optional<ruvia::tls_config> identity;
    {
        const std::pmr::string password(expected, &caller_resource);
        identity.emplace(
            tls_config("cert.pem", "key.pem", std::string(password.data(), password.size())));
    }
    caller_resource.release();
    RUVIA_CHECK_EQ(identity->private_key_password_, std::string_view(expected));
    identity.reset();
    RUVIA_CHECK(!caller_resource.deallocated_after_release());
}

RUVIA_TEST(self_contained_app_callbacks_release_owned_state) {
    std::weak_ptr<int> access_state;
    {
        auto state_value = std::make_shared<int>(1);
        access_state = state_value;
        ruvia::access_log_callback_type callback_value(
            [state_value](const ruvia::access_log_record&) noexcept { (void)state_value; });
        // Copy ownership is the behavior under test.
        const auto copy = callback_value;  // NOLINT(performance-unnecessary-copy-initialization)
        state_value.reset();
        RUVIA_CHECK(!access_state.expired());
        (void)copy;
    }
    RUVIA_CHECK(access_state.expired());

    std::weak_ptr<int> failure_state;
    {
        auto state_value = std::make_shared<int>(1);
        failure_state = state_value;
        ruvia::connection_failure_callback_type callback_value(
            [state_value](const ruvia::connection_failure_record&) noexcept { (void)state_value; });
        state_value.reset();
        RUVIA_CHECK(!failure_state.expired());
    }
    RUVIA_CHECK(failure_state.expired());

    std::weak_ptr<int> error_state;
    {
        auto state_value = std::make_shared<int>(1);
        error_state = state_value;
        ruvia::http_error_handler_type callback_value(
            [state_value](ruvia::context&, ruvia::http_error_info) -> ruvia::task<ruvia::http_response> {
                (void)state_value;
                co_return ruvia::http_response{};
            });
        state_value.reset();
        RUVIA_CHECK(!error_state.expired());
    }
    RUVIA_CHECK(error_state.expired());

    std::weak_ptr<int> not_found_state;
    {
        auto state_value = std::make_shared<int>(1);
        not_found_state = state_value;
        ruvia::http_not_found_handler_type callback_value(
            [state_value](ruvia::context&) -> ruvia::task<ruvia::http_response> {
                (void)state_value;
                co_return ruvia::http_response{};
            });
        state_value.reset();
        RUVIA_CHECK(!not_found_state.expired());
    }
    RUVIA_CHECK(not_found_state.expired());
}
