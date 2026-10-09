#include <cstddef>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>

#include <asio/ssl/context.hpp>
#include <asio/system_error.hpp>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include "client/ClientTransport.h"
#include "client/HttpClientConfigStorage.h"
#include "test_harness.h"
#include "tls_password_fixture.h"

RUVIA_TEST(client_tls_identity_loading_rejects_nul_file_paths_before_opening_the_prefix) {
    using namespace ruvia::detail;
    ruvia::test::tls_identity files("client-file-path.ruvia-test.local");
    const auto key = ruvia::test::write_encrypted_key(files, "key.pem", "path-password").string();
    const auto certificate = files.ca_file.string();
    for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
        for (const auto member : {&ruvia::HttpClientConfig::caFile,
                 &ruvia::HttpClientConfig::certificateChainFile,
                 &ruvia::HttpClientConfig::privateKeyFile}) {
            ruvia::HttpClientConfig config;
            config.host = "client-file-path.ruvia-test.local";
            config.scheme = ruvia::HttpScheme::kHttps;
            config.protocol = protocol == client_tls_protocol::quic
                                  ? ruvia::HttpClientProtocol::kHttp3Only
                                  : ruvia::HttpClientProtocol::kHttp1Only;
            config.caFile = certificate;
            config.certificateChainFile = certificate;
            config.privateKeyFile = key;
            config.privateKeyPassword = "path-password";
            auto& path = config.*member;
            path.push_back('\0');
            path.append("other.pem");
            RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                HttpClientConfigStorage storage(config, std::pmr::new_delete_resource());
            }));
            asio::ssl::context context(asio::ssl::context::tls_client);
            bool rejected = false;
            try {
                configure_client_tls_context(*context.native_handle(), clientTransportConfigView(config), protocol);
            } catch (const std::invalid_argument&) {
                rejected = true;
            } catch (const std::runtime_error&) {
            }
            RUVIA_CHECK(rejected);
            RUVIA_CHECK(SSL_CTX_get0_certificate(context.native_handle()) == nullptr);
            RUVIA_CHECK(SSL_CTX_get0_privatekey(context.native_handle()) == nullptr);
        }
    }
}

RUVIA_TEST(client_tls_identity_loading_does_not_prompt_for_unsupplied_password) {
    using namespace ruvia::detail;
    ruvia::test::tls_identity files("client-password-ui.ruvia-test.local");
    const auto required_key = ruvia::test::write_encrypted_key(files, "required.pem", "required-password").string();
    const auto empty_key = ruvia::test::write_encrypted_key(files, "empty.pem", "").string();
    const auto certificate = files.ca_file.string();
    int attempts = 0;
    ruvia::test::noninteractive_ui_scope ui(attempts);
    for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
        for (const bool explicit_default : {false, true}) {
            asio::ssl::context context(asio::ssl::context::tls_client);
            if (explicit_default) {
                SSL_CTX_set_default_passwd_cb(context.native_handle(), PEM_def_callback);
            }
            ClientTransportConfigView config;
            config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
            config.certificateChainFile = certificate;
            config.privateKeyFile = required_key;
            RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                configure_client_tls_context(*context.native_handle(), config, protocol);
            }));
            config.privateKeyFile = empty_key;
            bool loaded = false;
            try {
                configure_client_tls_context(*context.native_handle(), config, protocol);
                loaded = true;
            } catch (const std::runtime_error&) {
            }
            RUVIA_CHECK(loaded);
            if (loaded) {
                RUVIA_CHECK(SSL_CTX_check_private_key(context.native_handle()) == 1);
            }
        }
    }
    RUVIA_CHECK(attempts == 0);
}

