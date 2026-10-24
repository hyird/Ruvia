#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_datagram.h"

namespace ruvia {

namespace detail {
class http_capsule_stream_state;
class capsule_state_pin;
template <typename stream_type>
class http_tunnel_session;
}  // namespace detail

class http_tunnel;
class http_client_tunnel;
class http_udp_tunnel;
class http_datagram_stream;

// Owns one capsule payload. Its storage remains valid across subsequent reads
// and stream destruction. Like other worker results, destroy it on its owner
// worker before that worker retires.
class http_capsule final {
public:
    http_capsule(const http_capsule&) = delete;
    http_capsule& operator=(const http_capsule&) = delete;
    http_capsule(http_capsule&& other) noexcept;
    http_capsule& operator=(http_capsule&& other) noexcept;
    ~http_capsule();
    [[nodiscard]] std::uint64_t type() const noexcept {
        return type_;
    }
    [[nodiscard]] std::string_view payload() const& noexcept {
        return payload_ ? std::string_view(*payload_) : std::string_view{};
    }
    std::string_view payload() const&& = delete;

private:
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::pmr::string payload_value) noexcept;
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
    std::uint64_t type_{};
    std::optional<std::pmr::string> payload_;
};

// RFC 9297 reliable Capsule Protocol over an established tunnel. The protocol
// using the capsules owns negotiation. Server streams borrow their route
// tunnel; client streams take tunnel ownership. Use this adapter exclusively:
// do not interleave raw tunnel operations with capsule operations. Abort wakes
// pending I/O; join started operations before destroying their scope owner.
// Operations occupy their read or write/finish lane when started; unstarted
// operations may coexist. One read and one output operation may run concurrently.
class http_capsule_stream final {
public:
    http_capsule_stream(const http_capsule_stream&) = delete;
    http_capsule_stream& operator=(const http_capsule_stream&) = delete;
    http_capsule_stream(http_capsule_stream&& other) noexcept;
    http_capsule_stream& operator=(http_capsule_stream&& other) noexcept;
    ~http_capsule_stream();
    [[nodiscard]] task<std::optional<http_capsule>> read() &;
    task<std::optional<http_capsule>> read() && = delete;
    // Owns input before returning its cold operation. Unknown capsule types are
    // delivered to the protocol consumer, which must ignore types it does not understand.
    [[nodiscard]] task<void> write(std::uint64_t type, std::string_view payload) &;
    task<void> write(std::uint64_t, std::string_view) && = delete;
    [[nodiscard]] task<void> write(std::uint64_t type, std::span<const std::byte> payload) &;
    task<void> write(std::uint64_t, std::span<const std::byte>) && = delete;
    [[nodiscard]] task<void> finish() &;
    task<void> finish() && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    friend class http_datagram_stream;
    [[nodiscard]] static task<std::optional<http_capsule>> read_owned(detail::capsule_state_pin pin);
    [[nodiscard]] task<void> write_frame(std::string_view prefix, std::string_view payload);
    explicit http_capsule_stream(http_tunnel& tunnel, http_capsule_config config);
    explicit http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config);
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
};

// Owned opaque HTTP Datagram payload, valid across later reads and client
// shutdown. Destroy results on their worker before the worker retires.
class http_datagram final {
public:
    http_datagram(const http_datagram&) = delete;
    http_datagram& operator=(const http_datagram&) = delete;
    http_datagram(http_datagram&&) noexcept = default;
    http_datagram& operator=(http_datagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept {
        auto bytes_value = capsule_.payload();
        bytes_value.remove_prefix(std::min(offset_, bytes_value.size()));
        return std::as_bytes(std::span(bytes_value.data(), bytes_value.size()));
    }
    std::span<const std::byte> payload() const&& = delete;

private:
    friend class http_datagram_stream;
    http_datagram(http_capsule capsule, std::size_t offset) noexcept
        : capsule_(std::move(capsule)),
          offset_(offset) {}
    http_capsule capsule_;
    std::size_t offset_{};
};

// RFC 9297 channel using reliable DATAGRAM capsules over a negotiated tunnel.
// Create through http_tunnel::datagrams() or http_client_tunnel::datagrams();
// arbitrary Capsule Protocol streams cannot be promoted to this channel.
// Empty payloads are data; an empty optional is EOF. Use exclusively with the tunnel.
// Operations occupy separate read and send/finish lanes when started; unstarted
// operations may coexist. send() owns its payload before returning.
class http_datagram_stream final {
public:
    http_datagram_stream(const http_datagram_stream&) = delete;
    http_datagram_stream& operator=(const http_datagram_stream&) = delete;
    http_datagram_stream(http_datagram_stream&&) noexcept = default;
    http_datagram_stream& operator=(http_datagram_stream&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read() &;
    task<std::optional<http_datagram>> read() && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload) &;
    task<void> send(std::span<const std::byte>) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload) &;
    task<void> send(std::string_view) && = delete;
    [[nodiscard]] task<void> finish() & {
        return stream_.finish();
    }
    task<void> finish() && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    explicit http_datagram_stream(http_capsule_stream&& stream);
    static task<std::optional<http_datagram>> read_owned(detail::capsule_state_pin pin, bool udp);
    [[nodiscard]] task<void> send_payload(std::string_view payload, bool udp);
    http_capsule_stream stream_;
};

