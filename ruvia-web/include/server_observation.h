#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "callback_ref.h"
#include "http_message.h"
#include "http_status.h"
#include "ip_address.h"
#include "move_only_function.h"

#if defined(RUVIA_ENABLE_HTTP2)
#include "http2_connection.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_connection.h"
#endif

namespace ruvia {

namespace detail {
struct access_log_record_access;
struct connection_failure_record_access;
}  // namespace detail

// One terminal response outcome with a committed final status, passed to the
// access-log callback after a complete buffered response head has reached the
// transport, a stream head is committed, or a CONNECT/WebSocket handshake head
// has reached the transport (before the long-lived handler runs). The record
// and all returned views are valid only for the callback.
class access_log_record final {
public:
    [[nodiscard]] constexpr std::string_view method() const noexcept {
        return method_;
    }

    [[nodiscard]] constexpr http_known_method known_method() const noexcept {
        return known_method_;
    }

    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] constexpr ip_address client_address() const noexcept {
        return client_address_;
    }

    [[nodiscard]] constexpr ip_address remote_address() const noexcept {
        return remote_address_;
    }

    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] constexpr std::uint64_t duration_micros() const noexcept {
        return duration_micros_;
    }

    [[nodiscard]] constexpr std::uint64_t db_count() const noexcept {
        return db_count_;
    }

    [[nodiscard]] constexpr std::uint64_t db_duration_micros() const noexcept {
        return db_duration_micros_;
    }

    [[nodiscard]] constexpr std::uint64_t redis_count() const noexcept {
        return redis_count_;
    }

    [[nodiscard]] constexpr std::uint64_t redis_duration_micros() const noexcept {
        return redis_duration_micros_;
    }
    [[nodiscard]] constexpr http_protocol_version protocol_version() const noexcept {
        return protocol_version_;
    }

private:
    friend struct detail::access_log_record_access;

    struct fields_type final {
        std::string_view method_;
        http_known_method known_method_;
        std::string_view path_;
        http_protocol_version protocol_version_;
        ip_address remote_address_{};
        ip_address client_address_{};
        http_status_code status_;
        std::uint64_t duration_micros_;
        std::uint64_t db_count_{0};
        std::uint64_t db_duration_micros_{0};
        std::uint64_t redis_count_{0};
        std::uint64_t redis_duration_micros_{0};
    };

    explicit constexpr access_log_record(const fields_type& fields) noexcept :
        method_(fields.method_),
        path_(fields.path_),
        remote_address_(fields.remote_address_),
        client_address_(fields.client_address_),
        duration_micros_(fields.duration_micros_),
        db_count_(fields.db_count_),
        db_duration_micros_(fields.db_duration_micros_),
        redis_count_(fields.redis_count_),
        redis_duration_micros_(fields.redis_duration_micros_),
        status_(fields.status_),
        known_method_(fields.known_method_),
        protocol_version_(fields.protocol_version_) {}

    std::string_view method_;
    std::string_view path_;
    ip_address remote_address_{};
    ip_address client_address_{};
    std::uint64_t duration_micros_;
    std::uint64_t db_count_{0};
    std::uint64_t db_duration_micros_{0};
    std::uint64_t redis_count_{0};
    std::uint64_t redis_duration_micros_{0};
    http_status_code status_;
    http_known_method known_method_;
    http_protocol_version protocol_version_;
};

namespace detail {
using access_log_callback_ref = callback_ref<void(const access_log_record&) noexcept>;
}  // namespace detail

// application-owned access-log listener. Request dispatch receives only an internal,
// allocation-free callback_ref and never participates in this owner's lifetime.
// 空函数指针构造空回调；未配置时不得调用。所有权与退役语义同 move_only_function。
using access_log_callback = move_only_function<void(const access_log_record&) noexcept>;

// One connection lost to an exception that escaped its session: a handler bug
// past the response's point of no return, an error handler that itself failed,
// or resource exhaustion. The server closes that connection and keeps serving;
// this record is how the failure becomes visible instead of vanishing with the
// connection. A request that fails before its response is committed is
// answered with a 5xx through on_error and never gets here, with one
// exception: a failed write of an interim head or 100 Continue on the
// connection's own transport ends the connection and is reported here unless
// it is a peer reset/close or server-cancelled I/O.
//
// Views and the record itself are valid only for the callback invocation.
class connection_failure_record final {
public:
    // Empty when the peer address could not be read (the connection was
    // already gone) or when the failure happened before it was resolved.
    [[nodiscard]] constexpr ip_address remote_address() const noexcept {
        return remote_address_;
    }

    // Never null. Rethrow it to inspect the failure.
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

private:
    friend struct detail::connection_failure_record_access;

