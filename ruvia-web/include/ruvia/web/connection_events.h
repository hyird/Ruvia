#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <asio/ip/address.hpp>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/detail/callback.h"

namespace ruvia {

// Transport addresses, never values supplied by forwarding headers. Values own
// their address bytes, so copying an event does not retain connection storage.
class connection_endpoint final {
public:
    connection_endpoint() noexcept = default;
    connection_endpoint(asio::ip::address address, std::uint16_t port) noexcept
        : address_(std::move(address)),
          port_(port) {}
    [[nodiscard]] const asio::ip::address& address() const& noexcept RUVIA_LIFETIMEBOUND {
        return address_;
    }
    const asio::ip::address& address() const&& = delete;
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

private:
    asio::ip::address address_;
    std::uint16_t port_{};
};

class connection_event final {
public:
    connection_event(std::uint64_t id, connection_endpoint local, connection_endpoint remote, bool quic) noexcept
        : id_(id),
          local_(std::move(local)),
          remote_(std::move(remote)),
          quic_(quic) {}
    [[nodiscard]] std::uint64_t connection_id() const noexcept {
        return id_;
    }
    [[nodiscard]] const connection_endpoint& local() const& noexcept RUVIA_LIFETIMEBOUND {
        return local_;
    }
    const connection_endpoint& local() const&& = delete;
    [[nodiscard]] const connection_endpoint& remote() const& noexcept RUVIA_LIFETIMEBOUND {
        return remote_;
    }
    const connection_endpoint& remote() const&& = delete;
    [[nodiscard]] bool quic() const noexcept {
        return quic_;
    }

private:
    std::uint64_t id_;
    connection_endpoint local_;
    connection_endpoint remote_;
    bool quic_;
};

namespace detail {
using connection_admission_callback_ref = callback_ref<bool(const connection_event&) noexcept>;
using connection_open_callback_ref = callback_ref<std::shared_ptr<void>(const connection_event&)>;
using connection_close_callback_ref = callback_ref<void(const connection_event&, const std::shared_ptr<void>&) noexcept>;
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
using connection_admission_callback = detail::callback<bool(const connection_event&) noexcept>;
using connection_open_callback = detail::callback<std::shared_ptr<void>(const connection_event&)>;
using connection_close_callback = detail::callback<void(const connection_event&, const std::shared_ptr<void>&) noexcept>;

}  // namespace ruvia
