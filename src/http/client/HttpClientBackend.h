#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <chrono>
#include <memory>
#include <memory_resource>
#include <string_view>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

// Common interface for an outbound HTTP client transport bound to one origin (host:port,
// scheme). HttpClientPool implements it over HTTP/1.1 (a pool of connections); Http2ClientSession
// implements it over a single multiplexed HTTP/2 connection. The registry stores backends
// polymorphically and the request path is version-agnostic.
class HttpClientBackend {
public:
    virtual ~HttpClientBackend() = default;

    // Establish the connection(s) up front (optional warm-up before the first request).
    virtual Task<void> connect() = 0;
    virtual void closeNow() noexcept = 0;
    // True once closeNow() has run AND no in-flight request or self-referencing background
    // coroutine remains, so the backend can be safely destroyed. Used to defer destruction of a
    // client removed at runtime until its io_context has drained (removeHttpClient).
    [[nodiscard]] virtual bool isQuiescent() const noexcept = 0;
    [[nodiscard]] virtual bool hasAnyTimeout() const noexcept = 0;
    virtual void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept = 0;
    [[nodiscard]] virtual Task<FetchResponse> fetch(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource) = 0;

    // Like fetch(), but returns a stream whose body is pulled incrementally rather than buffered.
    [[nodiscard]] virtual Task<FetchResponseStream> fetchStream(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource) = 0;

    // Destroy and deallocate self through the concrete type's PMR resource. Implementations
    // call destroyPmrObject(this, resource_) so the correct object size is returned to the pool.
    virtual void destroy() noexcept = 0;
};

// Deleter for unique_ptr<HttpClientBackend, ...>: defers to the backend's own PMR-aware
// destroy(), sidestepping PmrObjectDeleter's need to know the concrete size statically.
struct HttpClientBackendDeleter final {
    void operator()(HttpClientBackend* backend) const noexcept {
        if (backend != nullptr) {
            backend->destroy();
        }
    }
};

using HttpClientBackendPtr = std::unique_ptr<HttpClientBackend, HttpClientBackendDeleter>;

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
