#pragma once

#include "net/server/HttpResponseTrailers.h"
#include "ruvia/http/HttpResponse.h"

#include <stdexcept>
#include <string_view>

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

    [[nodiscard]] bool bodyForbidden() const noexcept {
        return bodyForbidden_;
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

    void markCommitted(bool bodyForbidden) noexcept {
        bodyForbidden_ = bodyForbidden;
        committed_ = true;
    }

    void markEnded() noexcept {
        ended_ = true;
    }

    void ensureBodyAllowed() const {
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
        if (bodyForbidden_) {
            throw std::logic_error("response status does not allow a stream body");
        }
    }

    void ensureTrailerAllowed(std::string_view name, std::string_view value) const {
        ensureTrailerOpen();
        if (!responseTrailerFieldValid(name, value)) {
            throw std::invalid_argument("invalid response trailer field");
        }
    }

private:
    void ensureTrailerOpen() const {
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
    }

    Context* context_{nullptr};
    StreamingHeadThunk streamingHead_{nullptr};
    bool committed_{false};
    bool ended_{false};
    bool bodyForbidden_{false};
};

}  // namespace detail
}  // namespace ruvia