    connection_failure_record(ip_address remote_address, std::exception_ptr exception) noexcept :
        remote_address_(remote_address),
        exception_(std::move(exception)) {}

    ip_address remote_address_{};
    std::exception_ptr exception_;
};

namespace detail {
using connection_failure_callback_ref = callback_ref<void(const connection_failure_record&) noexcept>;
}  // namespace detail

// application-owned connection-failure listener. The listener must not throw: it runs on the last
// line of defense for a connection, where a second failure would have nowhere
// left to go, so the requirement is enforced at compile time rather than
// swallowed at runtime.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using connection_failure_callback = move_only_function<void(const connection_failure_record&) noexcept>;

class connection_event final {
public:
    connection_event(std::uint64_t id, ip_endpoint local, ip_endpoint remote, bool quic) noexcept :
        id_(id),
        local_(std::move(local)),
        remote_(std::move(remote)),
        quic_(quic) {}
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return id_;
    }
    [[nodiscard]] const ip_endpoint& local() const& noexcept RUVIA_LIFETIMEBOUND {
        return local_;
    }
    const ip_endpoint& local() const&& = delete;
    [[nodiscard]] const ip_endpoint& remote() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_;
    }
    const ip_endpoint& remote() const&& = delete;
    [[nodiscard]] bool quic() const noexcept {
        return quic_;
    }

private:
    std::uint64_t id_;
    ip_endpoint local_;
    ip_endpoint remote_;
    bool quic_;
};

enum class connection_close_reason : std::uint8_t {
    normal,
    peer_eof,
    timeout,
    cancelled,
    aborted,
    protocol_error,
    transport_error,
    handler_error,
};

struct connection_close_info final {
    connection_close_reason reason_{connection_close_reason::normal};
    // Transport bytes actually received/sent, including TLS ciphertext and
    // QUIC retransmissions; replayed prefixes never count twice.
    std::uint64_t bytes_in_{};
    std::uint64_t bytes_out_{};
    // New HTTP request heads, once per request; raw connections remain zero.
    std::uint64_t requests_{};
};

namespace detail {
using connection_admission_callback_ref = callback_ref<bool(const connection_event&) noexcept>;
using connection_open_callback_ref = callback_ref<std::shared_ptr<void>(const connection_event&)>;
using connection_close_callback_ref = callback_ref<void(const connection_event&, const std::shared_ptr<void>&, const connection_close_info&) noexcept>;
}  // namespace detail

// Synchronous worker callbacks; targets must support concurrent workers. False
// admission silently rejects before creating TLS or protocol session resources.
// Rejected connections have no open/close events. Every admitted connection has
// one open and one close, including failed handshakes and cancelled cold tasks.
// The optional shared state returned by open is retained through close. Contexts
// borrow its connection slot; connection_state() copies its shared ownership.
// The close argument borrows that shared_ptr only for the callback; copy it to
// retain the object beyond connection retirement.
// Close runs after ID retirement, outside locks, before the server releases its
// state reference. Admission and close must not throw. If open throws, close
// still runs once with empty state, and the failure reaches the failure sink.
using connection_admission_callback = move_only_function<bool(const connection_event&) noexcept>;
using connection_open_callback = move_only_function<std::shared_ptr<void>(const connection_event&)>;
using connection_close_callback = move_only_function<void(const connection_event&, const std::shared_ptr<void>&, const connection_close_info&) noexcept>;

namespace detail {
struct server_observation_access;
}  // namespace detail

// One ClientHello a server TLS connection received, passed to
// on_tls_client_hello before the server selects its certificate. The record
// and every view are valid only for the callback.
class tls_client_hello_record final {
public:
    // conn_info::connection_id() of every request the connection carries.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }

    // The server address the connection reached. For HTTP/3 it is the address
    // the listener's UDP socket is bound to.
    [[nodiscard]] constexpr ip_endpoint local() const noexcept {
        return local_;
    }

    [[nodiscard]] constexpr ip_endpoint remote() const noexcept {
        return remote_;
    }

    // True for the TLS of an HTTP/3 (QUIC) connection, false for TLS over TCP.
    [[nodiscard]] constexpr bool quic() const noexcept {
        return quic_;
    }

    // The complete handshake message (RFC 9846 §4.1.2, RFC 5246 §7.4.1.2):
    // msg_type 1, the 3-byte length and the body, reassembled from however
    // many records or CRYPTO frames carried it. The second ClientHello after a
    // HelloRetryRequest is delivered again, as its own record.
    [[nodiscard]] constexpr std::span<const std::byte> message() const noexcept {
        return message_;
    }

private:
    friend struct detail::server_observation_access;

