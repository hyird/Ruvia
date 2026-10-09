#include "ruvia/web/http_udp_tunnel.h"

#include <stdexcept>

#include "http/http_capsule_stream_state.h"
namespace ruvia {
scoped_operation<std::optional<http_udp_datagram>> http_udp_tunnel::read() & {
    auto* state_value = stream_.stream_.state_;
    if (!state_value) {
        throw std::logic_error("UDP tunnel is empty");
    }
    state_value->require(false);
    return ::ruvia::make_scoped_operation(state_value->read_scope_, read_owned(detail::capsule_state_pin(*state_value)));
}
task<std::optional<http_udp_datagram>> http_udp_tunnel::read_owned(detail::capsule_state_pin pin) {
    auto datagram = co_await http_datagram_stream::read_owned(std::move(pin), true);
    if (!datagram) {
        co_return std::nullopt;
    }
    co_return http_udp_datagram(std::move(*datagram));
}
scoped_operation<void> http_udp_tunnel::send(std::span<const std::byte> payload_value) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()));
}
scoped_operation<void> http_udp_tunnel::send(std::string_view payload_value) & {
    return stream_.send_payload(payload_value, true);
}
}  // namespace ruvia
