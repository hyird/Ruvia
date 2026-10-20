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
#include "ruvia/web/detail/callback_ref.h"

namespace ruvia {

class context;

namespace detail {
struct streaming_access;
}

class body_reader final {
private:
    friend struct detail::streaming_access;
    friend class multipart_reader;

    struct token_type final {};
    using read_type = detail::callback_ref_type<task<std::optional<std::span<const std::byte>>>()>;

public:
    body_reader(token_type, void* target, read_type::invoke_type read) noexcept
        : read_(detail::callback_access::make<task<std::optional<std::span<const std::byte>>>()>(target, read)) {}

    body_reader(const body_reader&) = delete;
    body_reader& operator=(const body_reader&) = delete;

    /// Reads the next chunk of the streamed request body.
    /// @warning The returned view borrows the connection read buffer and is only valid
    /// until the next read operation starts; copy it out if you need to retain it past then.
    /// An empty optional signals end-of-body. Only one read operation may run at a time;
    /// starting another before it completes throws because concurrent consumers cannot
    /// safely share the borrowed buffer. Unstarted operations may coexist.
    /// HTTP/1 enters the interim-write operation only while a 100 Continue is
    /// pending; later reads do not repeat that operation.
    [[nodiscard]] scoped_operation<std::optional<std::span<const std::byte>>> read() &;
    scoped_operation<std::optional<std::span<const std::byte>>> read() && = delete;

    /// Reads the next chunk as characters without charset conversion or UTF-8
    /// validation. Chunk boundaries may split encoded characters. Shares read()'s
    /// lifetime and linear operation contract.
    [[nodiscard]] scoped_operation<std::optional<std::string_view>> text() &;
    scoped_operation<std::optional<std::string_view>> text() && = delete;

private:
    read_type read_;
    bool read_active_{false};
    ::ruvia::operation_scope operation_scope_;
};

class response_stream_writer final {
public:
    response_stream_writer(const response_stream_writer&) = delete;
    response_stream_writer& operator=(const response_stream_writer&) = delete;

    /// Writes one body chunk. write(), writeln(), and end() share one linear output lane.
    /// An operation occupies the lane when started; starting another before it completes
    /// throws std::logic_error. Unstarted operations may coexist. The string_view overload
    /// copies the chunk into owner-worker PMR storage before returning.
    /// HTTP/1 retains framing bytes and the temporary write-deadline scope until
    /// the complete transport write settles.
    /// HTTP/2 may reuse normal DATA-frame transport storage between successful writes;
    /// oversized write buffers are released after completion.
    scoped_operation<void> write(std::span<const std::byte> chunk) &;
    scoped_operation<void> write(std::span<const std::byte>) && = delete;
    scoped_operation<void> write(std::string_view chunk) &;
    scoped_operation<void> write(std::string_view) && = delete;

    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                 std::constructible_from<std::string_view, text_type&&>)
    scoped_operation<void> write(text_type&& chunk) & {
        return write(std::string_view(std::forward<text_type>(chunk)));
    }
    template <typename text_type>
        requires(!std::same_as<std::remove_cvref_t<text_type>, std::pmr::string> &&
                    std::constructible_from<std::string_view, text_type&&>)
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
    /// request's stop token trips. HTTP/2 peer termination is reported as its
    /// transport error.
    scoped_operation<timer_sleep_result> sleep(std::chrono::milliseconds duration) &;
    scoped_operation<timer_sleep_result> sleep(std::chrono::milliseconds) && = delete;

    /// Whether the response stream can no longer be delivered. HTTP/2 also
    /// observes peer reset or connection termination, including while sleeping.
    /// HTTP/1 observes passive disconnect only when a subsequent write fails,
    /// including the head write of a response whose body is suppressed.
    /// False means no failure has been observed, not that the peer is connected.
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_(target_);
    }

    /// Atomically terminates the stream with an optional trailer section.
    /// Header views are copied into owner storage before this function returns.
    /// Framing that cannot carry optional trailers (for example, HTTP/1.0)
    /// omits them. A response status that forbids trailers rejects a non-empty
    /// section before an uncommitted response head is emitted.
    /// On HTTP/2, a committed head needs no further head-commit task, while
    /// trailer constraints remain enforced.
    scoped_operation<void> end(std::span<const http_header_view> trailers = {}) &;
    scoped_operation<void> end(std::span<const http_header_view> = {}) && = delete;

private:
    friend struct detail::streaming_access;

    using write_type = task<void> (*)(void*, std::string_view);
    using end_type = task<void> (*)(void*, std::span<const http_header_view>);
    using sleep_type = task<timer_sleep_result> (*)(void*, std::chrono::milliseconds, const stop_token&);
    using bind_context_type = void (*)(void*, context*);
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

    // The context's stop token travels with the binding so sleep() observes
    // worker shutdown and explicit subrequest cancellation.
    void bind_context(context& context_value, stop_token stop_token_value) {
        stop_token_ = std::move(stop_token_value);
        bind_context_(target_, &context_value);
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

    explicit sse_writer(response_stream_writer& writer) noexcept;
    [[nodiscard]] response_stream_writer& writer() const;
    static void expire_capability(void* target) noexcept;

    response_stream_writer* writer_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
