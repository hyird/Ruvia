#pragma once

#include "ruvia/http/HttpResponse.h"

#include <stdexcept>

namespace ruvia {

class Context;

namespace detail {

class ResponseStreamState final {
public:
    [[nodiscard]] bool committed() const noexcept {
        return committed_;
    }

    [[nodiscard]] bool ended() const noexcept {
        return ended_;
    }

    [[nodiscard]] bool bodySuppressed() const noexcept {
        return bodySuppressed_;
    }

    using StreamingHeadThunk = HttpResponse (*)(Context&);

    void bindContext(Context* context, StreamingHeadThunk streamingHead) noexcept {
        context_ = context;
        streamingHead_ = streamingHead;
    }

    [[nodiscard]] HttpResponse streamingHead() const {
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
        if (context_ == nullptr || streamingHead_ == nullptr) {
            throw std::logic_error("response stream context is not bound");
        }
        return streamingHead_(*context_);
    }

    void markCommitted(bool bodySuppressed) noexcept {
        bodySuppressed_ = bodySuppressed;
        committed_ = true;
    }

    void markEnded() noexcept {
        ended_ = true;
    }

    void ensureBodyAllowed() const {
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
        if (bodySuppressed_) {
            throw std::logic_error("response does not allow a stream body");
        }
    }

    void ensureTrailerOpen() const {
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
    }

private:

    Context* context_{nullptr};
    StreamingHeadThunk streamingHead_{nullptr};
    bool committed_{false};
    bool ended_{false};
    bool bodySuppressed_{false};
};

}  // namespace detail
}  // namespace ruvia
