#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia {

namespace detail {
class HttpClientPool;
class HttpClientResponseState;
}  // namespace detail

class ResponseStreamWriter;
class WorkerHandle;

// A borrow-only facade embedded in HttpClientResponse. It cannot be detached,
// independently destroyed, or moved away from the response. Body operations
// bind directly to the response's address-stable transport state, so moving
// their public response owner does not invalidate a cold or running operation.
class HttpClientResponseBody final {
public:
    HttpClientResponseBody(const HttpClientResponseBody&) = delete;
    HttpClientResponseBody& operator=(const HttpClientResponseBody&) = delete;
    HttpClientResponseBody(HttpClientResponseBody&&) = delete;
    HttpClientResponseBody& operator=(HttpClientResponseBody&&) = delete;

    // The returned view remains valid until the next body operation. A null
    // optional is the only end-of-body signal; an empty data chunk is never
    // returned. Reads are linear and concurrent operations are rejected.
    [[nodiscard]] ScopedOperation<std::optional<std::string_view>> read() &;
    ScopedOperation<std::optional<std::string_view>> read() && = delete;

    // Collects the unread remainder of this same stream. maxBytes is a caller
    // bound in addition to the origin's transport bound.
    [[nodiscard]] ScopedOperation<std::pmr::string> readAll(std::size_t maxBytes = kDefaultMaxBufferedBodyBytes) &;
    ScopedOperation<std::pmr::string> readAll(std::size_t = kDefaultMaxBufferedBodyBytes) && = delete;

    // Copies this same stream into a controller response stream with natural
    // backpressure. This is the common forwarding path for both small and
    // long-lived upstream responses.
    [[nodiscard]] ScopedOperation<void> pipeTo(ResponseStreamWriter& output) &;
    ScopedOperation<void> pipeTo(ResponseStreamWriter&) && = delete;

    [[nodiscard]] bool complete() const noexcept;

private:
    friend class HttpClientResponse;
    friend class detail::HttpClientPool;

    ~HttpClientResponseBody() = default;

    [[nodiscard]] static Task<std::optional<std::string_view>> readTask(detail::HttpClientResponseState& state);
    [[nodiscard]] static Task<std::pmr::string> readAllTask(detail::HttpClientResponseState& state, std::size_t maxBytes);
    [[nodiscard]] static Task<void> pipeToTask(detail::HttpClientResponseState& state, ResponseStreamWriter& output);

    explicit HttpClientResponseBody(detail::HttpClientResponseState* state) noexcept
        : state_(state) {}

    detail::HttpClientResponseState* state_{nullptr};
};

class HttpClientResponse final {
public:
    HttpClientResponse(const HttpClientResponse&) = delete;
    HttpClientResponse& operator=(const HttpClientResponse&) = delete;
    // Body operations bind to the address-stable response state, so the public
    // owner remains movable while an operation is cold or running.
    HttpClientResponse(HttpClientResponse&& other) noexcept;
    HttpClientResponse& operator=(HttpClientResponse&& other) noexcept;
    ~HttpClientResponse();

    [[nodiscard]] HttpStatusCode status() const noexcept;
    [[nodiscard]] HttpProtocolVersion protocolVersion() const noexcept;
    [[nodiscard]] std::span<const HttpClientResponseHeader> headers() const& noexcept;
    [[nodiscard]] std::span<const HttpClientResponseHeader> headers() const&& = delete;
    [[nodiscard]] std::span<const HttpClientResponseHeader> trailers() const& noexcept;
    [[nodiscard]] std::span<const HttpClientResponseHeader> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const& noexcept;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view) const&& = delete;
    [[nodiscard]] HttpClientResponseBody& body() & noexcept {
        return body_;
    }
    [[nodiscard]] const HttpClientResponseBody& body() const& = delete;
    [[nodiscard]] HttpClientResponseBody& body() && = delete;

private:
    friend class detail::HttpClientPool;

    HttpClientResponse(std::pmr::memory_resource* resource, const WorkerHandle& worker, detail::HttpClientPool& pool);
    HttpClientResponse(std::pmr::memory_resource*, WorkerHandle&&, detail::HttpClientPool&) = delete;
    HttpClientResponse(detail::HttpClientResponseState* state, bool retain) noexcept;
    void release() noexcept;

    detail::HttpClientResponseState* state_{nullptr};
    HttpClientResponseBody body_{state_};
    bool consumer_{true};
};

}  // namespace ruvia
