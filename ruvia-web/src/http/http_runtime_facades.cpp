#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/core/bytes.h"
#include "ruvia/core/task.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"

#include "context/context_capabilities.h"
#include "util/operation_lane_lease.h"

namespace {

ruvia::task<void> write_transferred_chunk(void* target,
    ruvia::task<void> (*write)(void*, std::string_view), std::pmr::string chunk,
    bool& output_active) {
    auto lease = ruvia::detail::claim_operation_lane(output_active, "response stream output operation is already in progress");
    co_await write(target, chunk);
}

struct owned_trailers final {
    explicit owned_trailers(
        std::span<const ruvia::http_header_view> source_value, std::pmr::memory_resource* resource)
        : fields_(resource),
          views_(resource) {
        fields_.reserve(source_value.size());
        views_.reserve(source_value.size());
        for (const auto& header : source_value) {
            const auto& field = fields_.emplace_back(ruvia::http_header::copy_of(header.name(), header.value(), resource));
            views_.emplace_back(field.name(), field.value());
        }
    }

    std::pmr::vector<ruvia::http_header> fields_;
    std::pmr::vector<ruvia::http_header_view> views_;
};

ruvia::task<void> end_owned(void* target,
    ruvia::task<void> (*end)(void*, std::span<const ruvia::http_header_view>), owned_trailers trailers,
    bool& output_active) {
    auto lease = ruvia::detail::claim_operation_lane(output_active, "response stream output operation is already in progress");
    co_await end(target, trailers.views_);
}

void require_websocket_worker(void* target) noexcept {
    if (!static_cast<const ruvia::worker_handle*>(target)->is_current()) {
        std::terminate();
    }
}

ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void* target,
    ruvia::task<std::optional<ruvia::websocket_message>> (*read)(void*),
    bool& read_active) {
    auto lease = ruvia::detail::claim_operation_lane(read_active, "concurrent websocket reads are not supported");
    co_return co_await read(target);
}

ruvia::task<void> write_websocket_payload(void* target,
    ruvia::task<void> (*write)(void*, ruvia::websocket_opcode, std::string_view, bool),
    ruvia::websocket_opcode opcode, std::pmr::string payload_value, bool& write_active, bool compress) {
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, opcode, payload_value, compress);
}

// The read lease is claimed by the caller before this task exists: the payload
// borrows the connection's read storage, and no read may run, even one started
// while this operation is still unstarted, until the frame has been written.
ruvia::task<void> write_websocket_message(void* target,
    ruvia::task<void> (*write)(void*, ruvia::websocket_opcode, std::string_view, bool),
    ruvia::websocket_message message, ruvia::detail::operation_lane_lease read_lease, bool& write_active,
    bool compress) {
    auto held_read = std::move(read_lease);
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, message.opcode(), message.payload(), compress);
}

ruvia::task<void> close_websocket_with_reason(void* target,
    ruvia::task<void> (*close)(void*, ruvia::websocket_close_options),
    ruvia::websocket_close_options options, std::pmr::string reason,
    bool& read_active, bool& write_active, bool& close_active) {
    auto read_lease = ruvia::detail::claim_operation_lane(read_active, "websocket close cannot overlap a read");
    auto write_lease = ruvia::detail::claim_operation_lane(write_active, "websocket close cannot overlap an output operation");
    auto close_lease = ruvia::detail::claim_operation_lane(close_active, "websocket close is already in progress");
    options.reason_ = reason;
    co_await close(target, options);
}

}  // namespace

namespace ruvia {

sse_writer::sse_writer(const sse_writer& other) noexcept
    : writer_(other.writer_),
      registration_(other.registration_, this) {}

sse_writer::sse_writer(sse_writer&& other) noexcept
    : writer_(std::exchange(other.writer_, nullptr)),
      registration_(std::move(other.registration_), this) {}

sse_writer::sse_writer(response_stream_writer& writer) noexcept
    : writer_(writer.operation_scope_.active() ? &writer : nullptr),
      registration_(writer.operation_scope_, this, &sse_writer::expire_capability) {}

response_stream_writer& sse_writer::writer() const {
    registration_.require_active();
    return *writer_;
}

void sse_writer::expire_capability(void* target) noexcept {
    static_cast<sse_writer*>(target)->writer_ = nullptr;
}

http_tunnel& context::tunnel() const {
    const auto* output = response_output().tunnel();
    if (output == nullptr) {
        throw std::logic_error("HTTP tunnel is available only in an established CONNECT route");
    }
    return output->tunnel();
}

websocket& context::get_websocket() const {
    const auto* output = response_output().get_websocket();
    if (output == nullptr) {
        throw std::logic_error("websocket is not available");
    }
    return output->get_websocket();
}

response_stream_writer& context::stream() {
    const auto* output = response_output().response_stream();
    if (output == nullptr) {
        throw std::logic_error("response body is not streamable");
    }
    return output->writer();
}

response_stream_writer& context::stream_text() {
    set_stable_response_header("Content-Type", "text/plain; charset=UTF-8");
    set_stable_response_header("X-Content-Type-Options", "nosniff");
    return stream();
}

sse_writer context::stream_sse() {
    set_stable_response_header("Content-Type", "text/event-stream");
    set_stable_response_header("Cache-Control", "no-cache");
    return sse_writer(stream());
}

namespace {

template <typename view_type>
task<std::optional<view_type>> read_body(
    detail::callback_ref_type<task<std::optional<std::span<const std::byte>>>()> read, bool& read_active) {
    auto lease = detail::claim_operation_lane(read_active, "request body read is already in progress");
    const auto chunk = co_await read();
    if (!chunk) {
        co_return std::nullopt;
    }
    if constexpr (std::same_as<view_type, std::string_view>) {
        co_return as_chars(*chunk);
    } else {
        co_return *chunk;
    }
}

}  // namespace

scoped_operation<std::optional<std::span<const std::byte>>> body_reader::read() & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::span<const std::byte>>(read_, read_active_));
}

