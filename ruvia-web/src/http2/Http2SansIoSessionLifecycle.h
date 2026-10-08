#pragma once

#include <cstdint>
#include <exception>
#include <utility>

namespace ruvia::detail {

enum class Http2SansIoSessionPhase : std::uint8_t {
    kRunning,
    kWriteFailed,
    kStopping,
    kStoppingAfterWriteFailure,
};

// Same-executor session state. Writer submission is recorded before co_spawn
// can invoke completion inline; completion remains true before reader stopping.
class Http2SansIoSessionLifecycle final {
public:
    [[nodiscard]] Http2SansIoSessionPhase phase() const noexcept {
        return phase_;
    }

    [[nodiscard]] bool writeFailed() const noexcept {
        return phase_ == Http2SansIoSessionPhase::kWriteFailed ||
               phase_ == Http2SansIoSessionPhase::kStoppingAfterWriteFailure;
    }

    [[nodiscard]] bool stopping() const noexcept {
        return phase_ == Http2SansIoSessionPhase::kStopping ||
               phase_ == Http2SansIoSessionPhase::kStoppingAfterWriteFailure;
    }

    [[nodiscard]] bool writerDone() const noexcept {
        return writer_ == writer_state::completed;
    }

    [[nodiscard]] bool writer_join_pending() const noexcept {
        return writer_ == writer_state::submitted;
    }

    void mark_writer_submitted() noexcept {
        if (writer_ != writer_state::not_submitted) {
            std::terminate();
        }
        writer_ = writer_state::submitted;
    }

    void mark_writer_launch_failed() noexcept {
        if (writer_ == writer_state::submitted) {
            writer_ = writer_state::not_submitted;
        }
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
        if (writer_ != writer_state::submitted) {
            std::terminate();
        }
        writer_ = writer_state::completed;
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
    enum class writer_state : std::uint8_t { not_submitted,
        submitted,
        completed };
    writer_state writer_{writer_state::not_submitted};
    Http2SansIoSessionPhase phase_{Http2SansIoSessionPhase::kRunning};
    std::exception_ptr writerFailure_;
};

}  // namespace ruvia::detail
