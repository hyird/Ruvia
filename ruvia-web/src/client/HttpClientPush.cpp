#include "ruvia/web/HttpClientPush.h"

#include <stdexcept>
#include <utility>

#include "client/HttpClientPool.h"
#include "client/HttpClientResponseMemory.h"
#include "client/HttpClientResponseState.h"

namespace ruvia {
HttpClientPush::HttpClientPush(HttpClientResponse response) noexcept
    : response_(std::move(response)) {}
HttpClientPush::HttpClientPush(HttpClientPush&& other) noexcept
    : response_(std::move(other.response_)) {}
HttpClientPush& HttpClientPush::operator=(HttpClientPush&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
    }
    return *this;
}
HttpClientPush::~HttpClientPush() {
    release();
}
void HttpClientPush::release() noexcept {
    if (auto* state = response_.state_; state != nullptr) {
        if (!state->memoryDomain()->worker().isCurrent()) {
            std::terminate();
        }
        state->pushResponseScope.close();
        if (state->pushResponseTaken) {
            response_.consumer_ = false;
        }
    }
    response_.release();
}
const HttpPushRequest& HttpClientPush::request() const& {
    const auto* state = response_.state_;
    if (state == nullptr || !state->promisedRequest) {
        throw std::logic_error("push owner has moved");
    }
    if (!state->memoryDomain()->worker().isCurrent()) {
        throw std::logic_error("push requires its owner worker");
    }
    return *state->promisedRequest;
}
ScopedOperation<HttpClientResponse> HttpClientPush::response() & {
    auto* state = response_.state_;
    (void)request();
    if (state->pushResponseTaken || state->pushResponseScope.has_pending_operations()) {
        throw std::logic_error("push response has already been transferred or is active");
    }
    return ::ruvia::make_scoped_operation(state->pushResponseScope, receiveOwned(HttpClientResponse(state, true)));
}
Task<HttpClientResponse> HttpClientPush::receiveOwned(HttpClientResponse pin) {
    auto& state = *pin.state_;
    while (!state.headReady && !state.failure && !state.errorCode) {
        co_await state.headSignal.wait();
    }
    if (state.failure) {
        std::rethrow_exception(state.failure);
    }
    if (state.errorCode) {
        throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "push failed before response head");
    }
    if (state.bodyDecodeRequired) {
        if (state.pool != nullptr) {
            state.pool->releaseResponseData(state);
        }
        state.notifyProducerSpace();
        while (!state.complete) {
            co_await state.dataSignal.wait();
        }
        if (state.failure) {
            std::rethrow_exception(state.failure);
        }
        if (state.errorCode) {
            throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "encoded push failed");
        }
    }
    state.pushResponseTaken = true;
    pin.consumer_ = true;
    co_return std::move(pin);
}
}  // namespace ruvia
