#pragma once

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

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/http/HttpDatagram.h"
#include "ruvia/web/HttpClientTunnel.h"
#include "ruvia/web/HttpTunnel.h"

#include "client/HttpClientResponseMemory.h"
#include "client/HttpClientResponseState.h"
#include "http3/Http3ClientConnection.h"

namespace ruvia::detail {
enum class CapsuleInterest : unsigned char { kAll,
    kDatagrams };
class HttpCapsuleStreamState final {
public:
    HttpCapsuleStreamState(HttpTunnel& tunnel, HttpCapsuleConfig config)
        : worker(tunnel.worker_),
          resource(&tunnel.resource_),
          server(&tunnel),
          decoder(config),
          config(config),
          input(resource),
          payload(resource) {}
    HttpCapsuleStreamState(HttpClientTunnel&& tunnel, HttpCapsuleConfig config)
        : worker(tunnel.response_.state_->memoryDomain()->worker()),
          resource(tunnel.response_.state_->resource),
          memoryPin(HttpClientResponse(tunnel.response_.state_, true)),
          client(std::move(tunnel)),
          decoder(config),
          config(config),
          input(resource),
          payload(resource) {}
    void require(bool output) const {
        if (closed || (client && (!client->response_.state_ || client->response_.state_->pool == nullptr))) {
            throw std::logic_error("capsule stream is closed");
        }
        if (!worker.isCurrent()) {
            throw std::logic_error("capsule stream requires its owner worker");
        }
        if ((output ? outputScope : readScope).has_pending_operations()) {
            throw std::logic_error("capsule stream operation is already active");
        }
    }
    void retain() noexcept {
        ++references;
    }
    void release() noexcept {
        if (!worker.isCurrent()) {
            std::terminate();
        }
        if (--references != 0) {
            return;
        }
        // Keep the client domain alive until after this object is deallocated
        // from its pool. Capsule results and cold/running operations pin it.
        auto pin = std::move(memoryPin);
        auto* allocator = resource;
        std::destroy_at(this);
        allocator->deallocate(this, sizeof(HttpCapsuleStreamState), alignof(HttpCapsuleStreamState));
    }
    void abort() noexcept {
        if (closed) {
            return;
        }
        if (client && (!client->response_.state_ || client->response_.state_->pool == nullptr)) {
            closed = true;
            return;
        }
        if (!worker.isCurrent()) {
            std::terminate();
        }
        closed = true;
        if (server) {
            server->abort();
        } else if (client) {
            client->abort();
        }
    }
    void close() noexcept {
        if (!worker.isCurrent()) {
            std::terminate();
        }
        // The caller joins started operations after abort() wakes transport I/O.
        // Destroying their scope while an operation is retiring is forbidden.
        if (running != 0) {
            std::terminate();
        }
        if (client) {
            abort();
        } else {
            closed = true;
        }
        readScope.close();
        outputScope.close();
    }
    // Parse the currently available input synchronously. Datagram consumers
    // skip unknown capsule payloads without allocating them or recursively
    // awaiting a completed child task for each ignored capsule.
    [[nodiscard]] bool pollCapsule(bool fin, CapsuleInterest interest) {
        struct Collection {
            HttpCapsuleStreamState& state;
            CapsuleInterest interest;
        } collection{*this, interest};
        const auto collect = [](void* raw, HttpCapsuleEvent event) {
            auto& current = *static_cast<Collection*>(raw);
            current.state.type = event.type;
            if (current.interest == CapsuleInterest::kDatagrams && event.type != kHttpDatagramCapsuleType) {
                return;
            }
            if (!event.payload.empty()) {
                current.state.payload.append(event.payload.data(), event.payload.size());
            }
        };
        const auto bytes = std::span<const char>(input).subspan(offset);
        const auto result = decoder.feedOne(bytes, fin, collect, &collection);
        offset += result.consumedBytes;
        if (result.status == HttpCapsuleStatus::kLimit || result.status == HttpCapsuleStatus::kTruncated) {
            abort();
            throw std::runtime_error("invalid or oversized capsule stream");
        }
        if (result.status == HttpCapsuleStatus::kEnd) {
            receiveEnded = true;
        }
        return result.capsuleComplete;
    }
    [[nodiscard]] Task<bool> readInput() {
        input.clear();
        offset = 0;
        if (server) {
            auto chunk = co_await server->read();
            if (!chunk) {
                co_return false;
            }
            input = std::move(*chunk);
        } else {
            auto chunk = co_await client->read();
            if (!chunk) {
                co_return false;
            }
            input.assign(reinterpret_cast<const char*>(chunk->data()), chunk->size());
        }
        co_return true;
    }
    [[nodiscard]] Task<void> sendBytes(std::string_view bytes) {
        const auto limit = client ? client->response_.state_->tunnel->config.maxChunkBytes : std::size_t{4096};
        while (!bytes.empty()) {
            const auto chunk = bytes.substr(0, limit);
            if (server) {
                co_await server->write(chunk);
            } else {
                co_await client->write(chunk);
            }
            bytes.remove_prefix(chunk.size());
        }
    }
    [[nodiscard]] HttpDatagramSessionConfig datagramConfig() const {
        if (server) {
            return server->datagramConfig_ ? server->datagramConfig_(server->target_) : HttpDatagramSessionConfig{};
        }
        auto& state = *client->response_.state_;
        return state.http3Connection ? state.http3Connection->datagramConfig(state.http3RequestId) : HttpDatagramSessionConfig{};
    }
    [[nodiscard]] Task<std::optional<HttpDatagramInput>> readDatagramInput() {
        if (server) {
            if (server->readDatagramInput_) {
                co_return co_await server->readDatagramInput();
            }
            auto bytes = co_await server->read();
            if (!bytes) {
                co_return std::nullopt;
            }
            co_return HttpDatagramInput{std::move(*bytes), false};
        }
        auto& state = *client->response_.state_;
        for (;;) {
            if (!state.failure && !state.errorCode && !state.abandoned && !state.tunnel->datagrams.empty()) {
                auto bytes = std::move(state.tunnel->datagrams.front());
                state.tunnel->datagrams.pop_front();
                co_return HttpDatagramInput{std::move(bytes), true};
            }
            if (!state.pending.empty() || state.offset < state.buffered.size() || state.receiveComplete() || state.failure || state.errorCode || state.abandoned) {
                auto bytes = co_await client->read();
                if (!bytes) {
                    co_return std::nullopt;
                }
                co_return HttpDatagramInput{std::pmr::string(reinterpret_cast<const char*>(bytes->data()), bytes->size(), resource), false};
            }
            co_await state.dataSignal.wait();
        }
    }
    [[nodiscard]] Task<void> sendDatagram(std::string_view bytes) {
        if (server) {
            co_await server->sendDatagram(bytes);
        } else {
            auto& state = *client->response_.state_;
            if (state.tunnel->output.stopped || state.tunnel->output.ended || state.tunnel->output.endRequested || state.abandoned || !state.http3Connection) {
                throw std::runtime_error("HTTP Datagram sending direction is closed");
            }
            static_cast<void>(state.http3Connection->sendDatagram(state.http3RequestId, std::as_bytes(std::span(bytes.data(), bytes.size()))));
        }
    }
    HttpDatagramSendPolicy datagramPolicy{HttpDatagramSendPolicy::kAutomatic};
    const WorkerHandle& worker;
    std::pmr::memory_resource* resource;
    std::optional<HttpClientResponse> memoryPin;
    HttpTunnel* server{};
    std::optional<HttpClientTunnel> client;
    HttpCapsuleDecoder decoder;
    HttpCapsuleConfig config;
    std::pmr::string input;
    std::size_t offset{};
    std::pmr::string payload;
    std::uint64_t type{};
    bool receiveEnded{};
    bool sendEnded{};
    bool closed{};
    unsigned running{};
    std::size_t references{1};
    ::ruvia::operation_scope readScope;
    ::ruvia::operation_scope outputScope;
};
class CapsuleStatePin final {
public:
    explicit CapsuleStatePin(HttpCapsuleStreamState& state) noexcept
        : state_(&state) {
        state_->retain();
    }
    CapsuleStatePin(CapsuleStatePin&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}
    CapsuleStatePin(const CapsuleStatePin&) = delete;
    ~CapsuleStatePin() {
        if (state_) {
            state_->release();
        }
    }
    [[nodiscard]] HttpCapsuleStreamState& state() const noexcept {
        return *state_;
    }

private:
    HttpCapsuleStreamState* state_;
};
struct CapsuleWriteInput final {
    CapsuleStatePin pin;
    std::pmr::string bytes;
};
class RunningCapsuleOperation final {
public:
    explicit RunningCapsuleOperation(HttpCapsuleStreamState& state)
        : state_(state),
          exceptions_(std::uncaught_exceptions()) {
        if (state.closed || !state.worker.isCurrent()) {
            throw std::logic_error("capsule stream operation expired");
        }
        ++state_.running;
    }
    ~RunningCapsuleOperation() {
        if (std::uncaught_exceptions() > exceptions_) {
            state_.abort();
        }
        --state_.running;
    }

private:
    HttpCapsuleStreamState& state_;
    int exceptions_;
};
}  // namespace ruvia::detail
