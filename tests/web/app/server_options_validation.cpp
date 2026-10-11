#include <filesystem>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/app.h"

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
