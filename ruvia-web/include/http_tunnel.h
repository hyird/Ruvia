#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "borrowed_text.h"
#include "http_capsule.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "task.h"
#include "worker_ref.h"
namespace ruvia {
namespace detail {
class http_capsule_stream_state;
class capsule_state_pin;
class inbound_buffer_account;
template <typename stream_type>
class http_tunnel_session;
// Transport input for a single tunnel read lane. Datagram bytes exclude the
// HTTP/3 Quarter Stream ID but retain any protocol-specific Context ID.
struct http_tunnel_input final {
    std::string bytes_;
    bool datagram_{};
};
}  // namespace detail

class http_tunnel;
class http_client_tunnel;
class http_udp_tunnel;
class http_datagram_stream;

// Owns one capsule payload. Its storage remains valid across subsequent reads,
// stream destruction and connection retirement, even if input remains unread.
// Like other worker results, destroy it on its owner worker before that worker retires.
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
    [[nodiscard]] std::string_view payload() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload_ ? std::string_view(*payload_) : std::string_view{};
    }
    std::string_view payload() const&& = delete;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = payload();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload();
    }
    std::string_view text() const&& = delete;

private:
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::string payload_value) noexcept;
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
    std::uint64_t type_{};
    std::optional<std::string> payload_;
};

