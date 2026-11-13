#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/model.h"
#include "ruvia/web/streaming.h"
namespace ruvia {

class context;
class context_response_editor;
class request_stream;
class response_stream_writer;
class http_client_request;
class http_client_connect_request;
class http_client_websocket_request;
class operation_deadline;

namespace detail {
class http_client_pool;
class http_client_registry;
class http_client_response_handoff;
class http_client_response_state;
class http_client_advertisement_queue;
struct http_client_advertisement_state;
class http_capsule_stream_state;
class http_client_result_budget_domain;
class http_client_result_budget_lease;
class http_client_state;
class http_client_websocket_transport;
class worker_client_registry_view;
}  // namespace detail

// Immutable metadata retained by its http_client_response. Field views borrow
// that response and remain valid through subsequent body reads.
// HTTP/1 transfers its already-owned parsed fields without another field copy.
// Other protocol resources are copied into response-owned storage.
class http_client_informational_response final {
public:
    http_client_informational_response(const http_client_informational_response&) = delete;
    http_client_informational_response& operator=(const http_client_informational_response&) = delete;
    http_client_informational_response(http_client_informational_response&&) noexcept = default;
    http_client_informational_response& operator=(http_client_informational_response&&) = delete;
    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }
    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;

private:
    friend class detail::http_client_response_state;
    explicit http_client_informational_response(http_status_code status) noexcept
        : status_(status) {}
    http_status_code status_;
    std::vector<http_header> headers_;
};

// Independently owned bytes collected from an outbound HTTP response body.
// The address-stable backing storage is detached from the client and worker
// lifecycle, and its read-only view remains valid across moves until destruction.
// Empty results retain neither backing storage nor a retained-byte reservation.
class http_client_response_bytes final {
public:
    http_client_response_bytes(const http_client_response_bytes&) = delete;
    http_client_response_bytes& operator=(const http_client_response_bytes&) = delete;
    http_client_response_bytes(http_client_response_bytes&&) noexcept;
    http_client_response_bytes& operator=(http_client_response_bytes&&) noexcept;
    ~http_client_response_bytes();

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] std::span<const std::byte> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return {data_.get(), size_};
    }
    std::span<const std::byte> bytes() const&& = delete;
    [[nodiscard]] std::string_view text() const& noexcept RUVIA_LIFETIMEBOUND {
        return {reinterpret_cast<const char*>(data_.get()), size_};
    }
    std::string_view text() const&& = delete;

private:
    friend class detail::http_client_response_state;

    http_client_response_bytes(
        std::size_t reserve_bytes, detail::http_client_result_budget_lease&& lease_value);
    http_client_response_bytes() noexcept = default;
    void append(std::span<const std::byte> bytes) noexcept;
    void release() noexcept;

    std::shared_ptr<detail::http_client_result_budget_domain> budget_domain_{};
    std::unique_ptr<std::byte[]> data_{};
    std::size_t size_{0};
    std::size_t reserved_{0};
};

// A borrow-only facade embedded in http_client_response. It cannot be detached,
// independently destroyed, or moved away from the response. Body operations
// bind directly to the response's address-stable transport state, so moving
// their public response owner does not invalidate a cold or running operation.
// HTTP/1 body-suppressed and known-zero-length responses do not allocate a
// framing-decoder scratch block; other response storage and budgets still apply.
// A body whose Content-Encoding stack is fully supported (gzip, deflate, br,
// zstd) is decoded before the first byte is exposed, and its Content-Encoding
// and encoded Content-Length fields are removed (RFC 9110 §8.4). A 206 body, or
// a stack containing an unknown coding, is exposed as received with its
// Content-Encoding field kept; a malformed or over-deep Content-Encoding field
// fails the response with protocol_error.
// decode_content(false) 保留编码字节与原 Content-Encoding/Content-Length，
// 不等正文结束即交付第一块；framing()/content_length() 始终表示线面计划。
// For HTTP/1, bytes received beyond a complete framed response retire the
// connection and are discarded; they do not turn that response into a failure.
//
// Every body operation takes its own operation_options, timed from when it
// starts; a non-positive timeout throws std::invalid_argument at the call. Its
// expiry or stop request ends only that wait with http_client_error
// (std::errc::timed_out / operation_canceled): buffered bytes and the read lane
// remain, so reading may resume.
// The request's own deadlines and host cancellation keep bounding the exchange.
class http_client_response_body final {
public:
    http_client_response_body(const http_client_response_body&) = delete;
    http_client_response_body& operator=(const http_client_response_body&) = delete;
    http_client_response_body(http_client_response_body&&) = delete;
    http_client_response_body& operator=(http_client_response_body&&) = delete;

