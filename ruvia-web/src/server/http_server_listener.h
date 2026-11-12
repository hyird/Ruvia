#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/server_config.h"

#include "server/inherited_listener_sockets.h"
#include "tls/tls_credential.h"
#include "tls/tls_files.h"
#include "tls/tls_session_ticket_keys.h"

namespace ruvia::detail {

// A server-chosen QUIC connection ID carries its owning worker's index in two
// bytes, so packets can be steered to that worker.
inline constexpr std::size_t http3_max_worker_count = 65536;

// One TLS listener's identities, immutable once loaded: a run's workers and
// the stored listener definition share one set, and
// application::replace_tls_identities() publishes a new set instead of
// changing one.
struct http_server_tls_identities final {
    struct named_identity final {
        named_identity(std::string_view name, tls_certificate certificate, std::pmr::memory_resource* resource)
            : name_(name, pmr_resource_or_default(resource)),
              certificate_(std::move(certificate)) {}

        // ASCII lowercase: a host name, or the parent domain a wildcard's
        // "*" label stands in front of.
        std::pmr::string name_;
        tls_certificate certificate_;
    };

    struct addressed_identity final {
        // An IPv4-mapped IPv6 address is held in its IPv4 form.
        asio::ip::address address_;
        tls_certificate certificate_;
    };

    http_server_tls_identities(tls_certificate primary, std::pmr::memory_resource* resource)
        : primary_(std::move(primary)),
          exact_names_(pmr_resource_or_default(resource)),
          wildcard_names_(pmr_resource_or_default(resource)),
          local_addresses_(pmr_resource_or_default(resource)) {}

    // The identity of a handshake whose ClientHello offered `server_name`
    // (empty without one) and arrived at `local_address` (null when
    // unknown): an exact name, then a wildcard covering exactly the name's
    // left-most label (RFC 6125 §6.4.3), then the local address, else the
    // primary.
    [[nodiscard]] const tls_certificate& select(
        std::string_view server_name, const asio::ip::address* local_address) const noexcept;

    tls_certificate primary_;
    std::pmr::vector<named_identity> exact_names_;
    std::pmr::vector<named_identity> wildcard_names_;
    std::pmr::vector<addressed_identity> local_addresses_;
};

struct http_server_listener_definition final {
    // Every TLS member is loaded when the listener is configured: no identity
    // file name, in-memory PEM or private-key password survives in a
    // definition or its clones.
    struct tls_client_certificate_policy final {
        tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
        tls_trust_store client_ca_;
        // The configured bundle's name, only so replace_tls_identities() can
        // recognize the unchanged policy it requires.
        tls_file_name ca_file_;
    };

    struct tls final {
        explicit tls(std::shared_ptr<const http_server_tls_identities> identities) noexcept
            : identities_(std::move(identities)) {}

        // Never null. A worker's copy is the set its handshakes select from
        // now; only that worker replaces it, between handshakes.
        std::shared_ptr<const http_server_tls_identities> identities_;
        std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
        std::optional<tls_client_certificate_policy> client_certificates_;
        // Created once by server configuration validation and shared by every
        // worker, which copies its keys into its own TLS contexts' key set.
        std::shared_ptr<tls_session_ticket_key_ring> session_ticket_keys_;
    };

    struct plain_http final {};

    struct redirect_http_to_https final {
        std::uint16_t https_port_;
    };

    // The HTTP/3 service of a TLS listener: a QUIC endpoint on the same
    // address and UDP port_, serving the listener's TLS identities.
    struct http3_service final {
        std::uint16_t port_;
        std::chrono::seconds alt_svc_max_age_;
        bool early_data_{false};
        bool retry_{false};
        // Created once by server configuration validation and shared by every
        // worker: keys the connection IDs that steer packets between workers
        // and derives stateless reset tokens (RFC 9000 §10.3.2).
        std::shared_ptr<const std::array<std::uint8_t, 32>> secret_;
    };

    // listener_dispatch::connection: application::on_connection serves every
    // connection. A TLS listener offers alpn_ (tls_config::alpn_ or its
    // default) in server preference, encoded as an RFC 7301 §3.1
    // ProtocolNameList without its length prefix; empty for a cleartext one.
    struct connection_dispatch final {
        connection_dispatch(std::string_view alpn, std::pmr::memory_resource* resource)
            : alpn_(alpn, pmr_resource_or_default(resource)) {}

