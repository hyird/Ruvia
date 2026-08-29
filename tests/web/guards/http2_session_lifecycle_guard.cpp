#include <ruvia/web/detail/http2/Http2SansIoSessionLifecycle.h>

#include <stdexcept>
#include <string_view>

namespace {

using ruvia::detail::Http2SansIoSessionLifecycle;
using ruvia::detail::Http2SansIoSessionPhase;

bool cleanShutdown() {
    Http2SansIoSessionLifecycle lifecycle;
    if (lifecycle.phase() != Http2SansIoSessionPhase::kRunning || lifecycle.stopping() ||
        lifecycle.writeFailed() || lifecycle.writerDone()) {
        return false;
    }
    lifecycle.beginStopping();
    if (lifecycle.phase() != Http2SansIoSessionPhase::kStopping || !lifecycle.stopping() ||
        lifecycle.writerDone()) {
        return false;
    }
    lifecycle.markWriterDone();
    return lifecycle.phase() == Http2SansIoSessionPhase::kWriterDone && lifecycle.writerDone();
}

bool failedWriteThenShutdown() {
    Http2SansIoSessionLifecycle lifecycle;
    lifecycle.markWriteFailed();
    if (lifecycle.phase() != Http2SansIoSessionPhase::kWriteFailed || !lifecycle.writeFailed() ||
        lifecycle.stopping()) {
        return false;
    }
    lifecycle.beginStopping();
    if (lifecycle.phase() != Http2SansIoSessionPhase::kStoppingAfterWriteFailure ||
        !lifecycle.writeFailed() || !lifecycle.stopping()) {
        return false;
    }
    lifecycle.markWriterDone();
    lifecycle.markWriteFailed();
    return lifecycle.phase() == Http2SansIoSessionPhase::kWriterDoneAfterWriteFailure &&
           lifecycle.writerDone() && lifecycle.writeFailed();
}

bool writerFailureSurvivesJoinState() {
    Http2SansIoSessionLifecycle lifecycle;
    lifecycle.recordWriterFailure(std::make_exception_ptr(std::runtime_error("h2 writer failed")));
    lifecycle.beginStopping();
    lifecycle.markWriterDone();
    try {
        lifecycle.rethrowWriterFailure();
    } catch (const std::runtime_error& error) {
        return lifecycle.writerDone() && std::string_view(error.what()) == "h2 writer failed";
    }
    return false;
}

}  // namespace

int main() {
    return cleanShutdown() && failedWriteThenShutdown() && writerFailureSurvivesJoinState() ? 0 : 1;
}
