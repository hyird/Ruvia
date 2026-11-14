#include "streaming.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "context.h"
#include "context_access.h"
#include "http1_server_response.h"
#include "http_client.h"
#include "http_client_pool.h"
#include "http_connect_udp.h"
#include "http_header.h"
#include "http_multipart.h"
#include "http_server_sent_events.h"
#include "inbound_buffer.h"
#include "inline_storage.h"
#include "middleware.h"
#include "route_table.h"
#include "stop_token.h"
#include "task.h"
#include "task_scope.h"
#include "websocket_connection.h"
#include "worker_ref.h"
namespace {

constexpr const char* response_stream_lane_in_progress = "response stream output operation is already in progress";

// What an output operation of a response stream borrows from its writer.
struct response_stream_output final {
    void* target_;
    bool& active_;
    ruvia::detail::operation_interrupt_source& interrupts_;
    const ruvia::worker_ref& worker_;
    ruvia::context* context_;
};

template <typename operation_type>
auto run_response_stream_output(response_stream_output output, ruvia::operation_options operation, const char* operation_name, operation_type start) -> decltype(start()) {
    auto lease = ruvia::detail::claim_operation_lane(output.active_, response_stream_lane_in_progress);
    ruvia::detail::operation_interrupt interrupt(output.interrupts_, output.worker_, operation, output.context_, &ruvia::detail::context_access::reset_interrupted_request, operation_name);
    try {
        if constexpr (std::is_void_v<typename decltype(start())::value_type>) {
            co_await start();
            interrupt.throw_if_interrupted();
        } else {
            auto result = co_await start();
            interrupt.throw_if_interrupted();
            co_return result;
        }
    } catch (...) {
        interrupt.throw_if_interrupted();
        throw;
    }
}

}  // namespace

namespace ruvia {

sse_writer::sse_writer(const sse_writer& other) noexcept :
    writer_(other.writer_),
    registration_(other.registration_, this) {}

sse_writer::sse_writer(sse_writer&& other) noexcept :
    writer_(std::exchange(other.writer_, nullptr)),
    registration_(std::move(other.registration_), this) {}

sse_writer::sse_writer(response_stream_writer& writer) noexcept :
    writer_(writer.operation_scope_.active() ? &writer : nullptr),
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
task<std::optional<view_type>> read_body(detail::callback_ref<task<std::optional<std::span<const std::byte>>>(operation_options)> read, bool& read_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, "request body read is already in progress");
    const auto chunk = co_await read(std::move(operation));
    if (!chunk) {
        co_return std::nullopt;
    }
    if constexpr (std::same_as<view_type, std::string_view>) {
        co_return as_chars(*chunk);
    } else {
        co_return *chunk;
    }
}

task<void> write_transferred_chunk(response_stream_output output, task<void> (*write)(void*, std::string_view), std::string chunk, operation_options operation) {
    co_await run_response_stream_output(output, std::move(operation), "response stream write", [&] {
        return write(output.target_, chunk);
    });
}

task<void> end_owned(response_stream_output output, task<void> (*end)(void*, std::span<const http_header_view>), std::vector<http_header> fields, operation_options operation) {
    const detail::inline_http_header_views views(fields);
    co_await run_response_stream_output(output, std::move(operation), "response stream end", [&] {
        return end(output.target_, views.view());
    });
}

task<timer_sleep_result> sleep_stopped_by_either(void* target, task<timer_sleep_result> (*sleep)(void*, std::chrono::milliseconds, const stop_token&), std::chrono::milliseconds duration, stop_token context_stop, stop_token stop) {
    const stop_group either(context_stop, stop);
    co_return co_await sleep(target, duration, either.token());
}

}  // namespace

task<std::optional<std::span<const std::byte>>> body_reader::read(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::span<const std::byte>>(read_, read_active_, std::move(operation)));
}

task<std::optional<std::string_view>> body_reader::text(operation_options operation) & {
    return ::ruvia::make_scoped_operation(operation_scope_, read_body<std::string_view>(read_, read_active_, std::move(operation)));
}

void response_stream_writer::require_output() const {
    require_active();
    if (worker_ == nullptr || context_ == nullptr) {
        throw std::logic_error("response stream is not bound to a request");
    }
}

task<stream_commit_result> response_stream_writer::commit(operation_options operation) & {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_, run_response_stream_output({target_, output_active_, *interrupts_, *worker_, context_}, std::move(operation), "response stream commit", [target = target_, commit = commit_] {
        return commit(target);
    }));
}

task<stream_commit_result> sse_writer::commit(operation_options operation) & {
    return writer().commit(std::move(operation));
}

task<void> response_stream_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return write(as_chars(chunk), std::move(operation));
}

task<void> response_stream_writer::write(std::string_view chunk, operation_options operation) & {
    require_active();
    return write_owned(std::string(chunk), std::move(operation));
}

task<void> response_stream_writer::write_owned(std::string&& chunk, operation_options operation) {
    require_output();
    return ::ruvia::make_scoped_operation(operation_scope_, write_transferred_chunk({target_, output_active_, *interrupts_, *worker_, context_}, write_, std::move(chunk), std::move(operation)));
}

task<void> response_stream_writer::writeln(std::string_view chunk, operation_options operation) & {
    require_active();
    std::string owned;
    owned.reserve(chunk.size() + 1U);
    owned.append(chunk);
    owned.push_back('\n');
    return write_owned(std::move(owned), std::move(operation));
}

task<timer_sleep_result> response_stream_writer::sleep(timeout_duration duration, stop_token stop) & {
    require_active();
    if (!stop.stoppable()) {
        return ::ruvia::make_scoped_operation(operation_scope_, sleep_(target_, duration.value(), stop_token_));
    }
    return ::ruvia::make_scoped_operation(operation_scope_, sleep_stopped_by_either(target_, sleep_, duration.value(), stop_token_, std::move(stop)));
}

task<void> response_stream_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    require_output();
    std::vector<http_header> owned_trailers_value;
    owned_trailers_value.reserve(trailers.size());
    for (const auto& header : trailers) {
        owned_trailers_value.emplace_back(header.name(), header.value());
    }
    return ::ruvia::make_scoped_operation(operation_scope_, end_owned({target_, output_active_, *interrupts_, *worker_, context_}, end_, std::move(owned_trailers_value), std::move(operation)));
}

task<timer_sleep_result> sse_writer::sleep(timeout_duration duration, stop_token stop) & {
    return writer().sleep(duration, std::move(stop));
}

task<void> sse_writer::end(std::span<const http_header_view> trailers, operation_options operation) & {
    return writer().end(trailers, std::move(operation));
}

task<void> sse_writer::write(const sse_message& message, operation_options operation) & {
    auto& stream_writer = writer();
    auto frame = format_sse_message(message);
    return stream_writer.write(std::move(frame), std::move(operation));
}

