#include "ruvia/core/detail/util/FailureReport.h"

#include <cstdio>
#include <exception>
#include <string_view>

namespace ruvia::detail {

void reportUnhandledFailure(
    std::string_view context,
    std::exception_ptr exception) noexcept {
    if (exception == nullptr) {
        return;
    }
    // Rethrowing is the only way to read the exception's message. A nested
    // failure here (a what() that allocates and fails) still leaves the
    // unknown-exception line below, so the report never disappears entirely.
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        std::fprintf(
            stderr,
            "ruvia: %.*s failed: %s\n",
            static_cast<int>(context.size()),
            context.data(),
            error.what());
    } catch (...) {
        std::fprintf(
            stderr,
            "ruvia: %.*s failed: unknown exception\n",
            static_cast<int>(context.size()),
            context.data());
    }
}

}  // namespace ruvia::detail
