#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "borrowed_text.h"
#include "http1_server_response.h"
#include "http_connect_udp.h"
#include "http_header.h"
#include "http_multipart.h"
#include "http_server_sent_events.h"
#include "middleware.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "websocket_connection.h"
#include "worker_ref.h"
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

    body_reader(void* target, read_type::invoke_type read) noexcept :
        read_(detail::callback_access::make<task<std::optional<std::span<const std::byte>>>(operation_options)>(target, read)) {}

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
    void bind_worker(const worker_ref& worker_value) noexcept {
        worker_ = &worker_value;
        operation_scope_.bind_worker(worker_value);
    }

    read_type read_;
    bool read_active_{false};
    const worker_ref* worker_{nullptr};
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
    /// output lane before returning.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& chunk, operation_options operation = {}) & {
        return write_owned(std::move(chunk), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
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
    response_stream_writer(void* target, detail::operation_interrupt_source& interrupts, commit_type commit, write_type write, end_type end, sleep_type sleep, bind_context_type bind_context, release_context_type release_context, committed_type committed, aborted_type aborted) noexcept :
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

    void bind_worker(const worker_ref& worker_value) noexcept {
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

    task<void> write_owned(std::string&& chunk, operation_options operation);

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
    const worker_ref* worker_{nullptr};
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

namespace detail {

struct streaming_access final {
    [[nodiscard]] static body_reader make_body_reader(void* target, body_reader::read_type::invoke_type read) noexcept {
        return body_reader(target, read);
    }

    [[nodiscard]] static response_stream_writer make_response_stream_writer(void* target, operation_interrupt_source& interrupts, response_stream_writer::commit_type commit, response_stream_writer::write_type write, response_stream_writer::end_type end, response_stream_writer::sleep_type sleep, response_stream_writer::bind_context_type bind_context, response_stream_writer::release_context_type release_context, response_stream_writer::committed_type committed, response_stream_writer::aborted_type aborted) noexcept {
        return response_stream_writer(target, interrupts, commit, write, end, sleep, bind_context, release_context, committed, aborted);
    }

    static void bind_worker(body_reader& reader, const worker_ref& worker_value) noexcept {
        reader.bind_worker(worker_value);
    }

    static void bind_worker(response_stream_writer& writer, const worker_ref& worker_value) noexcept {
        writer.bind_worker(worker_value);
    }

    [[nodiscard]] static bool& output_lane(response_stream_writer& writer) noexcept {
        return writer.output_active_;
    }

    static void bind_context(response_stream_writer& writer, context& context_value, stop_token stop_token_value) {
        writer.bind_context(context_value, std::move(stop_token_value));
    }

    static void release_context(response_stream_writer& writer) noexcept {
        writer.release_context();
    }
};

}  // namespace detail

class multipart_reader final {
public:
    multipart_reader(body_reader& body_reader_value, multipart_parse_options options) :
        body_reader_(nullptr),
        state_(std::in_place_type<expired_state_type>) {
        if (body_reader_value.operation_scope_.active()) {
            state_.emplace<receiving_state_type>(options);
            body_reader_ = &body_reader_value;
            if (body_reader_value.worker_ != nullptr) {
                operation_scope_.bind_worker(*body_reader_value.worker_);
            }
            registration_.bind(body_reader_value.operation_scope_, this, &multipart_reader::expire_capability);
        }
    }

    multipart_reader(const multipart_reader&) = delete;
    multipart_reader& operator=(const multipart_reader&) = delete;
    multipart_reader(multipart_reader&&) = delete;
    multipart_reader& operator=(multipart_reader&&) = delete;

    /// Returns one typed chunk of the current multipart part. All returned views
    /// remain valid only until the next read operation starts, parent body-reader
    /// expiry, or this reader's destruction. Unstarted read operations may coexist.
    /// `operation` bounds every body read this call makes, as body_reader::read()
    /// does one: a timeout or stop ends only this call and parsing resumes on the next.
    [[nodiscard]] task<std::optional<multipart_stream_part>> read(operation_options operation = {}) &;
    task<std::optional<multipart_stream_part>> read(operation_options = {}) && = delete;

private:
    struct receiving_state_type final {
        explicit receiving_state_type(multipart_parse_options options) :
            parser_(options) {}

        multipart_parser parser_;
    };
    struct finished_state_type final {};
    struct failed_state_type final {};
    struct expired_state_type final {};

    using state_type = std::variant<receiving_state_type, finished_state_type, failed_state_type, expired_state_type>;

    class read_guard_type final {
    public:
        explicit read_guard_type(multipart_reader& reader_value) noexcept :
            reader_(reader_value) {}

        ~read_guard_type() {
            if (!committed_ && std::holds_alternative<receiving_state_type>(reader_.state_)) {
                reader_.state_.emplace<failed_state_type>();
            }
        }

        void commit() noexcept {
            committed_ = true;
        }

    private:
        multipart_reader& reader_;
        bool committed_{false};
    };

    [[nodiscard]] body_reader& get_body_reader() const;
    static void expire_capability(void* target) noexcept;

    body_reader* body_reader_;
    state_type state_;
    bool read_active_{false};
    ::ruvia::operation_scope operation_scope_;
    scoped_capability_registration registration_;

    [[nodiscard]] task<std::optional<multipart_stream_part>> read_task(operation_options operation);
};

namespace detail {
class http_capsule_stream_state;
class capsule_state_pin;
class inbound_buffer_account;
template <typename stream_type>
class http_tunnel_session;
// Transport input for a single tunnel read lane. Datagram bytes exclude the
// HTTP/3 Quarter Stream ID but retain any protocol-specific Context ID.
struct http_tunnel_input final {
    std::string bytes_;
    bool datagram_{};
};
}  // namespace detail

class http_tunnel;
class http_client_tunnel;
class http_udp_tunnel;
class http_datagram_stream;

// Owns one capsule payload. Its storage remains valid across subsequent reads,
// stream destruction and connection retirement, even if input remains unread.
// Like other worker results, destroy it on its owner worker before that worker retires.
class http_capsule final {
public:
    http_capsule(const http_capsule&) = delete;
    http_capsule& operator=(const http_capsule&) = delete;
    http_capsule(http_capsule&& other) noexcept;
    http_capsule& operator=(http_capsule&& other) noexcept;
    ~http_capsule();
    [[nodiscard]] std::uint64_t type() const noexcept {
        return type_;
    }
    [[nodiscard]] std::string_view payload() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload_ ? std::string_view(*payload_) : std::string_view{};
    }
    std::string_view payload() const&& = delete;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = payload();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload();
    }
    std::string_view text() const&& = delete;

private:
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::string payload_value) noexcept;
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
    std::uint64_t type_{};
    std::optional<std::string> payload_;
};