task<std::optional<multipart_stream_part>> multipart_reader::read(operation_options operation) & {
    registration_.require_active();
    return ::ruvia::make_scoped_operation(operation_scope_, read_task(std::move(operation)));
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

task<std::optional<multipart_stream_part>> multipart_reader::read_task(operation_options operation) {
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

    if (operation.stop_token_.stop_requested()) {
        detail::operation_interrupt::throw_cause(detail::operation_interrupt_cause::cancelled, "multipart body read");
    }
    const detail::operation_steps steps(std::move(operation));
    read_guard_type read_guard_value(*this);
    try {
        for (;;) {
            auto result_value = std::get<receiving_state_type>(state_).parser_.poll();
            if (const auto* part = result_value.part()) {
                read_guard_value.commit();
                co_return *part;
            }
            if (result_value.done() != nullptr) {
                while (co_await get_body_reader().read(steps.next("multipart body read"))) {
                }
                state_.emplace<finished_state_type>();
                read_guard_value.commit();
                co_return std::nullopt;
            }
            if (result_value.need_input() != nullptr) {
                auto chunk = co_await get_body_reader().read(steps.next("multipart body read"));
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
    } catch (const std::system_error& error) {
        // An interrupted body read fed the parser nothing; parsing resumes on
        // the next read.
        if (steps.interrupted(error)) {
            read_guard_value.commit();
        }
        throw;
    }
}

}  // namespace ruvia

namespace {
ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void* target, ruvia::task<std::optional<ruvia::websocket_message>> (*read)(void*, ruvia::operation_options), bool& read_active, ruvia::operation_options operation) {
    auto lease = ruvia::detail::claim_operation_lane(read_active, "concurrent websocket reads are not supported");
    co_return co_await read(target, std::move(operation));
}

using websocket_write_type = ruvia::task<void> (*)(void*, ruvia::websocket_opcode, std::string_view, bool, ruvia::detail::websocket_write_extent, ruvia::operation_options);

// The final fragment releases the data lane of its fragmented message.
ruvia::task<void> write_websocket_payload(void* target, websocket_write_type write, ruvia::websocket_opcode opcode, std::string payload_value, bool& write_active, bool compress, ruvia::detail::websocket_write_extent extent, ruvia::operation_options operation, bool& fragmented_message_active) {
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, opcode, payload_value, compress, extent, std::move(operation));
    if (extent == ruvia::detail::websocket_write_extent::final_fragment) {
        fragmented_message_active = false;
    }
}

// The read leases are claimed by the caller before this task exists: the
// payload borrows the connection's read storage, and no read may run, even one
// started while this operation is still unstarted, until the frame has been written.
ruvia::task<void> write_websocket_message(void* target, websocket_write_type write, ruvia::websocket_message message, ruvia::detail::operation_lane_lease read_lease, ruvia::detail::operation_lane_lease message_lease, bool& write_active, bool compress, ruvia::operation_options operation) {
    auto held_read = std::move(read_lease);
    auto held_message = std::move(message_lease);
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, message.opcode(), message.payload(), compress, ruvia::detail::websocket_write_extent::message, std::move(operation));
}

ruvia::task<void> close_websocket_with_reason(void* target, ruvia::task<void> (*close)(void*, ruvia::websocket_close_options, ruvia::operation_options), ruvia::websocket_close_options options, std::string reason, ruvia::operation_options operation, bool& read_active, bool& write_active, const bool& message_send_active) {
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
namespace {
detail::websocket_state& require_websocket(const std::shared_ptr<detail::websocket_state>& state) {
    if (!state) {
        throw std::logic_error("websocket owner was moved");
    }
    state->require_active();
    return *state;
}
void retire_websocket(std::shared_ptr<detail::websocket_state> state) noexcept {
    if (!state) {
        return;
    }
    if (state->owner_) {
        state->abort();
    }
    state->operation_scope_.expire();
    if (auto owner = std::exchange(state->owner_, nullptr)) {
        if (state->retire_) {
            state->retire_(owner.get());
        }
    }
}
}  // namespace
websocket::websocket(std::shared_ptr<detail::websocket_state> state) noexcept :
    state_(std::move(state)) {}
websocket::websocket(websocket&&) noexcept = default;
websocket& websocket::operator=(websocket&& other) noexcept {
    if (this != &other) {
        retire_websocket(std::exchange(state_, std::move(other.state_)));
    }
    return *this;
}
websocket::~websocket() {
    retire_websocket(std::move(state_));
}
task<std::optional<websocket_message>> websocket::read(operation_options operation) & {
    return require_websocket(state_).read(std::move(operation));
}
task<void> websocket::text(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).text(payload, options, std::move(operation));
}
task<void> websocket::binary(std::string_view payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::binary(std::span<const std::byte> payload, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).binary(payload, options, std::move(operation));
}
task<void> websocket::ping(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).ping(payload, std::move(operation));
}
task<void> websocket::pong(std::string_view payload, operation_options operation) & {
    return require_websocket(state_).pong(payload, std::move(operation));
}
task<void> websocket::write_owned(websocket_opcode opcode, std::string&& payload, bool compress, operation_options operation) {
    return require_websocket(state_).write(opcode, std::move(payload), compress, std::move(operation));
}
task<void> websocket::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    return require_websocket(state_).send(std::move(message), options, std::move(operation));
}
task<void> websocket::close(websocket_close_options options, operation_options operation) & {
    return require_websocket(state_).close(options, std::move(operation));
}
void websocket::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
websocket_message_writer websocket::begin_text(websocket_send_options options) & {
    return require_websocket(state_).begin_text(options);
}
websocket_message_writer websocket::begin_binary(websocket_send_options options) & {
    return require_websocket(state_).begin_binary(options);
}
std::string_view websocket::subprotocol() const& noexcept {
    return state_ ? state_->subprotocol() : std::string_view{};
}
bool websocket::connected() const noexcept {
    if (!state_) {
        return false;
    }
    if (!state_->operation_scope_.active()) {
        return false;
    }
    state_->require_active();
    return state_->protocol_(state_->target_).liveness_mode() == websocket_liveness_mode::open;
}
const worker_ref& websocket::worker() const& {
    return *require_websocket(state_).worker_;
}
std::optional<std::uint16_t> websocket::peer_close_code() const noexcept {
    return state_ ? state_->peer_close_code() : std::nullopt;
}
std::string_view websocket::peer_close_reason() const& noexcept {
    return state_ ? state_->peer_close_reason() : std::string_view{};
}
websocket_termination websocket::termination() const noexcept {
    return state_ ? state_->termination() : websocket_termination::none;
}
std::uint64_t websocket::bytes_received() const noexcept {
    return state_ ? state_->bytes_received() : 0;
}
std::uint64_t websocket::bytes_sent() const noexcept {
    return state_ ? state_->bytes_sent() : 0;
}
task<std::optional<websocket_message>> detail::websocket_state::read(operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (read_timeout_) {
        operation = override_operation_options({.timeout_ = read_timeout_->value()}, std::move(operation));
    }
    return make_scoped_operation(operation_scope_, read_websocket(target_, read_, read_active_, std::move(operation)));
}

