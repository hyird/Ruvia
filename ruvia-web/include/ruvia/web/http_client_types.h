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

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/client_tls_config.h"
namespace ruvia {

enum class http_client_protocol : std::uint8_t {
    negotiate,
    http1_only,
    http2_only,
};

enum class http_client_received_cookie_policy : std::uint8_t {
    ignore,
    retain_and_send,
};

// Connection advertisements are optional observations. They do not change the
// fixed registered origin, connection coalescing, TLS identity or cache policy.
struct http_client_advertisement_config final {
    bool receive_origins_{false};
    bool receive_alternative_services_{false};
    std::size_t max_queued_advertisements_{32};
    // Includes observations retained by callers after polling the client.
    std::size_t max_retained_bytes_{64 * 1024};
};

struct http_client_push_config final {
    bool enabled_{false};
    std::size_t max_queued_pushes_{16};
    std::size_t max_concurrent_pushes_{16};
    std::optional<std::chrono::milliseconds> timeout_{30000};
};

struct http_client_upload_config final {
    // A known length must be fulfilled exactly; HTTP/1 trailers require chunked
    // framing. Without trailers, HTTP/1 finalization uses fixed-size wire storage.
    std::optional<std::uint64_t> content_length_{};
    http_client_request_expectation expectation_{http_client_request_expectation::none};
    std::size_t max_chunk_bytes_{64 * 1024};
    std::chrono::milliseconds continue_timeout_{1000};
};

// open_tunnel() and open_udp_tunnel() select the protocol; this policy bounds
// the size of one transport write, independently of Capsule payload limits.
struct http_client_tunnel_config final {
    std::size_t max_chunk_bytes_{64 * 1024};
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
    borrowed_text host_{};
    std::optional<std::uint16_t> port_{};
    std::size_t connection_count_{1};
    std::size_t max_concurrent_http2_streams_per_connection_{100};
    std::size_t max_buffered_requests_{1024};
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
    std::chrono::milliseconds connect_timeout_{5000};
    // Bound of each transport write. An ordinary HTTP/1 request head and
    // immediate content share one write; streamed upload chunks remain separate.
    std::optional<std::chrono::milliseconds> write_timeout_{30000};
    // Total deadline of one exchange, from slot acquisition through connect,
    // request transfer, response head and the complete body, including the
    // time the body producer waits for a slow consumer (read(), pipe_to()) to
    // drain max_response_bytes_ of buffered body. Expiry fails the response
    // with timeout and discards its connection. An operation's own timeout
    // replaces it; nullopt leaves the exchange unbounded.
    std::optional<std::chrono::milliseconds> request_timeout_{30000};
    // Bound of the wait for a free connection slot, within request_timeout_.
    std::optional<std::chrono::milliseconds> acquire_timeout_{5000};
    std::size_t max_response_bytes_{default_max_buffered_body_bytes};
    http_client_protocol protocol_{http_client_protocol::negotiate};
    http_client_advertisement_config advertisements_{};
    http_client_push_config push_{};
    // Used for https; client_tls_mode::disabled is rejected (select http instead).
    client_tls_config tls_{};
    client_tcp_config tcp_{};
    http_client_received_cookie_policy received_cookies_{http_client_received_cookie_policy::ignore};
    borrowed_text user_agent_{"Ruvia"};
    // Client-stored like retained cookies: never sent with TRACE (RFC 9110 §9.3.8).
    // Explicit caller Cookie fields are combined independently of the stored jar.
    std::vector<http_header_view> cookies_{};
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
    // Plaintext application octets completed at the transport's HTTP/tunnel
    // boundary, including a completed prefix reported with an I/O error. Includes
    // HTTP framing and tunnel payload; excludes TLS handshake/record overhead and
    // content-decoding expansion.
    std::size_t plaintext_bytes_sent_{0};
    std::size_t plaintext_bytes_received_{0};
    std::size_t dropped_advertisements_{0};
    std::size_t received_pushes_{0};
    std::size_t rejected_pushes_{0};
};

}  // namespace ruvia
