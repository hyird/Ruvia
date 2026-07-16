#include "test_harness.h"

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/detail/http/ContextServices.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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
using ruvia::detail::HttpRequestAccess;

template <typename Info>
concept HasLegacyConnInfoScalarAccessors = requires(const Info& info) {
    info.secure();
    info.clientCertificateSubject();
};

template <typename Services>
concept HasBooleanTransportRefinement = requires(
    const Services& services,
    std::string_view remoteAddress,
    std::string_view clientCertificateSubject,
    bool secure) {
    services.withTransport(
        remoteAddress,
        clientCertificateSubject,
        secure);
};

template <typename Services>
concept AcceptsRvaluePlainTransport = requires(const Services& services) {
    services.withPlainTransport(std::string("temporary"));
};

template <typename Services>
concept AcceptsRvalueTlsAddress = requires(const Services& services) {
    services.withTlsTransport(std::string("temporary"));
};

template <typename Services>
concept AcceptsRvalueTlsCertificate = requires(const Services& services) {
    services.withTlsTransport(
        std::string_view("stable"),
        std::string("temporary"));
};

template <typename Info>
concept ExposesRvalueTransportPointer = requires {
    std::declval<const Info&&>().plain();
    std::declval<const Info&&>().tls();
};

static_assert(std::is_same_v<
    decltype(std::declval<const ConnInfo&>().plain()),
    const PlainConnectionTransport*>);
static_assert(std::is_same_v<
    decltype(std::declval<const ConnInfo&>().tls()),
    const TlsConnectionTransport*>);
static_assert(std::is_same_v<
    decltype(std::declval<const ContextServices&>().connInfo()),
    const ConnInfo&>);
static_assert(!HasLegacyConnInfoScalarAccessors<ConnInfo>);
static_assert(!HasBooleanTransportRefinement<ContextServices>);
static_assert(!AcceptsRvaluePlainTransport<ContextServices>);
static_assert(!AcceptsRvalueTlsAddress<ContextServices>);
static_assert(!AcceptsRvalueTlsCertificate<ContextServices>);
static_assert(!ExposesRvalueTransportPointer<ConnInfo>);
static_assert(!std::is_default_constructible_v<PlainConnectionTransport>);
static_assert(!std::is_default_constructible_v<TlsConnectionTransport>);
static_assert(!std::is_default_constructible_v<ConnInfo>);
static_assert(std::is_nothrow_copy_constructible_v<ConnInfo>);
static_assert(std::is_nothrow_move_constructible_v<ConnInfo>);
static_assert(std::is_nothrow_copy_assignable_v<ConnInfo>);
static_assert(std::is_nothrow_move_assignable_v<ConnInfo>);

[[nodiscard]] std::size_t activeTransportCount(
    const ConnInfo& info) noexcept {
    return static_cast<std::size_t>(info.plain() != nullptr) +
        static_cast<std::size_t>(info.tls() != nullptr);
}

}  // namespace

RUVIA_TEST(conn_info_transport_has_one_active_alternative) {
    const ContextServices defaults;
    RUVIA_CHECK(defaults.connInfo().plain() != nullptr);
    RUVIA_CHECK(defaults.connInfo().tls() == nullptr);
    RUVIA_CHECK(defaults.connInfo().remote().address().empty());
    RUVIA_CHECK_EQ(activeTransportCount(defaults.connInfo()), std::size_t{1});

    const auto plain = defaults.withPlainTransport("192.0.2.10");
    RUVIA_CHECK(plain.connInfo().plain() != nullptr);
    RUVIA_CHECK(plain.connInfo().tls() == nullptr);
    RUVIA_CHECK_EQ(
        plain.connInfo().remote().address(),
        std::string_view("192.0.2.10"));
    RUVIA_CHECK_EQ(activeTransportCount(plain.connInfo()), std::size_t{1});

    const auto tlsWithoutClientCertificate =
        plain.withTlsTransport("198.51.100.20");
    RUVIA_CHECK(tlsWithoutClientCertificate.connInfo().plain() == nullptr);
    const auto* tls = tlsWithoutClientCertificate.connInfo().tls();
    RUVIA_CHECK(tls != nullptr);
    RUVIA_CHECK(tls->clientCertificateSubject().empty());
    RUVIA_CHECK_EQ(
        activeTransportCount(tlsWithoutClientCertificate.connInfo()),
        std::size_t{1});

    const auto mutualTls = defaults.withTlsTransport(
        "203.0.113.30",
        "CN=typed-client");
    RUVIA_CHECK(mutualTls.connInfo().plain() == nullptr);
    RUVIA_CHECK(mutualTls.connInfo().tls() != nullptr);
    RUVIA_CHECK_EQ(
        mutualTls.connInfo().tls()->clientCertificateSubject(),
        std::string_view("CN=typed-client"));
    RUVIA_CHECK_EQ(
        activeTransportCount(mutualTls.connInfo()),
        std::size_t{1});
}

RUVIA_TEST(context_preserves_typed_connection_info_for_handler) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setResource(request, memory.resource());
    HttpRequestAccess::setTarget(request, "/resource");
    HttpRequestAccess::setPath(request, "/resource");
    RUVIA_CHECK(HttpRequestAccess::addHeader(
        request,
        HttpHeaderView{"Host", "example.test"},
        HttpRequestAccess::knownHeaderSlot(
            ruvia::detail::RequestKnownHeader::kHost)));

    const auto plainContext = ContextAccess::make(
        memory,
        request,
        ContextServices{}.withPlainTransport("192.0.2.44"));
    const auto plainInfo = ruvia::getConnInfo(plainContext);
    RUVIA_CHECK(plainInfo.plain() != nullptr);
    RUVIA_CHECK(plainInfo.tls() == nullptr);
    RUVIA_CHECK_EQ(
        plainInfo.remote().address(),
        std::string_view("192.0.2.44"));

    const auto tlsContext = ContextAccess::make(
        memory,
        request,
        ContextServices{}.withTlsTransport(
            "198.51.100.55",
            "CN=request-client"));
    const auto tlsInfo = ruvia::getConnInfo(tlsContext);
    RUVIA_CHECK(tlsInfo.plain() == nullptr);
    RUVIA_CHECK(tlsInfo.tls() != nullptr);
    RUVIA_CHECK_EQ(
        tlsInfo.remote().address(),
        std::string_view("198.51.100.55"));
    RUVIA_CHECK_EQ(
        tlsInfo.tls()->clientCertificateSubject(),
        std::string_view("CN=request-client"));
}
