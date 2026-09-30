#include "ruvia/web/HttpClient.h"

#include <exception>
#include <stdexcept>
#include <utility>

#include <asio/bind_executor.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/web/detail/client/HttpClientState.h"

namespace ruvia::detail {

EventLoop HttpClientState::requireLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("HTTP client requires a valid event loop");
    }
    return loop;
}

HttpClientState::HttpClientState(EventLoop loop, const HttpClientConfig& config,
    HttpClientResultBudgetConfig resultBudget)
    : loop_(requireLoop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      clients_(loop_.ioContext(), worker_, memory_.resource(), config, resultBudget),
      closeState_(loop_, worker_) {}

HttpClientState::~HttpClientState() {
    if (phase_.load(std::memory_order_acquire) != Phase::kClosed || !closeState_.complete() ||
        operationScope_.hasPendingOperations()) {
        std::terminate();
    }
}

void HttpClientState::bindStop() {
    try {
        std::weak_ptr<HttpClientState> weak = shared_from_this();
        stopRegistration_ = loop_.onStop([weak = std::move(weak)]() -> Task<void> {
            if (const auto state = weak.lock()) {
                co_await shutdownOwned(state, ClientCloseState::ObservationMode::kRetirement);
            }
        });
    } catch (...) {
        clients_.closeNow();
        phase_.store(Phase::kClosed, std::memory_order_release);
        closeState_.completeBeforePublication();
        throw;
    }
}

HttpClientHandle HttpClientState::handle(OperationOptions options) {
    requireOpenOnWorker();
    options = mergeOperationOptions(
        OperationOptions{.timeout = std::nullopt, .stopToken = stopSource_.token()},
        std::move(options));
    return clients_.get(operationScope_, std::move(options));
}

HttpClientStats HttpClientState::stats() {
    requireOpenOnWorker();
    return clients_.get(operationScope_).stats();
}

std::string_view HttpClientState::host() {
    requireOpenOnWorker();
    const auto client = clients_.get(operationScope_);
    return client.host();
}

std::uint16_t HttpClientState::port() {
    requireOpenOnWorker();
    return clients_.get(operationScope_).port();
}

HttpScheme HttpClientState::scheme() {
    requireOpenOnWorker();
    return clients_.get(operationScope_).scheme();
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
    if (!phase_.compare_exchange_strong(
            expected, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    if (worker_.isCurrent()) {
        startCloseOnWorker();
        return;
    }
    try {
        if (!WorkerHandleAccess::deferIfAttached(
                worker_, [state = shared_from_this()] { state->startCloseOnWorker(); })) {
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

Task<void> HttpClientState::shutdown() {
    return shutdownOwned(shared_from_this(), ClientCloseState::ObservationMode::kCaller);
}

Task<void> HttpClientState::shutdownOwned(
    std::shared_ptr<HttpClientState> state, ClientCloseState::ObservationMode mode) {
    if (!state->worker_.isCurrent()) {
        throw std::logic_error("HTTP client shutdown must run on its bound event loop");
    }
    state->startCloseOnWorker();
    while (!state->closeState_.complete()) {
        co_await state->closeState_.wait();
    }
    state->closeState_.observeFailure(mode);
}

void HttpClientState::startCloseOnWorker() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    auto expected = Phase::kOpen;
    (void)phase_.compare_exchange_strong(
        expected, Phase::kClosing, std::memory_order_acq_rel, std::memory_order_acquire);
    stopSource_.requestStop();
    clients_.closeNow();
    if (phase_.load(std::memory_order_acquire) == Phase::kClosed || !closeState_.startTask()) {
        return;
    }
    try {
        auto state = shared_from_this();
        asyncStartTask(closeOnWorker(),
            asio::bind_executor(loop_.executor(),
                [state](const TaskCompletionResult<void>& result) { state->finishClose(result); }));
    } catch (...) {
        phase_.store(Phase::kClosed, std::memory_order_release);
        std::terminate();
    }
}

Task<void> HttpClientState::closeOnWorker() {
    std::exception_ptr failure;
    try {
        co_await clients_.join();
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        co_await operationScope_.closeAndJoin();
    } catch (...) {
        if (failure == nullptr) {
            failure = std::current_exception();
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

void HttpClientState::finishClose(const TaskCompletionResult<void>& result) {
    if (operationScope_.hasPendingOperations()) {
        std::terminate();
    }
    phase_.store(Phase::kClosed, std::memory_order_release);
    const auto* failure = result.failure();
    closeState_.finish(failure == nullptr ? std::exception_ptr{} : failure->exception());
}

}  // namespace ruvia::detail

namespace ruvia {

HttpClient::HttpClient(EventLoop loop, const HttpClientConfig& config,
    HttpClientResultBudgetConfig resultBudget)
    : state_(std::make_shared<detail::HttpClientState>(
          std::move(loop), config, resultBudget)) {
    state_->bindStop();
}

HttpClient::~HttpClient() {
    state_->requestClose();
}

HttpClientHandle HttpClient::withOptions(OperationOptions options) const& {
    return state_->handle(std::move(options));
}

ScopedOperation<HttpClientResponse> HttpClient::send(const HttpClientRequestView& request) const& {
    return withOptions({}).send(request);
}

void HttpClient::close() noexcept {
    state_->requestClose();
}

Task<void> HttpClient::shutdown() & {
    return state_->shutdown();
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
