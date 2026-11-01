#pragma once

#include <optional>
#include <system_error>

#include "ruvia/core/stop_token.h"

namespace ruvia::detail {

// Connection-owned terminal state shared by every Web capability derived from
// one HTTP/2 session. The state is confined to the connection worker; a wait
// that must end promptly on termination combines its own stop token with
// token(), and terminate() requests that stop.
class http2_sans_io_termination final {
public:
    http2_sans_io_termination() noexcept = default;

    http2_sans_io_termination(const http2_sans_io_termination&) = delete;
    http2_sans_io_termination& operator=(const http2_sans_io_termination&) = delete;

    [[nodiscard]] bool terminated() const noexcept {
        return static_cast<bool>(error_);
    }

    [[nodiscard]] std::error_code error() const noexcept {
        return error_;
    }

    [[nodiscard]] bool terminate(std::error_code error) noexcept {
        if (terminated()) {
            return false;
        }
        error_ = error ? error : std::make_error_code(std::errc::connection_aborted);
        if (stop_source_) {
            stop_source_->request_stop();
        }
        return true;
    }

    // Stopped once the session terminates (already stopped after it has). The
    // stop state is allocated on first use, so a connection that never waits
    // on termination pays nothing for it.
    [[nodiscard]] stop_token token() {
        if (!stop_source_) {
            stop_source_.emplace();
            if (terminated()) {
                stop_source_->request_stop();
            }
        }
        return stop_source_->token();
    }

private:
    std::error_code error_;
    std::optional<stop_source> stop_source_;
};

}  // namespace ruvia::detail
