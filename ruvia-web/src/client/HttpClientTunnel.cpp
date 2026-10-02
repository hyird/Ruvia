#include "ruvia/web/HttpClientTunnel.h"

#include <stdexcept>
#include <utility>

#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/HttpUdpTunnel.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseMemory.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/http3/Http3ClientConnection.h"

namespace ruvia::detail {
struct HttpClientTunnelWriteInput final {
    HttpClientResponse pin;
    std::pmr::string bytes;
};
}  // namespace ruvia::detail
namespace ruvia {
namespace {
void requireOutput(detail::HttpClientResponseState* state, bool finishing) {
    if (state == nullptr) {
        throw std::logic_error("HTTP client tunnel is empty");
    }
    if (auto* domain = state->memoryDomain(); domain != nullptr && !domain->worker().isCurrent()) {
        throw std::logic_error("HTTP client tunnel requires its owner worker");
    }
    if (finishing && state->tunnel && state->tunnel->ended && !state->tunnel->outputScope.hasPendingOperations()) {
        return;
    }
    if (!state->tunnel || !state->tunnel->accepted || state->tunnel->stopped || state->abandoned || state->failure || state->errorCode) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP client tunnel is closed");
    }
    auto& output = *state->tunnel;
    if (output.outputScope.hasPendingOperations()) {
        throw std::logic_error("HTTP client tunnel output operation is already active");
    }
    if (!finishing && (output.ended || output.endRequested)) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP client tunnel sending direction is closed");
    }
}
void throwOutputStopped(detail::HttpClientResponseState& state) {
    if (state.failure) {
        std::rethrow_exception(state.failure);
    }
    if (state.errorCode) {
        throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "HTTP client tunnel failed");
    }
    throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP client tunnel output stopped");
}
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
        state->tunnel->outputScope.close();
        state->tunnel->stop();
    }
    response_.release();
}
void HttpClientTunnel::abort() & noexcept {
    if (auto* state = response_.state_) {
        if (!state->memoryDomain()->worker().isCurrent()) {
            std::terminate();
        }
        state->tunnel->stop();
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
    return detail::makeScopedOperation(state.tunnel->outputScope, writeOwned(detail::HttpClientTunnelWriteInput{HttpClientResponse(&state, true), std::pmr::string(bytes, state.resource)}));
}
Task<void> HttpClientTunnel::writeOwned(detail::HttpClientTunnelWriteInput input) {
    auto& state = *input.pin.state_;
    auto& bytes = input.bytes;
    auto& output = *state.tunnel;
    if (output.stopped || output.endRequested || state.abandoned) {
        throwOutputStopped(state);
    }
    if (bytes.empty()) {
        co_return;
    }
    output.chunk = std::move(bytes);
    output.chunkReady = true;
    output.notifyData();
    while (output.chunkReady && !output.stopped) {
        co_await output.space.wait();
    }
    if (output.stopped) {
        throwOutputStopped(state);
    }
}
ScopedOperation<void> HttpClientTunnel::finish() & {
    requireOutput(response_.state_, true);
    auto& state = *response_.state_;
    return detail::makeScopedOperation(state.tunnel->outputScope, finishOwned(HttpClientResponse(&state, true)));
}
Task<void> HttpClientTunnel::finishOwned(HttpClientResponse pin) {
    auto& state = *pin.state_;
    auto& output = *state.tunnel;
    if (output.ended) {
        co_return;
    }
    if (output.stopped || state.abandoned) {
        throwOutputStopped(state);
    }
    output.endRequested = true;
    output.notifyData();
    while (!output.ended && (!output.stopped || output.completionPending)) {
        co_await output.space.wait();
    }
    if (!output.ended) {
        throwOutputStopped(state);
    }
}
}  // namespace ruvia
