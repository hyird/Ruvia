#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/sse.h"
#include "ruvia/web/detail/util/callable_ref.h"

namespace ruvia {

class context;
class http_response;

namespace detail {
struct streaming_access;
}

class body_reader final {
private:
    friend struct detail::streaming_access;
    friend class multipart_reader;

    struct token_type final {};

public:
    body_reader(token_type, void* target,
        detail::callable_ref<std::optional<std::span<const std::byte>>>::invoke_type read) noexcept
        : read_(target, read) {}

    body_reader(const body_reader&) = delete;
    body_reader& operator=(const body_reader&) = delete;

    /// Reads the next chunk of the streamed request body.
    /// @warning The returned view borrows the connection read buffer and is only valid
    /// until the NEXT read() call; copy it out if you need to retain it past then. An
    /// empty optional signals end-of-body. Only one read operation may be outstanding;
    /// creating another before it completes or is discarded throws because concurrent
    /// consumers cannot safely share the borrowed buffer.
    [[nodiscard]] scoped_operation<std::optional<std::span<const std::byte>>> read() &;
    scoped_operation<std::optional<std::span<const std::byte>>> read() && = delete;

    /// Reads the next chunk as characters without charset conversion or UTF-8
    /// validation. Chunk boundaries may split encoded characters. Shares read()'s
    /// lifetime and linear operation contract.
    [[nodiscard]] scoped_operation<std::optional<std::string_view>> text() &;
    scoped_operation<std::optional<std::string_view>> text() && = delete;

private:
    detail::callable_ref<std::optional<std::span<const std::byte>>> read_;
    ::ruvia::operation_scope operation_scope_;
};

class response_stream_writer final {
public:
    response_stream_writer(const response_stream_writer&) = delete;
    response_stream_writer& operator=(const response_stream_writer&) = delete;

    /// Writes one body chunk. write(), writeln(), and end() share one linear output lane.
    /// Each returned output operation reserves the lane immediately; creating another
    /// before it completes or is discarded throws std::logic_error. The string_view overload
    /// copies the chunk into owner-worker PMR storage before returning.
    scoped_operation<void> write(std::span<const std::byte> chunk) &;
    scoped_operation<void> write(std::span<const std::byte>) && = delete;
    scoped_operation<void> write(std::string_view chunk) &;
    scoped_operation<void> write(std::string_view) && = delete;

    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                 std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> write(text_type&& chunk) & {
        return write(std::string_view(std::forward<text_type>(chunk)));
    }
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                    std::constructible_from<std::string_view, text_type &&>)
    scoped_operation<void> write(text_type&&) && = delete;

    /// Takes ownership of an already-allocated chunk and transfers it into the
    /// output lane, rebinding it to owner storage when needed before returning.
    /// Compatible allocators transfer the
    /// existing allocation; incompatible allocators copy it before returning.
    scoped_operation<void> write(std::pmr::string&& chunk) &;
    scoped_operation<void> write(std::pmr::string&&) && = delete;

    scoped_operation<void> writeln(std::string_view chunk) &;
    scoped_operation<void> writeln(std::string_view) && = delete;

    /// Suspends the stream producer. The result is elapsed for a normal
    /// delay, or stop_requested when the owning worker is shutting down or the
    /// request's stop token trips. HTTP/2 peer termination remains reported as
    /// its transport error.
    scoped_operation<timer_sleep_result> sleep(std::chrono::milliseconds duration) &;
    scoped_operation<timer_sleep_result> sleep(std::chrono::milliseconds) && = delete;

    /// Whether the response stream can no longer be delivered. The signal is
    /// transport-dependent: on HTTP/2 it also turns true when the peer resets or
    /// terminates the stream, so a long-lived producer can observe a passive
    /// client disconnect; on HTTP/1 it reflects only a prior failed write, so a
    /// passive disconnect is not seen until the next write() fails. Treat a false
    /// result as "no failure observed yet", not a guarantee the client is still
    /// connected.
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_(target_);
    }

    /// Atomically terminates the stream with an optional trailer section.
    /// The header views only need to remain valid until the returned task completes.
    /// A non-empty section is never silently dropped when the selected HTTP
    /// version/method/status cannot represent trailers. If the stream is still
    /// uncommitted, that rejection occurs before the response head is emitted.
    scoped_operation<void> end(std::span<const http_header_view> trailers = {}) &;
    scoped_operation<void> end(std::span<const http_header_view> = {}) && = delete;

private:
    friend struct detail::streaming_access;

    using write_type = task<void> (*)(void*, std::string_view);
    using end_type = task<void> (*)(void*, std::span<const http_header_view>);
    using sleep_type = task<timer_sleep_result> (*)(void*, std::chrono::milliseconds, const stop_token&);
    using streaming_head_thunk_type = task<http_response> (*)(context&);
    using bind_context_type = void (*)(void*, context*, streaming_head_thunk_type);
    using release_context_type = void (*)(void*) noexcept;
    using committed_type = bool (*)(void*) noexcept;
    using aborted_type = bool (*)(void*) noexcept;

    response_stream_writer(std::pmr::memory_resource& resource, void* target, write_type write, end_type end, sleep_type sleep, bind_context_type bind_context,
        release_context_type release_context, committed_type committed, aborted_type aborted) noexcept
        : resource_(&resource),
          target_(target),
          write_(write),
          end_(end),
          sleep_(sleep),
          bind_context_(bind_context),
          release_context_(release_context),
          committed_(committed),
          aborted_(aborted) {}

    // The request's stop token travels with the binding so sleep() observes it;
    // every framework-provided wait must preserve the request deadline.
    void bind_context(context& context_value, stop_token stop_token_value, streaming_head_thunk_type streaming_head) {
        stop_token_ = std::move(stop_token_value);
        bind_context_(target_, &context_value, streaming_head);
    }

    void release_context() noexcept {
        operation_scope_.close();
        release_context_(target_);
    }

    void require_active() const {
        if (!operation_scope_.active()) {
            throw std::logic_error("response stream lifetime has expired");
        }
    }

    [[nodiscard]] bool committed() const noexcept {
        return committed_(target_);
    }

    std::pmr::memory_resource* resource_;
    void* target_;
    write_type write_;
    end_type end_;
    sleep_type sleep_;
    bind_context_type bind_context_;
    release_context_type release_context_;
    committed_type committed_;
    aborted_type aborted_;
    stop_token stop_token_;
    bool output_active_{false};
    ::ruvia::operation_scope operation_scope_;

    friend class sse_writer;
};

class sse_writer final {
public:
    sse_writer(const sse_writer& other) noexcept;
    sse_writer& operator=(const sse_writer&) = delete;
    sse_writer(sse_writer&& other) noexcept;
    sse_writer& operator=(sse_writer&&) = delete;

    scoped_operation<void> write(const sse_message& message);

    scoped_operation<timer_sleep_result> sleep(std::chrono::milliseconds duration);

    [[nodiscard]] bool aborted() const noexcept {
        return writer_ == nullptr || writer_->aborted();
    }

    scoped_operation<void> end(std::span<const http_header_view> trailers = {});

private:
    friend class context;
    friend struct detail::streaming_access;

    explicit sse_writer(response_stream_writer& writer) noexcept;
    [[nodiscard]] response_stream_writer& writer() const;
    static void expire_capability(void* target) noexcept;

    response_stream_writer* writer_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
