#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"

#include "context_services_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::ConnInfo;
using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::PlainConnectionTransport;
using ruvia::RequestMemory;
using ruvia::TlsConnectionTransport;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::ContextServices;

// TLS details belong behind the typed transport variant, not flattened back
// onto ConnInfo. The end-to-end scheme, including TLS a trusted proxy
// terminated, remains separate from this hop's typed transport.

// scheme() describes the client's connection, so it must NOT be a synonym for
// "this hop is TLS".

[[nodiscard]] std::size_t activeTransportCount(const ConnInfo& info) noexcept {
    return static_cast<std::size_t>(info.plain() != nullptr) +
           static_cast<std::size_t>(info.tls() != nullptr);
}

}  // namespace

RUVIA_TEST(conn_info_transport_has_one_active_alternative) {
    const auto defaults = ruvia::test::testContextServices();
    RUVIA_CHECK(defaults.connInfo().plain() != nullptr);
    RUVIA_CHECK(defaults.connInfo().tls() == nullptr);
    RUVIA_CHECK(defaults.connInfo().remote().address().empty());
    RUVIA_CHECK_EQ(activeTransportCount(defaults.connInfo()), std::size_t{1});

    const auto plain = defaults.withPlainTransport("192.0.2.10");
    RUVIA_CHECK(plain.connInfo().plain() != nullptr);
    RUVIA_CHECK(plain.connInfo().tls() == nullptr);
    RUVIA_CHECK_EQ(plain.connInfo().remote().address(), std::string_view("192.0.2.10"));
    RUVIA_CHECK_EQ(activeTransportCount(plain.connInfo()), std::size_t{1});

    const auto tlsWithoutClientCertificate = plain.withTlsTransport("198.51.100.20");
    RUVIA_CHECK(tlsWithoutClientCertificate.connInfo().plain() == nullptr);
    const auto* tls = tlsWithoutClientCertificate.connInfo().tls();
    RUVIA_CHECK(tls != nullptr);
    RUVIA_CHECK(tls->clientCertificateSubject().empty());
    RUVIA_CHECK_EQ(activeTransportCount(tlsWithoutClientCertificate.connInfo()), std::size_t{1});

    const auto mutualTls = defaults.withTlsTransport("203.0.113.30", "CN=typed-client");
    RUVIA_CHECK(mutualTls.connInfo().plain() == nullptr);
    RUVIA_CHECK(mutualTls.connInfo().tls() != nullptr);
    RUVIA_CHECK_EQ(mutualTls.connInfo().tls()->clientCertificateSubject(),
        std::string_view("CN=typed-client"));
    RUVIA_CHECK_EQ(activeTransportCount(mutualTls.connInfo()), std::size_t{1});
}

RUVIA_TEST(context_preserves_typed_connection_info_for_handler) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    const HttpHeaderView headers[] = {HttpHeaderView{"Host", "example.test"}};
    auto [request, error] = ruvia::makeParsedHttpRequest(
        "GET", "/resource", headers, {}, memory.resource());
    RUVIA_CHECK(!error.has_value());

    const auto plainContext = ContextAccess::make(
        memory, request, ruvia::test::testContextServices().withPlainTransport("192.0.2.44"));
    const auto plainInfo = plainContext.conn();
    RUVIA_CHECK(plainInfo.plain() != nullptr);
    RUVIA_CHECK(plainInfo.tls() == nullptr);
    RUVIA_CHECK_EQ(plainInfo.remote().address(), std::string_view("192.0.2.44"));

    const auto tlsContext = ContextAccess::make(memory, request,
        ruvia::test::testContextServices().withTlsTransport("198.51.100.55", "CN=request-client"));
    const auto tlsInfo = tlsContext.conn();
    RUVIA_CHECK(tlsInfo.plain() == nullptr);
    RUVIA_CHECK(tlsInfo.tls() != nullptr);
    RUVIA_CHECK_EQ(tlsInfo.remote().address(), std::string_view("198.51.100.55"));
    RUVIA_CHECK_EQ(
        tlsInfo.tls()->clientCertificateSubject(), std::string_view("CN=request-client"));
}
