#include <ruvia/core/detail/WorkerDispatcher.h>

#include <asio/io_context.hpp>

#include <cstdlib>
#include <exception>
#include <memory>

namespace {

[[noreturn]] void expectedTermination() noexcept {
    std::_Exit(EXIT_SUCCESS);
}

}

int main() {
    asio::io_context ioContext;
    ruvia::detail::WorkerDispatcher dispatcher(ioContext, 1);

    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            static_cast<void>(dispatcher.post([] {}));
        } catch (const std::bad_weak_ptr&) {
            continue;
        }
        return EXIT_FAILURE;
    }

    std::set_terminate(expectedTermination);
    dispatcher.deferOrTerminate([] {});
    return EXIT_FAILURE;
}
