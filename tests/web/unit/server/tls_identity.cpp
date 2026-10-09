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

#include "server/HttpServerOptionsValidation.h"
#include "server/HttpServerTlsIdentity.h"
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
    HttpServerListenerDefinition::Tls valid;
    valid.identity.certificateChainFile = files.ca_file.string();
    valid.identity.privateKeyFile = key;
    valid.identity.privateKeyPassword = "path-password";
    valid.clientCertificates.emplace();
    valid.clientCertificates->verifyFile = files.ca_file.string();
    for (const int field : {0, 1, 2}) {
        auto tls = valid;
        auto& path = field == 0 ? tls.identity.certificateChainFile
                                : (field == 1 ? tls.identity.privateKeyFile : tls.clientCertificates->verifyFile);
        path.push_back('\0');
        path.append("other.pem");
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            validateHttpServerTlsOptions(tls);
        }));
        asio::ssl::context context(asio::ssl::context::tls_server);
        bool rejected = false;
        try {
            configureHttpServerTlsIdentity(context.native_handle(), tls.identity, tls.clientCertificates);
        } catch (const std::invalid_argument&) {
            rejected = true;
        } catch (const asio::system_error&) {
        }
        RUVIA_CHECK(rejected);
        RUVIA_CHECK(SSL_CTX_get0_certificate(context.native_handle()) == nullptr);
        RUVIA_CHECK(SSL_CTX_get0_privatekey(context.native_handle()) == nullptr);
        if (field != 2) {
            auto sni_tls = valid;
            sni_tls.sniIdentities.emplace_back();
            sni_tls.sniIdentities.back().host = "sni-file-path.ruvia-test.local";
            sni_tls.sniIdentities.back().identity = tls.identity;
            RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                validateHttpServerTlsOptions(sni_tls);
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
        ruvia::detail::HttpServerListenerDefinition::TlsIdentity identity;
        identity.certificateChainFile = files.ca_file.string();
        identity.privateKeyFile = required_key.string();
        asio::ssl::context context(asio::ssl::context::tls_server);
        if (explicit_default) {
            SSL_CTX_set_default_passwd_cb(context.native_handle(), PEM_def_callback);
        }
        bool rejected = false;
        try {
            ruvia::detail::configureHttpServerTlsIdentity(context.native_handle(), identity, {});
        } catch (const asio::system_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        identity.privateKeyFile = empty_key.string();
        bool loaded = false;
        try {
            ruvia::detail::configureHttpServerTlsIdentity(context.native_handle(), identity, {});
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
        ruvia::detail::HttpServerListenerDefinition::TlsIdentity identity;
        identity.certificateChainFile = files.ca_file.string();
        identity.privateKeyFile = key.string();
        asio::ssl::context context(asio::ssl::context::tls_server);
        auto password = std::make_shared<std::string>("callback-password");
        callback_lifetime = password;
        context.set_password_callback([password, &calls](std::size_t, asio::ssl::context::password_purpose) {
            ++calls;
            return *password;
        });
        password.reset();
        ruvia::detail::configureHttpServerTlsIdentity(context.native_handle(), identity, {});
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
            ruvia::detail::HttpServerListenerDefinition::TlsIdentity identity;
            identity.certificateChainFile = files.ca_file.string();
            identity.privateKeyFile = configured_key.string();
            identity.privateKeyPassword = correct_password ? "configured-password" : "incorrect-password";
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
                ruvia::detail::configureHttpServerTlsIdentity(context.native_handle(), identity, {});
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
        ruvia::detail::HttpServerListenerDefinition::TlsIdentity identity;
        identity.certificateChainFile = files.ca_file.string();
        identity.privateKeyFile = key.string();
        identity.privateKeyPassword.assign(password.data(), password.size());
        asio::ssl::context context(asio::ssl::context::tls_server);
        password_callback_cleanup cleanup(context.native_handle());
        bool loaded = false;
        try {
            ruvia::detail::configureHttpServerTlsIdentity(context.native_handle(), identity, {});
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
    ruvia::detail::HttpServerListenerDefinition::TlsIdentity identity;
    identity.certificateChainFile = files.ca_file.string();
    identity.privateKeyFile = key.string();
    identity.privateKeyPassword.assign(PEM_BUFSIZE + 1U, 'p');
    asio::ssl::context context(asio::ssl::context::tls_server);
    password_callback_cleanup cleanup(context.native_handle());
    bool rejected = false;
    try {
        ruvia::detail::configureHttpServerTlsIdentity(context.native_handle(), identity, {});
    } catch (const asio::system_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(httpServerTlsIdentityRejectsInvalidConfiguration) {
    using namespace ruvia::detail;

    HttpServerListenerDefinition::TlsIdentity identity;
    asio::ssl::context context(asio::ssl::context::tls_server);
    const std::optional<HttpServerListenerDefinition::TlsClientCertificatePolicy> noPolicy;

    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        configureHttpServerTlsIdentity(nullptr, identity, noPolicy);
    }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        configureHttpServerTlsIdentity(context.native_handle(), identity, noPolicy);
    }));

    identity.certificateChainFile = "/ruvia-test-missing-certificate.pem";
    identity.privateKeyFile = "/ruvia-test-missing-private-key.pem";
    bool preservedSystemError = false;
    try {
        configureHttpServerTlsIdentity(context.native_handle(), identity, noPolicy);
    } catch (const asio::system_error&) {
        preservedSystemError = true;
    }
    RUVIA_CHECK(preservedSystemError);
}
