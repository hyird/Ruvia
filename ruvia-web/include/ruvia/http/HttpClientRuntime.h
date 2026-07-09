#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpBodyStream.h"
#include "ruvia/http/HttpClientTypes.h"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ruvia::detail {
struct FetchResponseStreamAccess;
}  // namespace ruvia::detail

namespace ruvia {

class RequestBodyStream {
public:
    using NextChunk = Task<std::string_view> (*)(void*);

    constexpr RequestBodyStream() noexcept = default;
    constexpr RequestBodyStream(void* target, NextChunk nextChunk) noexcept
        : target_(target),
          nextChunk_(nextChunk) {}

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return nextChunk_ != nullptr;
    }

    [[nodiscard]] Task<std::string_view> nextChunk() const;

private:
    void* target_{nullptr};
    NextChunk nextChunk_{nullptr};
};

struct FetchOptions : HttpFetchOptions {
    RequestBodyStream bodyStream{};
};

// Web reverse-proxy forwarding policy: the incoming request is forwarded to the
// upstream and the upstream's response is streamed straight back by the runtime
// driver.
struct ProxyOptions {
    // Forward incoming request headers to the upstream (default true). Hop-by-hop
    // and client-managed headers are always dropped; the client driver sets them.
    bool forwardRequestHeaders{true};
    // Maximum 3xx redirects to follow on the upstream. 0 (default) passes a 3xx
    // straight back to the downstream client rather than following it.
    std::uint32_t maxRedirects{0};
    // Overrides the client's proxy_read_timeout / proxy_send_timeout for this
    // request. 0 = use the client config's values.
    std::chrono::milliseconds timeout{0};
};

class FetchResponseStream final {
public:
    FetchResponseStream(const FetchResponseStream&) = delete;
    FetchResponseStream& operator=(const FetchResponseStream&) = delete;
    FetchResponseStream(FetchResponseStream&&) noexcept = default;
    FetchResponseStream& operator=(FetchResponseStream&&) noexcept = default;
    ~FetchResponseStream() = default;

    [[nodiscard]] std::uint16_t status() const noexcept { return status_; }

    [[nodiscard]] std::span<const FetchResponseHeader> headers() const noexcept {
        return std::span<const FetchResponseHeader>(headers_.data(), headers_.size());
    }

    [[nodiscard]] Task<std::string_view> readChunk();

    void close() noexcept { body_ = HttpBodyStream{}; }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(body_);
    }

    [[nodiscard]] HttpBodyStream takeBody() noexcept {
        return std::move(body_);
    }

private:
    friend struct detail::FetchResponseStreamAccess;

    FetchResponseStream() noexcept = default;

    FetchResponseStream(
        std::uint16_t status,
        std::pmr::vector<FetchResponseHeader> headers,
        HttpBodyStream body) noexcept
        : status_(status), headers_(std::move(headers)), body_(std::move(body)) {}

    std::uint16_t status_{0};
    std::pmr::vector<FetchResponseHeader> headers_;
    HttpBodyStream body_;
};

}  // namespace ruvia