scoped_operation<std::optional<std::string_view>> body_reader::text() & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::string_view>(read_, read_active_));
}

scoped_operation<void> response_stream_writer::write(std::span<const std::byte> chunk) & {
    return write(as_chars(chunk));
}

scoped_operation<void> response_stream_writer::write(std::string_view chunk) & {
    require_active();
    std::pmr::string owned(chunk, resource_);
    return write(std::move(owned));
}

scoped_operation<void> response_stream_writer::write(std::pmr::string&& chunk) & {
    require_active();
    std::pmr::string owned(std::move(chunk), resource_);
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_transferred_chunk(target_, write_, std::move(owned), output_active_));
}

scoped_operation<void> response_stream_writer::writeln(std::string_view chunk) & {
    require_active();
    std::pmr::string owned(chunk, resource_);
    owned.push_back('\n');
    return write(std::move(owned));
}

scoped_operation<timer_sleep_result> response_stream_writer::sleep(
    std::chrono::milliseconds duration) & {
    require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, sleep_(target_, duration, stop_token_));
}

stop_token response_stream_writer::cancel_on_peer_eof() & {
    require_active();
    stop_token_ = combine_stop_tokens(stop_token_, cancel_on_peer_eof_(target_));
    return stop_token_;
}

scoped_operation<void> response_stream_writer::end(std::span<const http_header_view> trailers) & {
    require_active();
    auto owned_trailers_value = owned_trailers(trailers, resource_);
    return ::ruvia::make_scoped_operation(
        operation_scope_, end_owned(target_, end_, std::move(owned_trailers_value), output_active_));
}

scoped_operation<timer_sleep_result> sse_writer::sleep(std::chrono::milliseconds duration) {
    return writer().sleep(duration);
}

stop_token sse_writer::cancel_on_peer_eof() {
    return writer().cancel_on_peer_eof();
}

scoped_operation<void> sse_writer::end(std::span<const http_header_view> trailers) {
    return writer().end(trailers);
}

scoped_operation<std::optional<websocket_message>> websocket::read() & {
    require_active();
    return ::ruvia::make_scoped_operation(operation_scope_,
        read_websocket(target_, read_, read_active_), &require_websocket_worker,
        const_cast<worker_handle*>(worker_));
}

scoped_operation<void> websocket::text(std::string_view payload_value, websocket_send_options options) & {
    return write(websocket_opcode::text, payload_value, options.compress_);
}

scoped_operation<void> websocket::binary(std::string_view payload_value, websocket_send_options options) & {
    return write(websocket_opcode::binary, payload_value, options.compress_);
}

scoped_operation<void> websocket::pong(std::string_view payload_value) & {
    return write(websocket_opcode::pong, payload_value);
}

scoped_operation<void> websocket::ping(std::string_view payload_value) & {
    return write(websocket_opcode::ping, payload_value);
}

scoped_operation<void> websocket::text(std::pmr::string&& payload_value, websocket_send_options options) & {
    return write(websocket_opcode::text, std::move(payload_value), options.compress_);
}

scoped_operation<void> websocket::binary(std::pmr::string&& payload_value, websocket_send_options options) & {
    return write(websocket_opcode::binary, std::move(payload_value), options.compress_);
}

scoped_operation<void> websocket::pong(std::pmr::string&& payload_value) & {
    return write(websocket_opcode::pong, std::move(payload_value));
}

scoped_operation<void> websocket::ping(std::pmr::string&& payload_value) & {
    return write(websocket_opcode::ping, std::move(payload_value));
}

scoped_operation<void> websocket::send(websocket_message&& message, websocket_send_options options) & {
    require_active();
    auto read_lease = detail::claim_operation_lane(read_active_, "websocket message send cannot overlap a read");
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_websocket_message(target_, write_, message, std::move(read_lease), write_active_, options.compress_),
        &require_websocket_worker, const_cast<worker_handle*>(worker_));
}

scoped_operation<void> websocket::close(websocket_close_options options) & {
    require_active();
    std::pmr::string owned(options.reason_.view(), resource_);
    return ::ruvia::make_scoped_operation(operation_scope_,
        close_websocket_with_reason(target_, close_, options, std::move(owned),
            read_active_, write_active_, close_active_),
        &require_websocket_worker, const_cast<worker_handle*>(worker_));
}

void websocket::abort() noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return;
    }
    abort_(target_);
}

scoped_operation<void> websocket::write(websocket_opcode opcode, std::string_view payload_value, bool compress) {
    require_active();
    std::pmr::string owned(payload_value, resource_);
    return write(opcode, std::move(owned), compress);
}

scoped_operation<void> websocket::write(websocket_opcode opcode, std::pmr::string&& payload_value, bool compress) {
    require_active();
    std::pmr::string owned(std::move(payload_value), resource_);
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_websocket_payload(target_, write_, opcode, std::move(owned), write_active_, compress),
        &require_websocket_worker, const_cast<worker_handle*>(worker_));
}

scoped_operation<void> sse_writer::write(const sse_message& message) {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message, {.resource_ = stream_writer.resource_});
    return stream_writer.write(std::move(frame));
}

}  // namespace ruvia
