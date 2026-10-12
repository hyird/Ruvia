#include "ruvia/core/failure_report.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <mutex>
#include <string_view>
#include <typeinfo>

namespace ruvia {

namespace {

// Reporting is loudest exactly when the process is least able to afford it: an
// allocation failure or a dead upstream fails every connection at once, and
// stderr is a synchronous write that can block the reporting thread when it is
// redirected to a slow or full file. A flood would then slow the very recovery
// it is describing, and bury the first failure -- the informative one -- under
// thousands of identical consequences.
//
// So the reporter emits at most burst lines per window and counts the rest.
// The count is not lost: the next line that gets through carries it.
constexpr std::size_t burst = 20;
constexpr auto window = std::chrono::seconds(1);

std::mutex& rate_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

struct rate_state final {
    std::chrono::steady_clock::time_point window_start_{};
    std::size_t emitted_{0};
    std::size_t suppressed_{0};
    bool started_{false};
};

rate_state& get_rate_state() noexcept {
    static rate_state state;
    return state;
}

// Decides whether this failure gets a line. Returns the number of failures
// suppressed since the last emitted line, to be reported alongside it.
struct rate_decision final {
    bool emit_{false};
    std::size_t suppressed_{0};
};

[[nodiscard]] rate_decision admit() noexcept {
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard guard(rate_mutex());
    auto& state_value = get_rate_state();

    if (!state_value.started_ || now - state_value.window_start_ >= window) {
        state_value.started_ = true;
        state_value.window_start_ = now;
        state_value.emitted_ = 0;
    }
    if (state_value.emitted_ >= burst) {
        ++state_value.suppressed_;
        return {};
    }
    ++state_value.emitted_;
    rate_decision decision;
    decision.emit_ = true;
    decision.suppressed_ = state_value.suppressed_;
    state_value.suppressed_ = 0;
    return decision;
}

void write_line(std::string_view context_value, std::string_view what, std::size_t occurrences,
    std::size_t suppressed) noexcept {
    // One fprintf per line keeps concurrent reports from interleaving.
    char occurrences_text[32] = "";
    if (occurrences > 1) {
        std::snprintf(occurrences_text, sizeof(occurrences_text), " (x%zu)", occurrences);
    }
    char suppressed_text[48] = "";
    if (suppressed != 0) {
        std::snprintf(suppressed_text, sizeof(suppressed_text), " (+%zu suppressed)", suppressed);
    }
    std::fprintf(stderr, "ruvia: %.*s failed: %.*s%s%s\n", static_cast<int>(context_value.size()),
        context_value.data(), static_cast<int>(what.size()), what.data(), occurrences_text, suppressed_text);
}

struct failure_identity final {
    const std::type_info* type_{nullptr};
    const char* what_{nullptr};
};

// The exception object outlives this call: `exception` keeps it alive, so the
// returned what() pointer stays valid while the caller holds the pointer.
[[nodiscard]] failure_identity identify(const std::exception_ptr& exception) noexcept {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return {&typeid(error), error.what()};
    } catch (...) {
        return {};
    }
}

}  // namespace

void report_unhandled_failure(
    std::string_view context_value, std::exception_ptr exception, std::size_t occurrences) noexcept {
    if (exception == nullptr) {
        return;
    }
    const auto decision = admit();
    if (!decision.emit_) {
        return;
    }
    // Rethrowing is the only way to read the exception's message. A nested
    // failure here (a what() that allocates and fails) still leaves the
    // unknown-exception line below, so the report never disappears entirely.
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        write_line(context_value, error.what(), occurrences, decision.suppressed_);
    } catch (...) {
        write_line(context_value, "unknown exception", occurrences, decision.suppressed_);
    }
}

void report_unhandled_failure(std::string_view context_value, std::string_view what) noexcept {
    const auto decision = admit();
    if (decision.emit_) {
        write_line(context_value, what, 1, decision.suppressed_);
    }
}

bool equivalent_failures(const std::exception_ptr& left, const std::exception_ptr& right) noexcept {
    if (left == right) {
        return true;
    }
    if (left == nullptr || right == nullptr) {
        return false;
    }
    const auto left_identity = identify(left);
    const auto right_identity = identify(right);
    return left_identity.type_ != nullptr && right_identity.type_ != nullptr &&
           *left_identity.type_ == *right_identity.type_ &&
           std::string_view(left_identity.what_) == std::string_view(right_identity.what_);
}

}  // namespace ruvia
