#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/tcp_socket_options.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/quic_types.h"
#include "ruvia/web/http3_qpack_config.h"
#include "ruvia/web/http_client_advertisement_config.h"
#include "ruvia/web/http_client_push_config.h"
#include "ruvia/web/tls_peer_verification.h"

namespace ruvia {

enum class http_client_protocol : std::uint8_t {
    negotiate,
    http1_only,
    http2_only,
    // HTTP/3 uses QUIC over UDP and never falls back to the TCP pool.
    http3_only,
};

enum class http_client_received_cookie_policy : std::uint8_t {
    ignore,
    retain_and_send,
};

// Bounds bytes retained by read_all() results: per standalone client or,
// in an application, across all registered aliases of one business worker. This is
// independent of per-response limits and worker-owned memory.
struct http_client_result_budget_config final {
    std::size_t max_retained_bytes_{std::size_t{64} * 1024 * 1024};
    // Worker-local live response allocations, including compressed input,
    // decoder output, metadata and simultaneous buffer growth.
    std::size_t max_in_flight_bytes_{std::size_t{64} * 1024 * 1024};
};

// Configuration for one http_client bound to one event_loop. application registration
// creates one such client per Web worker, so these limits remain per client
// without exposing the application deployment model in the standalone API.
struct http_client_config final {
    http_scheme scheme_{http_scheme::https};
    // Validated unbracketed transport host; DNS names may retain one trailing dot.
    std::string host_{};
    std::optional<std::uint16_t> port_{};
    std::size_t connection_count_{1};
    std::size_t max_concurrent_http2_streams_per_connection_{100};
    std::size_t max_buffered_requests_{1024};
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
    std::chrono::milliseconds connect_timeout_{5000};
    std::optional<std::chrono::milliseconds> write_timeout_{30000};
    std::optional<std::chrono::milliseconds> request_timeout_{30000};
    std::optional<std::chrono::milliseconds> acquire_timeout_{5000};
    std::size_t max_response_bytes_{default_max_buffered_body_bytes};
    http_client_protocol protocol_{http_client_protocol::negotiate};
    quic_version initial_quic_version_{quic_version::v1};
    // Enables client 0-RTT only for explicitly replay-safe bodyless GET/HEAD.
    bool http3_early_data_{false};
    http3_qpack_config qpack_{};
    http_client_advertisement_config advertisements_{};
    http_client_push_config push_{};
    tls_peer_verification_policy tls_peer_verification_{tls_peer_verification_policy::verify};
    tcp_no_delay_policy tcp_no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy tcp_keep_alive_{tcp_keep_alive_policy::enable};
    http_client_received_cookie_policy received_cookies_{http_client_received_cookie_policy::ignore};
    // TLS file paths must not contain NUL bytes.
    std::string ca_file_{};
    std::string certificate_chain_file_{};
    std::string private_key_file_{};
    // Binary password bytes, including embedded NUL, are preserved.
    std::string private_key_password_{};
    std::string user_agent_{"Ruvia"};
    std::vector<std::pair<std::string, std::string>> cookies_{};
};

class http_client_error final : public std::runtime_error {
public:
    enum class code_type : std::uint8_t {
        not_configured,
        invalid_request,
        timeout,
        cancelled,
        resolve_failed,
        connect_failed,
        tls_failed,
        protocol_unavailable,
        io_error,
        protocol_error,
        response_too_large,
        queue_full,
        closing,
        result_budget_exceeded,
    };

    http_client_error(code_type code, std::string_view message)
        : std::runtime_error(std::string(message)),
          code_(code) {}

    [[nodiscard]] code_type code() const noexcept {
        return code_;
    }

private:
    code_type code_;
};

struct http_client_stats final {
    std::size_t buffered_requests_{0};
    std::size_t in_flight_requests_{0};
    std::size_t completed_requests_{0};
    std::size_t failed_requests_{0};
    std::size_t bytes_sent_{0};
    std::size_t bytes_received_{0};
    std::size_t dropped_advertisements_{0};
    std::size_t received_pushes_{0};
    std::size_t rejected_pushes_{0};
};

}  // namespace ruvia
