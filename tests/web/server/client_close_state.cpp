#include "test_harness.h"

#include <stdexcept>

#include "ruvia/core/EventLoopPool.h"
#include "ruvia/web/detail/client/ClientCloseState.h"

RUVIA_TEST(client_close_state_wait_rejects_wrong_worker) {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    const auto worker = loop.handle();
    ruvia::detail::ClientCloseState state(worker);

    bool threw = false;
    try {
        auto pending = state.wait();
        static_cast<void>(pending);
    } catch (const std::logic_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}
