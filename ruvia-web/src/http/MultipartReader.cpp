#include "ruvia/web/MultipartReader.h"

#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/Bytes.h"
#include "ruvia/core/Task.h"

namespace ruvia {

ScopedOperation<std::optional<MultipartStreamPart>> MultipartReader::read() & {
    registration_.require_active();
    if (operationScope_.has_pending_operations()) {
        throw std::logic_error("multipart body read is already in progress");
    }
    return ::ruvia::make_scoped_operation(operationScope_, readTask());
}

BodyReader& MultipartReader::bodyReader() const {
    registration_.require_active();
    return *bodyReader_;
}

void MultipartReader::expire_capability(void* target) noexcept {
    auto& reader = *static_cast<MultipartReader*>(target);
    reader.operationScope_.close();
    reader.bodyReader_ = nullptr;
    reader.state_.emplace<ExpiredState>();
}

Task<std::optional<MultipartStreamPart>> MultipartReader::readTask() {
    if (std::holds_alternative<FinishedState>(state_)) {
        co_return std::nullopt;
    }
    if (std::holds_alternative<FailedState>(state_)) {
        throw std::logic_error("multipart body consumption previously failed");
    }
    if (!std::holds_alternative<ReceivingState>(state_)) {
        throw std::logic_error("multipart body lifetime has expired");
    }

    ReadGuard readGuard(*this);
    for (;;) {
        auto result = std::get<ReceivingState>(state_).parser.poll();
        if (const auto* part = result.part()) {
            readGuard.commit();
            co_return *part;
        }
        if (result.done() != nullptr) {
            // RFC 2046 permits an epilogue after the closing delimiter. It is
            // semantically ignored but the HTTP body still has to be consumed
            // before the connection can be reused.
            while (co_await bodyReader().read()) {
            }
            state_.emplace<FinishedState>();
            readGuard.commit();
            co_return std::nullopt;
        }
        if (result.needInput() != nullptr) {
            auto chunk = co_await bodyReader().read();
            if (!chunk) {
                std::get<ReceivingState>(state_).parser.finishInput();
            } else {
                std::get<ReceivingState>(state_).parser.feed(asChars(*chunk));
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
