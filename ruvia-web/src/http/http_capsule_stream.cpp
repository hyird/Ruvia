#include "ruvia/web/http_capsule_stream.h"

#include <array>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_tunnel.h"

#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "http/http_capsule_stream_state.h"

namespace ruvia {
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
    state_ = detail::make_pmr_object<detail::http_capsule_stream_state>(&tunnel.resource_, tunnel, config).release();
}
http_capsule_stream::http_capsule_stream(http_client_tunnel&& tunnel, http_capsule_config config) {
    if (!tunnel.response_.state_ || !tunnel.response_.state_->memory_domain()->worker().is_current()) {
        throw std::logic_error("capsule stream requires a live tunnel on its worker");
    }
    // Validate before consuming the caller's owner.
    http_capsule_decoder validation(config);
    auto* resource = tunnel.response_.state_->resource_;
    state_ = detail::make_pmr_object<detail::http_capsule_stream_state>(resource, std::move(tunnel), config).release();
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
scoped_operation<std::optional<http_capsule>> http_capsule_stream::read() & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require(false);
    return ::ruvia::make_scoped_operation(state_->read_scope_, read_owned(detail::capsule_state_pin(*state_)));
}
task<std::optional<http_capsule>> http_capsule_stream::read_owned(detail::capsule_state_pin pin) {
    auto& state_value = pin.state();
    detail::running_capsule_operation running(state_value);
    if (state_value.receive_ended_) {
        co_return std::nullopt;
    }
    for (;;) {
        bool fin{};
        if (state_value.offset_ == state_value.input_.size()) {
            fin = !co_await state_value.read_input();
        }
        if (state_value.poll_capsule(fin, detail::capsule_interest::all)) {
            auto payload_value = std::move(state_value.payload_);
            state_value.payload_ = std::pmr::string(state_value.resource_);
            co_return http_capsule(state_value, state_value.type_, std::move(payload_value));
        }
        if (state_value.receive_ended_) {
            co_return std::nullopt;
        }
    }
}
scoped_operation<void> http_capsule_stream::write(std::uint64_t type, std::span<const std::byte> payload_value) & {
    return write(type, std::string_view(reinterpret_cast<const char*>(payload_value.data()), payload_value.size()));
}
scoped_operation<void> http_capsule_stream::write(std::uint64_t type, std::string_view payload_value) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require(true);
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
scoped_operation<void> http_capsule_stream::write_frame(std::string_view prefix, std::string_view payload_value) {
    const auto write = +[](detail::capsule_write_input input) -> task<void> {
        auto& state_value = input.pin_.state();
        detail::running_capsule_operation running(state_value);
        try {
            co_await state_value.send_bytes(input.bytes_);
        } catch (...) {
            state_value.abort();
            throw;
        }
    };
    std::pmr::string bytes_value(prefix, state_->resource_);
    bytes_value.append(payload_value);
    return ::ruvia::make_scoped_operation(state_->output_scope_, write(detail::capsule_write_input{detail::capsule_state_pin(*state_), std::move(bytes_value)}));
}
scoped_operation<void> http_capsule_stream::finish() & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require(true);
    const auto finish_value = +[](detail::capsule_state_pin pin) -> task<void> {
        auto& state_value = pin.state();
        detail::running_capsule_operation running(state_value);
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
}  // namespace ruvia
