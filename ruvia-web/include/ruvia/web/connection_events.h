#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include "ruvia/core/move_only_function.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/ip_address.h"
#include "ruvia/web/error.h"

namespace ruvia {

class connection_event final {
public:
    connection_event(std::uint64_t id, ip_endpoint local, ip_endpoint remote, bool quic) noexcept
        : id_(id),
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

}  // namespace ruvia