task<void> detail::websocket_state::text(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::text, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::string_view payload_value, websocket_send_options options, operation_options operation) & {
    return write(websocket_opcode::binary, payload_value, options.compress_, std::move(operation));
}

task<void> detail::websocket_state::binary(std::span<const std::byte> payload_value, websocket_send_options options, operation_options operation) & {
    return binary(as_chars(payload_value), options, std::move(operation));
}

task<void> detail::websocket_state::pong(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::pong, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::ping(std::string_view payload_value, operation_options operation) & {
    return write(websocket_opcode::ping, payload_value, true, std::move(operation));
}

task<void> detail::websocket_state::send(websocket_message&& message, websocket_send_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    auto read_lease = detail::claim_operation_lane(read_active_, "websocket message send cannot overlap a read");
    auto message_lease = detail::claim_operation_lane(message_send_active_, "websocket message send is already pending");
    return make_scoped_operation(operation_scope_, write_websocket_message(target_, write_, message, std::move(read_lease), std::move(message_lease), write_active_, options.compress_, std::move(operation)));
}

task<void> detail::websocket_state::close(websocket_close_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    std::string owned(options.reason_.view());
    return make_scoped_operation(operation_scope_, close_websocket_with_reason(target_, close_, options, std::move(owned), std::move(operation), read_active_, write_active_, message_send_active_));
}

void detail::websocket_state::abort() & noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return;
    }
    abort_(target_);
}

std::optional<std::uint16_t> detail::websocket_state::peer_close_code() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return std::nullopt;
    }
    return peer_close_code_(target_);
}

std::string_view detail::websocket_state::peer_close_reason() const& noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return {};
    }
    return peer_close_reason_(target_);
}

websocket_termination detail::websocket_state::termination() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return websocket_termination::none;
    }
    return protocol_(target_).termination();
}

std::uint64_t detail::websocket_state::bytes_received() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_received();
}

std::uint64_t detail::websocket_state::bytes_sent() const noexcept {
    if (!worker_->is_current()) {
        std::terminate();
    }
    if (!operation_scope_.active()) {
        return 0;
    }
    return protocol_(target_).bytes_sent();
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string_view payload_value, bool compress, operation_options operation, detail::websocket_write_extent extent) {
    require_active();
    return write(opcode, std::string(payload_value), compress, std::move(operation), extent);
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string&& payload_value, bool compress, operation_options operation, detail::websocket_write_extent extent) {
    require_active();
    if (extent == detail::websocket_write_extent::message && fragmented_message_active_ && (opcode == websocket_opcode::text || opcode == websocket_opcode::binary)) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    operation = override_operation_options(defaults_, std::move(operation));
    return make_scoped_operation(operation_scope_, write_websocket_payload(target_, write_, opcode, std::move(payload_value), write_active_, compress, extent, std::move(operation), fragmented_message_active_));
}

