#include <gtest/gtest.h>

#include <cstdint>
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
#include "ruvia/web/context.h"

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
    return {.identity_ = {
                .certificate_chain_file_ = std::move(certificate_chain_file),
                .private_key_file_ = std::move(private_key_file),
                .private_key_password_ = std::move(private_key_password),
            }};
}

class querying_callback_target final {
public:
    explicit querying_callback_target(int& destroyed) noexcept
        : destroyed_(&destroyed) {}

    querying_callback_target(querying_callback_target&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

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

// Destroying it re-enters the application lock through http_stats() and
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
            (void)ruvia::app().http_stats();
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
    const auto rejects = [](std::vector<ruvia::tls_sni_config> sni) {
        return throws_invalid([&] {
            ruvia::app().listen({
                .address_ = "127.0.0.1",
                .https_ = 8443,
                .tls_ =
                    {
                        .identity_ = {.certificate_chain_file_ = "cert.pem", .private_key_file_ = "key.pem"},
                        .sni_ = std::move(sni),
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

TEST(Ruvia, tls_identity_owns_password_independently_of_caller_resource) {
    releasable_memory_resource caller_resource;
    const std::string expected(80, 's');
    std::optional<ruvia::tls_config> identity;
    {
        const std::pmr::string password(expected, &caller_resource);
        identity.emplace(
            tls_config("cert.pem", "key.pem", std::string(password.data(), password.size())));
    }
    caller_resource.release();
    EXPECT_EQ(identity->identity_.private_key_password_, std::string_view(expected));
    identity.reset();
    EXPECT_TRUE(!caller_resource.deallocated_after_release());
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
    bool empty_call_rejected = false;
    try {
        hook();
    } catch (const std::logic_error&) {
        empty_call_rejected = true;
    }
    EXPECT_TRUE(empty_call_rejected);

    void (*access_target)(const ruvia::access_log_record&) noexcept = nullptr;
    ruvia::access_log_callback access(std::move(access_target));
    EXPECT_TRUE(!access);

    ruvia::task<ruvia::http_response> (*error_target)(ruvia::context&, ruvia::http_error_info) = nullptr;
    ruvia::http_error_handler error(error_target);
    EXPECT_TRUE(!error);
}

TEST(Ruvia, app_hook_retires_owner_before_target_destruction) {
    for (int retirement = 0; retirement != 3; ++retirement) {
        int destroyed = 0;
        int replacement_calls = 0;
        bool owner_was_empty = false;
        {
            ruvia::app_hook hook;
            const auto release = [&](int* value) noexcept {
                ++destroyed;
                owner_was_empty = !hook;
                // 旧实现先报告状态失败，避免递归销毁拖垮整个测试进程。
                if (owner_was_empty) {
                    hook = nullptr;
                }
                delete value;
            };
            hook = [guard = std::unique_ptr<int, decltype(release)>(new int{0}, release)] {};
            if (retirement == 0) {
                hook = static_cast<void (*)()>(nullptr);
                EXPECT_TRUE(!hook);
            } else if (retirement == 1) {
                hook = [&] { ++replacement_calls; };
                hook();
            }
        }
        EXPECT_TRUE(owner_was_empty);
        EXPECT_EQ(destroyed, 1);
        EXPECT_EQ(replacement_calls, retirement == 1 ? 1 : 0);
    }
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
