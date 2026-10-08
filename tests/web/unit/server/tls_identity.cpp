#include <optional>

#include <asio/ssl/context.hpp>
#include <asio/system_error.hpp>

#include "server/HttpServerTlsIdentity.h"
#include "test_harness.h"

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