// RFC 9297 reliable Capsule Protocol over an established tunnel. The protocol
// using the capsules owns negotiation. Server streams borrow their route
// tunnel; client streams take tunnel ownership. Use this adapter exclusively:
// do not interleave raw tunnel operations with capsule operations. Abort wakes
// pending I/O; join started operations before destroying their scope owner.
// Operations occupy their read or write/finish lane when started; unstarted
// operations may coexist. One read and one output operation may run concurrently.
// `operation` bounds a whole call: a timeout or stop ends only that read,
// keeping a partly received capsule for the next one, and aborts the stream
// during an output operation. Either throws std::system_error (timed_out or
// operation_canceled) -- over an http_client_tunnel its http_client_error
// subclass -- and a stop requested before an output operation starts sends nothing.
class http_capsule_stream final {
public:
    http_capsule_stream(const http_capsule_stream&) = delete;
    http_capsule_stream& operator=(const http_capsule_stream&) = delete;
    http_capsule_stream(http_capsule_stream&& other) noexcept;
    http_capsule_stream& operator=(http_capsule_stream&& other) noexcept;
    ~http_capsule_stream();
    [[nodiscard]] task<std::optional<http_capsule>> read(operation_options operation = {}) &;
    task<std::optional<http_capsule>> read(operation_options = {}) && = delete;
    // Owns input before returning its cold operation. Unknown capsule types are
    // delivered to the protocol consumer, which must ignore types it does not understand.
    [[nodiscard]] task<void> write(std::uint64_t type, std::string_view payload, operation_options operation = {}) &;
    task<void> write(std::uint64_t, std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::uint64_t type, std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> write(std::uint64_t, std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(std::uint64_t type, string_type&& payload, operation_options operation = {}) & {
        return write_owned(type, std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(std::uint64_t, string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    friend class http_datagram_stream;
    [[nodiscard]] static task<std::optional<http_capsule>> read_owned(detail::capsule_state_pin pin, operation_options operation);
    [[nodiscard]] task<void> write_frame(std::string_view prefix, std::string_view payload, bool datagram, operation_options operation);
    [[nodiscard]] task<void> write_owned(std::uint64_t type, std::string&& payload, operation_options operation);
    [[nodiscard]] task<void> write_frame_owned(std::string_view prefix, std::string&& payload, bool datagram, operation_options operation);
    explicit http_capsule_stream(http_tunnel& tunnel, http_capsule_config config);
    explicit http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config);
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
};

// Owned opaque HTTP Datagram payload, valid across later reads, stream and
// connection retirement, and client shutdown even if input remains unread.
// Destroy results on their worker before the worker retires.
class http_datagram final {
public:
    http_datagram(const http_datagram&) = delete;
    http_datagram& operator=(const http_datagram&) = delete;
    http_datagram(http_datagram&&) noexcept = default;
    http_datagram& operator=(http_datagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = text();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> payload() const&& = delete;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload();
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        auto bytes_value = capsule_.payload();
        bytes_value.remove_prefix(std::min(offset_, bytes_value.size()));
        return bytes_value;
    }
    std::string_view text() const&& = delete;

private:
    friend class http_datagram_stream;
    http_datagram(http_capsule capsule, std::size_t offset) noexcept :
        capsule_(std::move(capsule)),
        offset_(offset) {}
    http_capsule capsule_;
    std::size_t offset_{};
};

// RFC 9297 channel using native QUIC DATAGRAMs when available, with reliable
// DATAGRAM capsule fallback. Reads accept both representations concurrently.
// Create through http_tunnel::datagrams() or http_client_tunnel::datagrams();
// arbitrary Capsule Protocol streams cannot be promoted to this channel.
// Empty payloads are data; an empty optional is EOF. Use exclusively with the tunnel.
// Operations occupy separate read and send/finish lanes when started; unstarted
// operations may coexist. send() owns its payload before returning. `operation`
// follows http_capsule_stream.
class http_datagram_stream final {
public:
    http_datagram_stream(const http_datagram_stream&) = delete;
    http_datagram_stream& operator=(const http_datagram_stream&) = delete;
    http_datagram_stream(http_datagram_stream&&) noexcept = default;
    http_datagram_stream& operator=(http_datagram_stream&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read(operation_options operation = {}) &;
    task<std::optional<http_datagram>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> send(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload, operation_options operation = {}) &;
    task<void> send(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> send(string_type&& payload, operation_options operation = {}) & {
        return send_owned(std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> send(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) & {
        return stream_.finish(std::move(operation));
    }
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    explicit http_datagram_stream(http_capsule_stream&& stream);
    static task<std::optional<http_datagram>> read_owned(detail::capsule_state_pin pin, bool udp, operation_options operation);
    [[nodiscard]] task<void> send_payload(std::string_view payload, bool udp, operation_options operation);
    [[nodiscard]] task<void> send_owned(std::string&& payload, operation_options operation);
    [[nodiscard]] task<void> send_payload_owned(std::string&& payload, bool udp, operation_options operation);
    http_capsule_stream stream_;
};

// RFC 9297 HTTP Datagrams and reliable DATAGRAM capsules for a negotiated RFC 9298 CONNECT-UDP
// tunnel. Unknown capsule types and unknown context IDs are silently dropped;
// malformed DATAGRAM capsules terminate only this tunnel. Empty UDP packets are
// data; an empty optional is EOF. Owns its capsule stream and preserves separate
// read and send/finish lanes when moved.
// Operations occupy their lane when started; unstarted operations may coexist.
// send() owns its payload before returning. `operation` follows http_capsule_stream.
class http_udp_tunnel final {
public:
    http_udp_tunnel(const http_udp_tunnel&) = delete;
    http_udp_tunnel& operator=(const http_udp_tunnel&) = delete;
    http_udp_tunnel(http_udp_tunnel&&) noexcept = default;
    http_udp_tunnel& operator=(http_udp_tunnel&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read(operation_options operation = {}) &;
    task<std::optional<http_datagram>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> send(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload, operation_options operation = {}) &;
    task<void> send(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> send(string_type&& payload, operation_options operation = {}) & {
        return send_owned(std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> send(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) & {
        return stream_.finish(std::move(operation));
    }
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    [[nodiscard]] task<void> send_owned(std::string&& payload, operation_options operation);
    explicit http_udp_tunnel(http_datagram_stream stream) noexcept :
        stream_(std::move(stream)) {}
    http_datagram_stream stream_;
};
struct http_tunnel_route_config final {
    // Aborts the tunnel's transport once no tunnel byte has been read or
    // written for this long, measured by the connection scanner. A write in
    // progress is not idle: the server write timeouts bound it. nullopt
    // disables this guard; a set value must be positive.
    std::optional<timeout_duration> idle_timeout_{std::chrono::minutes(5)};
    // Once the handler returns, bound flushing the local FIN and draining peer
    // input. A handler may half-close and continue reading without this deadline.
    timeout_duration peer_transport_fin_timeout_{5000};
    // Declares HTTP Datagram semantics for an upgrade-protocol
    // tunnel: a 2xx response carries Capsule-Protocol: ?1 (RFC 9297 section
    // 3.4) and the handler may use http_tunnel::datagrams(), udp() and
    // capsules(). HTTP/3 datagrams use native transport when negotiated and
    // within the path limit, otherwise capsules. CONNECT-UDP enables this
    // automatically; plain CONNECT rejects it.
    bool datagrams_{};
};

// An established CONNECT byte stream. The session owns this facade and its
// allocator. The tunnel retains the last received chunk until the next read
// starts; it counts against the connection's and the worker's inbound buffer
// budgets (server_config::max_inbound_buffer_bytes_per_connection_ /
// _per_worker_). A read that would exceed a budget throws std::bad_alloc.
class http_tunnel final {
public:
    http_tunnel(const http_tunnel&) = delete;
    http_tunnel& operator=(const http_tunnel&) = delete;

    // One running read and one running output operation are allowed.
    // An operation occupies its lane when started; unstarted operations may coexist.
    // A timeout or stop in a read's `operation` ends only that read with
    // std::system_error (timed_out or operation_canceled); no input is lost.
    // In a write or finish it aborts the tunnel (a partial write cannot be
    // withdrawn) and throws the same; a stop requested before the operation
    // starts sends nothing. A non-positive timeout throws std::invalid_argument.
    // The view borrows this tunnel until the next read starts; nullopt is EOF
    // and an empty chunk is never returned.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options operation = {}) & {
        return write_owned(std::move(bytes), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;

    // Half-close the send direction; reading remains available until peer EOF.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Borrow the established route tunnel for HTTP Datagrams. The route protocol
    // owns negotiation: these throw std::logic_error unless the route declared
    // http_tunnel_route_config::datagrams_. Use only in the negotiated datagram
    // handler, exclusively with this tunnel. Capsule Protocol payload limits
    // apply to both native datagrams and reliable DATAGRAM capsules.
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
    using read_type = task<std::optional<std::string>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, std::string_view, operation_options);
    using read_datagram_type = task<std::optional<detail::http_tunnel_input>> (*)(void*, operation_options);
    using write_datagram_type = task<bool> (*)(void*, std::string_view, operation_options);
    using finish_type = task<void> (*)(void*, operation_options);
    // malformed: the received tunnel data is malformed (RFC 9297 section 3.3).
    using abort_type = void (*)(void*, bool malformed) noexcept;
    http_tunnel(detail::inbound_buffer_account* inbound, const worker_ref& worker_value, void* target, read_type read, write_type write, finish_type finish_value, abort_type abort, bool datagrams, read_datagram_type read_datagram = nullptr, write_datagram_type write_datagram = nullptr) noexcept;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read_borrowed(operation_options operation);
    [[nodiscard]] task<std::optional<std::string>> read_raw(operation_options operation);
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation);
    [[nodiscard]] task<void> write_raw(std::string&& bytes, operation_options operation);
    [[nodiscard]] task<std::optional<detail::http_tunnel_input>> read_datagram_raw(operation_options operation);
    [[nodiscard]] task<bool> write_datagram_raw(std::string_view bytes, operation_options operation);
    void abort_input(bool malformed) noexcept;
    void require_active() const;
    void require_unadapted() const;
    detail::inbound_buffer_account* inbound_;
    const worker_ref& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    read_datagram_type read_datagram_;
    write_datagram_type write_datagram_;
    // The chunk the last read() returned.
    std::optional<std::string> read_chunk_;
    bool datagrams_;
    bool adapted_{false};
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    ::ruvia::operation_scope operations_;
};

}  // namespace ruvia
