#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_connect_udp.h"
#include "ruvia/http/http_connection_advertisement.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_push.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/http_tunnel.h"

namespace ruvia {

class context;
class response_stream_writer;

namespace detail {
class http_client_pool;
class http_client_registry;
class http_client_response_state;
class http_client_advertisement_queue;
struct http_client_advertisement_state;
class http_capsule_stream_state;
class http_client_result_budget_domain;
class http_client_result_budget_lease;
class http_client_state;
}  // namespace detail

// Immutable metadata retained by its http_client_response. Field views borrow
// that response and remain valid through subsequent body reads.
// HTTP/1 transfers its already-owned parsed fields without another field copy.
// Other protocol resources are copied into response-owned storage.
class http_client_informational_response final {
public:
    http_client_informational_response(const http_client_informational_response&) = delete;
    http_client_informational_response& operator=(const http_client_informational_response&) = delete;
    http_client_informational_response(http_client_informational_response&&) noexcept = default;
    http_client_informational_response& operator=(http_client_informational_response&&) = delete;
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::span<const http_header> headers() const& noexcept {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;

private:
    friend class detail::http_client_response_state;
    http_client_informational_response(http_status_code status, std::pmr::memory_resource* resource)
        : status_(status),
          headers_(resource) {}
    http_status_code status_;
    std::pmr::vector<http_header> headers_;
};

// Independently owned bytes collected from an outbound HTTP response body.
// The address-stable backing storage is detached from the client and worker
// lifecycle, and its read-only view remains valid across moves until destruction.
// Empty results retain neither backing storage nor a retained-byte reservation.
class http_client_response_bytes final {
public:
    http_client_response_bytes(const http_client_response_bytes&) = delete;
    http_client_response_bytes& operator=(const http_client_response_bytes&) = delete;
    http_client_response_bytes(http_client_response_bytes&&) noexcept;
    http_client_response_bytes& operator=(http_client_response_bytes&&) noexcept;
    ~http_client_response_bytes();

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept {
        return {data_.get(), size_};
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept {
        return {reinterpret_cast<const char*>(data_.get()), size_};
    }
    std::string_view text() const&& = delete;

private:
    friend class detail::http_client_response_state;

    http_client_response_bytes(
        std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value);
    http_client_response_bytes() noexcept = default;
    void append(std::span<const std::byte> bytes) noexcept;
    void release() noexcept;

    std::shared_ptr<detail::http_client_result_budget_domain> budget_domain_{};
    std::unique_ptr<std::byte[]> data_{};
    std::size_t size_{0};
    std::size_t reserved_{0};
};

// A borrow-only facade embedded in http_client_response. It cannot be detached,
// independently destroyed, or moved away from the response. Body operations
// bind directly to the response's address-stable transport state, so moving
// their public response owner does not invalidate a cold or running operation.
// HTTP/1 body-suppressed and known-zero-length responses do not allocate a
// framing-decoder scratch block; other response storage and budgets still apply.
// A body whose Content-Encoding stack is fully supported (gzip, deflate, br,
// zstd) is decoded before the first byte is exposed. A 206 body, or a stack
// containing an unsupported coding, is exposed as received with its
// Content-Encoding field kept; a malformed Content-Encoding field fails the
// response with protocol_error.
class http_client_response_body final {
public:
    http_client_response_body(const http_client_response_body&) = delete;
    http_client_response_body& operator=(const http_client_response_body&) = delete;
    http_client_response_body(http_client_response_body&&) = delete;
    http_client_response_body& operator=(http_client_response_body&&) = delete;

    // The returned view remains valid until the next body operation starts. A null
    // optional is the only end-of-body signal; an empty data chunk is never
    // returned. Reads are linear and concurrent operations are rejected.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() &;
    task<std::optional<std::span<const std::byte>>> read() && = delete;

    // Reads one chunk as characters, without decoding or charset validation.
    // Encoded characters may straddle chunks. Uses the same linear read lane.
    [[nodiscard]] task<std::optional<std::string_view>> text() &;
    task<std::optional<std::string_view>> text() && = delete;

    // Collects the unread remainder of this same stream. max_bytes is a caller
    // bound in addition to the origin's transport bound.
    [[nodiscard]] task<http_client_response_bytes> read_all(
        std::size_t max_bytes = default_max_buffered_body_bytes) &;
    task<http_client_response_bytes> read_all(
        std::size_t = default_max_buffered_body_bytes) && = delete;

    // Copies this same stream into a controller response stream with natural
    // backpressure. This is the common forwarding path for both small and
    // long-lived upstream responses.
    [[nodiscard]] task<void> pipe_to(response_stream_writer& output) &;
    task<void> pipe_to(response_stream_writer&) && = delete;

