#include <cstdlib>
#include <exception>
#include <memory>

#include <asio/io_context.hpp>

#include "ruvia/core/detail/worker/worker_dispatcher.h"

namespace {

[[noreturn]] void expected_termination() noexcept {
    std::_Exit(EXIT_SUCCESS);
}

}  // namespace

int main() {
    asio::io_context io_context;
    ruvia::detail::worker_dispatcher dispatcher(io_context, 1);

    for (int attempt_value = 0; attempt_value < 2; ++attempt_value) {
        try {
            static_cast<void>(dispatcher.post([] {}));
        } catch (const std::bad_weak_ptr&) {
            continue;
        }
        return EXIT_FAILURE;
    }

    std::set_terminate(expected_termination);
    dispatcher.defer_or_terminate([] {});
    return EXIT_FAILURE;
}