websocket_message_writer detail::websocket_state::begin_text(websocket_send_options options) & {
    return begin_message(websocket_opcode::text, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_binary(websocket_send_options options) & {
    return begin_message(websocket_opcode::binary, options.compress_);
}

websocket_message_writer detail::websocket_state::begin_message(websocket_opcode opcode, bool compress) {
    require_active();
    if (fragmented_message_active_) {
        throw std::logic_error("a fragmented websocket message is already open");
    }
    fragmented_message_active_ = true;
    ++fragmented_message_sequence_;
    return websocket_message_writer(*this, opcode, compress);
}

websocket_message_writer::websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept :
    owner_(&owner),
    message_id_(owner.fragmented_message_sequence_),
    opcode_(opcode),
    compress_(compress),
    registration_(owner.operation_scope_, this, &websocket_message_writer::expire_capability) {}

websocket_message_writer::websocket_message_writer(websocket_message_writer&& other) noexcept :
    owner_(std::exchange(other.owner_, nullptr)),
    message_id_(other.message_id_),
    opcode_(other.opcode_),
    compress_(other.compress_),
    registration_(std::move(other.registration_), this) {}

websocket_message_writer::~websocket_message_writer() {
    abort();
}

void websocket_message_writer::expire_capability(void* target) noexcept {
    static_cast<websocket_message_writer*>(target)->owner_ = nullptr;
}

bool websocket_message_writer::message_open() const noexcept {
    return owner_ != nullptr && owner_->fragmented_message_active_ && owner_->fragmented_message_sequence_ == message_id_;
}

void websocket_message_writer::abort() & noexcept {
    if (!message_open()) {
        owner_ = nullptr;
        return;
    }
    // The peer cannot tell where an unfinished message ends.
    auto* owner = std::exchange(owner_, nullptr);
    owner->fragmented_message_active_ = false;
    owner->abort();
}

task<void> websocket_message_writer::write(std::string_view chunk, operation_options operation) & {
    return submit(chunk, false, std::move(operation));
}

task<void> websocket_message_writer::write(std::span<const std::byte> chunk, operation_options operation) & {
    return submit(as_chars(chunk), false, std::move(operation));
}

task<void> websocket_message_writer::finish(std::string_view last_chunk, operation_options operation) & {
    return submit(last_chunk, true, std::move(operation));
}

task<void> websocket_message_writer::finish(std::span<const std::byte> last_chunk, operation_options operation) & {
    return submit(as_chars(last_chunk), true, std::move(operation));
}

task<void> websocket_message_writer::submit(std::string_view chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, chunk, compress_, std::move(operation), final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

task<void> websocket_message_writer::submit_owned(std::string&& chunk, bool final, operation_options operation) {
    registration_.require_active();
    if (!message_open()) {
        throw std::logic_error("websocket message writer is finished");
    }
    return owner_->write(opcode_, std::move(chunk), compress_, std::move(operation), final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

}  // namespace ruvia
namespace ruvia::detail {
class http_capsule_stream_state final {
public:
    http_capsule_stream_state(http_tunnel& tunnel, http_capsule_config config) :
        worker_(tunnel.worker_),
        server_(&tunnel),
        decoder_(config),
        config_(config),
        inbound_(tunnel.inbound_),
        payload_reservation_(inbound_) {}
    http_capsule_stream_state(http_client_tunnel&& tunnel, http_capsule_config config) :
        worker_(tunnel.response_.state_->memory_domain().worker()),
        memory_pin_(http_client_response(tunnel.response_.state_)),
        client_(std::move(tunnel)),
        decoder_(config),
        config_(config) {}
    void require() const {
        if (closed_ || (client_ && (!client_->response_.state_ || client_->response_.state_->pool_ == nullptr))) {
            throw std::logic_error("capsule stream is closed");
        }
        if (!worker_.is_current()) {
            throw std::logic_error("capsule stream requires its owner worker");
        }
    }
    [[nodiscard]] operation_lane_lease claim_lane(bool output) {
        require();
        return claim_operation_lane(output ? output_active_ : read_active_, "capsule stream operation is already active");
    }
    void retain() noexcept {
        ++references_;
    }
    void release() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (--references_ != 0) {
            return;
        }
        auto pin = std::move(memory_pin_);
        delete this;
    }
    void abort() noexcept {
        abort_input(false);
    }
    void abort_malformed() noexcept {
        abort_input(true);
    }
    void abort_input(bool malformed) noexcept {
        if (closed_) {
            return;
        }
        if (client_ && (!client_->response_.state_ || client_->response_.state_->pool_ == nullptr)) {
            closed_ = true;
            return;
        }
        if (!worker_.is_current()) {
            std::terminate();
        }
        closed_ = true;
        if (server_) {
            server_->abort_input(malformed);
        } else if (client_) {
            if (malformed) {
                client_->abort_malformed();
            } else {
                client_->abort();
            }
        }
    }
    void clear_input() noexcept {
        input_ = {};
        owned_input_.reset();
        offset_ = 0;
        native_input_ = false;
    }
    void close() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        if (read_active_ || output_active_) {
            std::terminate();
        }
        if (client_) {
            abort();
        } else {
            closed_ = true;
        }
        read_scope_.close();
        output_scope_.close();
        clear_input();
        server_ = nullptr;
    }
    [[nodiscard]] bool poll_capsule(bool fin, http_capsule_retention retention) {
        const auto collect = [](void* raw, http_capsule_event event) {
            auto& state = *static_cast<http_capsule_stream_state*>(raw);
            state.type_ = event.type_;
            if (!event.payload_.empty()) {
                state.payload_.append(event.payload_.data(), event.payload_.size());
            }
        };
        const auto bytes_value = std::span<const char>(input_).subspan(offset_);
        const auto result_value = decoder_.feed_one(bytes_value, fin, collect, this, retention);
        offset_ += result_value.consumed_bytes_;
        if (result_value.status_ == http_capsule_status::limit || result_value.status_ == http_capsule_status::truncated) {
            abort_malformed();
            throw std::runtime_error("invalid or oversized capsule stream");
        }
        if (result_value.status_ == http_capsule_status::end) {
            receive_ended_ = true;
        }
        return result_value.capsule_complete_;
    }
    // Interruptions are std::system_error (timed_out / operation_canceled);
    // a client tunnel reports them as its http_client_error subclass.
    [[noreturn]] void throw_interrupted(operation_interrupt_cause cause, const char* operation_name) const {
        if (client_) {
            throw http_client_error(std::make_error_code(cause == operation_interrupt_cause::timed_out ? std::errc::timed_out : std::errc::operation_canceled), operation_name);
        }
        operation_interrupt::throw_cause(cause, operation_name);
    }
    // A stop requested before an operation starts leaves the stream untouched.
    void throw_if_stopped_before_start(const operation_options& operation, const char* operation_name) const {
        if (operation.stop_token_.stop_requested()) {
            throw_interrupted(operation_interrupt_cause::cancelled, operation_name);
        }
    }
    [[nodiscard]] operation_options next_step(const operation_steps& steps, const char* operation_name) const {
        auto step = steps.try_next();
        if (!step.has_value()) {
            throw_interrupted(operation_interrupt_cause::timed_out, operation_name);
        }
        return std::move(*step);
    }
    // Whether the exception being handled is a step interrupted by `steps`.
    [[nodiscard]] static bool interrupted(const operation_steps& steps) noexcept {
        try {
            throw;
        } catch (const std::system_error& error) {
            return steps.interrupted(error);
        } catch (...) {
            return false;
        }
    }
    [[nodiscard]] task<bool> read_input(bool datagrams, const operation_steps& steps) {
        clear_input();
        if (datagrams && has_native_transport()) {
            std::optional<http_tunnel_input> chunk;
            if (server_) {
                chunk = co_await server_->read_datagram_raw(next_step(steps, "capsule read"));
            }
#ifdef RUVIA_ENABLE_HTTP3
            else {
                auto& state = *client_->response_.state_;
                chunk = co_await state.pool_->read_http3_tunnel_input(state, next_step(steps, "capsule read"));
            }
#endif
            if (!chunk) {
                co_return false;
            }
            native_input_ = chunk->datagram_;
            owned_input_.emplace(std::move(chunk->bytes_));
            input_ = *owned_input_;
            co_return true;
        }
        if (server_) {
            auto chunk = co_await server_->read_raw(next_step(steps, "capsule read"));
            if (!chunk) {
                co_return false;
            }
            owned_input_.emplace(std::move(*chunk));
            input_ = *owned_input_;
        } else {
            auto chunk = co_await client_->read(next_step(steps, "capsule read"));
            if (!chunk) {
                co_return false;
            }
            input_ = as_chars(*chunk);
        }
        co_return true;
    }
    [[nodiscard]] bool has_native_transport() const noexcept {
        if (server_) {
            return server_->read_datagram_ != nullptr && server_->write_datagram_ != nullptr;
        }
#ifdef RUVIA_ENABLE_HTTP3
        return client_ && client_->response_.state_->transport_ == http_client_response_transport::http3;
#else
        return false;
#endif
    }
    [[nodiscard]] task<bool> send_datagram(std::string_view bytes, const operation_steps& steps) {
        if (server_) {
            co_return co_await server_->write_datagram_raw(bytes, next_step(steps, "capsule write"));
        }
#ifdef RUVIA_ENABLE_HTTP3
        auto& state = *client_->response_.state_;
        co_return co_await state.pool_->send_http3_datagram(state, bytes, next_step(steps, "capsule write"));
#else
        co_return false;
#endif
    }
    [[nodiscard]] task<void> send_bytes(std::string&& bytes_value, const operation_steps& steps) {
        if (server_ != nullptr) {
            co_await server_->write_raw(std::move(bytes_value), next_step(steps, "capsule write"));
            co_return;
        }
        const auto limit = client_->response_.state_->tunnel_->config_.max_chunk_bytes_;
        if (bytes_value.size() <= limit) {
            co_await client_->write_owned(std::move(bytes_value), next_step(steps, "capsule write"));
            co_return;
        }
        std::string_view remaining = bytes_value;
        while (!remaining.empty()) {
            const auto chunk = remaining.substr(0, limit);
            co_await client_->write(chunk, next_step(steps, "capsule write"));
            remaining.remove_prefix(chunk.size());
        }
    }
    const worker_ref& worker_;
    std::optional<http_client_response> memory_pin_;
    http_tunnel* server_{};
    std::optional<http_client_tunnel> client_;
    http_capsule_decoder decoder_;
    http_capsule_config config_;
    std::optional<std::string> owned_input_;
    std::string_view input_;
    std::size_t offset_{};
    bool native_input_{};
    inbound_buffer_account* inbound_{nullptr};
    std::string payload_;
    inbound_buffer_reservation payload_reservation_{};
    std::uint64_t type_{};
    bool receive_ended_{};
    bool send_ended_{};
    bool closed_{};
    bool read_active_{};
    bool output_active_{};
    std::size_t references_{1};
    ::ruvia::operation_scope read_scope_;
    ::ruvia::operation_scope output_scope_;
};
class capsule_state_pin final {
public:
    explicit capsule_state_pin(http_capsule_stream_state& state_value) noexcept :
        state_(&state_value) {
        state_->retain();
    }
    capsule_state_pin(capsule_state_pin&& other) noexcept :
        state_(std::exchange(other.state_, nullptr)) {}
    capsule_state_pin(const capsule_state_pin&) = delete;
    ~capsule_state_pin() {
        if (state_) {
            state_->release();
        }
    }
    [[nodiscard]] http_capsule_stream_state& state() const noexcept {
        return *state_;
    }

private:
    http_capsule_stream_state* state_;
};
struct capsule_write_input final {
    capsule_state_pin pin_;
    std::string bytes_;
    bool datagram_{};
    operation_options operation_;
};
// Failure aborts the stream, except an interrupted read, which keeps the
// partly received capsule for the next read.
class running_capsule_operation final {
public:
    running_capsule_operation(http_capsule_stream_state& state_value, bool output) :
        state_(state_value),
        exceptions_(std::uncaught_exceptions()),
        lane_(state_value.claim_lane(output)) {}
    ~running_capsule_operation() {
        if (aborts_ && std::uncaught_exceptions() > exceptions_) {
            state_.abort();
        }
    }
    void keep_stream() noexcept {
        aborts_ = false;
    }

private:
    http_capsule_stream_state& state_;
    int exceptions_;
    bool aborts_{true};
    operation_lane_lease lane_;
};
}  // namespace ruvia::detail

namespace ruvia {
namespace {
constexpr const char* tunnel_lane_in_progress = "HTTP tunnel operation already in progress on this direction";
template <typename input_type>
task<std::optional<input_type>> read_tunnel(void* target, task<std::optional<input_type>> (*read)(void*, operation_options), bool& read_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, tunnel_lane_in_progress);
    co_return co_await read(target, std::move(operation));
}
task<void> write_tunnel(void* target, task<void> (*write)(void*, std::string_view, operation_options), std::string bytes_value, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_await write(target, bytes_value, std::move(operation));
}
task<bool> write_tunnel_datagram(void* target, task<bool> (*write)(void*, std::string_view, operation_options), std::string_view bytes, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_return co_await write(target, bytes, std::move(operation));
}
task<void> finish_tunnel(void* target, task<void> (*finish_value)(void*, operation_options), bool& ended, bool& write_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (!ended) {
        co_await finish_value(target, std::move(operation));
        ended = true;
    }
}
}  // namespace

http_tunnel::http_tunnel(detail::inbound_buffer_account* inbound, const worker_ref& worker_value, void* target, read_type read, write_type write, finish_type finish_value, abort_type abort, bool datagrams, read_datagram_type read_datagram, write_datagram_type write_datagram) noexcept :
    inbound_(inbound),
    worker_(worker_value),
    target_(target),
    read_(read),
    write_(write),
    finish_(finish_value),
    abort_(abort),
    read_datagram_(read_datagram),
    write_datagram_(write_datagram),
    datagrams_(datagrams) {
    operations_.bind_worker(worker_);
}

void http_tunnel::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("HTTP tunnel session has expired");
    }
}
void http_tunnel::require_unadapted() const {
    if (adapted_) {
        throw std::logic_error("HTTP tunnel is already adapted to a capsule stream");
    }
}
task<std::optional<std::span<const std::byte>>> http_tunnel::read(operation_options operation) & {
    require_active();
    require_unadapted();
    return ::ruvia::make_scoped_operation(operations_, read_borrowed(std::move(operation)));
}
task<std::optional<std::span<const std::byte>>> http_tunnel::read_borrowed(operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active_, tunnel_lane_in_progress);
    // The previous view expires as this read starts.
    read_chunk_.reset();
    const detail::operation_steps steps(std::move(operation));
    for (;;) {
        auto chunk = co_await read_(target_, steps.next("HTTP tunnel read"));
        if (!chunk) {
            co_return std::nullopt;
        }
        if (!chunk->empty()) {
            read_chunk_.emplace(std::move(*chunk));
            co_return as_bytes(std::string_view(*read_chunk_));
        }
    }
}
task<std::optional<std::string>> http_tunnel::read_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_, read_active_, std::move(operation)));
}
task<std::optional<detail::http_tunnel_input>> http_tunnel::read_datagram_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_datagram_, read_active_, std::move(operation)));
}
task<bool> http_tunnel::write_datagram_raw(std::string_view bytes, operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, write_tunnel_datagram(target_, write_datagram_, bytes, write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::write(std::string_view bytes_value, operation_options operation) & {
    require_active();
    require_unadapted();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    return write_raw(std::string(bytes_value), std::move(operation));
}
task<void> http_tunnel::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(as_chars(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_owned(std::string&& bytes_value, operation_options operation) {
    require_active();
    require_unadapted();
    return write_raw(std::move(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_raw(std::string&& bytes_value, operation_options operation) {
    require_active();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    auto owned = std::move(bytes_value);
    return ::ruvia::make_scoped_operation(operations_, write_tunnel(target_, write_, std::move(owned), write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::finish(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, finish_tunnel(target_, finish_, send_ended_, write_active_, std::move(operation)));
}
void http_tunnel::abort() & noexcept {
    abort_input(false);
}
void http_tunnel::abort_input(bool malformed) noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_, malformed);
    }
}

http_capsule::http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::string payload_value) noexcept :
    state_(&state_value),
    type_(type),
    payload_(std::move(payload_value)) {
    state_->retain();
}
http_capsule::http_capsule(http_capsule&& other) noexcept :
    state_(std::exchange(other.state_, nullptr)),
    type_(other.type_),
    payload_(std::move(other.payload_)) {
    other.payload_.reset();
}
http_capsule& http_capsule::operator=(http_capsule&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
        type_ = other.type_;
        // Reconstruct rather than propagate the previous capsule's allocator.
        if (other.payload_) {
            payload_.emplace(std::move(*other.payload_));
            other.payload_.reset();
        }
    }
    return *this;
}
http_capsule::~http_capsule() {
    release();
}
void http_capsule::release() noexcept {
    // Payload storage is returned before its domain pin can retire the pool.
    if (state_ && !state_->worker_.is_current()) {
        std::terminate();
    }
    payload_.reset();
    if (auto* state = std::exchange(state_, nullptr)) {
        state->release();
    }
}
http_capsule_stream::http_capsule_stream(http_tunnel& tunnel, http_capsule_config config) {
    tunnel.require_active();
    tunnel.require_unadapted();
    state_ = new detail::http_capsule_stream_state(tunnel, config);
    tunnel.adapted_ = true;
}
http_capsule_stream::http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config) {
    if (!tunnel.response_.state_ || !tunnel.response_.state_->memory_domain().worker().is_current()) {
        throw std::logic_error("capsule stream requires a live tunnel on its worker");
    }
    // Validate before consuming the caller's owner.
    http_capsule_decoder validation(config);
    state_ = new detail::http_capsule_stream_state(std::move(tunnel), config);
}
http_capsule_stream::http_capsule_stream(http_capsule_stream&& other) noexcept :
    state_(std::exchange(other.state_, nullptr)) {}
http_capsule_stream& http_capsule_stream::operator=(http_capsule_stream&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
http_capsule_stream::~http_capsule_stream() {
    release();
}
void http_capsule_stream::release() noexcept {
    if (auto* state = std::exchange(state_, nullptr)) {
        state->close();
        state->release();
    }
}
void http_capsule_stream::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
// datagrams() and udp() build on this one gate.
http_capsule_stream http_tunnel::capsules(http_capsule_config config) & {
    // The route protocol owns Capsule Protocol negotiation (RFC 9297 section
    // 3.4); a route that did not declare datagrams never advertised it.
    if (!datagrams_) {
        throw std::logic_error("HTTP tunnel route did not declare datagrams");
    }
    require_unadapted();
    return http_capsule_stream(*this, config);
}
http_capsule_stream http_client_tunnel::capsules(http_capsule_config config) && {
    return http_capsule_stream(std::move(*this), config);
}
task<std::optional<http_capsule>> http_capsule_stream::read(operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    return ::ruvia::make_scoped_operation(state_->read_scope_, read_owned(detail::capsule_state_pin(*state_), std::move(operation)));
}
task<std::optional<http_capsule>> http_capsule_stream::read_owned(detail::capsule_state_pin pin, operation_options operation) {
    auto& state_value = pin.state();
    state_value.throw_if_stopped_before_start(operation, "capsule read");
    const detail::operation_steps steps(std::move(operation));
    detail::running_capsule_operation running(state_value, false);
    if (state_value.receive_ended_) {
        co_return std::nullopt;
    }
    try {
        for (;;) {
            bool fin{};
            if (state_value.offset_ == state_value.input_.size()) {
                fin = !co_await state_value.read_input(false, steps);
            }
            if (state_value.poll_capsule(fin, http_capsule_retention::all)) {
                auto payload_value = std::move(state_value.payload_);
                state_value.payload_ = std::string{};
                co_return http_capsule(state_value, state_value.type_, std::move(payload_value));
            }
            if (state_value.receive_ended_) {
                co_return std::nullopt;
            }
        }
    } catch (...) {
        if (detail::http_capsule_stream_state::interrupted(steps)) {
            running.keep_stream();
        }
        throw;
    }
}
task<void> http_capsule_stream::write(std::uint64_t type, std::span<const std::byte> payload_value, operation_options operation) & {
    return write(type, std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()), std::move(operation));
}
task<void> http_capsule_stream::write(std::uint64_t type, std::string_view payload_value, operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    if (state_->send_ended_) {
        throw std::logic_error("capsule sending direction is closed");
    }
    std::array<char, 16> header;
    const auto encoded = encode_http_capsule_header(header, type, payload_value.size());
    if ((encoded.index() != 0) || payload_value.size() > state_->config_.max_capsule_length_) {
        throw std::length_error("invalid capsule type or payload length");
    }
    return write_frame(std::string_view(header.data(), std::get<0>(encoded)), payload_value, false, std::move(operation));
}
task<void> http_capsule_stream::write_owned(std::uint64_t type, std::string&& payload_value, operation_options operation) {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    if (state_->send_ended_) {
        throw std::logic_error("capsule sending direction is closed");
    }
    std::array<char, 16> header;
    const auto encoded = encode_http_capsule_header(header, type, payload_value.size());
    if ((encoded.index() != 0) || payload_value.size() > state_->config_.max_capsule_length_) {
        throw std::length_error("invalid capsule type or payload length");
    }
    return write_frame_owned(std::string_view(header.data(), std::get<0>(encoded)), std::move(payload_value), false, std::move(operation));
}
task<void> http_capsule_stream::write_frame(std::string_view prefix, std::string_view payload_value, bool datagram, operation_options operation) {
    const auto write = +[](detail::capsule_write_input input) -> task<void> {
        auto& state_value = input.pin_.state();
        state_value.throw_if_stopped_before_start(input.operation_, "capsule write");
        const detail::operation_steps steps(std::move(input.operation_));
        detail::running_capsule_operation running(state_value, true);
        if (state_value.send_ended_) {
            throw std::logic_error("capsule sending direction is closed");
        }
        try {
            if (input.datagram_) {
                if (co_await state_value.send_datagram(input.bytes_, steps)) {
                    co_return;
                }
                std::array<char, 16> header;
                const auto encoded = encode_http_capsule_header(header, http_datagram_capsule_type, input.bytes_.size());
                if (encoded.index() != 0) {
                    throw std::length_error("HTTP Datagram exceeds capsule bound");
                }
                input.bytes_.insert(0, header.data(), std::get<0>(encoded));
            }
            co_await state_value.send_bytes(std::move(input.bytes_), steps);
        } catch (...) {
            state_value.abort();
            throw;
        }
    };
    std::string bytes_value;
    bytes_value.reserve(prefix.size() + payload_value.size());
    bytes_value.append(prefix);
    bytes_value.append(payload_value);
    return ::ruvia::make_scoped_operation(state_->output_scope_, write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(bytes_value), datagram, std::move(operation)}));
}
task<void> http_capsule_stream::write_frame_owned(std::string_view prefix, std::string&& payload_value, bool datagram, operation_options operation) {
    if (prefix.empty()) {
        const auto write = +[](detail::capsule_write_input input) -> task<void> {
            auto& state_value = input.pin_.state();
            state_value.throw_if_stopped_before_start(input.operation_, "capsule write");
            const detail::operation_steps steps(std::move(input.operation_));
            detail::running_capsule_operation running(state_value, true);
            if (state_value.send_ended_) {
                throw std::logic_error("capsule sending direction is closed");
            }
            try {
                if (input.datagram_) {
                    if (co_await state_value.send_datagram(input.bytes_, steps)) {
                        co_return;
                    }
                    std::array<char, 16> header;
                    const auto encoded = encode_http_capsule_header(header, http_datagram_capsule_type, input.bytes_.size());
                    if (encoded.index() != 0) {
                        throw std::length_error("HTTP Datagram exceeds capsule bound");
                    }
                    input.bytes_.insert(0, header.data(), std::get<0>(encoded));
                }
                co_await state_value.send_bytes(std::move(input.bytes_), steps);
            } catch (...) {
                state_value.abort();
                throw;
            }
        };
        auto owned = std::move(payload_value);
        return ::ruvia::make_scoped_operation(state_->output_scope_, write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(owned), datagram, std::move(operation)}));
    }
    return write_frame(prefix, payload_value, datagram, std::move(operation));
}
task<void> http_capsule_stream::finish(operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    const auto finish_value = +[](detail::capsule_state_pin pin, operation_options finish_operation) -> task<void> {
        auto& state_value = pin.state();
        state_value.throw_if_stopped_before_start(finish_operation, "capsule finish");
        detail::running_capsule_operation running(state_value, true);
        if (!state_value.send_ended_) {
            if (state_value.server_) {
                co_await state_value.server_->finish(std::move(finish_operation));
            } else {
                co_await state_value.client_->finish(std::move(finish_operation));
            }
            state_value.send_ended_ = true;
        }
    };
    return ::ruvia::make_scoped_operation(state_->output_scope_, finish_value(detail::capsule_state_pin(*state_), std::move(operation)));
}

