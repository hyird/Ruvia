#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/web/HttpCapsuleStream.h"
#include "ruvia/web/HttpClientResponse.h"
#include "ruvia/web/HttpDatagramStream.h"

namespace ruvia {
class HttpUdpTunnel;
namespace detail {
class HttpClientPool;
class HttpCapsuleStreamState;
struct HttpClientTunnelWriteInput;
}  // namespace detail

// Owns an established CONNECT byte stream on its client worker. Metadata borrows
// this owner; a read view lasts until the next read. Inputs are copied before a
// cold write is returned. One read and one write/finish may coexist. Moving the
// owner preserves operations. Destruction aborts both directions; shutdown joins
// transport drivers. finish() closes only the local sending direction.
class HttpClientTunnel final {
public:
    HttpClientTunnel(const HttpClientTunnel&) = delete;
    HttpClientTunnel& operator=(const HttpClientTunnel&) = delete;
    HttpClientTunnel(HttpClientTunnel&& other) noexcept;
    HttpClientTunnel& operator=(HttpClientTunnel&& other) noexcept;
    ~HttpClientTunnel();
    [[nodiscard]] HttpStatusCode status() const noexcept {
        return response_.status();
    }
    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept {
        return response_.protocolVersion();
    }
    [[nodiscard]] std::span<const HttpHeader> headers() const& noexcept {
        return response_.headers();
    }
    std::span<const HttpHeader> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept {
        return response_.header(name);
    }
    std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::span<const HttpClientInformationalResponse> informationalResponses() const& noexcept {
        return response_.informationalResponses();
    }
    std::span<const HttpClientInformationalResponse> informationalResponses() const&& = delete;
    [[nodiscard]] ScopedOperation<std::optional<std::span<const std::byte>>> read() &;
    ScopedOperation<std::optional<std::span<const std::byte>>> read() && = delete;
    [[nodiscard]] ScopedOperation<void> write(std::string_view bytes) &;
    ScopedOperation<void> write(std::string_view) && = delete;
    [[nodiscard]] ScopedOperation<void> write(std::span<const std::byte> bytes) &;
    ScopedOperation<void> write(std::span<const std::byte>) && = delete;
    [[nodiscard]] ScopedOperation<void> finish() &;
    ScopedOperation<void> finish() && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Transfers tunnel ownership to the Capsule Protocol adapter.
    [[nodiscard]] HttpCapsuleStream capsules(HttpCapsuleConfig config = {}) &&;
    HttpCapsuleStream capsules(HttpCapsuleConfig = {}) & = delete;
    // Consumes an accepted CONNECT-UDP tunnel after validated negotiation.
    [[nodiscard]] HttpDatagramStream datagrams(HttpDatagramConfig config = {}) &&;
    HttpDatagramStream datagrams(HttpDatagramConfig = {}) & = delete;
    [[nodiscard]] HttpUdpTunnel udp(HttpDatagramConfig config = {}) &&;
    HttpUdpTunnel udp(HttpDatagramConfig = {}) & = delete;
    void reprioritize(HttpPriority priority) & {
        response_.reprioritize(priority);
    }
    void reprioritize(HttpPriority) && = delete;

private:
    friend class detail::HttpClientPool;
    friend class detail::HttpCapsuleStreamState;
    friend class HttpCapsuleStream;
    friend class HttpDatagramStream;
    explicit HttpClientTunnel(HttpClientResponse response) noexcept;
    void release() noexcept;
    static Task<void> writeOwned(detail::HttpClientTunnelWriteInput input);
    static Task<void> finishOwned(HttpClientResponse pin);
    HttpClientResponse response_;
};

// A rejected CONNECT keeps its ordinary HTTP status, headers and response body.
// Only a successful handshake exposes a tunnel.
class HttpClientTunnelResult final {
public:
    HttpClientTunnelResult(const HttpClientTunnelResult&) = delete;
    HttpClientTunnelResult& operator=(const HttpClientTunnelResult&) = delete;
    HttpClientTunnelResult(HttpClientTunnelResult&&) noexcept = default;
    HttpClientTunnelResult& operator=(HttpClientTunnelResult&&) noexcept = default;
    [[nodiscard]] HttpClientTunnel* tunnel() & noexcept {
        return std::get_if<HttpClientTunnel>(&value_);
    }
    HttpClientTunnel* tunnel() && = delete;
    [[nodiscard]] HttpClientResponse* response() & noexcept {
        return std::get_if<HttpClientResponse>(&value_);
    }
    HttpClientResponse* response() && = delete;

private:
    friend class detail::HttpClientPool;
    friend class detail::HttpCapsuleStreamState;
    friend class HttpCapsuleStream;
    friend class HttpDatagramStream;
    explicit HttpClientTunnelResult(HttpClientTunnel tunnel) noexcept
        : value_(std::move(tunnel)) {}
    explicit HttpClientTunnelResult(HttpClientResponse response) noexcept
        : value_(std::move(response)) {}
    std::variant<HttpClientTunnel, HttpClientResponse> value_;
};
}  // namespace ruvia
