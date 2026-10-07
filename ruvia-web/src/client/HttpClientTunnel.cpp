#include "ruvia/web/HttpClientTunnel.h"

#include <stdexcept>
#include <utility>

#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/HttpUdpTunnel.h"
#include "ruvia/web/detail/client/HttpClientOutputOperation.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseMemory.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/http3/Http3ClientConnection.h"

namespace ruvia {
namespace {
void requireOutput(detail::HttpClientResponseState* state, bool finishing) {
    if (state == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    if (auto* domain = state->memoryDomain(); domain != nullptr && !domain->worker().isCurrent()) {
        throw std::logic_error("HTTP client tunnel requires its owner worker");
    }
    // Normal retirement stops the queue after both directions complete, but
    // cannot invalidate a finish that has already succeeded. Writes and failed
    // or abandoned tunnels still reject terminal output operations.
    const bool completed_finish = finishing && state->tunnel && state->tunnel->output.ended && state->complete;
    if (!state->tunnel || (state->pool == nullptr && !completed_finish) || (state->tunnel->output.stopped && !completed_finish) || !state->tunnel->accepted ||
        state->abandoned || state->failure || state->errorCode) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP client tunnel is closed");
    }
    if (finishing && state->tunnel->output.ended && !state->tunnel->output.outputScope.has_pending_operations()) {
        return;
    }
    auto& output = state->tunnel->output;
    if (output.outputScope.has_pending_operations()) {
        throw std::logic_error("HTTP client tunnel output operation is already active");
    }
    if (!finishing && (output.ended || output.endRequested)) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP client tunnel sending direction is closed");
    }
}
struct tunnel_output_policy final {
    static constexpr bool validate_chunk = false;
    static detail::http_client_output_queue& output(detail::HttpClientResponseState& state) noexcept {
        return state.tunnel->output;
    }
    static void require_write(detail::HttpClientResponseState& state) {
        auto& queue = output(state);
        if (queue.stopped || queue.endRequested || state.abandoned) {
            throw_stopped(state);
        }
    }
    static bool begin_finish(detail::HttpClientResponseState& state, HttpClientResponse&) {
        auto& queue = output(state);
        if (queue.ended) {
            return false;
        }
        if (queue.stopped || state.abandoned) {
            throw_stopped(state);
        }
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::HttpClientResponseState& state) {
        if (state.failure) {
            std::rethrow_exception(state.failure);
        }
        if (state.errorCode) {
            throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "HTTP client tunnel failed");
        }
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP client tunnel output stopped");
    }
};
}  // namespace
HttpClientTunnel::HttpClientTunnel(HttpClientResponse response) noexcept
    : response_(std::move(response)) {}
HttpClientTunnel::HttpClientTunnel(HttpClientTunnel&& other) noexcept
    : response_(std::move(other.response_)) {}
HttpClientTunnel& HttpClientTunnel::operator=(HttpClientTunnel&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
    }
    return *this;
}
HttpClientTunnel::~HttpClientTunnel() {
    release();
}
void HttpClientTunnel::release() noexcept {
    if (auto* state = response_.state_; state != nullptr) {
        state->tunnel->output.outputScope.close();
        state->tunnel->output.stop();
    }
    response_.release();
}
void HttpClientTunnel::abort() & noexcept {
    if (auto* state = response_.state_) {
        if (!state->tunnel || state->tunnel->output.stopped || state->pool == nullptr) {
            return;
        }
        if (!state->memoryDomain()->worker().isCurrent()) {
            std::terminate();
        }
        state->tunnel->output.stop();
        if (!state->complete && !state->abandoned) {
            if (state->http3Connection) {
                state->http3Connection->abandonResponse(state->http3RequestId);
            } else if (state->pool) {
                state->pool->abandonResponse(*state);
            }
        }
    }
}
HttpUdpTunnel HttpClientTunnel::udp(HttpDatagramConfig config) && {
    if (!response_.state_ || !response_.state_->tunnel || !response_.state_->tunnel->udp || !response_.state_->tunnel->accepted) {
        throw std::logic_error("UDP tunnel requires an accepted CONNECT-UDP handshake");
    }
    return HttpUdpTunnel(std::move(*this).datagrams(config));
}
ScopedOperation<std::optional<std::span<const std::byte>>> HttpClientTunnel::read() & {
    if (response_.state_ == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    return response_.body().read();
}
ScopedOperation<void> HttpClientTunnel::write(std::span<const std::byte> bytes) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
ScopedOperation<void> HttpClientTunnel::write(std::string_view bytes) & {
    requireOutput(response_.state_, false);
    auto& state = *response_.state_;
    if (bytes.size() > state.tunnel->config.maxChunkBytes) {
        throw std::length_error("HTTP client tunnel chunk exceeds configured bound");
    }
    return ::ruvia::make_scoped_operation(state.tunnel->output.outputScope,
        detail::write_client_output<tunnel_output_policy>(&state, detail::http_client_output_write_input{HttpClientResponse(&state, true), std::pmr::string(bytes, state.resource)}));
}
ScopedOperation<void> HttpClientTunnel::finish() & {
    requireOutput(response_.state_, true);
    auto& state = *response_.state_;
    return ::ruvia::make_scoped_operation(state.tunnel->output.outputScope,
        detail::finish_client_output<tunnel_output_policy>(&state, HttpClientResponse(&state, true)));
}
}  // namespace ruvia