    [[nodiscard]] bool complete() const noexcept;

private:
    friend class http_client_response;

    ~http_client_response_body() = default;

    explicit http_client_response_body(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}

    detail::http_client_response_state* state_{nullptr};
};

// Owns worker-affine response storage independently of its client transport.
// May survive client shutdown/destruction on that worker, but must be destroyed
// before the bound event_loop retires. Metadata views borrow this response;
// body chunks remain valid until the next body operation starts.
class http_client_response final {
public:
    http_client_response(const http_client_response&) = delete;
    http_client_response& operator=(const http_client_response&) = delete;
    // Body operations bind to the address-stable response state, so the public
    // owner remains movable while an operation is cold or running.
    http_client_response(http_client_response&& other) noexcept;
    http_client_response& operator=(http_client_response&& other) noexcept;
    ~http_client_response();

    // Informational heads are retained in wire order without a count cap.
    // Aggregate field-byte and in-flight allocation budgets still apply.
    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& noexcept;
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    // The status the response is processed as; a received 600..999 is
    // processed as 500 (RFC 9110 section 15).
    [[nodiscard]] http_status_code status() const noexcept;
    // The status code as received, including an invalid 600..999.
    [[nodiscard]] http_received_status received_status() const noexcept;
    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    // Queues RFC 9218 PRIORITY_UPDATE on a live HTTP/2 response; a
    // response the peer has already completed is left unchanged. Throws
    // http_client_error(closing) once the response transport is retired.
    // Worker-affine; the connection driver owns writing the queued frame.
    void reprioritize(http_priority priority) &;
    void reprioritize(http_priority) && = delete;
    [[nodiscard]] std::span<const http_header> headers() const& noexcept;
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::span<const http_header> trailers() const& noexcept;
    [[nodiscard]] std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view) const&& = delete;
    [[nodiscard]] http_client_response_body& body() & noexcept {
        return body_;
    }
    [[nodiscard]] const http_client_response_body& body() const& = delete;
    [[nodiscard]] http_client_response_body& body() && = delete;

private:
    friend class detail::http_client_pool;
    friend class http_client_exchange;
    friend class http_client_tunnel;
    friend class http_capsule_stream;
    friend class detail::http_capsule_stream_state;
    friend class http_client_push;
    friend class http_client_request_body_writer;

    explicit http_client_response(detail::http_client_pool& pool);
    explicit http_client_response(detail::http_client_response_state* state_value) noexcept;
    void release() noexcept;

    detail::http_client_response_state* state_{nullptr};
    http_client_response_body body_{state_};
    bool consumer_{true};
};

// Owns an established CONNECT byte stream on its client worker. Metadata borrows
// this owner; a read view lasts until the next read starts. Inputs are copied before a
// cold write is returned. One running read and one running write/finish may coexist; cold operations do not reserve either lane. Moving the
// owner preserves operations. Destruction aborts both directions; shutdown joins
// transport drivers. finish() closes only the local sending direction and is
// idempotent after successful completion, including normal transport retirement.
class http_client_tunnel final {
public:
    http_client_tunnel(const http_client_tunnel&) = delete;
    http_client_tunnel& operator=(const http_client_tunnel&) = delete;
    http_client_tunnel(http_client_tunnel&& other) noexcept;
    http_client_tunnel& operator=(http_client_tunnel&& other) noexcept;
    ~http_client_tunnel();
    [[nodiscard]] http_status_code status() const noexcept {
        return response_.status();
    }
    [[nodiscard]] http_protocol_version protocol_version() const noexcept {
        return response_.protocol_version();
    }
    [[nodiscard]] std::span<const http_header> headers() const& noexcept {
        return response_.headers();
    }
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept {
        return response_.header(name);
    }
    std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& noexcept {
        return response_.informational_responses();
    }
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() &;
    task<std::optional<std::span<const std::byte>>> read() && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes) &;
    task<void> write(std::string_view) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes) &;
    task<void> write(std::span<const std::byte>) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes) & {
        return write_owned(std::move(bytes));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> write(string_type&&) && = delete;
    // A successful finish stays idempotent after transport retirement.
    [[nodiscard]] task<void> finish() &;
    task<void> finish() && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Transfers tunnel ownership to the Capsule Protocol adapter.
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &&;
    http_capsule_stream capsules(http_capsule_config = {}) & = delete;
    // Checked ownership transfers for accepted, negotiated datagram/CONNECT-UDP
    // tunnels. Ordinary CONNECT throws std::logic_error without consuming this
    // owner. The Capsule Protocol limits apply directly to encoded datagrams.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &&;
    http_datagram_stream datagrams(http_capsule_config = {}) & = delete;
    [[nodiscard]] http_udp_tunnel udp(http_capsule_config config = {}) &&;
    http_udp_tunnel udp(http_capsule_config = {}) & = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    explicit http_client_tunnel(http_client_response response) noexcept;
    [[nodiscard]] task<void> write_owned(std::pmr::string&& bytes);
    void release() noexcept;
    http_client_response response_;
};

