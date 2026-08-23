#include "ruvia/web/HttpClient.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include <asio/bind_executor.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/client/HttpClientState.h"

namespace ruvia::detail {

EventLoop HttpClientState::requireLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("HTTP client requires a valid event loop");
    }
    return loop;
}

std::pmr::vector<HttpClientDefinition> HttpClientState::makeDefinitions(const HttpClientConfig& config, std::pmr::memory_resource* resource) {
    std::pmr::vector<HttpClientDefinition> definitions(resource);
    definitions.push_back(HttpClientDefinition{
        std::pmr::string("default", resource),
        HttpClientConfigStorage(config, resource),
    });
    return definitions;
}

HttpClientState::HttpClientState(EventLoop loop, HttpClientConfig config)
    : loop_(requireLoop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      definitions_(makeDefinitions(config, memory_.resource())),
      clients_(loop_.ioContext(), worker_, memory_.resource(), definitions_) {}

HttpClientState::~HttpClientState() {
    if (phase_.load(std::memory_order_acquire) != Phase::kClosed || operationScope_.hasPendingOperations()) {
        std::terminate();
    }
}

void HttpClientState::bindStop() {
    try {
        std::weak_ptr<HttpClientState> weak = shared_from_this();
        stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
            if (const auto state = weak.lock()) {
                state->startCloseOnWorker();
            }
        });
    } catch (...) {
        clients_.closeNow();
        phase_.store(Phase::kClosed, std::memory_order_release);
        throw;
    }
}

HttpClientHandle HttpClientState::handle(OperationOptions options) {
    requireOpenOnWorker();
    options = mergeOperationOptions(OperationOptions{.timeout = std::nullopt, .stopToken = stopSource_.token()}, std::move(options));
    return clients_.get(memory_.resource(), operationScope_).withOptions(std::move(options));
}

HttpClientStats HttpClientState::stats() {
    requireOpenOnWorker();
    return clients_.get(memory_.resource(), operationScope_).stats();
}

std::string_view HttpClientState::host() {
    requireOpenOnWorker();
    const auto client = clients_.get(memory_.resource(), operationScope_);
    return client.host();
}

std::uint16_t HttpClientState::port() {
    requireOpenOnWorker();
    return clients_.get(memory_.resource(), operationScope_).port();
}

HttpScheme HttpClientState::scheme() {
    requireOpenOnWorker();
    return clients_.get(memory_.resource(), operationScope_).scheme();
}

void HttpClientState::requireOpenOnWorker() const {
    if (!worker_.isCurrent()) {
        throw std::logic_error("HTTP client must be used on its bound event loop");
    }
    if (phase_.load(std::memory_order_acquire) != Phase::kOpen) {
        throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client is closing");
    }
}

void HttpClientState::requestClose() noexcept {
    stopSource_.requestStop();
    auto expected = Phase::kOpen;
    if (!phase_.compare_exchange_strong(expected, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    if (worker_.isCurrent()) {
        startCloseOnWorker();
        return;
    }
    try {
        if (!WorkerHandleAccess::deferIfAttached(worker_, [state = shared_from_this()] { state->startCloseOnWorker(); })) {
            if (phase_.load(std::memory_order_acquire) != Phase::kClosed) {
                std::terminate();
            }
        }
    } catch (...) {
        if (phase_.load(std::memory_order_acquire) != Phase::kClosed) {
            std::terminate();
        }
    }
}

void HttpClientState::startCloseOnWorker() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    auto expected = Phase::kOpen;
    (void)phase_.compare_exchange_strong(expected, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire);
    stopSource_.requestStop();
    clients_.closeNow();
    if (closeTaskStarted_ || phase_.load(std::memory_order_acquire) == Phase::kClosed) {
        return;
    }
    closeTaskStarted_ = true;
    try {
        auto state = shared_from_this();
        asyncStartTask(closeOnWorker(), asio::bind_executor(loop_.executor(), [state](TaskCompletionResult<void> result) mutable { state->finishClose(std::move(result)); }));
    } catch (...) {
        phase_.store(Phase::kClosed, std::memory_order_release);
        std::terminate();
    }
}

Task<void> HttpClientState::closeOnWorker() {
    co_await clients_.join();
}

void HttpClientState::finishClose(TaskCompletionResult<void> result) {
    phase_.store(Phase::kClosed, std::memory_order_release);
    if (const auto* failed = result.failure()) {
        std::rethrow_exception(failed->exception());
    }
}

}  // namespace ruvia::detail

namespace ruvia {

HttpClient::HttpClient(EventLoop loop, HttpClientConfig config)
    : state_(std::make_shared<detail::HttpClientState>(std::move(loop), std::move(config))) {
    state_->bindStop();
}

HttpClient::~HttpClient() {
    state_->requestClose();
}

HttpClientHandle HttpClient::withOptions(OperationOptions options) const {
    return state_->handle(std::move(options));
}

ScopedOperation<HttpClientResponse> HttpClient::send(const HttpClientRequestView& request) const {
    return withOptions({}).send(request);
}

void HttpClient::close() noexcept {
    state_->requestClose();
}

HttpClientStats HttpClient::stats() const {
    return state_->stats();
}

std::string_view HttpClient::host() const& {
    return state_->host();
}

std::uint16_t HttpClient::port() const {
    return state_->port();
}

HttpScheme HttpClient::scheme() const {
    return state_->scheme();
}

const WorkerHandle& HttpClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
