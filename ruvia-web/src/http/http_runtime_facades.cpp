#include <array>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/bytes.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/task.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/multipart_reader.h"
#include "ruvia/web/streaming.h"
#include "ruvia/web/websocket.h"

#include "context/context_capabilities.h"
#include "http/inline_http_header_views.h"
#include "util/operation_lane_lease.h"
#include "util/pmr_owned_storage.h"

namespace {

ruvia::task<ruvia::stream_commit_result> commit_stream(void* target,
    ruvia::task<ruvia::stream_commit_result> (*commit)(void*), bool& output_active) {
    auto lease = ruvia::detail::claim_operation_lane(output_active, "response stream output operation is already in progress");
    co_return co_await commit(target);
}

ruvia::task<void> write_transferred_chunk(void* target,
    ruvia::task<void> (*write)(void*, std::string_view), std::pmr::string chunk,
    bool& output_active) {
    auto lease = ruvia::detail::claim_operation_lane(output_active, "response stream output operation is already in progress");
    co_await write(target, chunk);
}

ruvia::task<void> end_owned(void* target,
    ruvia::task<void> (*end)(void*, std::span<const ruvia::http_header_view>),
    std::pmr::vector<ruvia::http_header> fields,
    bool& output_active) {
    auto lease = ruvia::detail::claim_operation_lane(output_active, "response stream output operation is already in progress");
    const ruvia::detail::inline_http_header_views views(fields, fields.get_allocator().resource());
    co_await end(target, views.view());
}

ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void* target,
    ruvia::task<std::optional<ruvia::websocket_message>> (*read)(void*, ruvia::operation_options),
    bool& read_active, ruvia::operation_options operation) {
    auto lease = ruvia::detail::claim_operation_lane(read_active, "concurrent websocket reads are not supported");
    co_return co_await read(target, std::move(operation));
}

ruvia::task<void> write_websocket_payload(void* target,
    ruvia::task<void> (*write)(void*, ruvia::websocket_opcode, std::string_view, bool, ruvia::operation_options),
    ruvia::websocket_opcode opcode, std::pmr::string payload_value, bool& write_active, bool compress,
    ruvia::operation_options operation) {
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, opcode, payload_value, compress, std::move(operation));
}

// The read leases are claimed by the caller before this task exists: the
// payload borrows the connection's read storage, and no read may run, even one
// started while this operation is still unstarted, until the frame has been written.
ruvia::task<void> write_websocket_message(void* target,
    ruvia::task<void> (*write)(void*, ruvia::websocket_opcode, std::string_view, bool, ruvia::operation_options),
    ruvia::websocket_message message, ruvia::detail::operation_lane_lease read_lease,
    ruvia::detail::operation_lane_lease message_lease, bool& write_active, bool compress,
    ruvia::operation_options operation) {
    auto held_read = std::move(read_lease);
    auto held_message = std::move(message_lease);
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, message.opcode(), message.payload(), compress, std::move(operation));
}

ruvia::task<void> close_websocket_with_reason(void* target,
    ruvia::task<void> (*close)(void*, ruvia::websocket_close_options, ruvia::operation_options),
    ruvia::websocket_close_options options, std::pmr::string reason, ruvia::operation_options operation,
    bool& read_active, bool& write_active, const bool& message_send_active) {
    // The write lane also excludes another close.
    auto write_lease = ruvia::detail::claim_operation_lane(write_active, "websocket close cannot overlap an output operation");
    if (message_send_active) {
        throw std::logic_error("websocket close cannot overlap a message send");
    }
    // A pending read observes the peer Close itself; otherwise close reads,
    // and holds the read lane while it does.
    const ruvia::detail::operation_lane_lease read_lease(read_active);
    static_cast<void>(read_lease);
    options.reason_ = reason;
    co_await close(target, options, std::move(operation));
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

websocket& context::websocket() const {
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

sse_writer context::stream_sse() {
    auto& writer = stream();
    if (!writer.committed()) {
        set_stable_response_header("Content-Type", "text/event-stream");
        set_stable_response_header("Cache-Control", "no-cache");
    }
    return sse_writer(writer);
}

namespace {

template <typename view_type>
task<std::optional<view_type>> read_body(
    detail::callback_ref<task<std::optional<std::span<const std::byte>>>(stop_token)> read, bool& read_active, stop_token stop) {
    auto lease = detail::claim_operation_lane(read_active, "request body read is already in progress");
    const auto chunk = co_await read(std::move(stop));
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

task<std::optional<std::span<const std::byte>>> body_reader::read(stop_token stop) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::span<const std::byte>>(read_, read_active_, std::move(stop)));
}

task<std::optional<std::string_view>> body_reader::text(stop_token stop) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::string_view>(read_, read_active_, std::move(stop)));
}

task<stream_commit_result> response_stream_writer::commit() & {
    require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, commit_stream(target_, commit_, output_active_));
}

task<stream_commit_result> sse_writer::commit() {
    return writer().commit();
}

task<void> response_stream_writer::write(std::span<const std::byte> chunk) & {
    return write(as_chars(chunk));
}

task<void> response_stream_writer::write(std::string_view chunk) & {
    require_active();
    std::pmr::string owned(chunk, resource_);
    return write_owned(std::move(owned));
}

task<void> response_stream_writer::write_owned(std::pmr::string&& chunk) {
    require_active();
    auto owned = detail::rebind_owned_storage(chunk, resource_);
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_transferred_chunk(target_, write_, std::move(owned), output_active_));
}

task<void> response_stream_writer::writeln(std::string_view chunk) & {
    require_active();
    std::pmr::string owned(resource_);
    owned.reserve(chunk.size() + 1U);
    owned.append(chunk);
    owned.push_back('\n');
    return write_owned(std::move(owned));
}

