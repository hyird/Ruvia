#include "ruvia/web/http_tunnel.h"

#include <array>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/http_client.h"

#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "http/http_capsule_stream_state.h"
#include "util/operation_lane_lease.h"
#include "util/pmr_owned_storage.h"

namespace ruvia {
namespace {
constexpr const char* tunnel_lane_in_progress = "HTTP tunnel operation already in progress on this direction";
class running_tunnel_operation final {
public:
    explicit running_tunnel_operation(unsigned& count) noexcept
        : count_(count) {
        ++count_;
    }
    ~running_tunnel_operation() {
        --count_;
    }

private:
    unsigned& count_;
};
task<std::optional<std::pmr::string>> read_tunnel(void* target,
    task<std::optional<std::pmr::string>> (*read)(void*), bool& read_active, unsigned& running) {
    auto lease = detail::claim_operation_lane(read_active, tunnel_lane_in_progress);
    running_tunnel_operation active(running);
    co_return co_await read(target);
}
task<void> write_tunnel(void* target, task<void> (*write)(void*, std::string_view),
    std::pmr::string bytes_value, bool& write_active, bool& ended, unsigned& running) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    running_tunnel_operation active(running);
    co_await write(target, bytes_value);
}
task<void> finish_tunnel(void* target, task<void> (*finish_value)(void*),
    bool& ended, bool& write_active, unsigned& running) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    running_tunnel_operation active(running);
    if (!ended) {
        co_await finish_value(target);
        ended = true;
    }
}
}  // namespace