    // The returned view remains valid until the next body operation starts. A null
    // optional is the only end-of-body signal; an empty data chunk is never
    // returned. Reads are linear and concurrent operations are rejected.
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;

    // Reads one chunk as characters, without decoding or charset validation.
    // Encoded characters may straddle chunks. Uses the same linear read lane.
    [[nodiscard]] task<std::optional<std::string_view>> text(operation_options operation = {}) &;
    task<std::optional<std::string_view>> text(operation_options = {}) && = delete;

    // Collects the unread remainder of this same stream. max_bytes is a caller
    // bound in addition to the origin's transport bound. An interrupted
    // collection keeps the bytes received so far for the next body operation.
    [[nodiscard]] task<http_client_response_bytes> read_all(
        std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) &;
    task<http_client_response_bytes> read_all(
        std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;

    // 以自然背压转发当前上游正文；调用者必须先对 output 执行 commit()，
    // 只有 body 结果才启动转发，completed 时不得消费/生产转发正文。
    // operation 约束整个转发；中断抛出 errc::timed_out / operation_canceled，已提交的下游
    // 输出按流式提交契约随该异常中止，上游正文游标停在最后一块成功写出处。
    [[nodiscard]] task<void> pipe_to(response_stream_writer& output, operation_options operation = {}) &;
    task<void> pipe_to(response_stream_writer&, operation_options = {}) && = delete;
    // 尚有未读正文或接收尚未终止时返回 false；耗尽后仅正常完成返回 true。
    // 失败时与 read() 一样抛出，包括客户端提前关闭；独立上传失败不影响
    // 已完整接收的响应。移动后不再持有状态的响应仍返回 true。
    [[nodiscard]] bool complete() const;

private:
    friend class http_client_response;

    ~http_client_response_body() = default;

    explicit http_client_response_body(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}

    detail::http_client_response_state* state_{nullptr};
};

// Owns worker-affine response storage independently of its client transport.
// May survive client shutdown/destruction on that worker, but must be destroyed
// before the bound event_loop retires. Metadata views borrow this response;
// body chunks remain valid until the next body operation starts.
class http_client_response final {
public:
    http_client_response(const http_client_response&) = delete;
    http_client_response& operator=(const http_client_response&) = delete;
    // Body operations bind to the address-stable response state, so the public
    // owner remains movable while an operation is cold or running.
    http_client_response(http_client_response&& other) noexcept;
    http_client_response& operator=(http_client_response&& other) noexcept;
    ~http_client_response();

