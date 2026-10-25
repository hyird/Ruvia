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

// Route-level policy declaration that switches the matched route's request body
// from buffered to streamed (read via c.req().body_reader() or
// c.req().multipart_reader()). May be combined with either a buffered-response
// handler (task<http_response>) or a streaming-response handler (task<void>).
struct stream_body final {
    static constexpr bool ruvia_stream_request_body = true;
};
namespace detail {
struct streaming_access;
}

class body_reader final {
private:
    friend struct detail::streaming_access;
    friend class multipart_reader;

    struct token_type final {};
    using read_type = detail::callback_ref<task<std::optional<std::span<const std::byte>>>()>;

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
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read() &;
    task<std::optional<std::span<const std::byte>>> read() && = delete;

    /// Reads the next chunk as characters without charset conversion or UTF-8
    /// validation. Chunk boundaries may split encoded characters. Shares read()'s
    /// lifetime and linear operation contract.
    [[nodiscard]] task<std::optional<std::string_view>> text() &;
    task<std::optional<std::string_view>> text() && = delete;

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
    /// A body-suppressed response (HEAD on a GET stream route, 304) is complete
    /// once its head is sent: the first non-empty write commits that head, and
    /// it and every later write() / writeln() succeed without sending anything
    /// while the connection that carried the response lives; each later write
    /// suspends briefly instead of returning synchronously. Once that
    /// connection ends (the peer closes or resets it, or the server closes it
    /// on a timeout, drain or shutdown; on HTTP/2 also the stream is reset) the
    /// next write fails exactly as a GET write fails after a peer abort, so a
    /// loop that never checks aborted() ends no later than it would for GET.
    /// On HTTP/1 the server watches the connection's read side from that head
    /// on: a peer end with no further request bytes (or on a connection that
    /// will close after the response) ends the connection, while one behind
    /// pipelined requests on a reusable connection is a half-close whose
    /// requests are still answered. end() succeeds and
    /// discards trailers after validating them. From the head on aborted() is
    /// true, sleep() returns stop_requested at once, and the
    /// cancel_on_peer_eof() token is stopped.
    task<void> write(std::span<const std::byte> chunk) &;
    task<void> write(std::span<const std::byte>) && = delete;
    task<void> write(std::string_view chunk) &;
    task<void> write(std::string_view) && = delete;

    /// Takes ownership of an already-allocated chunk and transfers it into the
    /// output lane, rebinding it to owner storage when needed before returning.
    /// Compatible allocators transfer the
    /// existing allocation; incompatible allocators copy it before returning.
    template <std::same_as<std::pmr::string> string_type>
    task<void> write(string_type&& chunk) & {
        return write_owned(std::move(chunk));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> write(string_type&&) && = delete;

    task<void> writeln(std::string_view chunk) &;
    task<void> writeln(std::string_view) && = delete;

    /// Suspends the stream producer. The result is elapsed for a normal
    /// delay, or stop_requested when the owning worker is shutting down, the
    /// request's stop token trips, or a body-suppressed head already ended the
    /// response (returned at once). HTTP/2 peer termination is reported as its
    /// transport error.
    task<timer_sleep_result> sleep(std::chrono::milliseconds duration) &;
    task<timer_sleep_result> sleep(std::chrono::milliseconds) && = delete;

    /// 显式将 HTTP/1 peer EOF（包括合法的 TCP 写半关闭）视为放弃当前响应。
    /// 仅在请求体完整消费后可启用，否则抛出 std::logic_error；不主动读取或丢弃请求体。
    /// 返回与请求取消组合的 token，供订阅、数据库等异步等待使用；sleep() 也观察它。
    /// 不改变 context::stop_token() 或此前复制的 token。HTTP/2 只观察当前流终止。
    /// 不带内容的响应头（HEAD、304）结束响应后，该 token 即处于停止状态（之后调用也返回已停止的 token）。
    /// 检测由连接 owner 管理，响应结束前取消并等待其完成。后续请求字节完整保留；
    /// 读前探缓冲达到既有请求头上限时暂停检测，不为探测分配无界存储。
    /// 默认不开启；FIN 无法区分关闭整个客户端与仅关闭发送方向。
    [[nodiscard]] stop_token cancel_on_peer_eof() &;
    stop_token cancel_on_peer_eof() && = delete;

    /// Whether the response stream can no longer be delivered: a failure was
    /// observed, or a body-suppressed head (HEAD, 304) already ended the
    /// response. HTTP/2 also observes peer reset or connection termination,
    /// including while sleeping.
    /// HTTP/1 默认仅在写失败时观察断开；显式 cancel_on_peer_eof() 还观察读侧终止。
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
    task<void> end(std::span<const http_header_view> trailers = {}) &;
    task<void> end(std::span<const http_header_view> = {}) && = delete;

private:
    friend struct detail::streaming_access;

    using write_type = task<void> (*)(void*, std::string_view);
    using end_type = task<void> (*)(void*, std::span<const http_header_view>);
    using sleep_type = task<timer_sleep_result> (*)(void*, std::chrono::milliseconds, const stop_token&);
    using bind_context_type = void (*)(void*, context*);
    using release_context_type = void (*)(void*) noexcept;
    using committed_type = bool (*)(void*) noexcept;
    using aborted_type = bool (*)(void*) noexcept;
    using cancel_on_peer_eof_type = stop_token (*)(void*);

    response_stream_writer(std::pmr::memory_resource& resource, void* target, write_type write, end_type end, sleep_type sleep, bind_context_type bind_context,
        release_context_type release_context, committed_type committed, aborted_type aborted,
        cancel_on_peer_eof_type cancel_on_peer_eof) noexcept
        : resource_(&resource),
          target_(target),
          write_(write),
          end_(end),
          sleep_(sleep),
          bind_context_(bind_context),
          release_context_(release_context),
          committed_(committed),
          aborted_(aborted),
          cancel_on_peer_eof_(cancel_on_peer_eof) {}

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

    task<void> write_owned(std::pmr::string&& chunk);

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
    cancel_on_peer_eof_type cancel_on_peer_eof_;
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

    task<void> write(const sse_message& message);

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration);

    /// 与 response_stream_writer::cancel_on_peer_eof() 相同；将返回 token 传给业务等待。
    [[nodiscard]] stop_token cancel_on_peer_eof();

    [[nodiscard]] bool aborted() const noexcept {
        return writer_ == nullptr || writer_->aborted();
    }

    task<void> end(std::span<const http_header_view> trailers = {});

private:
    friend class context;

    explicit sse_writer(response_stream_writer& writer) noexcept;
    [[nodiscard]] response_stream_writer& writer() const;
    static void expire_capability(void* target) noexcept;

    response_stream_writer* writer_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
