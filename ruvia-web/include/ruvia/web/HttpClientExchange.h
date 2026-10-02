#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/web/HttpClientResponse.h"
#include "ruvia/web/HttpClientUploadConfig.h"

namespace ruvia {
namespace detail {
class HttpClientPool;
class HttpClientResponseState;
struct HttpClientUploadWriteInput;
struct HttpClientUploadEndInput;
}  // namespace detail

// One upload lane embedded in its exchange. Each chunk/trailer input is owned
// before return; completing write() means transport accepted the entire chunk.
// Only one write/end operation may exist at a time. Chunk size is bounded by
// maxChunkBytes; split larger sources into sequential writes.
class HttpClientRequestBodyWriter final {
public:
    HttpClientRequestBodyWriter(const HttpClientRequestBodyWriter&) = delete;
    HttpClientRequestBodyWriter& operator=(const HttpClientRequestBodyWriter&) = delete;
    [[nodiscard]] ScopedOperation<void> write(std::span<const std::byte> bytes) &;
    ScopedOperation<void> write(std::span<const std::byte>) && = delete;
    [[nodiscard]] ScopedOperation<void> write(std::string_view bytes) &;
    ScopedOperation<void> write(std::string_view) && = delete;
    [[nodiscard]] ScopedOperation<void> end(std::span<const HttpHeaderView> trailers = {}) &;
    ScopedOperation<void> end(std::span<const HttpHeaderView> = {}) && = delete;
    [[nodiscard]] bool complete() const noexcept;

private:
    friend class HttpClientExchange;
    explicit HttpClientRequestBodyWriter(detail::HttpClientResponseState* state) noexcept
        : state_(state) {}
    ~HttpClientRequestBodyWriter() = default;
    static Task<void> writeOwned(detail::HttpClientUploadWriteInput input);
    static Task<void> endOwned(detail::HttpClientUploadEndInput input);
    detail::HttpClientResponseState* state_;
};

// Owns a streaming request and its eventual response on one worker. Moving
// preserves operations because their state is address stable. Destruction
// abandons unfinished upload; client shutdown wakes and joins its producer.
class HttpClientExchange final {
public:
    HttpClientExchange(const HttpClientExchange&) = delete;
    HttpClientExchange& operator=(const HttpClientExchange&) = delete;
    HttpClientExchange(HttpClientExchange&& other) noexcept;
    HttpClientExchange& operator=(HttpClientExchange&& other) noexcept;
    ~HttpClientExchange();
    [[nodiscard]] HttpClientRequestBodyWriter& body() & noexcept {
        return body_;
    }
    HttpClientRequestBodyWriter& body() && = delete;
    // Can run concurrently with the upload lane to observe an early final
    // response. Transfers response ownership once, when actually started.
    [[nodiscard]] ScopedOperation<HttpClientResponse> response() &;
    ScopedOperation<HttpClientResponse> response() && = delete;

private:
    friend class detail::HttpClientPool;
    explicit HttpClientExchange(HttpClientResponse response) noexcept;
    void release() noexcept;
    static Task<HttpClientResponse> receiveOwned(HttpClientResponse pin);
    HttpClientResponse response_;
    HttpClientRequestBodyWriter body_;
};
}  // namespace ruvia