    // Informational heads are retained in wire order without a count cap.
    // Aggregate field-byte and in-flight allocation budgets still apply.
    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& RUVIA_LIFETIMEBOUND;
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    // The status the response is processed as; a received 600..999 is
    // processed as 500 (RFC 9110 section 15).
    [[nodiscard]] http_status_code status() const;
    [[nodiscard]] bool ok() const {
        return status().value() >= 200 && status().value() < 300;
    }
    // The status code as received, including an invalid 600..999.
    [[nodiscard]] http_received_status received_status() const;
    [[nodiscard]] http_protocol_version protocol_version() const;
    [[nodiscard]] http_client_response_framing framing() const;
    [[nodiscard]] std::optional<std::uint64_t> content_length() const;
    [[nodiscard]] const http_client_connection_info& connection_info() const& RUVIA_LIFETIMEBOUND;
    const http_client_connection_info& connection_info() const&& = delete;
    // Queues RFC 9218 PRIORITY_UPDATE on a live HTTP/2 response; a
    // response the peer has already completed is left unchanged. Throws
    // http_client_error(closing) once the response transport is retired.
    // Worker-affine; the connection driver owns writing the queued frame.
    void reprioritize(http_priority priority) &;
    void reprioritize(http_priority) && = delete;
    [[nodiscard]] std::span<const http_header> headers() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::span<const http_header> trailers() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::span<const http_header> trailers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view name) const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> trailer(std::string_view) const&& = delete;
    [[nodiscard]] http_client_response_body& stream() & noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    http_client_response_body& stream() && = delete;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) & {
        return body_.read(std::move(operation));
    }
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<http_client_response_bytes> bytes(std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) & {
        return body_.read_all(max_bytes, std::move(operation));
    }
    task<http_client_response_bytes> bytes(std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;
    [[nodiscard]] task<http_client_response_bytes> text(std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) & {
        return body_.read_all(max_bytes, std::move(operation));
    }
    task<http_client_response_bytes> text(std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;
    template <typename value_type>
        requires detail::is_model_json_value<value_type>
    [[nodiscard]] task<value_type> json(std::size_t max_bytes = default_max_buffered_body_bytes, operation_options operation = {}) & {
        return decode_json<value_type>(bytes(max_bytes, std::move(operation)));
    }
    template <typename value_type>
        requires detail::is_model_json_value<value_type>
    task<value_type> json(std::size_t = default_max_buffered_body_bytes, operation_options = {}) && = delete;
    // Forwards status, end-to-end fields, unread body and trailers. The cold
    // operation expires with either this response or the destination. Hop-by-hop
    // fields, those the Connection field names, and Content-Length are not
    // forwarded (RFC 9110 §7.6.1). operation bounds the whole forward, as
    // pipe_to(response_stream_writer&). Both destinations forward identically.
    [[nodiscard]] task<void> pipe_to(context& output, operation_options operation = {}) &;
    task<void> pipe_to(context&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> pipe_to(request_stream& output, operation_options operation = {}) &;
    task<void> pipe_to(request_stream&, operation_options = {}) && = delete;

private:
    // The one forwarding chain of both pipe_to() destinations.
    [[nodiscard]] task<void> pipe_to_destination(context_response_editor draft, response_stream_writer& writer,
        ::ruvia::operation_scope& destination, operation_options operation);
    template <typename value_type>
    static task<value_type> decode_json(task<http_client_response_bytes> input) {
        auto owned = co_await std::move(input);
        co_return from_json<value_type>(owned.text());
    }
    friend class detail::http_client_pool;
    friend class detail::http_client_websocket_transport;
    friend class http_client_exchange;
    friend class http_client_tunnel;
    friend class http_capsule_stream;
    friend class detail::http_capsule_stream_state;
    friend class detail::http_client_response_handoff;
    friend class http_client_push;
    friend class http_client_request_body_writer;
    friend class http_client_request;

    explicit http_client_response(detail::http_client_pool& pool);
    explicit http_client_response(detail::http_client_response_state* state_value) noexcept;
    void release() noexcept;

    detail::http_client_response_state* state_{nullptr};
    http_client_response_body body_{state_};
    bool consumer_{true};
};

// Owns an established CONNECT byte stream on its client worker. Metadata borrows
// this owner; a read view lasts until the next read starts. Inputs are copied before a
// cold write is returned. One running read and one running write/finish may coexist; cold operations do not reserve either lane. Moving the
// owner preserves operations. Destruction aborts both directions; shutdown joins
// transport drivers. finish() closes only the local sending direction and is
// idempotent after successful completion, including normal transport retirement.
// Each read/text/write/finish takes its own operation_options, timed from when
// it starts. A read's expiry or stop request ends only that read with
// http_client_error (errc::timed_out / operation_canceled) and keeps received bytes; a write's
// or finish's aborts the tunnel, since partly sent bytes cannot be withdrawn,
// and an already-stopped token fails before anything is queued.
class http_client_tunnel final {
public:
    http_client_tunnel(const http_client_tunnel&) = delete;
    http_client_tunnel& operator=(const http_client_tunnel&) = delete;
    http_client_tunnel(http_client_tunnel&& other) noexcept;
    http_client_tunnel& operator=(http_client_tunnel&& other) noexcept;
    ~http_client_tunnel();
    [[nodiscard]] http_status_code status() const {
        return response_.status();
    }
    [[nodiscard]] http_protocol_version protocol_version() const {
        return response_.protocol_version();
    }
    [[nodiscard]] http_client_response_framing framing() const {
        return response_.framing();
    }
    [[nodiscard]] std::optional<std::uint64_t> content_length() const {
        return response_.content_length();
    }
    [[nodiscard]] const http_client_connection_info& connection_info() const& RUVIA_LIFETIMEBOUND {
        return response_.connection_info();
    }
    const http_client_connection_info& connection_info() const&& = delete;
    [[nodiscard]] std::span<const http_header> headers() const& RUVIA_LIFETIMEBOUND {
        return response_.headers();
    }
    std::span<const http_header> headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& RUVIA_LIFETIMEBOUND {
        return response_.header(name);
    }
    std::optional<std::string_view> header(std::string_view) const&& = delete;
    [[nodiscard]] std::span<const http_client_informational_response> informational_responses() const& RUVIA_LIFETIMEBOUND {
        return response_.informational_responses();
    }
    std::span<const http_client_informational_response> informational_responses() const&& = delete;
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options operation = {}) &;
    task<std::optional<std::span<const std::byte>>> read(operation_options = {}) && = delete;
    [[nodiscard]] task<std::optional<std::string_view>> text(operation_options operation = {}) &;
    task<std::optional<std::string_view>> text(operation_options = {}) && = delete;
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
    // A successful finish stays idempotent after transport retirement.
    [[nodiscard]] task<void> finish(operation_options operation = {}) &;
    task<void> finish(operation_options = {}) && = delete;
    void abort() & noexcept;
    void abort() && = delete;
    // Transfers tunnel ownership to the Capsule Protocol adapter.
    [[nodiscard]] http_capsule_stream capsules(http_capsule_config config = {}) &&;
    http_capsule_stream capsules(http_capsule_config = {}) & = delete;
    // Checked ownership transfers for accepted, negotiated datagram/CONNECT-UDP
    // tunnels. Ordinary CONNECT throws std::logic_error without consuming this
    // owner. The Capsule Protocol limits apply directly to encoded datagrams.
    [[nodiscard]] http_datagram_stream datagrams(http_capsule_config config = {}) &&;
    http_datagram_stream datagrams(http_capsule_config = {}) & = delete;
    [[nodiscard]] http_udp_tunnel udp(http_capsule_config config = {}) &&;
    http_udp_tunnel udp(http_capsule_config = {}) & = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_client_websocket_transport;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    explicit http_client_tunnel(http_client_response response) noexcept;
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation = {});
    // abort() for received content that violates the tunnel's protocol (RFC 9297
    // section 3.3 capsule error): HTTP/2 resets the stream with PROTOCOL_ERROR.
    void abort_malformed() & noexcept;
    void release() noexcept;
    http_client_response response_;
};

