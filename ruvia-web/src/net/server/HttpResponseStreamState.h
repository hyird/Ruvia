#pragma once

#include "HttpResponseTrailers.h"

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

    void bindContext(Context* context) noexcept {
        context_ = context;
    }

    [[nodiscard]] Context& requireContextBeforeCommit() const {
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
        if (context_ == nullptr) {
            throw std::logic_error("response stream context is not bound");
        }
        return *context_;
    }

    void markCommitted(bool bodyForbidden) noexcept {
        bodyForbidden_ = bodyForbidden;
        committed_ = true;
    }

    void markEnded() noexcept {
        ended_ = true;
    }

    void ensureBodyAllowed() const {
        // A body chunk after end() would land after the terminal 0\r\n\r\n (HTTP/1.1)
        // or after END_STREAM (HTTP/2), desyncing framing on the connection. Reject
        // it, mirroring the trailer guard (ensureTrailerOpen) so both post-end write
        // paths fail identically.
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
    bool committed_{false};
    bool ended_{false};
    bool bodyForbidden_{false};
};

}  // namespace detail
}  // namespace ruvia