http_datagram_stream::http_datagram_stream(http_capsule_stream&& stream) :
    stream_(std::move(stream)) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
}
http_datagram_stream http_tunnel::datagrams(http_capsule_config config) & {
    return http_datagram_stream(capsules(config));
}
http_udp_tunnel http_tunnel::udp(http_capsule_config config) & {
    return http_udp_tunnel(datagrams(config));
}
http_datagram_stream http_client_tunnel::datagrams(http_capsule_config config) && {
    if (!response_.state_ || !response_.state_->tunnel_ || !response_.state_->tunnel_->udp_ || !response_.state_->tunnel_->accepted_) {
        throw std::logic_error("HTTP Datagrams require an accepted datagram tunnel");
    }
    return http_datagram_stream(std::move(*this).capsules(config));
}
task<std::optional<http_datagram>> http_datagram_stream::read(operation_options operation) & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->require();
    return ::ruvia::make_scoped_operation(stream_.state_->read_scope_, read_owned(detail::capsule_state_pin(*stream_.state_), false, std::move(operation)));
}
task<std::optional<http_datagram>> http_datagram_stream::read_owned(detail::capsule_state_pin pin, bool udp, operation_options operation) {
    auto& state_value = pin.state();
    state_value.throw_if_stopped_before_start(operation, "HTTP Datagram read");
    const detail::operation_steps steps(std::move(operation));
    detail::running_capsule_operation running(state_value, false);
    try {
        for (;;) {
            if (state_value.receive_ended_) {
                co_return std::nullopt;
            }
            bool fin{};
            if (state_value.offset_ == state_value.input_.size()) {
                fin = !co_await state_value.read_input(true, steps);
            }
            // The decoder drops other capsule types and, for CONNECT-UDP, unknown
            // Context IDs before the length limit applies (RFC 9297 section 3.2).
            if (!state_value.native_input_ && !state_value.poll_capsule(fin, udp ? http_capsule_retention::udp_payloads : http_capsule_retention::datagrams)) {
                continue;
            }
            const bool native = state_value.native_input_;
            auto& owned = native ? *state_value.owned_input_ : state_value.payload_;
            if (native) {
                state_value.offset_ = state_value.input_.size();
            }
            auto bytes_value = std::span<const char>(owned);
            http_datagram_session datagrams;
            std::size_t offset{};
            if (udp) {
                auto decoded = datagrams.receive_udp_datagram(bytes_value);
                if ((decoded.index() != 0)) {
                    state_value.abort_malformed();
                    throw std::runtime_error("malformed CONNECT-UDP datagram");
                }
                if (!std::get<0>(decoded)) {
                    if (!native) {
                        owned.clear();
                    }
                    continue;
                }
                offset = bytes_value.size() - (std::get<0>(decoded))->payload_.size();
            } else {
                auto decoded = datagrams.receive_datagram(bytes_value);
                if (!decoded) {
                    if (!native) {
                        owned.clear();
                    }
                    continue;
                }
                offset = bytes_value.size() - decoded->size();
            }
            if (native && owned.size() > state_value.config_.max_capsule_length_) {
                state_value.abort_malformed();
                throw std::runtime_error("oversized HTTP Datagram");
            }
            auto result_value = std::move(owned);
            if (native) {
                state_value.clear_input();
            } else {
                state_value.payload_ = std::string{};
            }
            co_return http_datagram(http_capsule(state_value, http_datagram_capsule_type, std::move(result_value)), offset);
        }
    } catch (...) {
        if (detail::http_capsule_stream_state::interrupted(steps)) {
            running.keep_stream();
        }
        throw;
    }
}
task<void> http_datagram_stream::send(std::span<const std::byte> payload_value, operation_options operation) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()), std::move(operation));
}
task<void> http_datagram_stream::send(std::string_view payload_value, operation_options operation) & {
    return send_payload(payload_value, false, std::move(operation));
}
task<void> http_datagram_stream::send_owned(std::string&& payload_value, operation_options operation) {
    return send_payload_owned(std::move(payload_value), false, std::move(operation));
}
task<void> http_datagram_stream::send_payload(std::string_view payload_value, bool udp, operation_options operation) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto context_bytes = udp ? std::size_t{1} : std::size_t{0};
    if (context_bytes > state_value.config_.max_capsule_length_ || payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size())) : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame(udp ? std::string_view("\0", 1) : std::string_view{}, payload_value, true, std::move(operation));
    }
    return stream_.write_frame(std::get<0>(plan).prefix(), payload_value, false, std::move(operation));
}
task<void> http_datagram_stream::send_payload_owned(std::string&& payload_value, bool udp, operation_options operation) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto context_bytes = udp ? std::size_t{1} : std::size_t{0};
    if (context_bytes > state_value.config_.max_capsule_length_ || payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size())) : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame_owned(udp ? std::string_view("\0", 1) : std::string_view{}, std::move(payload_value), true, std::move(operation));
    }
    return stream_.write_frame_owned(std::get<0>(plan).prefix(), std::move(payload_value), false, std::move(operation));
}
task<std::optional<http_datagram>> http_udp_tunnel::read(operation_options operation) & {
    auto* state_value = stream_.stream_.state_;
    if (!state_value) {
        throw std::logic_error("UDP tunnel is empty");
    }
    state_value->require();
    return ::ruvia::make_scoped_operation(state_value->read_scope_, http_datagram_stream::read_owned(detail::capsule_state_pin(*state_value), true, std::move(operation)));
}
task<void> http_udp_tunnel::send(std::span<const std::byte> payload_value, operation_options operation) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()), std::move(operation));
}
task<void> http_udp_tunnel::send(std::string_view payload_value, operation_options operation) & {
    return stream_.send_payload(payload_value, true, std::move(operation));
}
task<void> http_udp_tunnel::send_owned(std::string&& payload_value, operation_options operation) {
    return stream_.send_payload_owned(std::move(payload_value), true, std::move(operation));
}
}  // namespace ruvia