// A rejected CONNECT keeps its ordinary HTTP status, headers and response body.
// Only a successful handshake exposes a tunnel.
class http_client_tunnel_result final {
public:
    http_client_tunnel_result(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result& operator=(const http_client_tunnel_result&) = delete;
    http_client_tunnel_result(http_client_tunnel_result&&) noexcept = default;
    http_client_tunnel_result& operator=(http_client_tunnel_result&&) noexcept = default;
    [[nodiscard]] http_client_tunnel* tunnel() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_tunnel>(&value_);
    }
    http_client_tunnel* tunnel() && = delete;
    [[nodiscard]] http_client_response* response() & noexcept RUVIA_LIFETIMEBOUND {
        return std::get_if<http_client_response>(&value_);
    }
    http_client_response* response() && = delete;

private:
    friend class detail::http_client_pool;
    friend class detail::http_capsule_stream_state;
    friend class http_capsule_stream;
    friend class http_datagram_stream;
    explicit http_client_tunnel_result(http_client_tunnel tunnel) noexcept
        : value_(std::move(tunnel)) {}
    explicit http_client_tunnel_result(http_client_response response) noexcept
        : value_(std::move(response)) {}
    std::variant<http_client_tunnel, http_client_response> value_;
};

// A worker-affine, independently owned observation. Its metadata survives
// client shutdown and later observations, and expires when this owner dies.
// Destroy before the event_loop retires. A slot identifies the pool position
// of the observing connection owner, not a persistent connection: positions
// are reused once their owner is reclaimed.
class http_client_advertisement final {
public:
    http_client_advertisement(http_client_advertisement&& other) noexcept;
    http_client_advertisement& operator=(http_client_advertisement&& other) noexcept;
    http_client_advertisement(const http_client_advertisement&) = delete;
    http_client_advertisement& operator=(const http_client_advertisement&) = delete;
    ~http_client_advertisement();

    [[nodiscard]] http_protocol_version protocol_version() const noexcept;
    [[nodiscard]] std::size_t connection_slot() const noexcept;
    [[nodiscard]] const http_origin_advertisement* origins() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_origin_advertisement* origins() const&& = delete;
    [[nodiscard]] const http_alternative_service_advertisement* alternative_service() const& noexcept RUVIA_LIFETIMEBOUND;
    const http_alternative_service_advertisement* alternative_service() const&& = delete;

private:
    friend class detail::http_client_advertisement_queue;
    explicit http_client_advertisement(detail::http_client_advertisement_state* state_value) noexcept
        : state_(state_value) {}
    detail::http_client_advertisement_state* state_{};
};

