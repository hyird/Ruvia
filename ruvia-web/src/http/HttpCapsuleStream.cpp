#include "ruvia/web/HttpCapsuleStream.h"

#include <array>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/HttpClientTunnel.h"
#include "ruvia/web/HttpTunnel.h"
#include "ruvia/web/detail/client/HttpClientResponseMemory.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/http/HttpCapsuleStreamState.h"

namespace ruvia {
HttpCapsule::HttpCapsule(detail::HttpCapsuleStreamState& state, std::uint64_t type, std::pmr::string payload) noexcept
    : state_(&state),
      type_(type),
      payload_(std::move(payload)) {
    state_->retain();
}
HttpCapsule::HttpCapsule(HttpCapsule&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      type_(other.type_),
      payload_(std::move(other.payload_)) {
    other.payload_.reset();
}
HttpCapsule& HttpCapsule::operator=(HttpCapsule&& other) noexcept {
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
HttpCapsule::~HttpCapsule() {
    release();
}
void HttpCapsule::release() noexcept {
    // Payload storage is returned before its domain pin can retire the pool.
    if (state_ && !state_->worker.isCurrent()) {
        std::terminate();
    }
    payload_.reset();
    if (auto* state = std::exchange(state_, nullptr)) {
        state->release();
    }
}
HttpCapsuleStream::HttpCapsuleStream(HttpTunnel& tunnel, HttpCapsuleConfig config) {
    tunnel.requireActive();
    state_ = detail::makePmrObject<detail::HttpCapsuleStreamState>(&tunnel.resource_, tunnel, config).release();
}
HttpCapsuleStream::HttpCapsuleStream(HttpClientTunnel&& tunnel, HttpCapsuleConfig config) {
    if (!tunnel.response_.state_ || !tunnel.response_.state_->memoryDomain()->worker().isCurrent()) {
        throw std::logic_error("capsule stream requires a live tunnel on its worker");
    }
    // Validate before consuming the caller's owner.
    HttpCapsuleDecoder validation(config);
    auto* resource = tunnel.response_.state_->resource;
    state_ = detail::makePmrObject<detail::HttpCapsuleStreamState>(resource, std::move(tunnel), config).release();
}
HttpCapsuleStream::HttpCapsuleStream(HttpCapsuleStream&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {}
HttpCapsuleStream& HttpCapsuleStream::operator=(HttpCapsuleStream&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
}
HttpCapsuleStream::~HttpCapsuleStream() {
    release();
}
void HttpCapsuleStream::release() noexcept {
    if (auto* state = std::exchange(state_, nullptr)) {
        state->close();
        state->release();
    }
}
void HttpCapsuleStream::abort() & noexcept {
    if (state_) {
        state_->abort();
    }
}
HttpCapsuleStream HttpTunnel::capsules(HttpCapsuleConfig config) & {
    return HttpCapsuleStream(*this, config);
}
HttpCapsuleStream HttpClientTunnel::capsules(HttpCapsuleConfig config) && {
    return HttpCapsuleStream(std::move(*this), config);
}
ScopedOperation<std::optional<HttpCapsule>> HttpCapsuleStream::read() & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require(false);
    return ::ruvia::make_scoped_operation(state_->readScope, readOwned(detail::CapsuleStatePin(*state_)));
}
Task<std::optional<HttpCapsule>> HttpCapsuleStream::readOwned(detail::CapsuleStatePin pin) {
    auto& state = pin.state();
    detail::RunningCapsuleOperation running(state);
    if (state.receiveEnded) {
        co_return std::nullopt;
    }
    for (;;) {
        bool fin{};
        if (state.offset == state.input.size()) {
            fin = !co_await state.readInput();
        }
        if (state.pollCapsule(fin, detail::CapsuleInterest::kAll)) {
            auto payload = std::move(state.payload);
            state.payload = std::pmr::string(state.resource);
            co_return HttpCapsule(state, state.type, std::move(payload));
        }
        if (state.receiveEnded) {
            co_return std::nullopt;
        }
    }
}
ScopedOperation<void> HttpCapsuleStream::write(std::uint64_t type, std::span<const std::byte> payload) & {
    return write(type, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
}
ScopedOperation<void> HttpCapsuleStream::write(std::uint64_t type, std::string_view payload) & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require(true);
    if (state_->sendEnded) {
        throw std::logic_error("capsule sending direction is closed");
    }
    std::array<char, 16> header;
    const auto encoded = encodeHttpCapsuleHeader(header, type, payload.size());
    if (!encoded || payload.size() > state_->config.maxCapsuleLength) {
        throw std::length_error("invalid capsule type or payload length");
    }
    return writeFrame(std::string_view(header.data(), *encoded), payload);
}
ScopedOperation<void> HttpCapsuleStream::writeFrame(std::string_view prefix, std::string_view payload) {
    const auto write = +[](detail::CapsuleWriteInput input) -> Task<void> {
        auto& state = input.pin.state();
        detail::RunningCapsuleOperation running(state);
        try {
            co_await state.sendBytes(input.bytes);
        } catch (...) {
            state.abort();
            throw;
        }
    };
    std::pmr::string bytes(prefix, state_->resource);
    bytes.append(payload);
    return ::ruvia::make_scoped_operation(state_->outputScope, write(detail::CapsuleWriteInput{detail::CapsuleStatePin(*state_), std::move(bytes)}));
}
ScopedOperation<void> HttpCapsuleStream::finish() & {
    if (!state_) {
        throw std::logic_error("capsule stream is empty");
    }
    state_->require(true);
    const auto finish = +[](detail::CapsuleStatePin pin) -> Task<void> {
        auto& state = pin.state();
        detail::RunningCapsuleOperation running(state);
        if (!state.sendEnded) {
            if (state.server) {
                co_await state.server->finish();
            } else {
                co_await state.client->finish();
            }
            state.sendEnded = true;
        }
    };
    return ::ruvia::make_scoped_operation(state_->outputScope, finish(detail::CapsuleStatePin(*state_)));
}
}  // namespace ruvia
