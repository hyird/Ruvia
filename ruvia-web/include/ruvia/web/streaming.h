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

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_timer.h"
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
class operation_interrupt_source;
}  // namespace detail

class body_reader final {
private:
    friend struct detail::streaming_access;
    friend class multipart_reader;

    using read_type = detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)>;

    body_reader(void* target, read_type::invoke_type read) noexcept
        : read_(detail::callback_access::make<task<std::optional<std::span<const std::byte>>>(operation_options)>(target, read)) {}

public:
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
    /// `operation` 的超时或停止只结束本次等待，抛 system_error（timed_out /
    /// operation_canceled），不返回 EOF；已缓冲正文保留，可继续读取。
    /// 不取消响应方向，不放宽服务器已有正文超时；非正超时抛 invalid_argument。
    /// 启动的任务仍须等待完成后再释放宿主。
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;

    /// Reads the next chunk as characters without charset conversion or UTF-8
    /// validation. Chunk boundaries may split encoded characters. Shares read()'s
    /// lifetime, linear operation and `operation` contract.
    [[nodiscard]] task<std::optional<std::string_view>> text(operation_options operation = {}) &;
    task<std::optional<std::string_view>> text(operation_options = {}) && = delete;

private:
    void bind_worker(const worker_handle& worker_value) noexcept {
        worker_ = &worker_value;
        operation_scope_.bind_worker(worker_value);
    }

    read_type read_;
    const worker_handle* worker_{nullptr};
    bool read_active_{false};
    ::ruvia::operation_scope operation_scope_;
};

// 提交成功后只有这两种正常结果：可生产正文，或整个响应已完成。
enum class stream_commit_result { body,
    completed };

class response_stream_writer final {
public:
    response_stream_writer(const response_stream_writer&) = delete;
    response_stream_writer& operator=(const response_stream_writer&) = delete;

    /// 显式提交响应头（包含 session 持久化）；成功返回 body 才可生产正文。
    /// HEAD、304 返回 completed，属于正常完成，不设置 aborted 或取消 token。
    /// commit/write/writeln/end 共用线性输出通道，并发启动抛 logic_error。
    /// 重复 commit 抛 logic_error；提交前失败仍可由框架替换为缓冲错误响应。
    /// 输出操作的 `operation` 超时或停止中止本请求流（HTTP/2、HTTP/3 重置流，
    /// HTTP/1 关闭连接；已写出部分无法撤回）并抛 system_error（timed_out /
    /// operation_canceled）；启动前已停止则不发出任何字节。非正超时抛
    /// invalid_argument；服务器写超时继续生效。
    [[nodiscard]] task<stream_commit_result> commit(operation_options operation = {}) &;
    task<stream_commit_result> commit(operation_options = {}) && = delete;

    /// 只在 commit 返回 body 后写正文；提交前、完成后调用均抛 logic_error。
    /// string_view 在调用返回前复制到 owner-worker PMR，临时正文可安全跨挂起。
    /// 输出缓冲及写超时作用域保留至传输操作完成。
    [[nodiscard]] task<void> write(std::span<const std::byte> chunk, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view chunk, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;

    /// Takes ownership of an already-allocated chunk and transfers it into the
    /// output lane, rebinding it to owner storage when needed before returning.
    /// Compatible allocators transfer the
    /// existing allocation; incompatible allocators copy it before returning.
    template <std::same_as<std::pmr::string> string_type>
    [[nodiscard]] task<void> write(string_type&& chunk, operation_options operation = {}) & {
        return write_owned(std::move(chunk), std::move(operation));
    }
    template <std::same_as<std::pmr::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;

    [[nodiscard]] task<void> writeln(std::string_view chunk, operation_options operation = {}) &;
    task<void> writeln(std::string_view, operation_options = {}) && = delete;

    /// 仅在正文阶段挂起生产者；正常超时返回 elapsed，context::stop_token()
    /// 或 `stop` 取消（含客户端断开）返回 stop_requested，与 sleep_for 相同：
    /// duration 是睡眠目标而非操作时限，故不接受 operation_options。
    /// 提交前或响应完成后调用抛 logic_error；HTTP/2 对端终止仍作为传输错误报告。
    [[nodiscard]] task<timer_sleep_result> sleep(timeout_duration duration, stop_token stop = {}) &;
    task<timer_sleep_result> sleep(timeout_duration, stop_token = {}) && = delete;

    /// 仅报告已观察到的真实失败或取消；正常完成不等于 aborted。
    /// 观察写失败与读侧终止（HTTP/1 请求体完整消费后的 FIN/RST，HTTP/2 流终止）。
    /// false 不保证对端仍连接。
    [[nodiscard]] bool aborted() const noexcept {
        return aborted_(target_);
    }
    [[nodiscard]] bool committed() const noexcept {
        return committed_(target_);
    }

    /// 正常结束正文并可附 Trailer；未提交时可用于提交一个空响应。
    /// Trailer 视图在调用返回前复制到 owner storage；HTTP/1.0 等无法承载的可选
    /// Trailer 会被省略，禁止 Trailer 的状态在未发送头之前拒绝非空部分。
    /// 已正常完成后空 end() 幂等，非空 Trailer 抛 logic_error；生产正文仍必须
    /// 先显式 commit() 并检查结果，不能用 end() 隐藏无正文阶段。
    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {}) &;
    task<void> end(std::span<const http_header_view> = {}, operation_options = {}) && = delete;

private:
    friend struct detail::streaming_access;

