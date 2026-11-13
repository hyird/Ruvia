#include "ruvia/web/websocket.h"

#include "ruvia/core/bytes.h"
#include "ruvia/http/websocket_connection.h"

#include "util/operation_lane_lease.h"
#include "util/pmr_owned_storage.h"
#include "websocket/websocket_state.h"

namespace {
ruvia::task<std::optional<ruvia::websocket_message>> read_websocket(void* target,
    ruvia::task<std::optional<ruvia::websocket_message>> (*read)(void*, ruvia::operation_options),
    bool& read_active, ruvia::operation_options operation) {
    auto lease = ruvia::detail::claim_operation_lane(read_active, "concurrent websocket reads are not supported");
    co_return co_await read(target, std::move(operation));
}

using websocket_write_type = ruvia::task<void> (*)(void*, ruvia::websocket_opcode, std::string_view, bool,
    ruvia::detail::websocket_write_extent, ruvia::operation_options);

// The final fragment releases the data lane of its fragmented message.
ruvia::task<void> write_websocket_payload(void* target, websocket_write_type write, ruvia::websocket_opcode opcode,
    std::pmr::string payload_value, bool& write_active, bool compress, ruvia::detail::websocket_write_extent extent,
    ruvia::operation_options operation, bool& fragmented_message_active) {
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, opcode, payload_value, compress, extent, std::move(operation));
    if (extent == ruvia::detail::websocket_write_extent::final_fragment) {
        fragmented_message_active = false;
    }
}

// The read leases are claimed by the caller before this task exists: the
// payload borrows the connection's read storage, and no read may run, even one
// started while this operation is still unstarted, until the frame has been written.
ruvia::task<void> write_websocket_message(void* target, websocket_write_type write,
    ruvia::websocket_message message, ruvia::detail::operation_lane_lease read_lease,
    ruvia::detail::operation_lane_lease message_lease, bool& write_active, bool compress,
    ruvia::operation_options operation) {
    auto held_read = std::move(read_lease);
    auto held_message = std::move(message_lease);
    auto lease = ruvia::detail::claim_operation_lane(write_active, "concurrent websocket output operations are not supported");
    co_await write(target, message.opcode(), message.payload(), compress, ruvia::detail::websocket_write_extent::message,
        std::move(operation));
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
websocket::websocket(std::shared_ptr<detail::websocket_state> state) noexcept
    : state_(std::move(state)) {}
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
task<void> websocket::write_owned(websocket_opcode opcode, std::pmr::string&& payload, bool compress, operation_options operation) {
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
const worker_handle& websocket::worker() const& {
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
    return make_scoped_operation(operation_scope_,
        write_websocket_message(target_, write_, message, std::move(read_lease), std::move(message_lease),
            write_active_, options.compress_, std::move(operation)));
}

task<void> detail::websocket_state::close(websocket_close_options options, operation_options operation) & {
    require_active();
    operation = override_operation_options(defaults_, std::move(operation));
    std::pmr::string owned(options.reason_.view(), resource_);
    return make_scoped_operation(operation_scope_,
        close_websocket_with_reason(target_, close_, options, std::move(owned), std::move(operation),
            read_active_, write_active_, message_send_active_));
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

task<void> detail::websocket_state::write(websocket_opcode opcode, std::string_view payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    std::pmr::string owned(payload_value, resource_);
    return write(opcode, std::move(owned), compress, std::move(operation), extent);
}

task<void> detail::websocket_state::write(websocket_opcode opcode, std::pmr::string&& payload_value, bool compress, operation_options operation,
    detail::websocket_write_extent extent) {
    require_active();
    if (extent == detail::websocket_write_extent::message && fragmented_message_active_ &&
        (opcode == websocket_opcode::text || opcode == websocket_opcode::binary)) {
        throw std::logic_error("a fragmented websocket message is in progress");
    }
    operation = override_operation_options(defaults_, std::move(operation));
    auto owned = detail::rebind_owned_storage(payload_value, resource_);
    return make_scoped_operation(operation_scope_,
        write_websocket_payload(target_, write_, opcode, std::move(owned), write_active_, compress, extent,
            std::move(operation), fragmented_message_active_));
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

websocket_message_writer::websocket_message_writer(detail::websocket_state& owner, websocket_opcode opcode, bool compress) noexcept
    : owner_(&owner),
      message_id_(owner.fragmented_message_sequence_),
      opcode_(opcode),
      compress_(compress),
      registration_(owner.operation_scope_, this, &websocket_message_writer::expire_capability) {}

websocket_message_writer::websocket_message_writer(websocket_message_writer&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
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
    return owner_ != nullptr && owner_->fragmented_message_active_ &&
           owner_->fragmented_message_sequence_ == message_id_;
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
    return owner_->write(opcode_, chunk, compress_, std::move(operation),
        final ? detail::websocket_write_extent::final_fragment : detail::websocket_write_extent::fragment);
}

}  // namespace ruvia