// A rejected CONNECT keeps its ordinary HTTP status, headers and response body.
// Only a successful handshake exposes a tunnel.
class http_client_tunnel_result final {
public:
    http_client_tunnel_result(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result& operator=(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result(http_client_tunnel_result&&) noexcept = default;
    http_client_tunnel_result& operator=(http_client_tunnel_result&&) noexcept = default;
    [[nodiscard]] http_client_tunnel* tunnel() & noexcept {
        return std::get_if<http_client_tunnel>(&value_);
    }
    http_client_tunnel* tunnel() && = delete;
    [[nodiscard]] http_client_response* response() & noexcept {
        return std::get_if<http_client_response>(&value_);
    }
    http_client_response* response() && = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    explicit http_client_tunnel_result(http_client_tunnel tunnel) noexcept
        : value_(std::move(tunnel)) {}
    explicit http_client_tunnel_result(http_client_response response) noexcept
        : value_(std::move(response)) {}
    std::variant<http_client_tunnel, http_client_response> value_;
};

// A worker-affine, independently owned observation. Its metadata survives
// client shutdown and later observations, and expires when this owner dies.
// Destroy before the event_loop retires. A slot identifies the fixed client
// pool position, not a persistent connection across reconnects.
class http_client_advertisement final {
public:
    http_client_advertisement(http_client_advertisement&& other) noexcept;
    http_client_advertisement& operator=(http_client_advertisement&& other) noexcept;
    http_client_advertisement(const http_client_advertisement&) = delete;
    http_client_advertisement& operator=(const http_client_advertisement&) = delete;
    ~http_client_advertisement();

    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] std::size_t connection_slot() const noexcept;
    [[nodiscard]] const http_origin_advertisement* origins() const& noexcept;
    const http_origin_advertisement* origins() const&& = delete;
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service() const& noexcept;
    const http_alternative_service_advertisement* alternative_service() const&& = delete;

private:
    friend class detail::http_client_advertisement_queue;
    explicit http_client_advertisement(detail::http_client_advertisement_state* state_value) noexcept
        : state_(state_value) {}
    detail::http_client_advertisement_state* state_{};
};

// An accepted promise owns its copied request and response storage on the
// client worker. Destruction cancels unfinished push. Like an ordinary response,
// it can survive client shutdown but must retire before the event_loop.
class http_client_push final {
public:
    http_client_push(const http_client_push&) = delete;
    http_client_push& operator=(const http_client_push&) = delete;
    http_client_push(http_client_push&&) noexcept;
    http_client_push& operator=(http_client_push&&) noexcept;
    ~http_client_push();
    [[nodiscard]] const http_push_request& request() const&;
    const http_push_request& request() const&& = delete;
    // Transfers the response once, when the operation is started. Promise
    // metadata remains available through this push until it is destroyed.
    [[nodiscard]] task<http_client_response> response() &;
    task<http_client_response> response() && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_push(http_client_response response) noexcept;
    void release() noexcept;
    static task<http_client_response> receive_owned(http_client_response pin);
    http_client_response response_;
};

// One upload lane embedded in its exchange. Each chunk/trailer input is owned
// before return; completing write() means transport accepted the entire chunk.
// Only one write/end operation may run at a time. Cold operations can coexist
// and claim the output lane when awaited. Chunk size is bounded by
// max_chunk_bytes_; split larger sources into sequential writes.
class http_client_request_body_writer final {
public:
    http_client_request_body_writer(const http_client_request_body_writer&) = delete;
    http_client_request_body_writer& operator=(const http_client_request_body_writer&) = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes) &;
    task<void> write(std::span<const std::byte>) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes) &;
    task<void> write(std::string_view) && = delete;
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes) & {
        return write_owned(std::move(bytes));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> write(string_type&&) && = delete;
    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers = {}) &;
    task<void> end(std::span<const http_header_view> = {}) && = delete;
    [[nodiscard]] bool complete() const noexcept;

private:
    friend class http_client_exchange;
    explicit http_client_request_body_writer(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}
    ~http_client_request_body_writer() = default;
    [[nodiscard]] task<void> write_owned(std::pmr::string&& bytes);
    detail::http_client_response_state* state_;
};

