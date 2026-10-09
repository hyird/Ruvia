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

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_tunnel.h"

#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "http3/http3_client_connection.h"

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
          input_(resource_),
          payload_(resource_) {}
    http_capsule_stream_state(http_client_tunnel&& tunnel, http_capsule_config config)
        : worker_(tunnel.response_.state_->memory_domain()->worker()),
          resource_(tunnel.response_.state_->resource_),
          memory_pin_(http_client_response(tunnel.response_.state_, true)),
          client_(std::move(tunnel)),
          decoder_(config),
          config_(config),
          input_(resource_),
          payload_(resource_) {}
    void require(bool output) const {
        if (closed_ || (client_ && (!client_->response_.state_ || client_->response_.state_->pool_ == nullptr))) {
            throw std::logic_error("capsule stream is closed");
        }
        if (!worker_.is_current()) {
            throw std::logic_error("capsule stream requires its owner worker");
        }
        if ((output ? output_scope_ : read_scope_).has_pending_operations()) {
            throw std::logic_error("capsule stream operation is already active");
        }
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
        input_.clear();
        offset_ = 0;
        if (server_) {
            auto chunk = co_await server_->read();
            if (!chunk) {
                co_return false;
            }
            input_ = std::move(*chunk);
        } else {
            auto chunk = co_await client_->read();
            if (!chunk) {
                co_return false;
            }
            input_.assign(reinterpret_cast<const char*>(chunk->data()), chunk->size());
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
    [[nodiscard]] http_datagram_session_config datagram_config() const {
        if (server_) {
            return server_->datagram_config_ ? server_->datagram_config_(server_->target_) : http_datagram_session_config{};
        }
        auto& state_value = *client_->response_.state_;
        return state_value.http3_connection_ ? state_value.http3_connection_->datagram_config(state_value.http3_request_id_) : http_datagram_session_config{};
    }
    [[nodiscard]] task<std::optional<http_datagram_input>> read_datagram_input() {
        if (server_) {
            if (server_->read_datagram_input_) {
                co_return co_await server_->read_datagram_input();
            }
            auto bytes_value = co_await server_->read();
            if (!bytes_value) {
                co_return std::nullopt;
            }
            co_return http_datagram_input{std::move(*bytes_value), false};
        }
        auto& state_value = *client_->response_.state_;
        for (;;) {
            if (!state_value.failure_ && !state_value.error_code_ && !state_value.abandoned_ && !state_value.tunnel_->datagrams_.empty()) {
                auto bytes_value = std::move(state_value.tunnel_->datagrams_.front());
                state_value.tunnel_->datagrams_.pop_front();
                co_return http_datagram_input{std::move(bytes_value), true};
            }
            if (!state_value.pending_.empty() || state_value.offset_ < state_value.buffered_.size() || state_value.receive_complete() || state_value.failure_ || state_value.error_code_ || state_value.abandoned_) {
                auto bytes_value = co_await client_->read();
                if (!bytes_value) {
                    co_return std::nullopt;
                }
                co_return http_datagram_input{std::pmr::string(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size(), resource_), false};
            }
            co_await state_value.data_signal_.wait();
        }
    }
    [[nodiscard]] task<void> send_datagram(std::string_view bytes_value) {
        if (server_) {
            co_await server_->send_datagram(bytes_value);
        } else {
            auto& state_value = *client_->response_.state_;
            if (state_value.tunnel_->output_.stopped_ || state_value.tunnel_->output_.ended_ || state_value.tunnel_->output_.end_requested_ || state_value.abandoned_ || !state_value.http3_connection_) {
                throw std::runtime_error("HTTP Datagram sending direction is closed");
            }
            static_cast<void>(state_value.http3_connection_->send_datagram(state_value.http3_request_id_, std::as_bytes(std::span(bytes_value.data(), bytes_value.size()))));
        }
    }
    http_datagram_send_policy datagram_policy_{http_datagram_send_policy::automatic};
    const worker_handle& worker_;
    std::pmr::memory_resource* resource_;
    std::optional<http_client_response> memory_pin_;
    http_tunnel* server_{};
    std::optional<http_client_tunnel> client_;
    http_capsule_decoder decoder_;
    http_capsule_config config_;
    std::pmr::string input_;
    std::size_t offset_{};
    std::pmr::string payload_;
    std::uint64_t type_{};
    bool receive_ended_{};
    bool send_ended_{};
    bool closed_{};
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
    explicit running_capsule_operation(http_capsule_stream_state& state_value)
        : state_(state_value),
          exceptions_(std::uncaught_exceptions()) {
        if (state_value.closed_ || !state_value.worker_.is_current()) {
            throw std::logic_error("capsule stream operation expired");
        }
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
};
}  // namespace ruvia::detail
