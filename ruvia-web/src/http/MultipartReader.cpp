#include "ruvia/web/MultipartReader.h"

#include "ruvia/core/Task.h"
#include <stdexcept>
#include <utility>

namespace ruvia {

ScopedOperation<std::optional<MultipartStreamPart>> MultipartReader::read() {
    requireActive();
    return detail::makeScopedOperation(operationScope_, readTask());
}

BodyReader& MultipartReader::bodyReader() const {
    requireActive();
    return *bodyReader_;
}

void MultipartReader::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& reader = static_cast<MultipartReader&>(capability);
    reader.operationScope_.close();
    reader.bodyReader_ = nullptr;
    reader.state_ = State::kFailed;
}

Task<std::optional<MultipartStreamPart>> MultipartReader::readTask() {
    if (state_ == State::kFinished) {
        co_return std::nullopt;
    }
    if (state_ == State::kReading) {
        throw std::logic_error("multipart body read is already in progress");
    }
    if (state_ == State::kFailed) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    state_ = State::kReading;
    ReadGuard readGuard(state_);
    for (;;) {
        auto result = parser_.poll();
        if (const auto* part = result.part()) {
            readGuard.commit(State::kReady);
            co_return *part;
        }
        if (result.done() != nullptr) {
            // RFC 2046 permits an epilogue after the closing delimiter. It is
            // semantically ignored but the HTTP body still has to be consumed
            // before the connection can be reused.
            while (co_await bodyReader().read()) {
            }
            readGuard.commit(State::kFinished);
            co_return std::nullopt;
        }
        if (result.needInput() != nullptr) {
            auto chunk = co_await bodyReader().read();
            if (!chunk) {
                parser_.finishInput();
            } else {
                parser_.feed(*chunk);
            }
            continue;
        }
        if (const auto* failure = result.failure()) {
            throw failure->protocolError();
        }
        throw std::logic_error("unexpected multipart poll result");
    }
}

}  // namespace ruvia