// Owns a streaming request and its eventual response on one worker. Moving
// preserves operations because their state is address stable. Destruction
// abandons unfinished upload; client shutdown wakes and joins its producer.
class http_client_exchange final {
public:
    http_client_exchange(const http_client_exchange&) = delete;
    http_client_exchange& operator=(const http_client_exchange&) = delete;
    http_client_exchange(http_client_exchange&& other) noexcept;
    http_client_exchange& operator=(http_client_exchange&& other) noexcept;
    ~http_client_exchange();
    [[nodiscard]] http_client_request_body_writer& body() & noexcept {
        return body_;
    }
    http_client_request_body_writer& body() && = delete;
    [[nodiscard]] task<http_client_response> response() &;
    task<http_client_response> response() && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_exchange(http_client_response response) noexcept;
    void release() noexcept;
    static task<http_client_response> receive_owned(http_client_response pin);
    http_client_response response_;
    http_client_request_body_writer body_;
};

class http_client_handle final {
public:
    http_client_handle(const http_client_handle& other);
    http_client_handle& operator=(const http_client_handle&) = delete;

    [[nodiscard]] http_client_handle with_options(operation_options options) const;
    [[nodiscard]] task<http_client_response> send(
        const http_client_request_view& request) const;
    [[nodiscard]] task<http_client_exchange> open_request(
        const http_client_request_view& head, http_client_upload_config upload = {}) const;
    // CONNECT (empty protocol_) or Extended CONNECT. protocol_ "connect-udp"
    // requires exactly one caller-supplied Capsule-Protocol: ?1 field on every
    // HTTP version (RFC 9297 3.4); otherwise std::invalid_argument.
    [[nodiscard]] task<http_client_tunnel_result> open_tunnel(const http_client_tunnel_request_view& request, http_client_tunnel_config config = {}) const;
    // Negotiates RFC 9298 and returns an accepted tunnel or ordinary rejection.
    // The proxy authority comes from this registered origin. Capsule-Protocol
    // and HTTP/1 Upgrade fields are driver-owned. Use accepted.tunnel()->udp().
    [[nodiscard]] task<http_client_tunnel_result> open_udp_tunnel(const http_client_udp_tunnel_request_view& request, http_client_tunnel_config config = {}) const;
    [[nodiscard]] http_client_stats stats() const;
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() const;
    [[nodiscard]] std::optional<http_client_push> next_push() const;
    [[nodiscard]] std::string_view host() const&;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] http_scheme scheme() const;

private:
    friend class detail::http_client_registry;
    friend class detail::http_client_state;
    friend class context;
    friend class web_worker_context;
    http_client_handle(detail::http_client_pool& pool, std::pmr::memory_resource* resource,
        ::ruvia::operation_scope& scope) noexcept;
    http_client_handle(detail::http_client_pool& pool, std::pmr::memory_resource* resource,
        ::ruvia::operation_scope& scope, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;

    detail::http_client_pool* pool_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    operation_options options_;
    scoped_capability_registration registration_;
};

// One outbound HTTP origin bound to one Ruvia event loop. Construction does
// not create a thread, and connections are established lazily by send().
class http_client final {
public:
    http_client(event_loop loop, const http_client_config& config,
        http_client_result_budget_config result_budget = {});
    ~http_client();

    http_client(const http_client&) = delete;
    http_client& operator=(const http_client&) = delete;
    http_client(http_client&&) = delete;
    http_client& operator=(http_client&&) = delete;

    // Handles and operations borrow this client's open lifecycle. Starting one
    // from a temporary would immediately run the client destructor and begin
    // shutdown, so only lvalue clients may create them.
    [[nodiscard]] http_client_handle with_options(operation_options options) const&;
    http_client_handle with_options(operation_options) const&& = delete;
    // Returns the default-options handle. Each call registers one allocation-free
    // capability with the client operation scope, so keep the returned handle for
    // a sequence of requests instead of calling handle() per request.
    [[nodiscard]] http_client_handle handle() const&;
    http_client_handle handle() const&& = delete;

    // Idempotent and callable from any thread. It only requests immediate
    // shutdown; use shutdown() when the worker teardown must be awaited.
    void close() noexcept;
    // Cancels and joins client producers/send operations, then retires their
    // transport borrows on the bound loop. Response body consumers are owned by
    // the response, not this shutdown: finish/join them and destroy responses
    // on the owning worker before the event_loop retires.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;

private:
    std::shared_ptr<detail::http_client_state> state_;
};

}  // namespace ruvia
