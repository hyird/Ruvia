#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/worker_ref.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/http_client_types.h"

namespace ruvia::detail {

class http_client_result_budget_domain final {
public:
    explicit http_client_result_budget_domain(http_client_result_budget_config config);

    http_client_result_budget_domain(const http_client_result_budget_domain&) = delete;
    http_client_result_budget_domain& operator=(const http_client_result_budget_domain&) = delete;

    [[nodiscard]] bool try_reserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;

    // Only the owning worker accesses receive accounting. Escaped results
    // independently use the thread-safe retained-result counter above.
    [[nodiscard]] bool reserve_in_flight(std::size_t bytes) noexcept;
    void release_in_flight(std::size_t bytes) noexcept;

private:
    const std::size_t max_retained_bytes_;
    std::atomic<std::size_t> retained_bytes_{0};
    const std::size_t max_in_flight_bytes_;
    std::size_t in_flight_bytes_{0};
};

// Move-only reservation carried by an escaped result. It owns the budget domain
// independently of the client and releases its exact charge at destruction.
class http_client_result_budget_lease final {
public:
    http_client_result_budget_lease(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease& operator=(const http_client_result_budget_lease&) = delete;
    http_client_result_budget_lease(http_client_result_budget_lease&& other) noexcept;
    ~http_client_result_budget_lease();

    [[nodiscard]] static std::optional<http_client_result_budget_lease> try_acquire(
        const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes) noexcept;
    [[nodiscard]] std::pair<std::shared_ptr<http_client_result_budget_domain>, std::size_t> release() && noexcept {
        return {std::move(domain_), std::exchange(bytes_, 0)};
    }

private:
    http_client_result_budget_lease(
        const std::shared_ptr<http_client_result_budget_domain>& domain, std::size_t bytes_value) noexcept;

    // Keep the domain alive until after releasing the reservation.
    std::shared_ptr<http_client_result_budget_domain> domain_;
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_pool;

// Worker-affine bounds shared by every HTTP client alias of one owner (a
// standalone http_client, or one application worker): idle connections across
// aliases, ordered by one idle clock, and concurrent background DNS refreshes.
// Pools join with their first connection and leave before retiring them.
class http_client_worker_limits final {
public:
    explicit http_client_worker_limits(const http_client_worker_config& config);
    http_client_worker_limits(const http_client_worker_limits&) = delete;
    http_client_worker_limits& operator=(const http_client_worker_limits&) = delete;

    void join(http_client_pool& pool);
    void leave(http_client_pool& pool) noexcept;
    [[nodiscard]] std::uint64_t next_idle_order() noexcept {
        return ++idle_clock_;
    }
    // Closes the least recently used idle connections of any alias while the
    // worker keeps more than its bound; alias bounds apply first.
    void enforce_idle_limits() noexcept;
    // A background refresh lookup may start: false once the worker runs its
    // bound of them, and the caller defers to its next refresh period.
    [[nodiscard]] bool try_begin_dns_refresh() noexcept;
    void end_dns_refresh() noexcept;

private:
    void enforce_idle_limit(bool multiplexed, std::size_t maximum) noexcept;

    std::size_t max_idle_http1_;
    std::size_t max_idle_multiplexed_;
    std::size_t max_dns_refreshes_;
    std::size_t dns_refreshes_{0};
    std::uint64_t idle_clock_{0};
    std::vector<http_client_pool*> pools_{};
};

// The per-worker state every pool of one owner shares.
struct http_client_worker_services final {
    std::shared_ptr<http_client_result_budget_domain> result_budget_;
    std::shared_ptr<http_client_worker_limits> limits_;
};

// Validates `config`; throws std::invalid_argument.
[[nodiscard]] http_client_worker_services make_http_client_worker_services(const http_client_worker_config& config);

}  // namespace ruvia::detail

namespace ruvia::detail {
struct http_client_config_storage;
class http_client_request_storage;
struct client_received_cookie;

struct client_prepared_request_scratch final {
    static constexpr std::size_t inline_header_capacity = 16;
    static constexpr std::size_t inline_cookie_capacity = 256;

    client_prepared_request_scratch() = default;

