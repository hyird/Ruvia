#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/http/HttpDatagram.h"

namespace ruvia {
namespace detail {
class HttpCapsuleStreamState;
class CapsuleStatePin;
}  // namespace detail
class HttpTunnel;
class HttpClientTunnel;
class HttpUdpTunnel;
class HttpDatagramStream;

// Owns one capsule payload. Its storage remains valid across subsequent reads
// and stream destruction. Like other worker results, destroy it on its owner
// worker before that worker retires.
class HttpCapsule final {
public:
    HttpCapsule(const HttpCapsule&) = delete;
    HttpCapsule& operator=(const HttpCapsule&) = delete;
    HttpCapsule(HttpCapsule&& other) noexcept;
    HttpCapsule& operator=(HttpCapsule&& other) noexcept;
    ~HttpCapsule();
    [[nodiscard]] std::uint64_t type() const noexcept {
        return type_;
    }
    [[nodiscard]] std::string_view payload() const& noexcept {
        return payload_ ? std::string_view(*payload_) : std::string_view{};
    }
    std::string_view payload() const&& = delete;

private:
    friend class HttpCapsuleStream;
    friend class HttpUdpTunnel;
    friend class HttpDatagramStream;
    HttpCapsule(detail::HttpCapsuleStreamState& state, std::uint64_t type, std::pmr::string payload) noexcept;
    void release() noexcept;
    detail::HttpCapsuleStreamState* state_{};
    std::uint64_t type_{};
    std::optional<std::pmr::string> payload_;
};

// RFC 9297 reliable Capsule Protocol over an established tunnel. The protocol
// using the capsules owns negotiation. Server streams borrow their route
// tunnel; client streams take tunnel ownership. Use this adapter exclusively:
// do not interleave raw tunnel operations with capsule operations. Abort wakes
// pending I/O; join started operations before destroying their scope owner.
class HttpCapsuleStream final {
public:
    HttpCapsuleStream(const HttpCapsuleStream&) = delete;
    HttpCapsuleStream& operator=(const HttpCapsuleStream&) = delete;
    HttpCapsuleStream(HttpCapsuleStream&& other) noexcept;
    HttpCapsuleStream& operator=(HttpCapsuleStream&& other) noexcept;
    ~HttpCapsuleStream();
    [[nodiscard]] ScopedOperation<std::optional<HttpCapsule>> read() &;
    ScopedOperation<std::optional<HttpCapsule>> read() && = delete;
    // Owns input before returning its cold operation. One write/finish and one
    // read may coexist. Unknown capsule types are delivered to the protocol
    // consumer, which must ignore types it does not understand.
    [[nodiscard]] ScopedOperation<void> write(std::uint64_t type, std::string_view payload) &;
    ScopedOperation<void> write(std::uint64_t, std::string_view) && = delete;
    [[nodiscard]] ScopedOperation<void> write(std::uint64_t type, std::span<const std::byte> payload) &;
    ScopedOperation<void> write(std::uint64_t, std::span<const std::byte>) && = delete;
    [[nodiscard]] ScopedOperation<void> finish() &;
    ScopedOperation<void> finish() && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class HttpTunnel;
    friend class HttpClientTunnel;
    friend class HttpUdpTunnel;
    friend class HttpDatagramStream;
    [[nodiscard]] static Task<std::optional<HttpCapsule>> readOwned(detail::CapsuleStatePin pin);
    [[nodiscard]] ScopedOperation<void> writeFrame(std::string_view prefix, std::string_view payload);
    explicit HttpCapsuleStream(HttpTunnel& tunnel, HttpCapsuleConfig config);
    explicit HttpCapsuleStream(HttpClientTunnel&& tunnel, HttpCapsuleConfig config);
    void release() noexcept;
    detail::HttpCapsuleStreamState* state_{};
};
}  // namespace ruvia
