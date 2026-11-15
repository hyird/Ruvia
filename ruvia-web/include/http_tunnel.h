#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "borrowed_text.h"
#include "http_capsule.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "outbound_bytes.h"
#include "task.h"
#include "worker_ref.h"
namespace ruvia {
namespace detail {
class http_capsule_stream_state;
class capsule_state_pin;
class inbound_buffer_account;
class http_client_response_state;
class http_client_tunnel_state;
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
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = text();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload_ ? std::string_view(*payload_) : std::string_view{};
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
// using the capsules owns negotiation. The stream borrows its http_tunnel in
// both roles: destroy it before the route handler returns or, on a client,
// before the owning http_client_tunnel is destroyed. A client stream that
// outlives its owner is closed: its operations throw std::logic_error, when
// called or, if created earlier, when started, and abort() does nothing;
// capsules already received stay valid. Use this adapter
// exclusively: do not interleave raw tunnel operations with capsule
// operations. Abort wakes pending I/O; join started operations before
// destroying the stream or its tunnel's owner.
// Operations occupy their read or write/finish lane when started; unstarted
// operations may coexist. One read and one output operation may run concurrently.
// `operation` bounds a whole call: a timeout or stop ends only that read,
// keeping a partly received capsule for the next one, and aborts the stream
// during an output operation. Either throws std::system_error (timed_out or
// operation_canceled) -- over a client tunnel its http_client_error
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
    // Owns input before returning its cold operation (exact std::string rvalues
    // move without copying). Unknown capsule types are delivered to the protocol
    // consumer, which must ignore types it does not understand.
    [[nodiscard]] task<void> write(std::uint64_t type, outbound_bytes payload, operation_options operation = {}) &;
    task<void> write(std::uint64_t, outbound_bytes, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_datagram_stream;
    [[nodiscard]] static task<std::optional<http_capsule>> read_owned(detail::capsule_state_pin pin, operation_options operation);
    [[nodiscard]] task<void> write_frame(std::string_view prefix, outbound_bytes payload, bool datagram, operation_options operation);
    explicit http_capsule_stream(http_tunnel& tunnel, http_capsule_config config);
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
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = text();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        auto bytes_value = capsule_.text();
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
// Its mode is fixed at creation. http_tunnel::datagrams() exchanges raw HTTP
// Datagram payloads, each retaining any protocol-specific Context ID.
// http_tunnel::udp() exchanges the UDP packets of a negotiated RFC 9298
// CONNECT-UDP tunnel: it adds and strips Context ID 0 and silently drops
// unknown Context IDs. Both modes silently drop other capsule types; a
// malformed DATAGRAM capsule terminates only this tunnel. Arbitrary Capsule
// Protocol streams cannot be promoted to this channel.
// Empty payloads are data; an empty optional is EOF. Use exclusively with the tunnel.
// Operations occupy separate read and send/finish lanes when started; unstarted
// operations may coexist. send() owns its payload before returning. `operation`
// and borrowing follow http_capsule_stream.
class http_datagram_stream final {
public:
    http_datagram_stream(const http_datagram_stream&) = delete;
    http_datagram_stream& operator=(const http_datagram_stream&) = delete;
    http_datagram_stream(http_datagram_stream&&) noexcept = default;
    http_datagram_stream& operator=(http_datagram_stream&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read(operation_options operation = {}) &;
    task<std::optional<http_datagram>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(outbound_bytes payload, operation_options operation = {}) &;
    task<void> send(outbound_bytes, operation_options = {}) && = delete;
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
    explicit http_datagram_stream(http_capsule_stream&& stream, bool udp);
    static task<std::optional<http_datagram>> read_owned(detail::capsule_state_pin pin, bool udp, operation_options operation);
    http_capsule_stream stream_;
    // CONNECT-UDP mode: payloads travel under Context ID 0 (RFC 9298 section 5).
    bool udp_;
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

// An established CONNECT byte stream, one type for both roles. A server
// session owns the tunnel of its route (context::tunnel(),
// request_stream::accept_tunnel()); a client reaches the tunnel its
// http_client_tunnel owns through http_client_tunnel::stream(). Worker-affine.
// A server tunnel's received chunk counts against the connection's and the
// worker's inbound buffer budgets (server_config::max_inbound_buffer_bytes_per_connection_ /
// _per_worker_); a read that would exceed a budget throws std::bad_alloc.
// A client tunnel bounds each write by http_client_tunnel_config::max_chunk_bytes_
// (std::length_error) and reports transport failures, interruptions and
// operations on a closed tunnel as http_client_error, a std::system_error.
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
    // The same read lane, with the chunk viewed as characters: no decoding or
    // charset validation, and encoded characters may straddle chunks.
    [[nodiscard]] task<std::optional<std::string_view>> read_text(operation_options operation = {}) &;
    task<std::optional<std::string_view>> read_text(operation_options = {}) && = delete;
    // Owns `bytes` before returning; exact std::string rvalues move without copying.
    [[nodiscard]] task<void> write(outbound_bytes bytes, operation_options operation = {}) &;
    task<void> write(outbound_bytes, operation_options = {}) && = delete;

    // Half-close the send direction; reading remains available until peer EOF.
    // Idempotent after successful completion.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Borrow the established tunnel for the Capsule Protocol or HTTP Datagrams,
    // exclusively: afterwards read() and write() throw std::logic_error. The
    // tunnel's protocol owns negotiation, and without it these throw
    // std::logic_error and leave the tunnel usable. A server route negotiates
    // all three by declaring http_tunnel_route_config::datagrams_. On a client
    // every accepted tunnel may carry capsules(), while datagrams() and udp()
    // require an accepted CONNECT-UDP tunnel. Capsule Protocol payload limits
    // apply to both native datagrams and reliable DATAGRAM capsules.
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &;
    http_capsule_stream capsules(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &;
    http_datagram_stream datagrams(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_datagram_stream udp(http_capsule_config config = {}) &;
    http_datagram_stream udp(http_capsule_config = {}) && = delete;

private:
    template <typename stream_type>
    friend class detail::http_tunnel_session;
    friend class detail::http_client_tunnel_state;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    // The owner of `target` implements each operation, including its
    // validation, lanes and operation scope: the entries validate when called
    // and return cold operations.
    using read_type = task<std::optional<std::span<const std::byte>>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, std::string&&, operation_options);
    using finish_type = task<void> (*)(void*, operation_options);
    // malformed: the received tunnel data is malformed (RFC 9297 section 3.3).
    using abort_type = void (*)(void*, bool malformed) noexcept;
    // Native datagram transport; both null when datagrams travel as capsules.
    using read_datagram_type = task<std::optional<detail::http_tunnel_input>> (*)(void*, operation_options);
    using write_datagram_type = task<bool> (*)(void*, std::string_view, operation_options);
    struct negotiation final {
        bool capsules_{};
        bool datagrams_{};
    };
    http_tunnel(const worker_ref& worker_value, void* target, read_type read, write_type write, finish_type finish_value, abort_type abort, negotiation negotiated) noexcept :
        worker_(worker_value),
        target_(target),
        read_(read),
        write_(write),
        finish_(finish_value),
        abort_(abort),
        negotiated_(negotiated) {}
    http_tunnel(worker_ref&&, void*, read_type, write_type, finish_type, abort_type, negotiation) = delete;
    [[nodiscard]] http_datagram_stream datagram_stream(http_capsule_config config, bool udp);
    void require_unadapted() const;
    const worker_ref& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    read_datagram_type read_datagram_{};
    write_datagram_type write_datagram_{};
    // Server role: the connection account charged for collected capsule payloads.
    detail::inbound_buffer_account* inbound_{};
    // Client role: the response state that capsule results pin.
    detail::http_client_response_state* client_{};
    negotiation negotiated_;
    bool adapted_{false};
};

}  // namespace ruvia
