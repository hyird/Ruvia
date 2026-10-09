#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/http/http_datagram.h"

namespace ruvia {
namespace detail {
class http_capsule_stream_state;
class capsule_state_pin;
}  // namespace detail
class http_tunnel;
class http_client_tunnel;
class http_udp_tunnel;
class http_datagram_stream;

// Owns one capsule payload. Its storage remains valid across subsequent reads
// and stream destruction. Like other worker results, destroy it on its owner
// worker before that worker retires.
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
    [[nodiscard]] std::string_view payload() const& noexcept {
        return payload_ ? std::string_view(*payload_) : std::string_view{};
    }
    std::string_view payload() const&& = delete;

private:
    friend class http_capsule_stream;
    friend class http_udp_tunnel;
    friend class http_datagram_stream;
    http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::pmr::string payload_value) noexcept;
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
    std::uint64_t type_{};
    std::optional<std::pmr::string> payload_;
};

// RFC 9297 reliable Capsule Protocol over an established tunnel. The protocol
// using the capsules owns negotiation. Server streams borrow their route
// tunnel; client streams take tunnel ownership. Use this adapter exclusively:
// do not interleave raw tunnel operations with capsule operations. Abort wakes
// pending I/O; join started operations before destroying their scope owner.
class http_capsule_stream final {
public:
    http_capsule_stream(const http_capsule_stream&) = delete;
    http_capsule_stream& operator=(const http_capsule_stream&) = delete;
    http_capsule_stream(http_capsule_stream&& other) noexcept;
    http_capsule_stream& operator=(http_capsule_stream&& other) noexcept;
    ~http_capsule_stream();
    [[nodiscard]] scoped_operation<std::optional<http_capsule>> read() &;
    scoped_operation<std::optional<http_capsule>> read() && = delete;
    // Owns input before returning its cold operation. One write/finish and one
    // read may coexist. Unknown capsule types are delivered to the protocol
    // consumer, which must ignore types it does not understand.
    [[nodiscard]] scoped_operation<void> write(std::uint64_t type, std::string_view payload) &;
    scoped_operation<void> write(std::uint64_t, std::string_view) && = delete;
    [[nodiscard]] scoped_operation<void> write(std::uint64_t type, std::span<const std::byte> payload) &;
    scoped_operation<void> write(std::uint64_t, std::span<const std::byte>) && = delete;
    [[nodiscard]] scoped_operation<void> finish() &;
    scoped_operation<void> finish() && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    friend class http_datagram_stream;
    [[nodiscard]] static task<std::optional<http_capsule>> read_owned(detail::capsule_state_pin pin);
    [[nodiscard]] scoped_operation<void> write_frame(std::string_view prefix, std::string_view payload);
    explicit http_capsule_stream(http_tunnel& tunnel, http_capsule_config config);
    explicit http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config);
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
};
}  // namespace ruvia
