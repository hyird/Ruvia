#pragma once

#include <cstdint>
#include <exception>
#include <utility>

namespace ruvia::detail {

enum class http2_sans_io_session_phase : std::uint8_t {
    running,
    write_failed,
    stopping,
    stopping_after_write_failure,
};

// Same-executor session state. Writer submission is recorded before co_spawn
// can invoke completion inline; completion remains true before reader stopping.
class http2_sans_io_session_lifecycle final {
public:
    [[nodiscard]] http2_sans_io_session_phase phase() const noexcept {
        return phase_;
    }

    [[nodiscard]] bool write_failed() const noexcept {
        return phase_ == http2_sans_io_session_phase::write_failed ||
               phase_ == http2_sans_io_session_phase::stopping_after_write_failure;
    }

    [[nodiscard]] bool stopping() const noexcept {
        return phase_ == http2_sans_io_session_phase::stopping ||
               phase_ == http2_sans_io_session_phase::stopping_after_write_failure;
    }

    [[nodiscard]] bool writer_done() const noexcept {
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

    void mark_write_failed() noexcept {
        if (phase_ == http2_sans_io_session_phase::running) {
            phase_ = http2_sans_io_session_phase::write_failed;
        } else if (phase_ == http2_sans_io_session_phase::stopping) {
            phase_ = http2_sans_io_session_phase::stopping_after_write_failure;
        }
    }

    void begin_stopping() noexcept {
        if (phase_ == http2_sans_io_session_phase::running) {
            phase_ = http2_sans_io_session_phase::stopping;
        } else if (phase_ == http2_sans_io_session_phase::write_failed) {
            phase_ = http2_sans_io_session_phase::stopping_after_write_failure;
        }
    }

    void mark_writer_done() noexcept {
        if (writer_ != writer_state::submitted) {
            std::terminate();
        }
        writer_ = writer_state::completed;
    }

    void record_writer_failure(std::exception_ptr failure) noexcept {
        if (writer_failure_ == nullptr) {
            writer_failure_ = std::move(failure);
        }
    }

    void rethrow_writer_failure() const {
        if (writer_failure_ != nullptr) {
            std::rethrow_exception(writer_failure_);
        }
    }

private:
    enum class writer_state : std::uint8_t { not_submitted,
        submitted,
        completed };
    writer_state writer_{writer_state::not_submitted};
    http2_sans_io_session_phase phase_{http2_sans_io_session_phase::running};
    std::exception_ptr writer_failure_;
};

}  // namespace ruvia::detail
