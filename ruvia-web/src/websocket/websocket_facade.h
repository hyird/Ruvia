#pragma once

#include "websocket/websocket_state.h"

namespace ruvia::detail {

template <typename connection_type>
[[nodiscard]] task<std::optional<websocket_message>> websocket_read_thunk(void* target, operation_options options) {
    return static_cast<connection_type*>(target)->read(std::move(options));
}

template <typename connection_type>
task<void> websocket_write_thunk(void* target, websocket_opcode opcode, std::string_view payload_value, bool compress,
    websocket_write_extent extent, operation_options options) {
    return static_cast<connection_type*>(target)->write(opcode, payload_value, compress, extent, std::move(options));
}

template <typename connection_type>
task<void> websocket_close_thunk(void* target, ::ruvia::websocket_close_options options, operation_options operation) {
    return static_cast<connection_type*>(target)->close(options, std::move(operation));
}

template <typename connection_type>
void websocket_abort_thunk(void* target) noexcept {
    static_cast<connection_type*>(target)->abort();
}

template <typename connection_type>
[[nodiscard]] std::optional<std::uint16_t> websocket_peer_close_code_thunk(void* target) noexcept {
    return static_cast<connection_type*>(target)->peer_close_code();
}

template <typename connection_type>
[[nodiscard]] std::string_view websocket_peer_close_reason_thunk(void* target) noexcept {
    return static_cast<connection_type*>(target)->peer_close_reason();
}

template <typename connection_type>
[[nodiscard]] const websocket_connection& websocket_protocol_thunk(void* target) noexcept {
    return static_cast<connection_type*>(target)->protocol();
}

template <typename connection_type>
[[nodiscard]] websocket make_websocket_facade(
    connection_type& connection, std::pmr::memory_resource& resource) {
    auto state = std::allocate_shared<websocket_state>(std::pmr::polymorphic_allocator<websocket_state>(&resource), resource, connection.worker(), &connection, connection.subprotocol(),
        &websocket_read_thunk<connection_type>, &websocket_write_thunk<connection_type>,
        &websocket_close_thunk<connection_type>, &websocket_abort_thunk<connection_type>,
        &websocket_peer_close_code_thunk<connection_type>,
        &websocket_peer_close_reason_thunk<connection_type>,
        &websocket_protocol_thunk<connection_type>);
    connection.bind_facade(state);
    return websocket(std::move(state));
}

}  // namespace ruvia::detail
