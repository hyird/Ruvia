#include "ruvia/web/HttpUdpTunnel.h"

#include <stdexcept>

#include "http/HttpCapsuleStreamState.h"
namespace ruvia {
ScopedOperation<std::optional<HttpUdpDatagram>> HttpUdpTunnel::read() & {
    auto* state = stream_.stream_.state_;
    if (!state) {
        throw std::logic_error("UDP tunnel is empty");
    }
    state->require(false);
    return ::ruvia::make_scoped_operation(state->readScope, readOwned(detail::CapsuleStatePin(*state)));
}
Task<std::optional<HttpUdpDatagram>> HttpUdpTunnel::readOwned(detail::CapsuleStatePin pin) {
    auto datagram = co_await HttpDatagramStream::readOwned(std::move(pin), true);
    if (!datagram) {
        co_return std::nullopt;
    }
    co_return HttpUdpDatagram(std::move(*datagram));
}
ScopedOperation<void> HttpUdpTunnel::send(std::span<const std::byte> payload) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
}
ScopedOperation<void> HttpUdpTunnel::send(std::string_view payload) & {
    return stream_.sendPayload(payload, true);
}
}  // namespace ruvia