        std::pmr::string alpn_;
    };

    using transport_type = std::variant<plain_http, tls, redirect_http_to_https>;

    http_server_listener_definition(asio::ip::tcp::endpoint configured_endpoint,
        transport_type configured_transport = plain_http{},
        std::optional<http3_service> configured_http3 = std::nullopt)
        : endpoint_(std::move(configured_endpoint)),
          transport_(std::move(configured_transport)),
          http3_(std::move(configured_http3)) {}

    [[nodiscard]] http_server_listener_definition clone(std::pmr::memory_resource* resource) const;

    asio::ip::tcp::endpoint endpoint_;
    // listen_config::inherited_sockets_ of this listener, shared by its
    // clones; null when it binds. The run that prepares the listener claims
    // them; the stored definition drops them once a run took them.
    std::shared_ptr<inherited_listener_sockets> inherited_sockets_;
    transport_type transport_;
    // Only a TLS listener carries one.
    std::optional<http3_service> http3_;
    // Absent for listener_dispatch::routes.
    std::optional<connection_dispatch> connection_;
    // listener_dispatch::request_stream: application::on_request_stream serves
    // every request, on this listener and on its HTTP/3 service.
    bool request_stream_{false};
};

// Only validated configurations carry a key ring; an unvalidated definition
// must never reach a serving TLS context.
[[nodiscard]] inline tls_session_ticket_key_ring& tls_session_ticket_keys(
    const http_server_listener_definition::tls& tls) {
    if (tls.session_ticket_keys_ == nullptr) {
        throw std::logic_error("TLS listener session ticket keys were not generated");
    }
    return *tls.session_ticket_keys_;
}

class http_server_session_config final {
public:
    explicit http_server_session_config(const http_server_listener_definition& definition,
        std::pmr::memory_resource* resource);

    http_server_session_config(const http_server_session_config&) = delete;
    http_server_session_config& operator=(const http_server_session_config&) = delete;
    http_server_session_config(http_server_session_config&&) = delete;
    http_server_session_config& operator=(http_server_session_config&&) = delete;

    [[nodiscard]] const http_server_listener_definition::tls* tls() const& noexcept {
        return std::get_if<http_server_listener_definition::tls>(&transport_);
    }
    const http_server_listener_definition::tls* tls() const&& = delete;

    [[nodiscard]] const http_server_listener_definition::redirect_http_to_https* redirect()
        const& noexcept {
        return std::get_if<http_server_listener_definition::redirect_http_to_https>(&transport_);
    }
    const http_server_listener_definition::redirect_http_to_https* redirect() const&& = delete;

    // Worker thread, between handshakes: every ClientHello this listener (TLS
    // over TCP and its HTTP/3 service) processes later selects from
    // `identities`. Handshakes keep only the identity they selected, so the
    // previous set may be released here.
    void adopt_tls_identities(std::shared_ptr<const http_server_tls_identities> identities) noexcept;

    http_server_listener_definition::transport_type transport_;
    std::optional<http_server_listener_definition::http3_service> http3_;
    // Stable for the session's lifetime: the TLS contexts' ALPN selection
    // borrows connection_->alpn_.
    std::optional<http_server_listener_definition::connection_dispatch> connection_;
    bool request_stream_{false};
    // The Alt-Svc field value advertising http3_ (RFC 7838 §3), sent on every
    // response of this TLS listener; empty without an HTTP/3 service.
    std::pmr::string alternative_service_;
    // This worker's copy of the listener's ticket keys, refreshed on the
    // worker thread once per key epoch. The TLS contexts below point at it, so
    // it is declared before them and outlives them.
    std::optional<tls_session_ticket_key_set> ticket_keys_;
    // One context per family for the worker's whole run, carrying no
    // identity: replacing identities keeps its session cache and ticket keys,
    // and every handshake installs the identity it selects on its own SSL.
    // TLS over TCP:
    std::optional<asio::ssl::context> tls_context_;
    // TLS 1.3 with ALPN h3 for the HTTP/3 service:
    std::optional<asio::ssl::context> quic_tls_context_;
};

}  // namespace ruvia::detail
