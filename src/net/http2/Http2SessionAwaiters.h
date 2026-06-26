#pragma once

#include <coroutine>
#include <cstdint>

#include "Http2BodyQueue.h"
#include "Http2FlowControl.h"

namespace ruvia::detail {

template <typename Session>
class Http2DispatchGuard final {
public:
    explicit Http2DispatchGuard(Session& session) noexcept : session_(&session) {
        ++session_->dispatchDepth_;
    }

    Http2DispatchGuard(const Http2DispatchGuard&) = delete;
    Http2DispatchGuard& operator=(const Http2DispatchGuard&) = delete;

    ~Http2DispatchGuard() {
        if (session_ != nullptr && session_->dispatchDepth_ > 0) {
            --session_->dispatchDepth_;
        }
    }

private:
    Session* session_;
};

template <typename Session>
class Http2WriteTurnAwaiter final {
public:
    explicit Http2WriteTurnAwaiter(Session& session) noexcept : session_(&session) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return !session_->writeInProgress_;
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        session_->writeWaiters_.push(continuation);
        return true;
    }

    void await_resume() const noexcept {}

private:
    Session* session_;
};

template <typename Session>
class Http2BodyChunkAwaiter final {
public:
    Http2BodyChunkAwaiter(Session& session, std::uint32_t streamId) noexcept
        : session_(&session), streamId_(streamId) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return http2StreamBodyReadReady(session_->findStream(streamId_), session_->closing_);
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        auto* stream = session_->findStream(streamId_);
        if (http2StreamBodyReadReady(stream, session_->closing_)) {
            return false;
        }
        http2SetBodyWaiter(*stream, continuation);
        return true;
    }

    void await_resume() const noexcept {}

private:
    Session* session_;
    std::uint32_t streamId_{0};
};

template <typename Session>
class Http2SendWindowAwaiter final {
public:
    Http2SendWindowAwaiter(Session& session, std::uint32_t streamId) noexcept
        : session_(&session), streamId_(streamId) {}

    [[nodiscard]] bool await_ready() const noexcept {
        const auto* stream = session_->findStream(streamId_);
        return stream == nullptr ||
            stream->isReset() ||
            session_->closing_ ||
            http2SendWindowAvailable(session_->connectionSendWindow_, *stream);
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        const auto* stream = session_->findStream(streamId_);
        if (stream == nullptr ||
            stream->isReset() ||
            session_->closing_ ||
            http2SendWindowAvailable(session_->connectionSendWindow_, *stream)) {
            return false;
        }
        session_->sendWindowWaiters_.push(continuation);
        return true;
    }

    void await_resume() const noexcept {}

private:
    Session* session_;
    std::uint32_t streamId_{0};
};

template <typename Session>
class Http2DispatchDrainAwaiter final {
public:
    explicit Http2DispatchDrainAwaiter(Session& session) noexcept : session_(&session) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return session_->activeDispatches_ == 0;
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (session_->activeDispatches_ == 0) {
            return false;
        }
        session_->dispatchDrainWaiter_ = continuation;
        return true;
    }

    void await_resume() const noexcept {}

private:
    Session* session_;
};

}  // namespace ruvia::detail
