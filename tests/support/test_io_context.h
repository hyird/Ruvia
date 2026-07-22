#pragma once

#include <asio/io_context.hpp>

#include <deque>

namespace ruvia::test {

// Hands each socket-driven unit test a fresh asio::io_context that is deliberately
// never destroyed. This is a correctness requirement on Windows, not an optimization.
//
// asio's win_iocp backend spawns a dedicated timer thread the first time any
// steady_timer is bound to a context (e.g. via WorkerDispatcher). Joining that thread
// while the context is destroyed can deadlock after a few rapid create/destroy cycles
// on the Windows IOCP backend -- see the long-standing Asio reports #424 and #520.
// Because every socket test builds and tears down its own io_context, that
// teardown is exactly the trigger: once enough of them have run, ~io_context hangs and
// the whole test binary times out. (A no-timer context never spawns the thread, so
// non-socket tests are unaffected -- but sharing one helper keeps the rule uniform.)
//
// Skipping io_context destruction entirely sidesteps the buggy join while keeping each
// test fully isolated with its own context. The contexts are owned by a leaked deque so
// they stay reachable (leak-sanitizer clean) and are reclaimed by the OS at process
// exit; deque never invalidates references to existing elements, so the returned
// reference stays valid for the test's lifetime.
inline asio::io_context& newTestIoContext() {
    static std::deque<asio::io_context>& registry = *new std::deque<asio::io_context>();
    return registry.emplace_back();
}

}  // namespace ruvia::test