// An accepted promise owns its copied request and response storage on the
// client worker. Destruction cancels unfinished push. Like an ordinary response,
// it can survive client shutdown but must retire before the event_loop.
class http_client_push final {
public:
    http_client_push(const http_client_push&) = delete;
    http_client_push& operator=(const http_client_push&) = delete;
    http_client_push(http_client_push&&) noexcept;
    http_client_push& operator=(http_client_push&&) noexcept;
    ~http_client_push();
    [[nodiscard]] const http_push_request& request() const& RUVIA_LIFETIMEBOUND;
    const http_push_request& request() const&& = delete;
    // Transfers the response once, when the operation is started. Promise
    // metadata remains available through this push until it is destroyed.
    // An operation timeout or stop request ends only this wait for the head
    // with http_client_error (errc::timed_out / operation_canceled); a later response() retries.
    [[nodiscard]] task<http_client_response> response(operation_options operation = {}) &;
    task<http_client_response> response(operation_options = {}) && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_push(http_client_response response) noexcept;
    void release() noexcept;
    http_client_response response_;
};

// One upload lane embedded in its exchange. Each chunk/trailer input is owned
// before return; completing write() means transport accepted the entire chunk.
// Only one write/end operation may run at a time. Cold operations can coexist
// and claim the output lane when awaited. Chunk size is bounded by
// max_chunk_bytes_; split larger sources into sequential writes. An HTTP/1
// transport write failure does not discard a complete final response arriving
// from the peer: response() remains consumable, while the failed upload is
// reported independently and its connection is not reused.
// A write()/end() timeout or stop request in its operation_options aborts the
// exchange with http_client_error (errc::timed_out / operation_canceled), since a partly sent
// chunk cannot be withdrawn; an already-stopped token fails before queuing.
class http_client_request_body_writer final {
public:
    http_client_request_body_writer(const http_client_request_body_writer&) = delete;
    http_client_request_body_writer& operator=(const http_client_request_body_writer&) = delete;
    [[nodiscard]] task<void> write(std::span<const std::byte> bytes, operation_options operation = {}) &;
    task<void> write(std::span<const std::byte>, operation_options = {}) && = delete;
    [[nodiscard]] task<void> write(std::string_view bytes, operation_options operation = {}) &;
    task<void> write(std::string_view, operation_options = {}) && = delete;
    template <std::same_as<std::string> string_type>
    [[nodiscard]] task<void> write(string_type&& bytes, operation_options operation = {}) & {
        return write_owned(std::move(bytes), std::move(operation));
    }
    template <std::same_as<std::string> string_type>
    task<void> write(string_type&&, operation_options = {}) && = delete;
    [[nodiscard]] task<void> end(std::span<const http_header_view> trailers = {}, operation_options operation = {}) &;
    task<void> end(std::span<const http_header_view> = {}, operation_options = {}) && = delete;
    [[nodiscard]] bool complete() const noexcept;
    // Whether write() and end() may still start: false once the content ended
    // or end() started, after abandon(), and once the exchange stopped the
    // upload itself (an early final response that completes the exchange, or a
    // failure). A false result never turns a later response into a failure.
    [[nodiscard]] bool writable() const noexcept;
    // Stops sending without failing the exchange: the request content stays
    // incomplete and response() remains consumable. HTTP/1 closes the
    // connection after the response; HTTP/2 and HTTP/3 end only the sending
    // direction (RFC 9113 section 8.1, RFC 9114 section 4.1). A running write()
    // or end() completes with errc::operation_canceled; idempotent, no-op once complete().
    void abandon() & noexcept;
    void abandon() && = delete;

private:
    friend class http_client_exchange;
    friend class http_client_request;
    explicit http_client_request_body_writer(detail::http_client_response_state* state_value) noexcept
        : state_(state_value) {}
    ~http_client_request_body_writer() = default;
    [[nodiscard]] task<void> write_owned(std::string&& bytes, operation_options operation);
    detail::http_client_response_state* state_;
};