task<timer_sleep_result> response_stream_writer::sleep(
    timeout_duration duration) & {
    require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, sleep_(target_, duration.value(), stop_token_));
}

task<void> response_stream_writer::end(std::span<const http_header_view> trailers) & {
    require_active();
    std::pmr::vector<http_header> owned_trailers_value(resource_);
    owned_trailers_value.reserve(trailers.size());
    for (const auto& header : trailers) {
        owned_trailers_value.emplace_back(header.name(), header.value(), resource_);
    }
    return ::ruvia::make_scoped_operation(
        operation_scope_, end_owned(target_, end_, std::move(owned_trailers_value), output_active_));
}

task<timer_sleep_result> sse_writer::sleep(timeout_duration duration) {
    return writer().sleep(duration);
}

task<void> sse_writer::end(std::span<const http_header_view> trailers) {
    return writer().end(trailers);
}

task<std::optional<websocket_message>> websocket::read(operation_options operation) & {
    require_active();
    validate_operation_options(operation);
    return ::ruvia::make_scoped_operation(operation_scope_, read_websocket(target_, read_, read_active_, std::move(operation)));
}

task<void> websocket::text(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::text, payload_value, options.compress_, std::move(operation));
}

task<void> websocket::binary(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::binary, payload_value, options.compress_, std::move(operation));
}

task<void> websocket::binary(std::span<const std::byte> payload_value, websocket_send_options options, operation_options operation) & {
    return binary(as_chars(payload_value), options, std::move(operation));
}

task<void> websocket::pong(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::pong, payload_value, true, std::move(operation));
}

task<void> websocket::ping(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::ping, payload_value, true, std::move(operation));
}

task<void> websocket::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    require_active();
    validate_operation_options(operation);
    auto read_lease = detail::claim_operation_lane(read_active_, "websocket message send cannot overlap a read");
    auto message_lease = detail::claim_operation_lane(message_send_active_, "websocket message send is already pending");
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_websocket_message(target_, write_, message, std::move(read_lease), std::move(message_lease),
            write_active_, options.compress_, std::move(operation)));
}

task<void> websocket::close(websocket_close_options options, operation_options operation) & {
    require_active();
    validate_operation_options(operation);
    std::pmr::string owned(options.reason_.view(), resource_);
    return ::ruvia::make_scoped_operation(operation_scope_,
        close_websocket_with_reason(target_, close_, options, std::move(owned), std::move(operation),
            read_active_, write_active_, message_send_active_));
}

void websocket::abort() & noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return;
    }
    abort_(target_);
}

std::optional<std::uint16_t> websocket::peer_close_code() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return std::nullopt;
    }
    return peer_close_code_(target_);
}

std::string_view websocket::peer_close_reason() const& noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return {};
    }
    return peer_close_reason_(target_);
}

websocket_termination websocket::termination() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return websocket_termination::none;
    }
    return protocol_(target_).termination();
}

std::uint64_t websocket::bytes_received() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_received();
}

std::uint64_t websocket::bytes_sent() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_sent();
}

task<void> websocket::write(websocket_opcode opcode, std::string_view payload_value, bool compress, operation_options operation) {
    require_active();
    std::pmr::string owned(payload_value, resource_);
    return write(opcode, std::move(owned), compress, std::move(operation));
}

task<void> websocket::write(websocket_opcode opcode, std::pmr::string&& payload_value, bool compress, operation_options operation) {
    require_active();
    validate_operation_options(operation);
    auto owned = detail::rebind_owned_storage(payload_value, resource_);
    return ::ruvia::make_scoped_operation(operation_scope_,
        write_websocket_payload(target_, write_, opcode, std::move(owned), write_active_, compress, std::move(operation)));
}

task<void> sse_writer::write(const sse_message& message) {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message, {.resource_ = stream_writer.resource_});
    return stream_writer.write(std::move(frame));
}

task<std::optional<multipart_stream_part>> multipart_reader::read() & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, read_task());
}

body_reader& multipart_reader::get_body_reader() const {
    registration_.require_active();
    return *body_reader_;
}

void multipart_reader::expire_capability(void* target) noexcept {
    auto& reader_value = *static_cast<multipart_reader*>(target);
    reader_value.operation_scope_.close();
    reader_value.body_reader_ = nullptr;
    reader_value.state_.emplace<expired_state_type>();
}

task<std::optional<multipart_stream_part>> multipart_reader::read_task() {
    auto lease = detail::claim_operation_lane(read_active_, "multipart body read is already in progress");
    if (std::holds_alternative<finished_state_type>(state_)) {
        co_return std::nullopt;
    }
    if (std::holds_alternative<failed_state_type>(state_)) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    if (!std::holds_alternative<receiving_state_type>(state_)) {
        throw std::logic_error("multipart body lifetime has expired");
    }

    read_guard_type read_guard_value(*this);
    for (;;) {
        auto result_value = std::get<receiving_state_type>(state_).parser_.poll();
        if (const auto* part = result_value.part()) {
            read_guard_value.commit();
            co_return *part;
        }
        if (result_value.done() != nullptr) {
            while (co_await get_body_reader().read()) {
            }
            state_.emplace<finished_state_type>();
            read_guard_value.commit();
            co_return std::nullopt;
        }
        if (result_value.need_input() != nullptr) {
            auto chunk = co_await get_body_reader().read();
            if (!chunk) {
                std::get<receiving_state_type>(state_).parser_.finish_input();
            } else {
                std::get<receiving_state_type>(state_).parser_.feed(as_chars(*chunk));
            }
            continue;
        }
        if (const auto* failure = result_value.failure()) {
            throw failure->protocol_error();
        }
        throw std::logic_error("unexpected multipart poll result");
    }
}

}  // namespace ruvia
