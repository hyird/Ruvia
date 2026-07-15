#include <ruvia/web/detail/server/Http2SansIoSessionLifecycle.h>

namespace {

using ruvia::detail::Http2SansIoSessionLifecycle;
using ruvia::detail::Http2SansIoSessionPhase;

bool cleanShutdown() {
    Http2SansIoSessionLifecycle lifecycle;
    if (lifecycle.phase() != Http2SansIoSessionPhase::kRunning ||
        lifecycle.stopping() || lifecycle.writeFailed() ||
        lifecycle.writerDone()) {
        return false;
    }
    lifecycle.beginStopping();
    if (lifecycle.phase() != Http2SansIoSessionPhase::kStopping ||
        !lifecycle.stopping() || lifecycle.writerDone()) {
        return false;
    }
    lifecycle.markWriterDone();
    return lifecycle.phase() == Http2SansIoSessionPhase::kWriterDone &&
        lifecycle.writerDone();
}

bool failedWriteThenShutdown() {
    Http2SansIoSessionLifecycle lifecycle;
    lifecycle.markWriteFailed();
    if (lifecycle.phase() != Http2SansIoSessionPhase::kWriteFailed ||
        !lifecycle.writeFailed() || lifecycle.stopping()) {
        return false;
    }
    lifecycle.beginStopping();
    if (lifecycle.phase() !=
            Http2SansIoSessionPhase::kStoppingAfterWriteFailure ||
        !lifecycle.writeFailed() || !lifecycle.stopping()) {
        return false;
    }
    lifecycle.markWriterDone();
    lifecycle.markWriteFailed();
    return lifecycle.phase() ==
            Http2SansIoSessionPhase::kWriterDoneAfterWriteFailure &&
        lifecycle.writerDone() && lifecycle.writeFailed();
}

}  // namespace

int main() {
    return cleanShutdown() && failedWriteThenShutdown() ? 0 : 1;
}