// Owns a streaming request and its eventual response on one worker. Moving
// preserves operations because their state is address stable. Destruction
// abandons unfinished upload; client shutdown wakes and joins its producer.
class http_client_exchange final {
public:
    http_client_exchange(const http_client_exchange&) = delete;
    http_client_exchange& operator=(const http_client_exchange&) = delete;
    http_client_exchange(http_client_exchange&& other) noexcept;
    http_client_exchange& operator=(http_client_exchange&& other) noexcept;
    ~http_client_exchange();
    [[nodiscard]] http_client_request_body_writer& body() & noexcept RUVIA_LIFETIMEBOUND {
        return body_;
    }
    http_client_request_body_writer& body() && = delete;
    // Waits for the final head. An operation timeout or stop request ends only
    // this wait with http_client_error (errc::timed_out / operation_canceled); a later
    // response() retries, and the upload lane is unaffected.
    [[nodiscard]] task<http_client_response> response(operation_options operation = {}) &;
    task<http_client_response> response(operation_options = {}) && = delete;

private:
    friend class detail::http_client_pool;
    explicit http_client_exchange(http_client_response response) noexcept;
    void release() noexcept;
    http_client_response response_;
    http_client_request_body_writer body_;
};

class http_client_ref final {
public:
    http_client_ref(const http_client_ref& other);
    http_client_ref& operator=(const http_client_ref&) = delete;

    // Inherits an empty token; a supplied state replaces user cancellation.
    // Timeout can only tighten. Accumulation requires a caller-owned stop_group.
    // Request-host cancellation remains independent and cannot be replaced.
    [[nodiscard]] http_client_ref with_options(operation_options options) const;
    [[nodiscard]] http_client_request request(std::string_view method, std::string_view path) const;
    [[nodiscard]] http_client_request get(std::string_view path) const;
    [[nodiscard]] http_client_request post(std::string_view path) const;
    [[nodiscard]] http_client_request put(std::string_view path) const;
    [[nodiscard]] http_client_request patch(std::string_view path) const;
    [[nodiscard]] http_client_request del(std::string_view path) const;
    [[nodiscard]] http_client_request head(std::string_view path) const;
    [[nodiscard]] http_client_request options(std::string_view path) const;
    [[nodiscard]] http_client_connect_request connect(std::string_view authority) const;
    [[nodiscard]] http_client_connect_request udp(std::string_view path) const;
    [[nodiscard]] http_client_websocket_request websocket(std::string_view path) const;
    // Migrates existing authenticated HTTP/3 sessions only, without reconnect or
    // replay. Cancellation and the tighter of operation/connect timeout cover
    // handshake confirmation, CID availability and path validation together.
    [[nodiscard]] task<void> migrate(std::string_view local_address, std::uint16_t port = 0) const;
    [[nodiscard]] http_client_stats stats() const;
    [[nodiscard]] std::vector<http_client_endpoint_address> dns_snapshot() const;
    void update_dns(std::span<const http_client_endpoint_address> addresses) const;
    [[nodiscard]] task<void> refresh_dns() const;
    // Non-destructive DNS resolution: resolves the client's dial host (or a
    // specified host) into a sorted, deduplicated IP address list without
    // retiring existing active connections or WebSockets.
    [[nodiscard]] task<std::vector<ip_address>> resolve_dns(
        operation_options options = {}) const;
    [[nodiscard]] task<std::vector<ip_address>> resolve_dns(
        std::string_view host, operation_options options = {}) const;
    [[nodiscard]] bool close_connection(std::uint64_t id) const;
    [[nodiscard]] std::optional<http_client_advertisement> next_advertisement() const;
    [[nodiscard]] std::optional<http_client_push> next_push() const;
    [[nodiscard]] std::string_view host() const& RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view host() const&& = delete;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] http_scheme scheme() const;
    [[nodiscard]] http_origin_view origin() const& RUVIA_LIFETIMEBOUND;
    http_origin_view origin() const&& = delete;

private:
    friend class http_client_request;
    friend class http_client_connect_request;
    friend class http_client_websocket_request;
    [[nodiscard]] task<http_client_response> dispatch(const http_client_request_view& request, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    [[nodiscard]] task<http_client_exchange> dispatch_upload(const http_client_request_view& head, http_client_upload_config upload, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    [[nodiscard]] task<http_client_tunnel_result> dispatch_connect(const http_client_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    [[nodiscard]] task<http_client_tunnel_result> dispatch_udp(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& options,
        const std::shared_ptr<http_client_informational_callback>& informational) const;
    friend class detail::http_client_registry;
    friend class detail::http_client_state;
    friend class context;
    friend class web_worker_context;
    friend class detail::worker_client_registry_view;
    http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope) noexcept;
    http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope, operation_options options) noexcept;
    [[nodiscard]] http_client_ref bind_caller_scope(
        ::ruvia::operation_scope& scope, const stop_token& host_stop) && noexcept;
    static void expire_capability(void* target) noexcept;

    detail::http_client_pool* pool_{nullptr};
    std::shared_ptr<detail::http_client_pool> pool_owner_;
    operation_options options_;
    stop_token host_stop_token_;
    scoped_capability_registration registration_;
    const detail::http_client_registry* registry_{};
};

