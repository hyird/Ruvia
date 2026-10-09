#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/ssl/context.hpp>
#include <asio/system_error.hpp>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include "server/http_server_options_validation.h"
#include "server/http_server_tls_identity.h"
#include "test_harness.h"
#include "tls_password_fixture.h"

namespace {

using ruvia::test::noninteractive_ui_scope;
using ruvia::test::password_callback_cleanup;
using ruvia::test::write_encrypted_key;

}  // namespace

RUVIA_TEST(tls_identity_loading_rejects_nul_file_paths_before_opening_the_prefix) {
    using namespace ruvia::detail;
    ruvia::test::tls_identity files("server-file-path.ruvia-test.local");
    const auto key = write_encrypted_key(files, "key.pem", "path-password").string();
    http_server_listener_definition::tls_type valid;
    valid.identity_.certificate_chain_file_ = files.ca_file_.string();
    valid.identity_.private_key_file_ = key;
    valid.identity_.private_key_password_ = "path-password";
    valid.client_certificates_.emplace();
    valid.client_certificates_->verify_file_ = files.ca_file_.string();
    for (const int field : {0, 1, 2}) {
        auto tls = valid;
        auto& path = field == 0 ? tls.identity_.certificate_chain_file_
                                : (field == 1 ? tls.identity_.private_key_file_ : tls.client_certificates_->verify_file_);
        path.push_back('\0');
        path.append("other.pem");
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            validate_http_server_tls_options(tls);
        }));
        asio::ssl::context context(asio::ssl::context::tls_server);
        bool rejected = false;
        try {
            configure_http_server_tls_identity(context.native_handle(), tls.identity_, tls.client_certificates_);
        } catch (const std::invalid_argument&) {
            rejected = true;
        } catch (const asio::system_error&) {
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK(SSL_CTX_get0_certificate(context.native_handle()) == nullptr);
        RUVIA_CHECK(SSL_CTX_get0_privatekey(context.native_handle()) == nullptr);
        if (field != 2) {
            auto sni_tls = valid;
            sni_tls.sni_identities_.emplace_back();
            sni_tls.sni_identities_.back().host_ = "sni-file-path.ruvia-test.local";
            sni_tls.sni_identities_.back().identity_ = tls.identity_;
            RUVIA_CHECK(ruvia::testing::throws_on([&] {
                validate_http_server_tls_options(sni_tls);
            }));
        }
    }
}

