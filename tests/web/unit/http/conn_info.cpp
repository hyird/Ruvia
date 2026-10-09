#include "ruvia/web/conn_info.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "context_services_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::conn_info;
using ruvia::http_header_view;
using ruvia::http_request;
using ruvia::plain_connection_transport;
using ruvia::request_memory;
using ruvia::tls_connection_transport;
using ruvia::worker_memory;
using ruvia::detail::context_access;
using ruvia::detail::context_services;

// TLS details belong behind the typed transport variant, not flattened back
// onto conn_info. The end-to-end scheme, including TLS a trusted proxy
// terminated, remains separate from this hop's typed transport.

// scheme() describes the client's connection, so it must NOT be a synonym for
// "this hop is TLS".

[[nodiscard]] std::size_t active_transport_count(const conn_info& info) noexcept {
    return static_cast<std::size_t>(info.plain() != nullptr) +
           static_cast<std::size_t>(info.tls() != nullptr);
}

}  // namespace

RUVIA_TEST(conn_info_transport_has_one_active_alternative) {
    const auto defaults = ruvia::test::test_context_services();
    RUVIA_CHECK(defaults.get_conn_info().plain() != nullptr);
    RUVIA_CHECK(defaults.get_conn_info().tls() == nullptr);
    RUVIA_CHECK(defaults.get_conn_info().remote().address().empty());
    RUVIA_CHECK_EQ(active_transport_count(defaults.get_conn_info()), std::size_t{1});

    const auto plain = defaults.with_plain_transport("192.0.2.10");
    RUVIA_CHECK(plain.get_conn_info().plain() != nullptr);
    RUVIA_CHECK(plain.get_conn_info().tls() == nullptr);
    RUVIA_CHECK_EQ(plain.get_conn_info().remote().address(), std::string_view("192.0.2.10"));
    RUVIA_CHECK_EQ(plain.get_conn_info().remote().port(), std::uint16_t{0});
    RUVIA_CHECK_EQ(active_transport_count(plain.get_conn_info()), std::size_t{1});

    const auto tls_without_client_certificate = plain.with_tls_transport("198.51.100.20");
    RUVIA_CHECK(tls_without_client_certificate.get_conn_info().plain() == nullptr);
    const auto* tls = tls_without_client_certificate.get_conn_info().tls();
    RUVIA_CHECK(tls != nullptr);
    RUVIA_CHECK(tls->client_certificate_subject().empty());
    RUVIA_CHECK_EQ(active_transport_count(tls_without_client_certificate.get_conn_info()), std::size_t{1});

    const auto mutual_tls = defaults.with_tls_transport("203.0.113.30", "CN=typed-client");
    RUVIA_CHECK(mutual_tls.get_conn_info().plain() == nullptr);
    RUVIA_CHECK(mutual_tls.get_conn_info().tls() != nullptr);
    RUVIA_CHECK_EQ(mutual_tls.get_conn_info().tls()->client_certificate_subject(),
        std::string_view("CN=typed-client"));
    RUVIA_CHECK_EQ(active_transport_count(mutual_tls.get_conn_info()), std::size_t{1});
}

RUVIA_TEST(context_preserves_typed_connection_info_for_handler) {
    worker_memory worker;
    request_memory memory(worker);
    const http_header_view headers[] = {http_header_view{"Host", "example.test"}};
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "/resource", headers, {}, memory.resource());
    RUVIA_CHECK(!error.has_value());

    const auto plain_context = context_access::make(
        memory, request, ruvia::test::test_context_services().with_plain_transport("192.0.2.44"));
    const auto plain_info = plain_context.conn();
    RUVIA_CHECK(plain_info.plain() != nullptr);
    RUVIA_CHECK(plain_info.tls() == nullptr);
    RUVIA_CHECK_EQ(plain_info.remote().address(), std::string_view("192.0.2.44"));
    RUVIA_CHECK_EQ(plain_info.remote().port(), std::uint16_t{0});

    const auto tls_context = context_access::make(memory, request,
        ruvia::test::test_context_services().with_tls_transport("198.51.100.55", "CN=request-client"));
    const auto tls_info = tls_context.conn();
    RUVIA_CHECK(tls_info.plain() == nullptr);
    RUVIA_CHECK(tls_info.tls() != nullptr);
    RUVIA_CHECK_EQ(tls_info.remote().address(), std::string_view("198.51.100.55"));
    RUVIA_CHECK_EQ(
        tls_info.tls()->client_certificate_subject(), std::string_view("CN=request-client"));
}

RUVIA_TEST(context_connection_metadata_preserves_socket_peer_port) {
    const auto services = ruvia::test::test_context_services();
    const auto plain = services.with_plain_transport("192.0.2.10", 49152);
    RUVIA_CHECK_EQ(plain.get_conn_info().remote().port(), std::uint16_t{49152});
    RUVIA_CHECK_EQ(plain.get_conn_info().client().port(), std::uint16_t{49152});

    const auto tls = services.with_tls_transport("2001:db8::2", "CN=client", 44321);
    RUVIA_CHECK_EQ(tls.get_conn_info().remote().address(), std::string_view("2001:db8::2"));
    RUVIA_CHECK_EQ(tls.get_conn_info().remote().port(), std::uint16_t{44321});
    RUVIA_CHECK_EQ(tls.get_conn_info().client().port(), std::uint16_t{44321});
    RUVIA_CHECK_EQ(tls.get_conn_info().tls()->client_certificate_subject(),
        std::string_view("CN=client"));
}
