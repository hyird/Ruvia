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

#include "ruvia/core/bytes.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_tunnel.h"

#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "util/operation_lane_lease.h"

namespace ruvia::detail {
enum class capsule_interest : unsigned char { all,
    datagrams };
class http_capsule_stream_state final {
public:
    http_capsule_stream_state(http_tunnel& tunnel, http_capsule_config config)
        : worker_(tunnel.worker_),
          resource_(&tunnel.resource_),
          server_(&tunnel),
          decoder_(config),
          config_(config),
          server_input_(resource_),
          payload_(resource_) {}
    http_capsule_stream_state(http_client_tunnel&& tunnel, http_capsule_config config)
        : worker_(tunnel.response_.state_->memory_domain().worker()),
          resource_(tunnel.response_.state_->resource_),
          memory_pin_(http_client_response(tunnel.response_.state_)),
          client_(std::move(tunnel)),
          decoder_(config),
          config_(config),
          server_input_(resource_),
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
        // Keep the client domain alive until after this object is deallocated
        // from its pool. Capsule results and cold/running operations pin it.
        auto pin = std::move(memory_pin_);
        auto* allocator = resource_;
        std::destroy_at(this);
        allocator->deallocate(this, sizeof(http_capsule_stream_state), alignof(http_capsule_stream_state));
    }
    void abort() noexcept {
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
            server_->abort();
        } else if (client_) {
            client_->abort();
        }
    }
    void close() noexcept {
        if (!worker_.is_current()) {
            std::terminate();
        }
        // The caller joins started operations after abort() wakes transport I/O.
        // Destroying their scope while an operation is retiring is forbidden.
        if (running_ != 0) {
            std::terminate();
        }
        if (client_) {
            abort();
        } else {
            closed_ = true;
        }
        read_scope_.close();
        output_scope_.close();
    }
    // Parse the currently available input synchronously. Datagram consumers
    // skip unknown capsule payloads without allocating them or recursively
    // awaiting a completed child task for each ignored capsule.
    [[nodiscard]] bool poll_capsule(bool fin, capsule_interest interest) {
        struct collection {
            http_capsule_stream_state& state_;
            capsule_interest interest_;
        } collection_value{*this, interest};
        const auto collect = [](void* raw, http_capsule_event event) {
            auto& current = *static_cast<collection*>(raw);
            current.state_.type_ = event.type_;
            if (current.interest_ == capsule_interest::datagrams && event.type_ != http_datagram_capsule_type) {
                return;
            }
            if (!event.payload_.empty()) {
                current.state_.payload_.append(event.payload_.data(), event.payload_.size());
            }
        };
        const auto bytes_value = std::span<const char>(input_).subspan(offset_);
        const auto result_value = decoder_.feed_one(bytes_value, fin, collect, &collection_value);
        offset_ += result_value.consumed_bytes_;
        if (result_value.status_ == http_capsule_status::limit || result_value.status_ == http_capsule_status::truncated) {
            abort();
            throw std::runtime_error("invalid or oversized capsule stream");
        }
        if (result_value.status_ == http_capsule_status::end) {
            receive_ended_ = true;
        }
        return result_value.capsule_complete_;
    }
    [[nodiscard]] task<bool> read_input() {
        input_ = {};
        server_input_.clear();
        offset_ = 0;
        if (server_) {
            auto chunk = co_await server_->read();
            if (!chunk) {
                co_return false;
            }
            server_input_ = std::move(*chunk);
            input_ = server_input_;
        } else {
            auto chunk = co_await client_->read();
            if (!chunk) {
                co_return false;
            }
            // The owned tunnel keeps this view valid until the next read.
            // Consume it completely before read_input() advances the source.
            input_ = as_chars(*chunk);
        }
        co_return true;
    }
    [[nodiscard]] task<void> send_bytes(std::string_view bytes_value) {
        const auto limit = client_ ? client_->response_.state_->tunnel_->config_.max_chunk_bytes_ : std::size_t{4096};
        while (!bytes_value.empty()) {
            const auto chunk = bytes_value.substr(0, limit);
            if (server_) {
                co_await server_->write(chunk);
            } else {
                co_await client_->write(chunk);
            }
            bytes_value.remove_prefix(chunk.size());
        }
    }
    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    std::optional<http_client_response> memory_pin_;
    http_tunnel* server_{};
    std::optional<http_client_tunnel> client_;
    http_capsule_decoder decoder_;
    http_capsule_config config_;
    std::pmr::string server_input_;
    std::string_view input_;
    std::size_t offset_{};
    std::pmr::string payload_;
    std::uint64_t type_{};
    bool receive_ended_{};
    bool send_ended_{};
    bool closed_{};
    bool read_active_{};
    bool output_active_{};
    unsigned running_{};
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
};
class running_capsule_operation final {
public:
    running_capsule_operation(http_capsule_stream_state& state_value, bool output)
        : state_(state_value),
          exceptions_(std::uncaught_exceptions()),
          lane_(state_value.claim_lane(output)) {
        ++state_.running_;
    }
    ~running_capsule_operation() {
        if (std::uncaught_exceptions() > exceptions_) {
            state_.abort();
        }
        --state_.running_;
    }

private:
    http_capsule_stream_state& state_;
    int exceptions_;
    operation_lane_lease lane_;
};
}  // namespace ruvia::detail