RUVIA_TEST(tls_identity_loading_does_not_prompt_for_unsupplied_password) {
    ruvia::test::tls_identity files("password-ui.ruvia-test.local");
    const auto required_key = write_encrypted_key(files, "required-password.pem", "required-password");
    const auto empty_key = write_encrypted_key(files, "empty-password.pem", "");
    int attempts = 0;
    noninteractive_ui_scope ui(attempts);
    for (const bool explicit_default : {false, true}) {
        ruvia::detail::http_server_listener_definition::tls_identity_type identity;
        identity.certificate_chain_file_ = files.ca_file_.string();
        identity.private_key_file_ = required_key.string();
        asio::ssl::context context(asio::ssl::context::tls_server);
        if (explicit_default) {
            SSL_CTX_set_default_passwd_cb(context.native_handle(), PEM_def_callback);
        }
        bool rejected = false;
        try {
            ruvia::detail::configure_http_server_tls_identity(context.native_handle(), identity, {});
        } catch (const asio::system_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        identity.private_key_file_ = empty_key.string();
        bool loaded = false;
        try {
            ruvia::detail::configure_http_server_tls_identity(context.native_handle(), identity, {});
            loaded = true;
        } catch (const asio::system_error&) {
        }
        RUVIA_CHECK(loaded);
        if (loaded) {
            RUVIA_CHECK(SSL_CTX_check_private_key(context.native_handle()) == 1);
        }
    }
    RUVIA_CHECK(attempts == 0);
}

RUVIA_TEST(tls_identity_loading_uses_a_caller_password_callback_when_password_is_unsupplied) {
    ruvia::test::tls_identity files("password-provider.ruvia-test.local");
    const auto key = write_encrypted_key(files, "callback-password.pem", "callback-password");
    std::weak_ptr<std::string> callback_lifetime;
    int calls = 0;
    {
        ruvia::detail::http_server_listener_definition::tls_identity_type identity;
        identity.certificate_chain_file_ = files.ca_file_.string();
        identity.private_key_file_ = key.string();
        asio::ssl::context context(asio::ssl::context::tls_server);
        auto password = std::make_shared<std::string>("callback-password");
        callback_lifetime = password;
        context.set_password_callback([password, &calls](std::size_t, asio::ssl::context::password_purpose) {
            ++calls;
            return *password;
        });
        password.reset();
        ruvia::detail::configure_http_server_tls_identity(context.native_handle(), identity, {});
        RUVIA_CHECK(calls > 0);
        RUVIA_CHECK(SSL_CTX_check_private_key(context.native_handle()) == 1);
    }
    RUVIA_CHECK(callback_lifetime.expired());
}

RUVIA_TEST(tls_identity_loading_preserves_the_context_password_callback_owner) {
    ruvia::test::tls_identity files("password-owner.ruvia-test.local");
    const auto configured_key = write_encrypted_key(files, "configured.pem", "configured-password");
    const auto original_key = write_encrypted_key(files, "original.pem", "original-password");
    for (const bool correct_password : {false, true}) {
        std::weak_ptr<std::string> callback_lifetime;
        int calls = 0;
        {
            ruvia::detail::http_server_listener_definition::tls_identity_type identity;
            identity.certificate_chain_file_ = files.ca_file_.string();
            identity.private_key_file_ = configured_key.string();
            identity.private_key_password_ = correct_password ? "configured-password" : "incorrect-password";
            asio::ssl::context context(asio::ssl::context::tls_server);
            auto password = std::make_shared<std::string>("original-password");
            callback_lifetime = password;
            context.set_password_callback([password, &calls](std::size_t, asio::ssl::context::password_purpose) {
                ++calls;
                return *password;
            });
            password.reset();
            password_callback_cleanup cleanup(context.native_handle());
            bool configured = false;
            try {
                ruvia::detail::configure_http_server_tls_identity(context.native_handle(), identity, {});
                configured = true;
            } catch (const asio::system_error&) {
            }
            RUVIA_CHECK(configured == correct_password);
            RUVIA_CHECK(calls == 0);
            bool loaded = false;
            try {
                context.use_private_key_file(original_key.string(), asio::ssl::context::pem);
                loaded = true;
            } catch (const asio::system_error&) {
            }
            RUVIA_CHECK(loaded);
            RUVIA_CHECK(calls > 0);
            RUVIA_CHECK(!callback_lifetime.expired());
        }
        RUVIA_CHECK(callback_lifetime.expired());
    }
}

RUVIA_TEST(tls_identity_loading_accepts_binary_passwords_up_to_callback_capacity) {
    ruvia::test::tls_identity files("password-capacity.ruvia-test.local");
    constexpr auto capacity = static_cast<std::size_t>(PEM_BUFSIZE);
    for (const std::size_t length : {capacity - 1, capacity, capacity + 1}) {
        std::string password(length, 'p');
        password[length / 2] = '\0';
        const auto key = write_encrypted_key(files, "capacity.pem", password);
        ruvia::detail::http_server_listener_definition::tls_identity_type identity;
        identity.certificate_chain_file_ = files.ca_file_.string();
        identity.private_key_file_ = key.string();
        identity.private_key_password_.assign(password.data(), password.size());
        asio::ssl::context context(asio::ssl::context::tls_server);
        password_callback_cleanup cleanup(context.native_handle());
        bool loaded = false;
        try {
            ruvia::detail::configure_http_server_tls_identity(context.native_handle(), identity, {});
            loaded = true;
        } catch (const asio::system_error&) {
        }
        RUVIA_CHECK(loaded == (length <= capacity));
        if (loaded) {
            RUVIA_CHECK(SSL_CTX_check_private_key(context.native_handle()) == 1);
        }
    }
}

RUVIA_TEST(tls_identity_loading_rejects_oversized_password_instead_of_using_empty_password) {
    ruvia::test::tls_identity files("password-error.ruvia-test.local");
    const auto key = write_encrypted_key(files, "empty-password.pem", "");
    ruvia::detail::http_server_listener_definition::tls_identity_type identity;
    identity.certificate_chain_file_ = files.ca_file_.string();
    identity.private_key_file_ = key.string();
    identity.private_key_password_.assign(PEM_BUFSIZE + 1U, 'p');
    asio::ssl::context context(asio::ssl::context::tls_server);
    password_callback_cleanup cleanup(context.native_handle());
    bool rejected = false;
    try {
        ruvia::detail::configure_http_server_tls_identity(context.native_handle(), identity, {});
    } catch (const asio::system_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(http_server_tls_identity_rejects_invalid_configuration) {
    using namespace ruvia::detail;

    http_server_listener_definition::tls_identity_type identity;
    asio::ssl::context context(asio::ssl::context::tls_server);
    const std::optional<http_server_listener_definition::tls_client_certificate_policy_type> no_policy;

    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        configure_http_server_tls_identity(nullptr, identity, no_policy);
    }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        configure_http_server_tls_identity(context.native_handle(), identity, no_policy);
    }));

    identity.certificate_chain_file_ = "/ruvia-test-missing-certificate.pem";
    identity.private_key_file_ = "/ruvia-test-missing-private-key.pem";
    bool preserved_system_error = false;
    try {
        configure_http_server_tls_identity(context.native_handle(), identity, no_policy);
    } catch (const asio::system_error&) {
        preserved_system_error = true;
    }
    RUVIA_CHECK(preserved_system_error);
}
