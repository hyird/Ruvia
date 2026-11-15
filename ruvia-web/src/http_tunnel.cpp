#include "http_tunnel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "byte_view.h"
#include "http_client.h"
#include "http_client_pool.h"
#include "http_connect_udp.h"
#include "inbound_buffer.h"
#include "io_operation.h"
#include "task.h"
#include "task_scope.h"
#include "worker_ref.h"
namespace ruvia::detail {
class http_capsule_stream_state final {
public:
    http_capsule_stream_state(http_tunnel& tunnel, http_capsule_config config) :
        worker_(tunnel.worker_),
        tunnel_(&tunnel),
        decoder_(config),
        config_(config),
        inbound_(tunnel.inbound_),
        payload_reservation_(inbound_) {
        if (tunnel.client_ != nullptr) {
            memory_pin_.emplace(http_client_response(tunnel.client_));
        }
    }
    // A client tunnel whose owner was destroyed (which closes its output
    // scope) or whose pool has shut down: the pinned response state keeps
    // results and the borrowed input readable, the transport is gone.
    [[nodiscard]] bool client_closed() const noexcept {
        return memory_pin_ && (memory_pin_->state_->pool_ == nullptr || !memory_pin_->state_->tunnel_->output_.output_scope_.active());
    }
    void require() const {
        if (closed_ || client_closed()) {
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
        if (client_closed()) {
            closed_ = true;
            return;
        }
        if (!worker_.is_current()) {
            std::terminate();
        }
        closed_ = true;
        tunnel_->abort_(tunnel_->target_, malformed);
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
        closed_ = true;
        read_scope_.close();
        output_scope_.close();
        clear_input();
        tunnel_ = nullptr;
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
        if (memory_pin_) {
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
            auto chunk = co_await tunnel_->read_datagram_(tunnel_->target_, next_step(steps, "capsule read"));
            if (!chunk) {
                co_return false;
            }
            native_input_ = chunk->datagram_;
            owned_input_.emplace(std::move(chunk->bytes_));
            input_ = *owned_input_;
            co_return true;
        }
        // The tunnel keeps this chunk until its next read starts.
        const auto chunk = co_await tunnel_->read_(tunnel_->target_, next_step(steps, "capsule read"));
        if (!chunk) {
            co_return false;
        }
        input_ = as_chars(*chunk);
        co_return true;
    }
    [[nodiscard]] bool has_native_transport() const noexcept {
        return tunnel_->read_datagram_ != nullptr && tunnel_->write_datagram_ != nullptr;
    }
    [[nodiscard]] task<bool> send_datagram(std::string_view bytes, const operation_steps& steps) {
        return tunnel_->write_datagram_(tunnel_->target_, bytes, next_step(steps, "capsule write"));
    }
    [[nodiscard]] task<void> send_bytes(std::string&& bytes_value, const operation_steps& steps) {
        // A client tunnel bounds each write: a larger frame goes out in order.
        const auto limit = memory_pin_ ? memory_pin_->state_->tunnel_->config_.max_chunk_bytes_ : bytes_value.size();
        if (bytes_value.size() <= limit) {
            co_await tunnel_->write_(tunnel_->target_, std::move(bytes_value), next_step(steps, "capsule write"));
            co_return;
        }
        std::string_view remaining = bytes_value;
        while (!remaining.empty()) {
            const auto chunk = remaining.substr(0, limit);
            co_await tunnel_->write_(tunnel_->target_, std::string(chunk), next_step(steps, "capsule write"));
            remaining.remove_prefix(chunk.size());
        }
    }
    [[nodiscard]] task<void> finish_output(operation_options operation) {
        return tunnel_->finish_(tunnel_->target_, std::move(operation));
    }
    const worker_ref& worker_;
    std::optional<http_client_response> memory_pin_;
    http_tunnel* tunnel_{};
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
    detail::operation_scope read_scope_;
    detail::operation_scope output_scope_;
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
task<std::optional<std::string_view>> read_text_chunk(task<std::optional<std::span<const std::byte>>> read) {
    const auto chunk = co_await std::move(read);
    if (!chunk) {
        co_return std::nullopt;
    }
    co_return as_chars(*chunk);
}
}  // namespace

void http_tunnel::require_unadapted() const {
    if (adapted_) {
        throw std::logic_error("HTTP tunnel is already adapted to a capsule stream");
    }
}
task<std::optional<std::span<const std::byte>>> http_tunnel::read(operation_options operation) & {
    require_unadapted();
    return read_(target_, std::move(operation));
}
task<std::optional<std::string_view>> http_tunnel::read_text(operation_options operation) & {
    return read_text_chunk(read(std::move(operation)));
}
task<void> http_tunnel::write(outbound_bytes bytes_value, operation_options operation) & {
    require_unadapted();
    return write_(target_, std::move(bytes_value).into_string(), std::move(operation));
}
task<void> http_tunnel::finish(operation_options operation) & {
    return finish_(target_, std::move(operation));
}
void http_tunnel::abort() & noexcept {
    abort_(target_, false);
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
    if (!tunnel.worker_.is_current()) {
        throw std::logic_error("capsule stream requires its owner worker");
    }
    state_ = new detail::http_capsule_stream_state(tunnel, config);
    tunnel.adapted_ = true;
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
http_capsule_stream http_tunnel::capsules(http_capsule_config config) & {
    // The tunnel's protocol owns Capsule Protocol negotiation (RFC 9297
    // section 3.4); a server route that did not declare datagrams never
    // advertised it.
    if (!negotiated_.capsules_) {
        throw std::logic_error("HTTP tunnel did not negotiate the Capsule Protocol");
    }
    require_unadapted();
    return http_capsule_stream(*this, config);
}
task<std::optional<http_capsule>> http_capsule_stream::read(operation_options operation) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    return state_->read_scope_.attach(read_owned(detail::capsule_state_pin(*state_), std::move(operation)));
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
task<void> http_capsule_stream::write(std::uint64_t type, outbound_bytes payload_value, operation_options operation) & {
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
    return write_frame(std::string_view(header.data(), std::get<0>(encoded)), std::move(payload_value), false, std::move(operation));
}
task<void> http_capsule_stream::write_frame(std::string_view prefix, outbound_bytes payload_value, bool datagram, operation_options operation) {
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
    // An owned payload moves only when nothing precedes it; otherwise one
    // buffer receives prefix and payload.
    std::string bytes_value;
    if (prefix.empty()) {
        bytes_value = std::move(payload_value).into_string();
    } else {
        const auto payload_view = payload_value.view();
        bytes_value.reserve(prefix.size() + payload_view.size());
        bytes_value.append(prefix);
        bytes_value.append(payload_view);
    }
    return state_->output_scope_.attach(write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(bytes_value), datagram, std::move(operation)}));
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
            co_await state_value.finish_output(std::move(finish_operation));
            state_value.send_ended_ = true;
        }
    };
    return state_->output_scope_.attach(finish_value(detail::capsule_state_pin(*state_), std::move(operation)));
}