// Owns request inputs before any cold operation is returned. Each send/upload
// consumes this builder; response bodies are one linear stream, not replayable.
class http_client_request final {
public:
    http_client_request(const http_client_request&) = delete;
    http_client_request& operator=(const http_client_request&) = delete;
    http_client_request(http_client_request&&) = default;
    http_client_request& operator=(http_client_request&&) = delete;
    http_client_request& header(std::string_view name, std::string_view value);
    http_client_request& headers(std::span<const http_header_view> fields);
    http_client_request& query(std::string_view name, std::string_view value);
    http_client_request& body(std::string_view bytes);
    http_client_request& body(std::span<const std::byte> bytes);
    // Replaces every per-request option, including the informational callback.
    http_client_request& configure(http_client_request_options options);
    // Per-hop redirect observation and control callback invoked on every
    // redirect hop (same-origin and cross-origin alike) before following it.
    // Returning false stops following and returns the redirect response.
    http_client_request& on_redirect(http_client_redirect_observer observer);
    http_client_request& on_redirect(http_client_async_redirect_observer observer);
    template <typename value_type>
        requires detail::is_model_json_value<value_type>
    http_client_request& json(const value_type& value) {
        if (consumed_) {
            throw std::logic_error("HTTP request builder was consumed");
        }
        auto encoded = to_json(value);
        header("Content-Type", "application/json");
        body_.swap(encoded);
        has_body_ = true;
        return *this;
    }
    http_client_request& redirects(std::size_t maximum);
    [[nodiscard]] task<http_client_response> send();
    [[nodiscard]] task<http_client_exchange> upload(http_client_upload_config config = {});
    [[nodiscard]] task<http_client_tunnel_result> upgrade(http_client_tunnel_config config = {});

private:
    friend class http_client_ref;
    friend class detail::http_client_response_handoff;
    http_client_request(const http_client_ref& client, std::string_view method, std::string_view path);
    static task<http_client_response> send_owned(http_client_request request);
    static task<http_client_exchange> upload_owned(
        http_client_request request, http_client_upload_config config);
    static task<http_client_response> follow_redirects(http_client_request request,
        std::optional<http_client_response> initial_response, const operation_deadline& total,
        bool upload_initial_hop);
    [[nodiscard]] std::vector<http_header_view> field_views() const;
    http_client_ref client_;
    std::string method_;
    std::string target_;
    std::vector<http_header> headers_;
    std::string body_;
    std::size_t max_redirects_{5};
    // informational_ is moved out into informational_callback_, which every
    // redirect hop's exchange shares.
    http_client_request_options request_options_;
    std::shared_ptr<http_client_informational_callback> informational_callback_;
    http_client_redirect_observer redirect_observer_;
    http_client_async_redirect_observer async_redirect_observer_;
    bool has_body_{false};
    bool consumed_{false};
};

class http_client_connect_request final {
public:
    http_client_connect_request(const http_client_connect_request&) = delete;
    http_client_connect_request& operator=(const http_client_connect_request&) = delete;
    http_client_connect_request(http_client_connect_request&&) = default;
    http_client_connect_request& operator=(http_client_connect_request&&) = delete;
    http_client_connect_request& header(std::string_view name, std::string_view value);
    http_client_connect_request& headers(std::span<const http_header_view> fields);
    http_client_connect_request& configure(http_client_request_options options);
    http_client_connect_request& protocol(std::string_view value);
    http_client_connect_request& target(std::string_view value);
    [[nodiscard]] task<http_client_tunnel_result> open(http_client_tunnel_config config = {});

private:
    friend class http_client_ref;
    http_client_connect_request(const http_client_ref& client, std::string_view authority, std::string_view target, bool udp);
    http_client_ref client_;
    std::string authority_;
    std::string protocol_;
    std::string target_;
    std::vector<http_header> headers_;
    http_client_request_options request_options_;
    std::shared_ptr<http_client_informational_callback> informational_callback_;
    bool udp_;
    bool consumed_{false};
};

// 绑定一个 Ruvia event loop 的出站 origin 集合，可在所属 worker 动态更新。
// 构造不创建线程；send() 按需建连。
class http_client final {
public:
    http_client(event_loop loop, const http_client_config& config,
        const http_client_worker_config& worker = {});
    http_client(event_loop loop, std::span<const http_client_config> configs,
        const http_client_worker_config& worker = {});
    ~http_client();