namespace ruvia::detail {

const std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> tls_tunnel_output::bio_method_ = [] {
    std::unique_ptr<BIO_METHOD, decltype(&BIO_meth_free)> method(BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Ruvia TLS tunnel output"), &BIO_meth_free);
    if (!method || BIO_meth_set_create(method.get(), &create_bio) != 1 || BIO_meth_set_destroy(method.get(), &destroy_bio) != 1 || BIO_meth_set_write_ex(method.get(), &write_bio) != 1 || BIO_meth_set_ctrl(method.get(), &control_bio) != 1) {
        throw std::bad_alloc();
    }
    return method;
}();

tls_tunnel_output::tls_tunnel_output(SSL& ssl, asio::ip::tcp::socket& socket, const worker_ref& worker_value, connection_lifecycle* lifecycle) :
    ssl_(ssl),
    socket_(socket),
    worker_(worker_value),
    lifecycle_(lifecycle),
    available_(worker_value),
    flushed_(worker_value),
    tasks_(worker_value) {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!SSL_is_init_finished(&ssl_)) {
        throw std::logic_error("TLS tunnel output requires an established TLS connection");
    }
    // Application writes are flushed in 4 KiB pieces. Space also covers TLS
    // control output generated from one buffered input record while a write waits.
    bytes_.resize(64 * 1024);
    auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(bio_method_.get()), &BIO_free);
    if (!bio) {
        throw std::bad_alloc();
    }
    previous_ = SSL_get_wbio(&ssl_);
    if (previous_ == nullptr || BIO_up_ref(previous_) != 1) {
        previous_ = nullptr;
        throw std::bad_alloc();
    }
    BIO_set_data(bio.get(), this);
    SSL_set0_wbio(&ssl_, bio.release());
}

