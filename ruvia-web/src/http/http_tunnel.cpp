#include "ruvia/web/http_tunnel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/web/http_client.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "util/operation_interrupt.h"
#include "util/operation_lane_lease.h"
#include "util/pmr_owned_storage.h"

namespace ruvia::detail {
class http_capsule_stream_state final {
public:
    http_capsule_stream_state(http_tunnel& tunnel, http_capsule_config config)
        : worker_(tunnel.worker_),
          resource_(&tunnel.resource_),
          server_(&tunnel),
          decoder_(config),
          config_(config),
          payload_(resource_) {}
    http_capsule_stream_state(http_client_tunnel&& tunnel, http_capsule_config config)
        : worker_(tunnel.response_.state_->memory_domain().worker()),
          resource_(tunnel.response_.state_->resource_),
          memory_pin_(http_client_response(tunnel.response_.state_)),
          client_(std::move(tunnel)),
          decoder_(config),
          config_(config),
          payload_(resource_) {}
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
        auto* allocator = resource_;
        std::destroy_at(this);
        allocator->deallocate(this, sizeof(http_capsule_stream_state), alignof(http_capsule_stream_state));
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
            throw http_client_error(std::make_error_code(cause == operation_interrupt_cause::timed_out
                                                             ? std::errc::timed_out
                                                             : std::errc::operation_canceled),
                operation_name);
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
    [[nodiscard]] task<void> send_bytes(std::pmr::string&& bytes_value, const operation_steps& steps) {
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
    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    std::optional<http_client_response> memory_pin_;
    http_tunnel* server_{};
    std::optional<http_client_tunnel> client_;
    http_capsule_decoder decoder_;
    http_capsule_config config_;
    std::optional<std::pmr::string> owned_input_;
    std::string_view input_;
    std::size_t offset_{};
    bool native_input_{};
    std::pmr::string payload_;
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
    explicit capsule_state_pin(http_capsule_stream_state& state_value) noexcept
        : state_(&state_value) {
        state_->retain();
    }
    capsule_state_pin(capsule_state_pin&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
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
    std::pmr::string bytes_;
    bool datagram_{};
    operation_options operation_;
};
// Failure aborts the stream, except an interrupted read, which keeps the
// partly received capsule for the next read.
class running_capsule_operation final {
public:
    running_capsule_operation(http_capsule_stream_state& state_value, bool output)
        : state_(state_value),
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
task<std::optional<input_type>> read_tunnel(void* target,
    task<std::optional<input_type>> (*read)(void*, operation_options), bool& read_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, tunnel_lane_in_progress);
    co_return co_await read(target, std::move(operation));
}
task<void> write_tunnel(void* target, task<void> (*write)(void*, std::string_view, operation_options),
    std::pmr::string bytes_value, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_await write(target, bytes_value, std::move(operation));
}
task<bool> write_tunnel_datagram(void* target, task<bool> (*write)(void*, std::string_view, operation_options),
    std::string_view bytes, bool& write_active, bool& ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    co_return co_await write(target, bytes, std::move(operation));
}
task<void> finish_tunnel(void* target, task<void> (*finish_value)(void*, operation_options),
    bool& ended, bool& write_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (!ended) {
        co_await finish_value(target, std::move(operation));
        ended = true;
    }
}
}  // namespace

http_tunnel::http_tunnel(std::pmr::memory_resource& resource, const worker_handle& worker_value,
    void* target, read_type read, write_type write, finish_type finish_value, abort_type abort,
    bool datagrams, read_datagram_type read_datagram, write_datagram_type write_datagram) noexcept
    : resource_(resource),
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
task<std::optional<std::pmr::string>> http_tunnel::read_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_, read_active_, std::move(operation)));
}
task<std::optional<detail::http_tunnel_input>> http_tunnel::read_datagram_raw(operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, read_tunnel(target_, read_datagram_, read_active_, std::move(operation)));
}
task<bool> http_tunnel::write_datagram_raw(std::string_view bytes, operation_options operation) {
    require_active();
    return ::ruvia::make_scoped_operation(operations_,
        write_tunnel_datagram(target_, write_datagram_, bytes, write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::write(std::string_view bytes_value, operation_options operation) & {
    require_active();
    require_unadapted();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    return write_raw(std::pmr::string(bytes_value, &resource_), std::move(operation));
}
task<void> http_tunnel::write(std::span<const std::byte> bytes_value, operation_options operation) & {
    return write(as_chars(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_owned(std::pmr::string&& bytes_value, operation_options operation) {
    require_active();
    require_unadapted();
    return write_raw(std::move(bytes_value), std::move(operation));
}
task<void> http_tunnel::write_raw(std::pmr::string&& bytes_value, operation_options operation) {
    require_active();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    auto owned = detail::rebind_owned_storage(bytes_value, &resource_);
    return ::ruvia::make_scoped_operation(operations_,
        write_tunnel(target_, write_, std::move(owned), write_active_, send_ended_, std::move(operation)));
}
task<void> http_tunnel::finish(operation_options operation) & {
    require_active();
    return ::ruvia::make_scoped_operation(
        operations_, finish_tunnel(target_, finish_, send_ended_, write_active_, std::move(operation)));
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

http_capsule::http_capsule(detail::http_capsule_stream_state& state_value, std::uint64_t type, std::pmr::string payload_value) noexcept
    : state_(&state_value),
      type_(type),
      payload_(std::move(payload_value)) {
    state_->retain();
}
http_capsule::http_capsule(http_capsule&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
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
    state_ = make_pmr_object<detail::http_capsule_stream_state>(&tunnel.resource_, tunnel, config).release();
    tunnel.adapted_ = true;
}
http_capsule_stream::http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config) {
    if (!tunnel.response_.state_ || !tunnel.response_.state_->memory_domain().worker().is_current()) {
        throw std::logic_error("capsule stream requires a live tunnel on its worker");
    }
    // Validate before consuming the caller's owner.
    http_capsule_decoder validation(config);
    auto* resource = tunnel.response_.state_->resource_;
    state_ = make_pmr_object<detail::http_capsule_stream_state>(resource, std::move(tunnel), config).release();
}
http_capsule_stream::http_capsule_stream(http_capsule_stream&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
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
    return ::ruvia::make_scoped_operation(
        state_->read_scope_, read_owned(detail::capsule_state_pin(*state_), std::move(operation)));
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
                state_value.payload_ = std::pmr::string(state_value.resource_);
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
    return write(type, std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
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
task<void> http_capsule_stream::write_frame(
    std::string_view prefix, std::string_view payload_value, bool datagram, operation_options operation) {
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
    std::pmr::string bytes_value(state_->resource_);
    bytes_value.reserve(prefix.size() + payload_value.size());
    bytes_value.append(prefix);
    bytes_value.append(payload_value);
    return ::ruvia::make_scoped_operation(state_->output_scope_,
        write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(bytes_value), datagram, std::move(operation)}));
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
    return ::ruvia::make_scoped_operation(
        state_->output_scope_, finish_value(detail::capsule_state_pin(*state_), std::move(operation)));
}

http_datagram_stream::http_datagram_stream(http_capsule_stream&& stream)
    : stream_(std::move(stream)) {
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
    return ::ruvia::make_scoped_operation(
        stream_.state_->read_scope_, read_owned(detail::capsule_state_pin(*stream_.state_), false, std::move(operation)));
}
task<std::optional<http_datagram>> http_datagram_stream::read_owned(
    detail::capsule_state_pin pin, bool udp, operation_options operation) {
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
            if (!state_value.native_input_ && !state_value.poll_capsule(
                                                  fin, udp ? http_capsule_retention::udp_payloads : http_capsule_retention::datagrams)) {
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
            auto result_value = detail::rebind_owned_storage(owned, state_value.resource_);
            if (native) {
                state_value.clear_input();
            } else {
                state_value.payload_ = std::pmr::string(state_value.resource_);
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
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_datagram_stream::send(std::string_view payload_value, operation_options operation) & {
    return send_payload(payload_value, false, std::move(operation));
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
    if (context_bytes > state_value.config_.max_capsule_length_ ||
        payload_value.size() > state_value.config_.max_capsule_length_ - context_bytes) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size()))
                          : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (state_value.has_native_transport()) {
        return stream_.write_frame(udp ? std::string_view("\0", 1) : std::string_view{}, payload_value, true, std::move(operation));
    }
    return stream_.write_frame(std::get<0>(plan).prefix(), payload_value, false, std::move(operation));
}
task<std::optional<http_datagram>> http_udp_tunnel::read(operation_options operation) & {
    auto* state_value = stream_.stream_.state_;
    if (!state_value) {
        throw std::logic_error("UDP tunnel is empty");
    }
    state_value->require();
    return ::ruvia::make_scoped_operation(state_value->read_scope_,
        http_datagram_stream::read_owned(detail::capsule_state_pin(*state_value), true, std::move(operation)));
}
task<void> http_udp_tunnel::send(std::span<const std::byte> payload_value, operation_options operation) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()),
        std::move(operation));
}
task<void> http_udp_tunnel::send(std::string_view payload_value, operation_options operation) & {
    return stream_.send_payload(payload_value, true, std::move(operation));
}
}  // namespace ruvia