http_datagram_stream::http_datagram_stream(http_capsule_stream&& stream, bool udp) :
    stream_(std::move(stream)),
    udp_(udp) {}
http_datagram_stream http_tunnel::datagram_stream(http_capsule_config config, bool udp) {
    if (!negotiated_.datagrams_) {
        throw std::logic_error("HTTP tunnel did not negotiate HTTP Datagrams");
    }
    return http_datagram_stream(capsules(config), udp);
}
http_datagram_stream http_tunnel::datagrams(http_capsule_config config) & {
    return datagram_stream(config, false);
}
http_datagram_stream http_tunnel::udp(http_capsule_config config) & {
    return datagram_stream(config, true);
}
task<std::optional<http_datagram>> http_datagram_stream::read(operation_options operation) & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->require();
    return stream_.state_->read_scope_.attach(read_owned(detail::capsule_state_pin(*stream_.state_), udp_, std::move(operation)));
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
task<void> http_datagram_stream::send(outbound_bytes payload_value, operation_options operation) & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto context_bytes = udp_ ? std::size_t{1} : std::size_t{0};
    if (context_bytes > state_value.config_.max_capsule_length_ || payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    const auto payload_view = payload_value.view();
    http_datagram_session session_value;
    const auto plan = udp_ ? session_value.prepare_udp_datagram(std::span(payload_view.data(), payload_view.size())) : session_value.prepare_datagram(std::span(payload_view.data(), payload_view.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame(udp_ ? std::string_view("\0", 1) : std::string_view{}, std::move(payload_value), true, std::move(operation));
    }
    return stream_.write_frame(std::get<0>(plan).prefix(), std::move(payload_value), false, std::move(operation));
}
}  // namespace ruvia