    using commit_type = task<stream_commit_result> (*)(void*);
    using write_type = task<void> (*)(void*, std::string_view);
    using end_type = task<void> (*)(void*, std::span<const http_header_view>);
    using sleep_type = task<timer_sleep_result> (*)(void*, std::chrono::milliseconds, const stop_token&);
    using bind_context_type = void (*)(void*, context*);
    using release_context_type = void (*)(void*) noexcept;
    using committed_type = bool (*)(void*) noexcept;
    using aborted_type = bool (*)(void*) noexcept;

    // `interrupts` is the sink's; it routes stop requests to output operations.
    response_stream_writer(std::pmr::memory_resource& resource, void* target, detail::operation_interrupt_source& interrupts,
        commit_type commit, write_type write, end_type end, sleep_type sleep, bind_context_type bind_context,
        release_context_type release_context, committed_type committed, aborted_type aborted) noexcept
        : resource_(&resource),
          target_(target),
          interrupts_(&interrupts),
          commit_(commit),
          write_(write),
          end_(end),
          sleep_(sleep),
          bind_context_(bind_context),
          release_context_(release_context),
          committed_(committed),
          aborted_(aborted) {}

    void bind_worker(const worker_handle& worker_value) noexcept {
        worker_ = &worker_value;
        operation_scope_.bind_worker(worker_value);
    }

    // The context's stop token travels with the binding so sleep() observes
    // worker shutdown, the client leaving, and explicit subrequest cancellation.
    // An interrupted output operation resets the request through the context.
    void bind_context(context& context_value, stop_token stop_token_value) {
        stop_token_ = std::move(stop_token_value);
        bind_context_(target_, &context_value);
        context_ = &context_value;
    }

    void release_context() noexcept {
        operation_scope_.close();
        release_context_(target_);
        context_ = nullptr;
    }

    void require_active() const {
        if (!operation_scope_.active()) {
            throw std::logic_error("response stream lifetime has expired");
        }
    }
    void require_output() const;

    task<void> write_owned(std::pmr::string&& chunk, operation_options operation);

    std::pmr::memory_resource* resource_;
    void* target_;
    detail::operation_interrupt_source* interrupts_;
    commit_type commit_;
    write_type write_;
    end_type end_;
    sleep_type sleep_;
    bind_context_type bind_context_;
    release_context_type release_context_;
    committed_type committed_;
    aborted_type aborted_;
    stop_token stop_token_;
    const worker_handle* worker_{nullptr};
    context* context_{nullptr};
    bool output_active_{false};
    ::ruvia::operation_scope operation_scope_;

    friend class sse_writer;
};

// A copyable handle to the context's response stream; operations borrow that
// stream, not this handle.
class sse_writer final {
public:
    sse_writer(const sse_writer& other) noexcept;
    sse_writer& operator=(const sse_writer&) = delete;
    sse_writer(sse_writer&& other) noexcept;
    sse_writer& operator=(sse_writer&&) = delete;

    /// 与 response_stream_writer 相同；completed 时停止事件生产，不伪造取消。
    [[nodiscard]] task<stream_commit_result> commit(operation_options operation = {}) &;
    task<stream_commit_result> commit(operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(const sse_message& message, operation_options operation = {}) &;
    task<void> write(const sse_message&, operation_options = {}) && = delete;

    [[nodiscard]] task<timer_sleep_result> sleep(timeout_duration duration, stop_token stop = {}) &;
    task<timer_sleep_result> sleep(timeout_duration, stop_token = {}) && = delete;

    [[nodiscard]] bool aborted() const noexcept {
        return writer_ == nullptr || writer_->aborted();
    }
    [[nodiscard]] bool committed() const noexcept {
        return writer().committed();
    }

    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {}) &;
    task<void> end(std::span<const http_header_view> = {}, operation_options = {}) && = delete;

private:
    friend class context;

    explicit sse_writer(response_stream_writer& writer) noexcept;
    [[nodiscard]] response_stream_writer& writer() const;
    static void expire_capability(void* target) noexcept;

    response_stream_writer* writer_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
