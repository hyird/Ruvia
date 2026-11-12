#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/web/context.h"
#include "ruvia/web/detail/callback.h"

namespace ruvia {

namespace detail {
struct server_observation_access;
}  // namespace detail

// One ClientHello a server TLS connection received, passed to
// on_tls_client_hello before the server selects its certificate. The record
// and every view are valid only for the callback.
class tls_client_hello_record final {
public:
    // conn_info::id() of every request the connection carries.
    [[nodiscard]] constexpr std::uint64_t connection_id() const noexcept {
        return connection_id_;
    }

    // The server address the connection reached. For HTTP/3 it is the address
    // the listener's UDP socket is bound to.
    [[nodiscard]] constexpr conn_info::endpoint local() const noexcept {
        return local_;
    }

    [[nodiscard]] constexpr conn_info::endpoint remote() const noexcept {
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

    explicit constexpr tls_client_hello_record(const fields_type& fields) noexcept
        : connection_id_(fields.connection_id_),
          local_(fields.local_address_, fields.local_port_),
          remote_(fields.remote_address_, fields.remote_port_),
          message_(fields.message_),
          quic_(fields.quic_) {}

    std::uint64_t connection_id_;
    conn_info::endpoint local_;
    conn_info::endpoint remote_;
    std::span<const std::byte> message_;
    bool quic_;
};

// One read of decrypted HTTP/2 connection input, passed to on_http2_input
// before the server parses it. The record and its view are valid only for the
// callback.
class http2_input_record final {
public:
    // conn_info::id() of every request the connection carries.
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

    constexpr http2_input_record(std::uint64_t connection_id, std::span<const std::byte> bytes) noexcept
        : connection_id_(connection_id),
          bytes_(bytes) {}

    std::uint64_t connection_id_;
    std::span<const std::byte> bytes_;
};

namespace detail {
using tls_client_hello_callback_ref = callback_ref<void(const tls_client_hello_record&) noexcept>;
using http2_input_callback_ref = callback_ref<void(const http2_input_record&) noexcept>;
}  // namespace detail

// application-owned observers, invoked synchronously on the worker thread that
// owns the connection; workers call the same target concurrently, so it must
// be thread-safe. Connections only borrow an allocation-free callback_ref.
// 空函数指针与目标析构重入的契约同 access_log_callback。
using tls_client_hello_callback = detail::callback<void(const tls_client_hello_record&) noexcept>;
using http2_input_callback = detail::callback<void(const http2_input_record&) noexcept>;

}  // namespace ruvia
