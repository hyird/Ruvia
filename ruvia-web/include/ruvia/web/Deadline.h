#pragma once

#include <chrono>
#include <cstdint>

#include "ruvia/core/Task.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/Next.h"

namespace ruvia {

// How long a route's handler may take before the request is told to stop:
//
//     RUVIA_POST("/report", build, ruvia::Deadline<2000>);
//
// The server's phase timeouts bound reading the head, reading the body and
// writing the response. Between them -- the handler's own execution -- was
// unbounded, so a handler awaiting something that never completes held its
// connection until the client gave up.
//
// This is COOPERATIVE, and the name says deadline rather than timeout for that
// reason. A suspended coroutine cannot be abandoned in C++ without destroying a
// frame something else still points at, so nothing is forcibly aborted. What
// happens is that Context::stopToken() is stopped, and every wait that TAKES
// that token returns at once. The handler then unwinds normally and onError
// turns it into whatever that failure should be.
//
// What it bounds, precisely: waits you pass c.stopToken() to -- db(), redis(),
// httpClient()'s sendRequest, runBlocking(). That covers the ordinary cause of
// a stuck handler, which is a slow dependency.
//
// What it does NOT bound, and there is currently no backstop for either:
//   - a handler awaiting nothing cancellable, a pure computation say;
//   - a streaming producer's sleep(), which watches worker shutdown rather than
//     this token, so a committed stream is unaffected.
// Both hold the connection until the client gives up. Do not reach for this
// expecting a hard timeout; it stops the WAITS, not the handler.
//
// Composition follows the one rule every policy with an app-wide and a
// route-level form follows: the narrower scope may only TIGHTEN. A route cannot
// extend App::setDeadline()'s handler deadline, and where a controller-wide and
// a route-specific declaration both exist the stricter wins.
template <std::int64_t Milliseconds>
class Deadline final : public Middleware<Deadline<Milliseconds>> {
public:
    static_assert(Milliseconds > 0, "deadline must be greater than 0ms");

    static constexpr std::int64_t ruviaDeadlineMs = Milliseconds;

    // Declaration only: the server arms the deadline before dispatch, so there
    // is nothing to do in the chain.
    Task<void> handle(Context&, Next& next) {
        co_await next();
    }
};

}  // namespace ruvia
