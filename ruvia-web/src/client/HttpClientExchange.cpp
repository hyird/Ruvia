#include "ruvia/web/HttpClientExchange.h"

#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpRequestTrailers.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientPool.h"
#include "ruvia/web/detail/client/HttpClientResponseMemory.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"

namespace ruvia::detail {
struct HttpClientUploadWriteInput final {
    HttpClientResponse pin;
    std::pmr::string bytes;
};
struct HttpClientUploadEndInput final {
    HttpClientResponse pin;
    std::pmr::vector<HttpHeader> fields;
};
}  // namespace ruvia::detail
namespace ruvia {
namespace {
void requireWritable(detail::HttpClientResponseState& state) {
    if (auto* domain = state.memoryDomain(); domain != nullptr && !domain->worker().isCurrent()) {
        throw std::logic_error("HTTP exchange must run on its owner worker");
    }
    if (!state.upload || state.upload->stopped || state.upload->ended || state.upload->endRequested) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload is no longer writable");
    }
    if (state.upload->outputScope.hasPendingOperations()) {
        throw std::logic_error("HTTP request upload operation is already active");
    }
}
}  // namespace

HttpClientExchange::HttpClientExchange(HttpClientResponse response) noexcept
    : response_(std::move(response)),
      body_(response_.state_) {}
HttpClientExchange::HttpClientExchange(HttpClientExchange&& other) noexcept
    : response_(std::move(other.response_)),
      body_(response_.state_) {
    other.body_.state_ = nullptr;
}
HttpClientExchange& HttpClientExchange::operator=(HttpClientExchange&& other) noexcept {
    if (this != &other) {
        release();
        response_ = std::move(other.response_);
        body_.state_ = response_.state_;
        other.body_.state_ = nullptr;
    }
    return *this;
}
HttpClientExchange::~HttpClientExchange() {
    release();
}
void HttpClientExchange::release() noexcept {
    auto* state = response_.state_;
    if (state == nullptr) {
        return;
    }
    auto& upload = *state->upload;
    upload.outputScope.close();
    upload.responseScope.close();
    if (upload.responseTaken) {
        response_.consumer_ = false;
    }
    if (!upload.ended) {
        upload.stop();
    }
    response_.release();
    body_.state_ = nullptr;
}

ScopedOperation<void> HttpClientRequestBodyWriter::write(std::span<const std::byte> bytes) & {
    return write(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
ScopedOperation<void> HttpClientRequestBodyWriter::write(std::string_view bytes) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    requireWritable(*state_);
    if (bytes.size() > state_->upload->config.maxChunkBytes) {
        throw std::length_error("HTTP upload chunk exceeds configured bound");
    }
    return detail::makeScopedOperation(state_->upload->outputScope,
        writeOwned(detail::HttpClientUploadWriteInput{HttpClientResponse(state_, true), std::pmr::string(bytes, state_->resource)}));
}
Task<void> HttpClientRequestBodyWriter::writeOwned(detail::HttpClientUploadWriteInput input) {
    auto& state = *input.pin.state_;
    auto& bytes = input.bytes;
    auto& upload = *state.upload;
    if (upload.stopped || upload.endRequested) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload stopped");
    }
    if (bytes.empty()) {
        co_return;
    }
    if (upload.chunkReady) {
        throw std::logic_error("HTTP upload queue still owns a chunk");
    }
    upload.chunk = std::move(bytes);
    upload.chunkReady = true;
    upload.notifyData();
    while (upload.chunkReady && !upload.stopped) {
        co_await upload.space.wait();
    }
    if (upload.stopped) {
        if (state.failure) {
            std::rethrow_exception(state.failure);
        }
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload stopped");
    }
}
ScopedOperation<void> HttpClientRequestBodyWriter::end(std::span<const HttpHeaderView> fields) & {
    if (state_ == nullptr) {
        throw std::logic_error("HTTP request exchange is empty");
    }
    requireWritable(*state_);
    HttpRequestTrailers validation(state_->resource);
    for (const auto& field : fields) {
        std::pmr::string name(field.name(), state_->resource);
        for (auto& ch : name) {
            ch = static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(ch)));
        }
        if (!validation.append(name, field.value())) {
            throw std::invalid_argument("invalid request trailer section");
        }
    }
    return detail::makeScopedOperation(state_->upload->outputScope, endOwned(detail::HttpClientUploadEndInput{HttpClientResponse(state_, true), std::move(validation).takeFields()}));
}
Task<void> HttpClientRequestBodyWriter::endOwned(detail::HttpClientUploadEndInput input) {
    auto& state = *input.pin.state_;
    auto& fields = input.fields;
    auto& upload = *state.upload;
    if (upload.stopped || upload.endRequested) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload stopped");
    }
    upload.trailers = std::move(fields);
    upload.endRequested = true;
    upload.notifyData();
    while (!upload.ended && (!upload.stopped || upload.completionPending)) {
        co_await upload.space.wait();
    }
    if (!upload.ended) {
        if (state.failure) {
            std::rethrow_exception(state.failure);
        }
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload stopped");
    }
}
bool HttpClientRequestBodyWriter::complete() const noexcept {
    return state_ == nullptr || state_->upload->ended;
}

ScopedOperation<HttpClientResponse> HttpClientExchange::response() & {
    auto* state = response_.state_;
    if (state == nullptr || state->upload->responseTaken) {
        throw std::logic_error("HTTP exchange response has already been transferred");
    }
    if (auto* domain = state->memoryDomain(); domain != nullptr && !domain->worker().isCurrent()) {
        throw std::logic_error("HTTP exchange must run on its owner worker");
    }
    if (state->upload->responseScope.hasPendingOperations()) {
        throw std::logic_error("HTTP exchange response operation is already active");
    }
    return detail::makeScopedOperation(state->upload->responseScope, receiveOwned(HttpClientResponse(state, true)));
}
Task<HttpClientResponse> HttpClientExchange::receiveOwned(HttpClientResponse pin) {
    auto& state = *pin.state_;
    while (!state.headReady && !state.failure && !state.errorCode) {
        co_await state.headSignal.wait();
    }
    if (state.failure) {
        std::rethrow_exception(state.failure);
    }
    if (state.errorCode) {
        throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "HTTP request failed before response head");
    }
    if (state.bodyDecodeRequired) {
        if (state.http2DataCredit) {
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
            throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "HTTP encoded response failed");
        }
    }
    state.upload->responseTaken = true;
    pin.consumer_ = true;
    co_return std::move(pin);
}
}  // namespace ruvia
