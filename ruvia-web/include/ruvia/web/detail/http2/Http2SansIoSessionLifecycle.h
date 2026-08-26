#pragma once

#include <exception>
#include <utility>

namespace ruvia::detail {

enum class Http2SansIoSessionPhase {
    kRunning,
    kWriteFailed,
    kStopping,
    kStoppingAfterWriteFailure,
    kWriterDone,
    kWriterDoneAfterWriteFailure,
};

// Same-executor session state. Reader completion, write failure, and writer
// join are ordered transitions rather than independently mutable flags.
class Http2SansIoSessionLifecycle final {
public:
    [[nodiscard]] Http2SansIoSessionPhase phase() const noexcept {
        return phase_;
    }

    [[nodiscard]] bool writeFailed() const noexcept {
        return phase_ == Http2SansIoSessionPhase::kWriteFailed ||
               phase_ == Http2SansIoSessionPhase::kStoppingAfterWriteFailure ||
               phase_ == Http2SansIoSessionPhase::kWriterDoneAfterWriteFailure;
    }

    [[nodiscard]] bool stopping() const noexcept {
        return phase_ == Http2SansIoSessionPhase::kStopping ||
               phase_ == Http2SansIoSessionPhase::kStoppingAfterWriteFailure ||
               phase_ == Http2SansIoSessionPhase::kWriterDone ||
               phase_ == Http2SansIoSessionPhase::kWriterDoneAfterWriteFailure;
    }

    [[nodiscard]] bool writerDone() const noexcept {
        return phase_ == Http2SansIoSessionPhase::kWriterDone ||
               phase_ == Http2SansIoSessionPhase::kWriterDoneAfterWriteFailure;
    }

    void markWriteFailed() noexcept {
        if (phase_ == Http2SansIoSessionPhase::kRunning) {
            phase_ = Http2SansIoSessionPhase::kWriteFailed;
        } else if (phase_ == Http2SansIoSessionPhase::kStopping) {
            phase_ = Http2SansIoSessionPhase::kStoppingAfterWriteFailure;
        }
    }

    void beginStopping() noexcept {
        if (phase_ == Http2SansIoSessionPhase::kRunning) {
            phase_ = Http2SansIoSessionPhase::kStopping;
        } else if (phase_ == Http2SansIoSessionPhase::kWriteFailed) {
            phase_ = Http2SansIoSessionPhase::kStoppingAfterWriteFailure;
        }
    }

    void markWriterDone() noexcept {
        if (phase_ == Http2SansIoSessionPhase::kStopping) {
            phase_ = Http2SansIoSessionPhase::kWriterDone;
        } else if (phase_ == Http2SansIoSessionPhase::kStoppingAfterWriteFailure) {
            phase_ = Http2SansIoSessionPhase::kWriterDoneAfterWriteFailure;
        }
    }

    void recordWriterFailure(std::exception_ptr failure) noexcept {
        if (writerFailure_ == nullptr) {
            writerFailure_ = std::move(failure);
        }
    }

    void rethrowWriterFailure() const {
        if (writerFailure_ != nullptr) {
            std::rethrow_exception(writerFailure_);
        }
    }

private:
    Http2SansIoSessionPhase phase_{Http2SansIoSessionPhase::kRunning};
    std::exception_ptr writerFailure_;
};

}  // namespace ruvia::detail
