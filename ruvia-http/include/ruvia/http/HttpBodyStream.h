#pragma once

#include <string_view>
#include <utility>

namespace ruvia {

template <typename T>
class Task;

// The framework's single pull-based streaming body. A consumer calls nextChunk() repeatedly and
// uses each returned slice -- a borrowed view valid only until the next nextChunk() call or until
// the stream is destroyed -- stopping when an empty view signals end of stream.
//
// Owning + move-only: the producer is moved in (destroy_ frees it), so the stream outlives the
// coroutine that created it (e.g. a route handler that returns a streaming HttpResponse). The
// concrete producer is type-erased behind void* + two function pointers -- no std::function, no
// vtable, one PMR allocation for the producer -- so this header stays dependency-free and the same
// type serves the HTTP server (streaming response body) and the HTTP client (streamed response).
//
// destroy_ == nullptr denotes a borrowed producer the stream does not own (the caller keeps it
// alive); such a handle is still move-only for a single, uniform ownership story.
class HttpBodyStream final {
public:
    using NextChunk = Task<std::string_view> (*)(void*);
    using Destroy = void (*)(void*) noexcept;

    HttpBodyStream() noexcept = default;
    HttpBodyStream(void* target, NextChunk next, Destroy destroy = nullptr) noexcept
        : target_(target), next_(next), destroy_(destroy) {}

    HttpBodyStream(HttpBodyStream&& other) noexcept
        : target_(std::exchange(other.target_, nullptr)),
          next_(std::exchange(other.next_, nullptr)),
          destroy_(other.destroy_) {}

    HttpBodyStream& operator=(HttpBodyStream&& other) noexcept {
        if (this != &other) {
            reset();
            target_ = std::exchange(other.target_, nullptr);
            next_ = std::exchange(other.next_, nullptr);
            destroy_ = other.destroy_;
        }
        return *this;
    }

    HttpBodyStream(const HttpBodyStream&) = delete;
    HttpBodyStream& operator=(const HttpBodyStream&) = delete;

    ~HttpBodyStream() { reset(); }

    [[nodiscard]] explicit operator bool() const noexcept { return next_ != nullptr; }

    // Next slice of the body; an empty view signals end of stream. The view is valid until the
    // next call to nextChunk() or the stream's destruction. An empty/closed stream (no producer)
    // safely yields end of stream rather than calling a null function pointer.
    [[nodiscard]] Task<std::string_view> nextChunk() const;

private:
    void reset() noexcept {
        if (target_ != nullptr && destroy_ != nullptr) {
            destroy_(target_);
        }
        target_ = nullptr;
        next_ = nullptr;
    }

    void* target_{nullptr};
    NextChunk next_{nullptr};
    Destroy destroy_{nullptr};
};

}  // namespace ruvia