// RFC 9297 HTTP Datagrams and reliable DATAGRAM capsules for a negotiated RFC 9298 CONNECT-UDP
// tunnel. Unknown capsule types and unknown context IDs are silently dropped;
// malformed DATAGRAM capsules terminate only this tunnel. Empty UDP packets are
// data; an empty optional is EOF. Owns its capsule stream and preserves separate
// read and send/finish lanes when moved.
// Operations occupy their lane when started; unstarted operations may coexist.
// send() owns its payload before returning.
class http_udp_tunnel final {
public:
    explicit http_udp_tunnel(http_datagram_stream stream) noexcept
        : stream_(std::move(stream)) {}
    http_udp_tunnel(const http_udp_tunnel&) = delete;
    http_udp_tunnel& operator=(const http_udp_tunnel&) = delete;
    http_udp_tunnel(http_udp_tunnel&&) noexcept = default;
    http_udp_tunnel& operator=(http_udp_tunnel&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read() &;
    task<std::optional<http_datagram>> read() && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload) &;
    task<void> send(std::span<const std::byte>) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload) &;
    task<void> send(std::string_view) && = delete;
    [[nodiscard]] task<void> finish() & {
        return stream_.finish();
    }
    task<void> finish() && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    http_datagram_stream stream_;
};
struct http_tunnel_route_config final {
    // Once the handler returns, bound flushing the local FIN and draining peer
    // input. A handler may half-close and continue reading without this deadline.
    std::chrono::milliseconds peer_transport_fin_timeout_{5000};
    // Declares HTTP Datagram semantics (capsules) for an upgrade-protocol
    // tunnel. CONNECT-UDP enables this automatically.
    bool datagrams_{};
};

// An established CONNECT byte stream. The session owns this facade and its
// allocator. Received chunks own their storage until destruction; it counts
// against the connection's and the worker's inbound buffer budgets
// (server_config::max_inbound_buffer_bytes_per_connection_ / _per_worker_), so
// destroy chunks before the tunnel handler returns. A read that would exceed
// a budget throws std::bad_alloc.
class http_tunnel final {
public:
    http_tunnel(const http_tunnel&) = delete;
    http_tunnel& operator=(const http_tunnel&) = delete;

    // One running read and one running output operation are allowed.
    // An operation occupies its lane when started; unstarted operations may coexist.
    [[nodiscard]] task<std::optional<std::pmr::string>> read() &;
    task<std::optional<std::pmr::string>> read() && = delete;
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

    // Half-close the send direction; reading remains available until peer EOF.
    [[nodiscard]] task<void> finish() &;
    task<void> finish() && = delete;
    void abort() noexcept;
    // Borrow the established route tunnel for HTTP Datagrams. The route protocol
    // owns negotiation; use only in its negotiated datagram handler, exclusively
    // with this tunnel. Capsule Protocol limits apply to encoded datagrams.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &;
    http_datagram_stream datagrams(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_udp_tunnel udp(http_capsule_config config = {}) &;
    http_udp_tunnel udp(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &;
    http_capsule_stream capsules(http_capsule_config = {}) && = delete;

private:
    template <typename stream_type>
    friend class detail::http_tunnel_session;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    using read_type = task<std::optional<std::pmr::string>> (*)(void*);
    using write_type = task<void> (*)(void*, std::string_view);
    using finish_type = task<void> (*)(void*);
    // malformed: the received tunnel data is malformed (RFC 9297 section 3.3).
    using abort_type = void (*)(void*, bool malformed) noexcept;
    http_tunnel(std::pmr::memory_resource& resource, const worker_handle& worker_value,
        void* target, read_type read, write_type write, finish_type finish_value, abort_type abort) noexcept;
    [[nodiscard]] task<void> write_owned(std::pmr::string&& bytes);
    void abort_input(bool malformed) noexcept;
    void require_active() const;
    std::pmr::memory_resource& resource_;
    const worker_handle& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    unsigned running_operations_{0};
    ::ruvia::operation_scope operations_;
};
}  // namespace ruvia