    http_client(const http_client&) = delete;
    http_client& operator=(const http_client&) = delete;
    http_client(http_client&&) = delete;
    http_client& operator=(http_client&&) = delete;

    // Handles and operations borrow this client's open lifecycle. Starting one
    // from a temporary would immediately run the client destructor and begin
    // shutdown, so only lvalue clients may create them. Like context::client(),
    // a handle carries no operation options; derive them with with_options().
    // client() selects the sole configuration, or alias "default" in a set.
    // 默认重定向仅可到达已注册 origin；请求策略可显式扩充集合。
    [[nodiscard]] http_client_ref client() const&;
    http_client_ref client() const&& = delete;
    [[nodiscard]] http_client_ref client(std::string_view alias) const&;
    http_client_ref client(std::string_view) const&& = delete;
    // Worker-local 发布；调用时拥有配置，await 在 blocking_pool 准备 TLS 后原子替换。
    // 无 TLS 文件准备时无需 blocking_pool；取消或准备失败不改变既有 alias。
    // 旧池取消并 join，最后一个旧句柄/冷操作释放后回收；其他 alias 不变。
    [[nodiscard]] task<void> upsert(const http_client_config& config, operation_options options = {}) &;
    task<void> upsert(const http_client_config&, operation_options = {}) && = delete;
    [[nodiscard]] bool remove(std::string_view alias) &;
    bool remove(std::string_view) && = delete;

    // Idempotent and callable from any thread. It only requests immediate
    // shutdown; use shutdown() when the worker teardown must be awaited.
    void close() noexcept;
    // Cancels and joins client producers/send operations, then retires their
    // transport borrows on the bound loop. Response body consumers are owned by
    // the response, not this shutdown: finish/join them and destroy responses
    // on the owning worker before the event_loop retires.
    [[nodiscard]] task<void> shutdown() &;
    task<void> shutdown() && = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;

private:
    std::shared_ptr<detail::http_client_state> state_;
};

// Standalone non-destructive asynchronous DNS resolution bound to a worker or
// event loop. Returns a sorted, deduplicated list of numeric IP addresses.
[[nodiscard]] task<std::vector<ip_address>> resolve_dns(
    const worker_ref& worker, std::string_view host, operation_options options = {});
[[nodiscard]] task<std::vector<ip_address>> resolve_dns(
    event_loop loop, std::string_view host, operation_options options = {});

}  // namespace ruvia

namespace ruvia {

// Every input is owned before connect() returns its cold operation. The builder
// is consumed once; headers remain application fields, never handshake fields.
class http_client_websocket_request final {
public:
    http_client_websocket_request(const http_client_websocket_request&) = delete;
    http_client_websocket_request& operator=(const http_client_websocket_request&) = delete;
    http_client_websocket_request(http_client_websocket_request&&) = default;
    http_client_websocket_request& operator=(http_client_websocket_request&&) = delete;
    http_client_websocket_request& headers(std::span<const http_header_view> fields);
    http_client_websocket_request& header(std::string_view name, std::string_view value);
    http_client_websocket_request& configure(http_client_request_options options);
    http_client_websocket_request& subprotocol(std::string_view value);
    http_client_websocket_request& deflate(websocket_client_deflate_offer offer);
    // Pong expiry runs only while receive can observe it; blocked writes retain
    // their independent liveness bound. An absent ping interval disables probing.
    http_client_websocket_request& lifecycle(websocket_lifecycle_options options);
    // Applies to incoming messages and submitted application frames; zero is invalid.
    http_client_websocket_request& max_message_bytes(std::size_t limit);
    // Connect and request deadlines bound opening only, not the session lifetime.
    [[nodiscard]] task<websocket> connect();

private:
    friend class http_client_ref;
    http_client_websocket_request(const http_client_ref& client, std::string_view path);
    [[nodiscard]] static task<websocket> connect_owned(http_client_websocket_request request);
    void require_open() const;
    http_client_ref client_;
    std::string path_;
    std::vector<http_header> headers_;
    std::vector<std::string> subprotocols_;
    websocket_client_deflate_offer deflate_{};
    websocket_lifecycle_options lifecycle_{};
    std::size_t max_message_bytes_{default_max_websocket_message_bytes};
    http_client_request_options request_options_;
    bool consumed_{false};
};

}  // namespace ruvia
