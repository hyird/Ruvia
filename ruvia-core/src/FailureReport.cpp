#include "ruvia/core/detail/util/FailureReport.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string_view>

namespace ruvia::detail {

namespace {

// Reporting is loudest exactly when the process is least able to afford it: an
// allocation failure or a dead upstream fails every connection at once, and
// stderr is a synchronous write that can block the reporting thread when it is
// redirected to a slow or full file. A flood would then slow the very recovery
// it is describing, and bury the first failure -- the informative one -- under
// thousands of identical consequences.
//
// So the reporter emits at most kBurst lines per kWindow and counts the rest.
// The count is not lost: the next line that gets through carries it.
constexpr std::size_t kBurst = 20;
constexpr auto kWindow = std::chrono::seconds(1);

std::mutex& rateMutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

struct RateState final {
    std::chrono::steady_clock::time_point windowStart{};
    std::size_t emitted{0};
    std::size_t suppressed{0};
    bool started{false};
};

RateState& rateState() noexcept {
    static RateState state;
    return state;
}

// Decides whether this failure gets a line. Returns the number of failures
// suppressed since the last emitted line, to be reported alongside it.
struct RateDecision final {
    bool emit{false};
    std::size_t suppressed{0};
};

[[nodiscard]] RateDecision admit() noexcept {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard guard(rateMutex());
    auto& state = rateState();

    if (!state.started || now - state.windowStart >= kWindow) {
        state.started = true;
        state.windowStart = now;
        state.emitted = 0;
    }
    if (state.emitted >= kBurst) {
        ++state.suppressed;
        return {};
    }
    ++state.emitted;
    RateDecision decision;
    decision.emit = true;
    decision.suppressed = state.suppressed;
    state.suppressed = 0;
    return decision;
}

void writeLine(std::string_view context, std::string_view what, std::size_t suppressed) noexcept {
    if (suppressed == 0) {
        std::fprintf(stderr, "ruvia: %.*s failed: %.*s\n", static_cast<int>(context.size()),
            context.data(), static_cast<int>(what.size()), what.data());
        return;
    }
    std::fprintf(stderr, "ruvia: %.*s failed: %.*s (+%zu suppressed)\n",
        static_cast<int>(context.size()), context.data(), static_cast<int>(what.size()),
        what.data(), suppressed);
}

}  // namespace

void reportUnhandledFailure(std::string_view context, std::exception_ptr exception) noexcept {
    if (exception == nullptr) {
        return;
    }
    const auto decision = admit();
    if (!decision.emit) {
        return;
    }
    // Rethrowing is the only way to read the exception's message. A nested
    // failure here (a what() that allocates and fails) still leaves the
    // unknown-exception line below, so the report never disappears entirely.
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        writeLine(context, error.what(), decision.suppressed);
    } catch (...) {
        writeLine(context, "unknown exception", decision.suppressed);
    }
}

}  // namespace ruvia::detail