    struct fields_type final {
        std::uint64_t connection_id_;
        std::string_view local_address_;
        std::uint16_t local_port_;
        std::string_view remote_address_;
        std::uint16_t remote_port_;
        bool quic_;
        std::span<const std::byte> message_;
    };

    explicit tls_client_hello_record(const fields_type& fields) noexcept :
        connection_id_(fields.connection_id_),
        local_(ip_address::parse(fields.local_address_).value_or(ip_address{}), fields.local_port_),
        remote_(ip_address::parse(fields.remote_address_).value_or(ip_address{}), fields.remote_port_),
        message_(fields.message_),
        quic_(fields.quic_) {}

    std::uint64_t connection_id_;
    ip_endpoint local_;
    ip_endpoint remote_;
    std::span<const std::byte> message_;
    bool quic_;
};

// One read of decrypted HTTP/2 connection input, passed to on_http2_input
// before the server parses it. The record and its view are valid only for the
// callback.
class http2_input_record final {
public:
    // conn_info::connection_id() of every request the connection carries.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }

    // The connection's inbound bytes in wire order: the records of one
    // connection concatenate to its input from the client connection preface
    // (RFC 9113 §3.4) on. Boundaries are read boundaries, not frame boundaries.
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::server_observation_access;

    constexpr http2_input_record(std::uint64_t connection_id, std::span<const std::byte> bytes) noexcept :
        connection_id_(connection_id),
        bytes_(bytes) {}

    std::uint64_t connection_id_;
    std::span<const std::byte> bytes_;
};

#if defined(RUVIA_ENABLE_HTTP2)
// Typed facts produced by the one protocol parser/HPACK decoder, not TCP read
// boundaries. Records and decoded field views live only for the callback.
// observation_sequence_ counts parser-exposed frame headers, not transport reads.
// Frames are observed before processing their final payload fragment; headers
// are observed at decoder begin/field/end points before HTTP validation.
// Observers cannot change input, cancel validation, or imply frame acceptance.
class http2_frame_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http2_frame_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http2_frame_record(std::uint64_t connection_id, const http2_frame_metadata& metadata) noexcept :
        connection_id_(connection_id),
        metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http2_frame_metadata& metadata_;
};

class http2_headers_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http2_header_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http2_headers_record(std::uint64_t connection_id, const http2_header_metadata& metadata) noexcept :
        connection_id_(connection_id),
        metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http2_header_metadata& metadata_;
};
#endif
#if defined(RUVIA_ENABLE_HTTP3)
class http3_frame_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http3_frame_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http3_frame_record(std::uint64_t connection_id, const http3_frame_metadata& metadata) noexcept :
        connection_id_(connection_id),
        metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http3_frame_metadata& metadata_;
};

class http3_field_record final {
public:
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }
    [[nodiscard]] const http3_field_metadata& metadata() const noexcept {
        return metadata_;
    }

private:
    friend struct detail::server_observation_access;
    constexpr http3_field_record(std::uint64_t connection_id, const http3_field_metadata& metadata) noexcept :
        connection_id_(connection_id),
        metadata_(metadata) {}
    std::uint64_t connection_id_;
    const http3_field_metadata& metadata_;
};
#endif

namespace detail {
using tls_client_hello_callback_ref = callback_ref<void(const tls_client_hello_record&) noexcept>;
using http2_input_callback_ref = callback_ref<void(const http2_input_record&) noexcept>;
#if defined(RUVIA_ENABLE_HTTP2)
using http2_frame_callback_ref = callback_ref<void(const http2_frame_record&) noexcept>;
using http2_headers_callback_ref = callback_ref<void(const http2_headers_record&) noexcept>;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
using http3_frame_callback_ref = callback_ref<void(const http3_frame_record&) noexcept>;
using http3_field_callback_ref = callback_ref<void(const http3_field_record&) noexcept>;
#endif
}  // namespace detail

// application-owned observers, invoked synchronously on the worker thread that
// owns the connection; workers call the same target concurrently, so it must
// be thread-safe. Connections only borrow an allocation-free callback_ref.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using tls_client_hello_callback = move_only_function<void(const tls_client_hello_record&) noexcept>;
using http2_input_callback = move_only_function<void(const http2_input_record&) noexcept>;
#if defined(RUVIA_ENABLE_HTTP2)
using http2_frame_callback = move_only_function<void(const http2_frame_record&) noexcept>;
using http2_headers_callback = move_only_function<void(const http2_headers_record&) noexcept>;
#endif
#if defined(RUVIA_ENABLE_HTTP3)
using http3_frame_callback = move_only_function<void(const http3_frame_record&) noexcept>;
using http3_field_callback = move_only_function<void(const http3_field_record&) noexcept>;
#endif

}  // namespace ruvia