http_tunnel::http_tunnel(std::pmr::memory_resource& resource, const worker_handle& worker_value,
    void* target, read_type read, write_type write, finish_type finish_value, abort_type abort) noexcept
    : resource_(resource),
      worker_(worker_value),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish_value),
      abort_(abort) {
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
task<std::optional<std::pmr::string>> http_tunnel::read() & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_, read_active_, running_operations_));
}
task<void> http_tunnel::write(std::string_view bytes_value) & {
    require_active();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    return write_owned(std::pmr::string(bytes_value, &resource_));
}
task<void> http_tunnel::write(std::span<const std::byte> bytes_value) & {
    return write(as_chars(bytes_value));
}
task<void> http_tunnel::write_owned(std::pmr::string&& bytes_value) {
    require_active();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    auto owned = detail::rebind_owned_storage(bytes_value, &resource_);
    return ::ruvia::make_scoped_operation(operations_, write_tunnel(target_, write_, std::move(owned), write_active_, send_ended_, running_operations_));
}
task<void> http_tunnel::finish() & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, finish_tunnel(target_, finish_, send_ended_, write_active_, running_operations_));
}
void http_tunnel::abort() noexcept {
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
    state_ = make_pmr_object<detail::http_capsule_stream_state>(&tunnel.resource_, tunnel, config).release();
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
http_capsule_stream http_tunnel::capsules(http_capsule_config config) & {
    return http_capsule_stream(*this, config);
}
http_capsule_stream http_client_tunnel::capsules(http_capsule_config config) && {
    return http_capsule_stream(std::move(*this), config);
}
task<std::optional<http_capsule>> http_capsule_stream::read() & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    return ::ruvia::make_scoped_operation(state_->read_scope_, read_owned(detail::capsule_state_pin(*state_)));
}
task<std::optional<http_capsule>> http_capsule_stream::read_owned(detail::capsule_state_pin pin) {
    auto& state_value = pin.state();
    detail::running_capsule_operation running(state_value, false);
    if (state_value.receive_ended_) {
        co_return std::nullopt;
    }
    for (;;) {
        bool fin{};
        if (state_value.offset_ == state_value.input_.size()) {
            fin = !co_await state_value.read_input();
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
}
task<void> http_capsule_stream::write(std::uint64_t type, std::span<const std::byte> payload_value) & {
    return write(type, std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()));
}
task<void> http_capsule_stream::write(std::uint64_t type, std::string_view payload_value) & {
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
    return write_frame(std::string_view(header.data(), std::get<0>(encoded)), payload_value);
}
task<void> http_capsule_stream::write_frame(std::string_view prefix, std::string_view payload_value) {
    const auto write = +[](detail::capsule_write_input input) -> task<void> {
        auto& state_value = input.pin_.state();
        state_value.require();
        if (state_value.send_ended_) {
            throw std::logic_error("capsule sending direction is closed");
        }
        detail::running_capsule_operation running(state_value, true);
        try {
            co_await state_value.send_bytes(std::move(input.bytes_));
        } catch (...) {
            state_value.abort();
            throw;
        }
    };
    std::pmr::string bytes_value(state_->resource_);
    bytes_value.reserve(prefix.size() + payload_value.size());
    bytes_value.append(prefix);
    bytes_value.append(payload_value);
    return ::ruvia::make_scoped_operation(state_->output_scope_, write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(bytes_value)}));
}
task<void> http_capsule_stream::finish() & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require();
    const auto finish_value = +[](detail::capsule_state_pin pin) -> task<void> {
        auto& state_value = pin.state();
        detail::running_capsule_operation running(state_value, true);
        if (!state_value.send_ended_) {
            if (state_value.server_) {
                co_await state_value.server_->finish();
            } else {
                co_await state_value.client_->finish();
            }
            state_value.send_ended_ = true;
        }
    };
    return ::ruvia::make_scoped_operation(state_->output_scope_, finish_value(detail::capsule_state_pin(*state_)));
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
task<std::optional<http_datagram>> http_datagram_stream::read() & {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    stream_.state_->require();
    return ::ruvia::make_scoped_operation(stream_.state_->read_scope_, read_owned(detail::capsule_state_pin(*stream_.state_), false));
}
task<std::optional<http_datagram>> http_datagram_stream::read_owned(detail::capsule_state_pin pin, bool udp) {
    auto& state_value = pin.state();
    detail::running_capsule_operation running(state_value, false);
    for (;;) {
        if (state_value.receive_ended_) {
            co_return std::nullopt;
        }
        bool fin{};
        if (state_value.offset_ == state_value.input_.size()) {
            fin = !co_await state_value.read_input();
        }
        // The decoder drops other capsule types and, for CONNECT-UDP, unknown
        // Context IDs before the length limit applies (RFC 9297 section 3.2).
        if (!state_value.poll_capsule(
                fin, udp ? http_capsule_retention::udp_payloads : http_capsule_retention::datagrams)) {
            continue;
        }
        auto& owned = state_value.payload_;
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
                owned.clear();
                continue;
            }
            offset = bytes_value.size() - (std::get<0>(decoded))->payload_.size();
        } else {
            auto decoded = datagrams.receive_datagram(bytes_value);
            if (!decoded) {
                owned.clear();
                continue;
            }
            offset = bytes_value.size() - decoded->size();
        }
        auto result_value = std::move(owned);
        state_value.payload_ = std::pmr::string(state_value.resource_);
        co_return http_datagram(http_capsule(state_value, http_datagram_capsule_type, std::move(result_value)), offset);
    }
}
task<void> http_datagram_stream::send(std::span<const std::byte> payload_value) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()));
}
task<void> http_datagram_stream::send(std::string_view payload_value) & {
    return send_payload(payload_value, false);
}
task<void> http_datagram_stream::send_payload(std::string_view payload_value, bool udp) {
    if (!stream_.state_) {
        throw std::logic_error("HTTP Datagram stream is empty");
    }
    auto& state_value = *stream_.state_;
    state_value.require();
    if (state_value.send_ended_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    http_datagram_session session_value;
    const auto plan = udp ? session_value.prepare_udp_datagram(std::span(payload_value.data(), payload_value.size()))
                          : session_value.prepare_datagram(std::span(payload_value.data(), payload_value.size()));
    if (plan.index() != 0) {
        throw std::length_error("HTTP Datagram payload exceeds its bound");
    }
    if (payload_value.size() + (udp ? 1 : 0) > state_value.config_.max_capsule_length_) {
        throw std::length_error("HTTP Datagram exceeds capsule bound");
    }
    return stream_.write_frame(std::get<0>(plan).prefix(), payload_value);
}
task<std::optional<http_datagram>> http_udp_tunnel::read() & {
    auto* state_value = stream_.stream_.state_;
    if (!state_value) {
        throw std::logic_error("UDP tunnel is empty");
    }
    state_value->require();
    return ::ruvia::make_scoped_operation(state_value->read_scope_, http_datagram_stream::read_owned(detail::capsule_state_pin(*state_value), true));
}
task<void> http_udp_tunnel::send(std::span<const std::byte> payload_value) & {
    return send(std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()));
}
task<void> http_udp_tunnel::send(std::string_view payload_value) & {
    return stream_.send_payload(payload_value, true);
}
}  // namespace ruvia
