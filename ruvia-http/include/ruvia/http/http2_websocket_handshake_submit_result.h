#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/websocket_protocol.h"

namespace ruvia {

class http2_connection;
namespace detail {
class http2_connection;
}  // namespace detail

enum class http2_websocket_handshake_submit_error : std::uint8_t { closed,
    invalid_state };

// The negotiation committed on the wire by the RFC 8441 200 response. The
// runtime configures its websocket connection from this value, never from a
// separately recomputed compression/subprotocol tuple.
class http2_websocket_negotiation final {
public:
    [[nodiscard]] std::string_view subprotocol() const& noexcept {
        return subprotocol_;
    }
    std::string_view subprotocol() const&& = delete;
    [[nodiscard]] websocket_compression compression() const noexcept {
        return compression_;
    }

private:
    friend class detail::http2_connection;
    http2_websocket_negotiation(std::pmr::string subprotocol, websocket_compression compression) noexcept
        : subprotocol_(std::move(subprotocol)),
          compression_(compression) {}
    std::pmr::string subprotocol_;
    websocket_compression compression_;
};

class http2_websocket_handshake_submit_failure final {
public:
    [[nodiscard]] constexpr http2_websocket_handshake_submit_error error() const noexcept {
        return error_;
    }

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    explicit constexpr http2_websocket_handshake_submit_failure(
        http2_websocket_handshake_submit_error error) noexcept
        : error_(error) {}
    http2_websocket_handshake_submit_error error_;
};

class http2_websocket_handshake_submit_result final {
public:
    [[nodiscard]] const http2_websocket_negotiation* submitted() const& noexcept {
        return (value_.index() == 0) ? &std::get<0>(value_) : nullptr;
    }
    const http2_websocket_negotiation* submitted() const&& = delete;
    [[nodiscard]] const http2_websocket_handshake_submit_failure* failure() const& noexcept {
        return (value_.index() == 0) ? nullptr : &std::get<1>(value_);
    }
    const http2_websocket_handshake_submit_failure* failure() const&& = delete;

private:
    friend class http2_connection;
    friend class detail::http2_connection;
    using value_type = std::variant<http2_websocket_negotiation, http2_websocket_handshake_submit_failure>;
    explicit http2_websocket_handshake_submit_result(http2_websocket_negotiation value) noexcept
        : value_(std::move(value)) {}
    explicit http2_websocket_handshake_submit_result(http2_websocket_handshake_submit_failure failure) noexcept
        : value_(failure) {}
    value_type value_;
};

}  // namespace ruvia