    std::array<http_header_view, inline_header_capacity> inline_headers_;
    std::vector<http_header_view> overflow_headers_;
    std::array<char, inline_cookie_capacity> inline_cookie_header_;
    std::string overflow_cookie_header_;
};

// Origin policy owns cookies and automatic headers independently of connection
// negotiation, cancellation, and transport retirement.
class client_request_policy final {
public:
    explicit client_request_policy(const http_client_config_storage& config);
    // The complete view borrows the request, policy text and both scratch buffers;
    // keep their owners alive through protocol serialization.
    [[nodiscard]] http_client_request_view prepare_request(
        const http_client_request_storage& request, client_prepared_request_scratch& scratch);
    // Stored and configured cookies belong to the fixed origin: false for a
    // request whose authority override or tunnel authority names another
    // host or port. Requires a resolved request route.
    [[nodiscard]] bool addresses_origin(const http_client_request_storage& request) const noexcept;
    // Retains the Set-Cookie fields of a final head received for an origin
    // request with `request_target`.
    void retain_response_cookies(std::string_view request_target, std::span<const http_header> headers);

private:
    const http_client_config_storage& config_;
    http_cookie_jar jar_;
};
}  // namespace ruvia::detail

namespace ruvia {
class operation_deadline;
}  // namespace ruvia

namespace ruvia::detail {

class client_request_policy;
class http_client_response_state;
struct http_client_websocket_handshake;

// One received final response head, after its protocol driver stored the
// response fields in the state's headers_.
struct http_client_final_head final {
    http_received_status status_;
    http_protocol_version protocol_version_;
    std::optional<http_client_request_content_signal> request_content_signal_;
    // Target of a request addressed to the fixed origin, whose jar retains the
    // head's Set-Cookie fields; nullopt for a tunnel to another authority.
    std::optional<std::string_view> origin_target_;
    // The protocol's verdict that this head opens the request's tunnel: a 2xx
    // to CONNECT, or the HTTP/1 101 upgrade of CONNECT-UDP (RFC 9298 section 3).
    bool opens_tunnel_{false};
};

// Stores a received HTTP/2 or HTTP/3 final head in the state: its fields
// (moved when they already use the state's resource), Content-Length and the
// framing its status implies for the request method.
void retain_http_client_binary_response_head(http_client_response_state& state_value, http_client_response_head&& head);

// The final-head publication shared by HTTP/1, HTTP/2 and HTTP/3: stops an upload the
// response completed, records status, version and body plan, retains origin
// cookies, settles tunnel acceptance (rejecting an invalid CONNECT-UDP head),
// selects collection of an encoded body, then wakes the head consumer.
void publish_http_client_final_head(
    http_client_response_state& state_value, client_request_policy& policy, const http_client_final_head& head);

// Validates an RFC 8441 Extended CONNECT WebSocket response head (HTTP/2 or
// HTTP/3) and records the negotiated subprotocol and compression on `websocket`.
void accept_extended_connect_websocket_response(http_client_websocket_handshake& websocket,
    http_status_code status, std::span<const http_header> headers, bool stream_ended,
    std::string_view error_message);

// Ends a received body: decodes a collected Content-Encoding within the
// response byte limit, failing with timeout when `timeout` expired before or
// during decoding.
void finish_http_client_response_body(http_client_response_state& state_value, const operation_deadline& timeout,
    bool content_semantics_present, std::size_t max_decoded_bytes);

}  // namespace ruvia::detail

namespace ruvia::detail {

class http_client_pool;
class http_client_response_state;
class http_client_result_budget_domain;

// Response storage and in-flight budget accounting of one client pool, plus the
// registry of live response states so the pool can retire their transport
// borrows on join. Worker-affine, reference counted by the pool root and every
// live state.
class http_client_response_memory_domain final {
public:
    class deleter_type final {
    public:
        void operator()(http_client_response_memory_domain* domain) const noexcept;
    };

    using owner = std::unique_ptr<http_client_response_memory_domain, deleter_type>;

    [[nodiscard]] static owner create(const worker_ref& worker,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);

    http_client_response_memory_domain(const http_client_response_memory_domain&) = delete;
    http_client_response_memory_domain& operator=(const http_client_response_memory_domain&) = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept {
        return worker_;
    }

    void charge_in_flight(std::size_t bytes_value);
    void release_in_flight(std::size_t bytes_value) noexcept;

    [[nodiscard]] http_client_response_state* create_state(http_client_pool& pool);
    void destroy_state(http_client_response_state* state) noexcept;
    void detach_transport_bindings(http_client_pool& pool) noexcept;

private:
    enum class phase_type : unsigned char { prepared,
        active,
        retired };

    http_client_response_memory_domain(const worker_ref& worker_value,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    ~http_client_response_memory_domain();

    void destroy() noexcept;
    void require_current() const noexcept;
    void attach_state(http_client_response_state& state) noexcept;
    void unlink_state(http_client_response_state& state) noexcept;
    void retain() noexcept;
    void release() noexcept;
    void release_root() noexcept;

    worker_ref worker_;
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    http_client_response_state* state_head_{};
    std::size_t references_{1};
    phase_type phase_{phase_type::prepared};

    friend class deleter_type;
    friend class http_client_response_state;
};

class http_client_in_flight_reservation final {
public:
    constexpr http_client_in_flight_reservation() noexcept = default;
    explicit constexpr http_client_in_flight_reservation(http_client_response_memory_domain* domain) noexcept
        : domain_(domain) {}
    ~http_client_in_flight_reservation() {
        clear();
    }
    http_client_in_flight_reservation(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation& operator=(const http_client_in_flight_reservation&) = delete;
    http_client_in_flight_reservation(http_client_in_flight_reservation&& other) noexcept
        : domain_(std::exchange(other.domain_, nullptr)),
          bytes_(std::exchange(other.bytes_, 0)) {}
    http_client_in_flight_reservation& operator=(http_client_in_flight_reservation&& other) noexcept {
        if (this != &other) {
            clear();
            domain_ = std::exchange(other.domain_, nullptr);
            bytes_ = std::exchange(other.bytes_, 0);
        }
        return *this;
    }

    void resize_to(std::size_t target_bytes) {
        if (domain_ == nullptr || target_bytes == bytes_) {
            return;
        }
        if (target_bytes > bytes_) {
            domain_->charge_in_flight(target_bytes - bytes_);
        } else {
            domain_->release_in_flight(bytes_ - target_bytes);
        }
        bytes_ = target_bytes;
    }

    void shrink_to(std::size_t target_bytes) noexcept {
        if (domain_ != nullptr && target_bytes < bytes_) {
            domain_->release_in_flight(bytes_ - target_bytes);
            bytes_ = target_bytes;
        }
    }

    void clear() noexcept {
        if (domain_ != nullptr && bytes_ != 0) {
            domain_->release_in_flight(bytes_);
            bytes_ = 0;
        }
    }

    void swap(http_client_in_flight_reservation& other) noexcept {
        std::swap(domain_, other.domain_);
        std::swap(bytes_, other.bytes_);
    }

private:
    http_client_response_memory_domain* domain_{nullptr};
    std::size_t bytes_{0};
};

}  // namespace ruvia::detail