// RFC 9297 reliable Capsule Protocol over an established tunnel. The protocol
// using the capsules owns negotiation. Server streams borrow their route
// tunnel; client streams take tunnel ownership. Use this adapter exclusively:
// do not interleave raw tunnel operations with capsule operations. Abort wakes
// pending I/O; join started operations before destroying their scope owner.
// Operations occupy their read or write/finish lane when started; unstarted
// operations may coexist. One read and one output operation may run concurrently.
// `operation` bounds a whole call: a timeout or stop ends only that read,
// keeping a partly received capsule for the next one, and aborts the stream
// during an output operation. Either throws std::system_error (timed_out or
// operation_canceled) -- over an http_client_tunnel its http_client_error
// subclass -- and a stop requested before an output operation starts sends nothing.
class http_capsule_stream final {
public:
    http_capsule_stream(const http_capsule_stream&) = delete;
    http_capsule_stream& operator=(const http_capsule_stream&) = delete;
    http_capsule_stream(http_capsule_stream&& other) noexcept;
    http_capsule_stream& operator=(http_capsule_stream&& other) noexcept;
    ~http_capsule_stream();
    [[nodiscard]] task<std::optional<http_capsule>> read(operation_options operation = {}) &;
    task<std::optional<http_capsule>> read(operation_options = {}) && = delete;
    // Owns input before returning its cold operation. Unknown capsule types are
    // delivered to the protocol consumer, which must ignore types it does not understand.
    [[nodiscard]] task<void> write(std::uint64_t type, std::string_view payload, operation_options operation = {}) &;
    task<void> write(std::uint64_t, std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::uint64_t type, std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> write(std::uint64_t, std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(std::uint64_t type, string_type&& payload, operation_options operation = {}) & {
        return write_owned(type, std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(std::uint64_t, string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    friend class http_datagram_stream;
    [[nodiscard]] static task<std::optional<http_capsule>> read_owned(detail::capsule_state_pin pin, operation_options operation);
    [[nodiscard]] task<void> write_frame(std::string_view prefix, std::string_view payload, bool datagram, operation_options operation);
    [[nodiscard]] task<void> write_owned(std::uint64_t type, std::string&& payload, operation_options operation);
    [[nodiscard]] task<void> write_frame_owned(std::string_view prefix, std::string&& payload, bool datagram, operation_options operation);
    explicit http_capsule_stream(http_tunnel& tunnel, http_capsule_config config);
    explicit http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config);
    void release() noexcept;
    detail::http_capsule_stream_state* state_{};
};

// Owned opaque HTTP Datagram payload, valid across later reads, stream and
// connection retirement, and client shutdown even if input remains unread.
// Destroy results on their worker before the worker retires.
class http_datagram final {
public:
    http_datagram(const http_datagram&) = delete;
    http_datagram& operator=(const http_datagram&) = delete;
    http_datagram(http_datagram&&) noexcept = default;
    http_datagram& operator=(http_datagram&&) noexcept = default;
    [[nodiscard]] std::span<const std::byte> payload() const& noexcept RUVIA_LIFETIMEBOUND {
        const auto view = text();
        return std::as_bytes(std::span(view.data(), view.size()));
    }
    std::span<const std::byte> payload() const&& = delete;
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return payload();
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        auto bytes_value = capsule_.payload();
        bytes_value.remove_prefix(std::min(offset_, bytes_value.size()));
        return bytes_value;
    }
    std::string_view text() const&& = delete;

private:
    friend class http_datagram_stream;
    http_datagram(http_capsule capsule, std::size_t offset) noexcept :
        capsule_(std::move(capsule)),
        offset_(offset) {}
    http_capsule capsule_;
    std::size_t offset_{};
};

// RFC 9297 channel using native QUIC DATAGRAMs when available, with reliable
// DATAGRAM capsule fallback. Reads accept both representations concurrently.
// Create through http_tunnel::datagrams() or http_client_tunnel::datagrams();
// arbitrary Capsule Protocol streams cannot be promoted to this channel.
// Empty payloads are data; an empty optional is EOF. Use exclusively with the tunnel.
// Operations occupy separate read and send/finish lanes when started; unstarted
// operations may coexist. send() owns its payload before returning. `operation`
// follows http_capsule_stream.
class http_datagram_stream final {
public:
    http_datagram_stream(const http_datagram_stream&) = delete;
    http_datagram_stream& operator=(const http_datagram_stream&) = delete;
    http_datagram_stream(http_datagram_stream&&) noexcept = default;
    http_datagram_stream& operator=(http_datagram_stream&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read(operation_options operation = {}) &;
    task<std::optional<http_datagram>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> send(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload, operation_options operation = {}) &;
    task<void> send(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> send(string_type&& payload, operation_options operation = {}) & {
        return send_owned(std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> send(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) & {
        return stream_.finish(std::move(operation));
    }
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    friend class http_udp_tunnel;
    explicit http_datagram_stream(http_capsule_stream&& stream);
    static task<std::optional<http_datagram>> read_owned(detail::capsule_state_pin pin, bool udp, operation_options operation);
    [[nodiscard]] task<void> send_payload(std::string_view payload, bool udp, operation_options operation);
    [[nodiscard]] task<void> send_owned(std::string&& payload, operation_options operation);
    [[nodiscard]] task<void> send_payload_owned(std::string&& payload, bool udp, operation_options operation);
    http_capsule_stream stream_;
};

// RFC 9297 HTTP Datagrams and reliable DATAGRAM capsules for a negotiated RFC 9298 CONNECT-UDP
// tunnel. Unknown capsule types and unknown context IDs are silently dropped;
// malformed DATAGRAM capsules terminate only this tunnel. Empty UDP packets are
// data; an empty optional is EOF. Owns its capsule stream and preserves separate
// read and send/finish lanes when moved.
// Operations occupy their lane when started; unstarted operations may coexist.
// send() owns its payload before returning. `operation` follows http_capsule_stream.
class http_udp_tunnel final {
public:
    http_udp_tunnel(const http_udp_tunnel&) = delete;
    http_udp_tunnel& operator=(const http_udp_tunnel&) = delete;
    http_udp_tunnel(http_udp_tunnel&&) noexcept = default;
    http_udp_tunnel& operator=(http_udp_tunnel&&) noexcept = default;
    [[nodiscard]] task<std::optional<http_datagram>> read(operation_options operation = {}) &;
    task<std::optional<http_datagram>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::span<const std::byte> payload, operation_options operation = {}) &;
    task<void> send(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> send(std::string_view payload, operation_options operation = {}) &;
    task<void> send(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> send(string_type&& payload, operation_options operation = {}) & {
        return send_owned(std::move(payload), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> send(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(operation_options operation = {}) & {
        return stream_.finish(std::move(operation));
    }
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept {
        stream_.abort();
    }
    void abort() && = delete;

private:
    friend class http_tunnel;
    friend class http_client_tunnel;
    [[nodiscard]] task<void> send_owned(std::string&& payload, operation_options operation);
    explicit http_udp_tunnel(http_datagram_stream stream) noexcept :
        stream_(std::move(stream)) {}
    http_datagram_stream stream_;
};
struct http_tunnel_route_config final {
    // Once the handler returns, bound flushing the local FIN and draining peer
    // input. A handler may half-close and continue reading without this deadline.
    timeout_duration peer_transport_fin_timeout_{5000};
    // Declares HTTP Datagram semantics for an upgrade-protocol
    // tunnel: a 2xx response carries Capsule-Protocol: ?1 (RFC 9297 section
    // 3.4) and the handler may use http_tunnel::datagrams(), udp() and
    // capsules(). HTTP/3 datagrams use native transport when negotiated and
    // within the path limit, otherwise capsules. CONNECT-UDP enables this
    // automatically; plain CONNECT rejects it.
    bool datagrams_{};
};

// An established CONNECT byte stream. The session owns this facade and its
// allocator. The tunnel retains the last received chunk until the next read
// starts; it counts against the connection's and the worker's inbound buffer
// budgets (server_config::max_inbound_buffer_bytes_per_connection_ /
// _per_worker_). A read that would exceed a budget throws std::bad_alloc.
class http_tunnel final {
public:
    http_tunnel(const http_tunnel&) = delete;
    http_tunnel& operator=(const http_tunnel&) = delete;

    // One running read and one running output operation are allowed.
    // An operation occupies its lane when started; unstarted operations may coexist.
    // A timeout or stop in a read's `operation` ends only that read with
    // std::system_error (timed_out or operation_canceled); no input is lost.
    // In a write or finish it aborts the tunnel (a partial write cannot be
    // withdrawn) and throws the same; a stop requested before the operation
    // starts sends nothing. A non-positive timeout throws std::invalid_argument.
    // The view borrows this tunnel until the next read starts; nullopt is EOF
    // and an empty chunk is never returned.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options operation = {}) & {
        return write_owned(std::move(bytes), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;

    // Half-close the send direction; reading remains available until peer EOF.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Borrow the established route tunnel for HTTP Datagrams. The route protocol
    // owns negotiation: these throw std::logic_error unless the route declared
    // http_tunnel_route_config::datagrams_. Use only in the negotiated datagram
    // handler, exclusively with this tunnel. Capsule Protocol payload limits
    // apply to both native datagrams and reliable DATAGRAM capsules.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &;
    http_datagram_stream datagrams(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_udp_tunnel udp(http_capsule_config config = {}) &;
    http_udp_tunnel udp(http_capsule_config = {}) && = delete;
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &;
    http_capsule_stream capsules(http_capsule_config = {}) && = delete;

private:
    template <typename stream_type>
    friend class detail::http_tunnel_session;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    using read_type = task<std::optional<std::string>> (*)(void*, operation_options);
    using write_type = task<void> (*)(void*, std::string_view, operation_options);
    using read_datagram_type = task<std::optional<detail::http_tunnel_input>> (*)(void*, operation_options);
    using write_datagram_type = task<bool> (*)(void*, std::string_view, operation_options);
    using finish_type = task<void> (*)(void*, operation_options);
    // malformed: the received tunnel data is malformed (RFC 9297 section 3.3).
    using abort_type = void (*)(void*, bool malformed) noexcept;
    http_tunnel(detail::inbound_buffer_account* inbound, const worker_ref& worker_value, void* target, read_type read, write_type write, finish_type finish_value, abort_type abort, bool datagrams, read_datagram_type read_datagram = nullptr, write_datagram_type write_datagram = nullptr) noexcept;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read_borrowed(operation_options operation);
    [[nodiscard]] task<std::optional<std::string>> read_raw(operation_options operation);
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation);
    [[nodiscard]] task<void> write_raw(std::string&& bytes, operation_options operation);
    [[nodiscard]] task<std::optional<detail::http_tunnel_input>> read_datagram_raw(operation_options operation);
    [[nodiscard]] task<bool> write_datagram_raw(std::string_view bytes, operation_options operation);
    void abort_input(bool malformed) noexcept;
    void require_active() const;
    void require_unadapted() const;
    detail::inbound_buffer_account* inbound_;
    const worker_ref& worker_;
    void* target_;
    read_type read_;
    write_type write_;
    finish_type finish_;
    abort_type abort_;
    read_datagram_type read_datagram_;
    write_datagram_type write_datagram_;
    // The chunk the last read() returned.
    std::optional<std::string> read_chunk_;
    bool datagrams_;
    bool adapted_{false};
    bool read_active_{false};
    bool write_active_{false};
    bool send_ended_{false};
    ::ruvia::operation_scope operations_;
};
struct websocket_heartbeat_config final {
    // Absence disables heartbeat. When ping is present and pong is absent,
    // pong defaults to the ping interval during configuration normalization.
    std::optional<timeout_duration> ping_interval_{};
    std::optional<timeout_duration> pong_timeout_{};
};

// Runtime-only liveness policy. Wire framing and close-handshake state remain in
// ruvia-http; timers and transport abort policy belong to the Web runtime.
struct websocket_lifecycle_options final {
    websocket_heartbeat_config heartbeat_{};
    // A locally initiated Close waits for the peer Close before the underlying
    // transport is ended. The timeout starts when the Close is submitted, so it
    // also bounds writing the Close frame. nullopt disables this guard.
    std::optional<timeout_duration> close_handshake_timeout_{std::chrono::seconds(5)};
};

struct websocket_route_config final {
    // Server preference order. Every entry must be a nonempty, unique HTTP token.
    std::vector<std::string> subprotocols_{};
    // By default a client offering none of subprotocols_ is still accepted,
    // with no Sec-WebSocket-Protocol response field (RFC 6455 4.2.2). When
    // set, such an upgrade is refused with 400 before any middleware or
    // handler runs; subprotocols_ must then be nonempty.
    bool require_subprotocol_{false};
    websocket_lifecycle_options lifecycle_{};
    websocket_deflate_config deflate_{};
    // 单会话接收消息上限（含解压后）；unlimited 继承服务器上限，只能收紧。
    protocol_byte_limit max_message_bytes_{};
};

struct websocket_send_options final {
    // False bypasses compression and never adds payload bytes to its dictionary.
    bool compress_{true};
};

struct websocket_close_options final {
    websocket_close_code code_{websocket_close_status::normal_closure};
    ::ruvia::borrowed_text reason_{};
};

class websocket;

namespace detail {
template <typename connection_type>
[[nodiscard]] websocket make_websocket_facade(connection_type& connection);
struct websocket_accept_access;
class websocket_state;
// Whether a write submits a whole message or one fragment of one.
enum class websocket_write_extent : std::uint8_t {
    message,
    fragment,
    final_fragment,
};
}  // namespace detail

class websocket_message_writer;

// A handshake the server refused. The server sends the selected response,
// with this status, after the request callback returns.
struct websocket_rejection final {
    http_status_code status_;
    // The failed handshake check, as in the error response's code:
    // invalid_websocket_handshake, unsupported_websocket_version or
    // unsupported_websocket_subprotocol.
    std::string_view code_;
};

// Outcome of accepting a WebSocket upgrade, fixed at construction: the
// established session, borrowed until the request callback ends, or the
// rejection.
class websocket_accept_result final {
public:
    websocket_accept_result(const websocket_accept_result&) = delete;
    websocket_accept_result& operator=(const websocket_accept_result&) = delete;
    websocket_accept_result(websocket_accept_result&&) noexcept = default;
    websocket_accept_result& operator=(websocket_accept_result&&) = delete;

    // nullptr when the handshake was rejected.
    [[nodiscard]] websocket* session() const noexcept {
        const auto* accepted = std::get_if<websocket*>(&value_);
        return accepted != nullptr ? *accepted : nullptr;
    }
    // nullptr when the session was established.
    [[nodiscard]] const websocket_rejection* rejection() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<websocket_rejection>(&value_);
    }
    const websocket_rejection* rejection() const&& = delete;

private:
    friend struct detail::websocket_accept_access;
    explicit websocket_accept_result(websocket& session) noexcept :
        value_(&session) {}
    explicit websocket_accept_result(websocket_rejection rejection) noexcept :
        value_(rejection) {}
    std::variant<websocket*, websocket_rejection> value_;
};

// One worker-affine session API for both roles. Client handshakes return an owning
// handle; server acceptance borrows the request lifetime. Moving preserves writers
// and cold/running operations, but never extends a server request lifetime.
class websocket final {
public:
    websocket(const websocket&) = delete;
    websocket& operator=(const websocket&) = delete;
    websocket(websocket&&) noexcept;
    websocket& operator=(websocket&&) noexcept;
    ~websocket();

    /// Only one read operation may run at a time. Starting another before the
    /// current operation completes throws std::logic_error. Unstarted operations may coexist.
    /// One reader may overlap one writer. A reader waiting behind an in-flight
    /// message polls control frames at message boundaries before the writer's
    /// next operation; the current complete message is never interrupted.
    /// nullopt is the only end: the peer's or this side's completed close
    /// handshake, EOF, a rejected frame, abort() or a heartbeat/close timeout;
    /// termination() tells which. A stop request or timeout in `operation`
    /// ends only this read with std::system_error (operation_canceled or
    /// timed_out); buffered input, the read lane and the session stay usable.
    [[nodiscard]] task<std::optional<websocket_message>> read(operation_options operation = {}) &;
    task<std::optional<websocket_message>> read(operation_options = {}) && = delete;

    /// The string_view overloads copy payloads into owned storage before
    /// returning. std::string&& inputs transfer their existing allocation.
    /// Output operations share one write lane: starting a second before the
    /// current operation completes throws std::logic_error. A stop request or
    /// timeout in `operation` aborts the session (a partly written frame
    /// cannot be withdrawn) and throws std::system_error (operation_canceled
    /// or timed_out); an already-stopped token fails before any output.
    [[nodiscard]] task<void> text(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> text(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy text frame: takes ownership of an already-allocated payload.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> text(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::text, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> text(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::string_view payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::string_view, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> binary(std::span<const std::byte> payload, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> binary(std::span<const std::byte>, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Zero-copy binary frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> binary(string_type&& payload, websocket_send_options options = {}, operation_options operation = {}) & {
        return write_owned(websocket_opcode::binary, std::move(payload), options.compress_, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> binary(string_type&&, websocket_send_options = {}, operation_options = {}) && = delete;

    [[nodiscard]] task<void> pong(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> pong(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy pong frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> pong(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::pong, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> pong(string_type&&, operation_options = {}) && = delete;

    [[nodiscard]] task<void> ping(std::string_view payload = {}, operation_options operation = {}) &;
    task<void> ping(std::string_view = {}, operation_options = {}) && = delete;

    /// Zero-copy ping frame.
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> ping(string_type&& payload, operation_options operation = {}) & {
        return write_owned(websocket_opcode::ping, std::move(payload), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> ping(string_type&&, operation_options = {}) && = delete;

    /// Zero-copy send of a message returned by this connection's read(), with
    /// the message's own opcode (for example an echo). The frame borrows the
    /// payload from the connection's read storage, so this call claims the
    /// read lane immediately and holds it until the operation completes or is
    /// destroyed: read() and close() throw std::logic_error meanwhile, and this
    /// call throws std::logic_error while a read is pending. A message built
    /// with websocket_message::borrow over caller storage must keep that
    /// storage alive and unchanged until the operation completes.
    [[nodiscard]] task<void> send(websocket_message&& message, websocket_send_options options = {}, operation_options operation = {}) &;
    task<void> send(websocket_message&&, websocket_send_options = {}, operation_options = {}) && = delete;

    /// Claims the write lane only, so it may start while a read is pending:
    /// that read returns nullopt once the peer answers (termination()
    /// local_close) or the close-handshake timeout aborts the transport
    /// (timeout). Without a pending read, close() itself reads until the peer
    /// Close and a read started meanwhile throws std::logic_error. A handshake
    /// timeout throws std::system_error (timed_out); `operation` bounds the
    /// whole close like an output operation.
    [[nodiscard]] task<void> close(websocket_close_options options = {}, operation_options operation = {}) &;
    task<void> close(websocket_close_options = {}, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

    /// Starts a data message sent as several frames; see websocket_message_writer.
    /// Throws std::logic_error while another such message is open.
    [[nodiscard]] websocket_message_writer begin_text(websocket_send_options options = {}) &;
    websocket_message_writer begin_text(websocket_send_options = {}) && = delete;
    [[nodiscard]] websocket_message_writer begin_binary(websocket_send_options options = {}) &;
    websocket_message_writer begin_binary(websocket_send_options = {}) && = delete;

    [[nodiscard]] std::string_view subprotocol() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view subprotocol() const&& = delete;

    [[nodiscard]] std::optional<std::uint16_t> peer_close_code() const noexcept;
    [[nodiscard]] std::string_view peer_close_reason() const& noexcept RUVIA_LIFETIMEBOUND;
    std::string_view peer_close_reason() const&& = delete;

    /// Why the session ended; none while it is active. The first cause wins
    /// and stays readable once read() has returned nullopt.
    [[nodiscard]] websocket_termination termination() const noexcept;
    /// RFC 6455 frame bytes on the wire (header, mask key and payload, after
    /// permessage-deflate) of data and control frames; the HTTP upgrade and
    /// the HTTP/2 or HTTP/3 framing that carries the frames are excluded.
    [[nodiscard]] std::uint64_t bytes_received() const noexcept;
    [[nodiscard]] std::uint64_t bytes_sent() const noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const worker_ref& worker() const& RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    friend class http_client_websocket_request;
    template <typename connection_type>
    friend websocket detail::make_websocket_facade(connection_type&);
    explicit websocket(std::shared_ptr<detail::websocket_state> state) noexcept;
    [[nodiscard]] task<void> write_owned(websocket_opcode, std::string&&, bool, operation_options);
    std::shared_ptr<detail::websocket_state> state_;
};

/// A data message sent as several frames (RFC 6455 section 5.4), so that a
/// caller can pace a large message, for example under a shared bandwidth
/// budget. Each write() sends one non-final frame and completes once the
/// transport accepted it; finish() sends the final frame. Fragments of a
/// compressed message continue one permessage-deflate stream. Until finish()
/// completes the message owns the data lane: text(), binary(), send() and
/// another begin_*() throw std::logic_error, while ping(), pong(), close() and
/// automatic control frames may go out between fragments. Fragment operations
/// share the write lane with them and accept the same operation_options.
/// Destroying or abort()ing an unfinished message aborts the session, since
/// the peer could not tell where the message ends. The writer borrows the
/// websocket and expires with it.
class websocket_message_writer final {
public:
    websocket_message_writer(websocket_message_writer&& other) noexcept;
    websocket_message_writer& operator=(websocket_message_writer&&) = delete;
    websocket_message_writer(const websocket_message_writer&) = delete;
    websocket_message_writer& operator=(const websocket_message_writer&) = delete;
    ~websocket_message_writer();

    [[nodiscard]] task<void> write(std::string_view chunk, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> chunk, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& chunk, operation_options operation = {}) & {
        return submit_owned(std::move(chunk), false, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::string_view last_chunk = {}, operation_options operation = {}) &;
    task<void> finish(std::string_view = {}, operation_options = {}) && = delete;
    [[nodiscard]] task<void> finish(std::span<const std::byte> last_chunk, operation_options operation = {}) &;
    task<void> finish(std::span<const std::byte>, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> finish(string_type&& last_chunk, operation_options operation = {}) & {
        return submit_owned(std::move(last_chunk), true, std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> finish(string_type&&, operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;

private:
    friend class detail::websocket_state;
    websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept;
    [[nodiscard]] task<void> submit(std::string_view chunk, bool final, operation_options operation);
    [[nodiscard]] task<void> submit_owned(std::string&& chunk, bool final, operation_options operation);
    [[nodiscard]] bool message_open() const noexcept;
    static void expire_capability(void* target) noexcept;

    detail::websocket_state* owner_;
    std::uint64_t message_id_;
    websocket_opcode opcode_;
    bool compress_;
    scoped_capability_registration registration_;
};

}  // namespace ruvia