tls_tunnel_output::~tls_tunnel_output() {
    if (!worker_.is_current() || tasks_.size() != 0) {
        std::terminate();
    }
    SSL_set0_wbio(&ssl_, previous_);
}

void tls_tunnel_output::start() {
    if (!worker_.is_current() || started_) {
        std::terminate();
    }
    tasks_.spawn(run_writer());
    started_ = true;
}

int tls_tunnel_output::create_bio(BIO* bio) noexcept {
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    return 1;
}
int tls_tunnel_output::destroy_bio(BIO* bio) noexcept {
    BIO_set_data(bio, nullptr);
    BIO_set_init(bio, 0);
    return 1;
}
int tls_tunnel_output::write_bio(BIO* bio, const char* bytes_value, std::size_t size, std::size_t* written) noexcept {
    *written = 0;
    auto& output = *static_cast<tls_tunnel_output*>(BIO_get_data(bio));
    if (!output.worker_.is_current()) {
        std::terminate();
    }
    BIO_clear_retry_flags(bio);
    if (size == 0) {
        return 1;
    }
    const auto count = size;
    if (output.error_ || output.stopping_ || count > output.bytes_.size() - output.size_) {
        output.fail(std::make_error_code(std::errc::no_buffer_space));
        return 0;
    }
    const auto tail = (output.head_ + output.size_) % output.bytes_.size();
    const auto first = std::min(count, output.bytes_.size() - tail);
    std::memcpy(output.bytes_.data() + tail, bytes_value, first);
    std::memcpy(output.bytes_.data(), bytes_value + first, count - first);
    output.size_ += count;
    output.available_.notify();
    *written = size;
    return 1;
}
long tls_tunnel_output::control_bio(BIO*, int command, long, void*) noexcept {
    return command == BIO_CTRL_FLUSH ? 1 : 0;
}
void tls_tunnel_output::fail(std::error_code error) noexcept {
    if (!error_) {
        if (lifecycle_ != nullptr) {
            lifecycle_->note_error(error);
        }
        error_ = error;
    }
    close_socket(socket_);
    available_.notify();
    flushed_.notify();
}
void tls_tunnel_output::abort() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    fail(std::make_error_code(std::errc::operation_canceled));
}
task<void> tls_tunnel_output::run_writer() {
    try {
        co_await run_writer_inner();
    } catch (...) {
        fail(std::make_error_code(std::errc::io_error));
        throw;
    }
}
task<void> tls_tunnel_output::run_writer_inner() {
    for (;;) {
        if (error_) {
            co_return;
        }
        if (size_ == 0) {
            flushed_.notify();
            if (stopping_) {
                co_return;
            }
            co_await available_.wait();
            continue;
        }
        const auto count = std::min(size_, bytes_.size() - head_);
        const auto result_value = co_await async_asio<std::size_t>([this, count](auto handler) {
            asio::async_write(socket_, asio::buffer(bytes_.data() + head_, count), std::move(handler));
        });
        if (lifecycle_ != nullptr) {
            lifecycle_->note_write(result_value.result());
        }
        if (result_value.error_code()) {
            fail(result_value.error_code());
            co_return;
        }
        head_ = (head_ + result_value.result()) % bytes_.size();
        size_ -= result_value.result();
        flushed_.notify();
    }
}
task<std::error_code> tls_tunnel_output::flush() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    while (!error_ && size_ != 0) {
        co_await flushed_.wait();
    }
    co_return error_;
}
task<std::error_code> tls_tunnel_output::finish() {
    if (!worker_.is_current() || !started_) {
        std::terminate();
    }
    if (error_) {
        co_return error_;
    }
    if (!send_ended_) {
        ERR_clear_error();
        const auto result_value = SSL_shutdown(&ssl_);
        if (result_value < 0) {
            const auto code = ERR_get_error();
            fail(code == 0 ? std::make_error_code(std::errc::protocol_error) : std::error_code(static_cast<int>(code), asio::error::get_ssl_category()));
            co_return error_;
        }
        send_ended_ = true;
    }
    co_return co_await flush();
}
task<void> tls_tunnel_output::join() {
    stopping_ = true;
    available_.notify();
    co_await tasks_.join();
}

}  // namespace ruvia::detail