RUVIA_TEST(client_tls_identity_loading_preserves_the_context_password_callback_owner) {
    using namespace ruvia::detail;
    ruvia::test::tls_identity files("client-password-owner.ruvia-test.local");
    const auto configured_key = ruvia::test::write_encrypted_key(files, "configured.pem", "configured-password").string();
    const auto original_key = ruvia::test::write_encrypted_key(files, "original.pem", "original-password").string();
    const auto certificate = files.ca_file.string();
    int attempts = 0;
    ruvia::test::noninteractive_ui_scope ui(attempts);
    for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
        for (const bool correct_password : {false, true}) {
            std::weak_ptr<std::string> callback_lifetime;
            int calls = 0;
            {
                asio::ssl::context context(asio::ssl::context::tls_client);
                auto password = std::make_shared<std::string>("original-password");
                callback_lifetime = password;
                context.set_password_callback([password, &calls](std::size_t, asio::ssl::context::password_purpose) {
                    ++calls;
                    return *password;
                });
                password.reset();
                ruvia::test::password_callback_cleanup cleanup(context.native_handle());
                ClientTransportConfigView config;
                config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
                config.certificateChainFile = certificate;
                config.privateKeyFile = configured_key;
                config.privateKeyPassword = correct_password ? "configured-password" : "incorrect-password";
                bool configured = false;
                try {
                    configure_client_tls_context(*context.native_handle(), config, protocol);
                    configured = true;
                } catch (const std::runtime_error&) {
                }
                RUVIA_CHECK(configured == correct_password);
                RUVIA_CHECK(calls == 0);
                bool loaded = false;
                try {
                    context.use_private_key_file(original_key, asio::ssl::context::pem);
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
    RUVIA_CHECK(attempts == 0);
}

RUVIA_TEST(client_tls_identity_loading_uses_a_caller_password_callback_when_password_is_unsupplied) {
    using namespace ruvia::detail;
    ruvia::test::tls_identity files("client-password-provider.ruvia-test.local");
    const auto key = ruvia::test::write_encrypted_key(files, "callback.pem", "callback-password").string();
    const auto certificate = files.ca_file.string();
    for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
        std::weak_ptr<std::string> callback_lifetime;
        int calls = 0;
        {
            asio::ssl::context context(asio::ssl::context::tls_client);
            auto password = std::make_shared<std::string>("callback-password");
            callback_lifetime = password;
            context.set_password_callback([password, &calls](std::size_t, asio::ssl::context::password_purpose) {
                ++calls;
                return *password;
            });
            password.reset();
            ruvia::test::password_callback_cleanup cleanup(context.native_handle());
            ClientTransportConfigView config;
            config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
            config.certificateChainFile = certificate;
            config.privateKeyFile = key;
            bool loaded = false;
            try {
                configure_client_tls_context(*context.native_handle(), config, protocol);
                loaded = true;
            } catch (const std::runtime_error&) {
            }
            RUVIA_CHECK(loaded);
            RUVIA_CHECK(calls > 0);
            if (loaded) {
                RUVIA_CHECK(SSL_CTX_check_private_key(context.native_handle()) == 1);
            }
        }
        RUVIA_CHECK(callback_lifetime.expired());
    }
}

RUVIA_TEST(client_tls_identity_loading_rejects_oversized_password_instead_of_using_empty_password) {
    using namespace ruvia::detail;
    ruvia::test::tls_identity files("client-password-error.ruvia-test.local");
    const auto key = ruvia::test::write_encrypted_key(files, "empty.pem", "").string();
    const auto certificate = files.ca_file.string();
    const std::string password(PEM_BUFSIZE + 1U, 'p');
    for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
        asio::ssl::context context(asio::ssl::context::tls_client);
        ClientTransportConfigView config;
        config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
        config.certificateChainFile = certificate;
        config.privateKeyFile = key;
        config.privateKeyPassword = password;
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            configure_client_tls_context(*context.native_handle(), config, protocol);
        }));
    }
}

RUVIA_TEST(client_tls_identity_loading_accepts_binary_passwords_up_to_callback_capacity) {
    using namespace ruvia::detail;
    ruvia::test::tls_identity files("client-password-capacity.ruvia-test.local");
    const auto certificate = files.ca_file.string();
    constexpr auto capacity = static_cast<std::size_t>(PEM_BUFSIZE);
    for (const std::size_t length : {capacity - 1, capacity, capacity + 1}) {
        std::string password(length, 'p');
        password[length / 2] = '\0';
        const auto key = ruvia::test::write_encrypted_key(files, "capacity.pem", password).string();
        for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
            asio::ssl::context context(asio::ssl::context::tls_client);
            ClientTransportConfigView config;
            config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification;
            config.certificateChainFile = certificate;
            config.privateKeyFile = key;
            config.privateKeyPassword = password;
            bool loaded = false;
            try {
                configure_client_tls_context(*context.native_handle(), config, protocol);
                loaded = true;
            } catch (const std::runtime_error&) {
            }
            RUVIA_CHECK(loaded == (length <= capacity));
            if (loaded) {
                RUVIA_CHECK(SSL_CTX_check_private_key(context.native_handle()) == 1);
            }
        }
    }
}
