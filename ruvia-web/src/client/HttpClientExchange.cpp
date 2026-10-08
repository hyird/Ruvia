#include "ruvia/web/HttpClientExchange.h"

#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpRequestTrailers.h"
#include "ruvia/web/HttpClientTypes.h"

#include "client/HttpClientOutputOperation.h"
#include "client/HttpClientPool.h"
#include "client/HttpClientResponseMemory.h"
#include "client/HttpClientResponseState.h"

namespace ruvia::detail {
struct http_client_upload_end_input final {
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
    if (!state.upload || state.upload->output.stopped || state.upload->output.ended || state.upload->output.endRequested) {
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload is no longer writable");
    }
    if (state.upload->output.outputScope.has_pending_operations()) {
        throw std::logic_error("HTTP request upload operation is already active");
    }
}
struct upload_output_policy final {
    static constexpr bool validate_chunk = true;
    static detail::http_client_output_queue& output(detail::HttpClientResponseState& state) noexcept {
        return state.upload->output;
    }
    static void require_write(detail::HttpClientResponseState& state) {
        auto& queue = output(state);
        if (queue.stopped || queue.endRequested) {
            throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload stopped");
        }
    }
    static void require_empty_chunk(detail::http_client_output_queue& queue) {
        if (queue.chunkReady) {
            throw std::logic_error("HTTP upload queue still owns a chunk");
        }
    }
    static bool begin_finish(detail::HttpClientResponseState& state, detail::http_client_upload_end_input& input) {
        require_write(state);
        state.upload->trailers = std::move(input.fields);
        return true;
    }
    [[noreturn]] static void throw_stopped(detail::HttpClientResponseState& state) {
        if (state.failure) {
            std::rethrow_exception(state.failure);
        }
        throw HttpClientError(HttpClientError::Code::kCancelled, "HTTP request upload stopped");
    }
};
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
    upload.output.outputScope.close();
    upload.responseScope.close();
    if (upload.responseTaken) {
        response_.consumer_ = false;
    }
    if (!upload.output.ended) {
        upload.output.stop();
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
    return ::ruvia::make_scoped_operation(state_->upload->output.outputScope,
        detail::write_client_output<upload_output_policy>(state_, detail::http_client_output_write_input{HttpClientResponse(state_, true), std::pmr::string(bytes, state_->resource)}));
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
    return ::ruvia::make_scoped_operation(state_->upload->output.outputScope,
        detail::finish_client_output<upload_output_policy>(state_, detail::http_client_upload_end_input{HttpClientResponse(state_, true), std::move(validation).takeFields()}));
}
bool HttpClientRequestBodyWriter::complete() const noexcept {
    return state_ == nullptr || state_->upload->output.ended;
}

ScopedOperation<HttpClientResponse> HttpClientExchange::response() & {
    auto* state = response_.state_;
    if (state == nullptr || state->upload->responseTaken) {
        throw std::logic_error("HTTP exchange response has already been transferred");
    }
    if (auto* domain = state->memoryDomain(); domain != nullptr && !domain->worker().isCurrent()) {
        throw std::logic_error("HTTP exchange must run on its owner worker");
    }
    if (state->upload->responseScope.has_pending_operations()) {
        throw std::logic_error("HTTP exchange response operation is already active");
    }
    return ::ruvia::make_scoped_operation(state->upload->responseScope, receiveOwned(HttpClientResponse(state, true)));
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
