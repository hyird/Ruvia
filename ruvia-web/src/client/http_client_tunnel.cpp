#include "ruvia/web/http_client_tunnel.h"

#include <stdexcept>
#include <utility>

#include "ruvia/web/http_client_types.h"
#include "ruvia/web/http_udp_tunnel.h"

#include "client/http_client_output_operation.h"
#include "client/http_client_pool.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_response_state.h"
#include "http3/http3_client_connection.h"

namespace ruvia {
namespace {
void require_output(detail::http_client_response_state* state_value, bool finishing) {
    if (state_value == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    if (auto* domain = state_value->memory_domain(); domain != nullptr && !domain->worker().is_current()) {
        throw std::logic_error("HTTP client tunnel requires its owner worker");
    }
    // Normal retirement stops the queue after both directions complete, but
    // cannot invalidate a finish that has already succeeded. Writes and failed
    // or abandoned tunnels still reject terminal output operations.
    const bool completed_finish = finishing && state_value->tunnel_ && state_value->tunnel_->output_.ended_ && state_value->complete_;
    if (!state_value->tunnel_ || (state_value->pool_ == nullptr && !completed_finish) || (state_value->tunnel_->output_.stopped_ && !completed_finish) || !state_value->tunnel_->accepted_ ||
        state_value->abandoned_ || state_value->failure_ || state_value->error_code_) {
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP client tunnel is closed");
    }
    if (finishing && state_value->tunnel_->output_.ended_ && !state_value->tunnel_->output_.output_scope_.has_pending_operations()) {
        return;
    }
    auto& output = state_value->tunnel_->output_;
    if (output.output_scope_.has_pending_operations()) {
        throw std::logic_error("HTTP client tunnel output operation is already active");
    }
    if (!finishing && (output.ended_ || output.end_requested_)) {
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP client tunnel sending direction is closed");
    }
}
struct tunnel_output_policy final {
    static constexpr bool validate_chunk = false;
    static detail::http_client_output_queue& output(detail::http_client_response_state& state_value) noexcept {
        return state_value.tunnel_->output_;
    }
    static void require_write(detail::http_client_response_state& state_value) {
        auto& queue = output(state_value);
        if (queue.stopped_ || queue.end_requested_ || state_value.abandoned_) {
            throw_stopped(state_value);
        }
    }
    static bool begin_finish(detail::http_client_response_state& state_value, http_client_response&) {
        auto& queue = output(state_value);
        if (queue.ended_) {
            return false;
        }
        if (queue.stopped_ || state_value.abandoned_) {
            throw_stopped(state_value);
        }
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::http_client_response_state& state_value) {
        if (state_value.failure_) {
            std::rethrow_exception(state_value.failure_);
        }
        if (state_value.error_code_) {
            throw http_client_error(static_cast<http_client_error::code_type>(*state_value.error_code_), "HTTP client tunnel failed");
        }
        throw http_client_error(http_client_error::code_type::cancelled, "HTTP client tunnel output stopped");
    }
};
}  // namespace
http_client_tunnel::http_client_tunnel(http_client_response response) noexcept
    : response_(std::move(response)) {}
http_client_tunnel::http_client_tunnel(http_client_tunnel&& other) noexcept
    : response_(std::move(other.response_)) {}
http_client_tunnel& http_client_tunnel::operator=(http_client_tunnel&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
    }
    return *this;
}
http_client_tunnel::~http_client_tunnel() {
    release();
}
void http_client_tunnel::release() noexcept {
    if (auto* state_value = response_.state_; state_value != nullptr) {
        state_value->tunnel_->output_.output_scope_.close();
        state_value->tunnel_->output_.stop();
    }
    response_.release();
}
void http_client_tunnel::abort() & noexcept {
    if (auto* state = response_.state_) {
        if (!state->tunnel_ || state->tunnel_->output_.stopped_ || state->pool_ == nullptr) {
            return;
        }
        if (!state->memory_domain()->worker().is_current()) {
            std::terminate();
        }
        state->tunnel_->output_.stop();
        if (!state->complete_ && !state->abandoned_) {
            if (state->http3_connection_) {
                state->http3_connection_->abandon_response(state->http3_request_id_);
            } else if (state->pool_) {
                state->pool_->abandon_response(*state);
            }
        }
    }
}
http_udp_tunnel http_client_tunnel::udp(http_datagram_config config) && {
    if (!response_.state_ || !response_.state_->tunnel_ || !response_.state_->tunnel_->udp_ || !response_.state_->tunnel_->accepted_) {
        throw std::logic_error("UDP tunnel requires an accepted CONNECT-UDP handshake");
    }
    return http_udp_tunnel(std::move(*this).datagrams(config));
}
scoped_operation<std::optional<std::span<const std::byte>>> http_client_tunnel::read() & {
    if (response_.state_ == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    return response_.body().read();
}
scoped_operation<void> http_client_tunnel::write(std::span<const std::byte> bytes_value) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()));
}
scoped_operation<void> http_client_tunnel::write(std::string_view bytes_value) & {
    require_output(response_.state_, false);
    auto& state_value = *response_.state_;
    if (bytes_value.size() > state_value.tunnel_->config_.max_chunk_bytes_) {
        throw std::length_error("HTTP client tunnel chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::write_client_output<tunnel_output_policy>(&state_value, detail::http_client_output_write_input{http_client_response(&state_value, true), std::pmr::string(bytes_value, state_value.resource_)}));
}
scoped_operation<void> http_client_tunnel::finish() & {
    require_output(response_.state_, true);
    auto& state_value = *response_.state_;
    return ::ruvia::make_scoped_operation(state_value.tunnel_->output_.output_scope_,
        detail::finish_client_output<tunnel_output_policy>(&state_value, http_client_response(&state_value, true)));
}
}  // namespace ruvia
