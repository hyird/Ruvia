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
// writing the response. They also provide a coarse socket-level deadman for a
// handler that is suspended without doing I/O. This deadline adds the
// cooperative route-level signal that lets that handler unwind and answer.
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
// What it does NOT stop: a handler awaiting nothing cancellable -- a pure
// computation, or a wait the application built out of raw Asio without
// threading a stop token through it. Every wait Ruvia hands a handler takes
// one, a streaming producer's sleep() included, so what remains is code the
// framework never sees.
//
// That is not equivalent to an unbounded connection. A handler that is
// suspended still lets the worker run, so the connection scanner eventually
// closes the socket through the active protocol phase: HTTP/1 dispatch after a
// complete head uses keepaliveTimeout; HTTP/2 active stream runtimes use the
// payload phase and therefore clientBodyTimeout. The consequences differ, which
// is the whole reason to prefer a deadline: the scanner drops the connection
// with no response, while a deadline lets the handler unwind and answer.
//
// A handler that never suspends at all -- pure computation -- is not caught by
// either. The scanner runs on the same single-threaded worker, so a handler
// that will not yield prevents the scan that would have noticed it.
//
// Do not reach for this expecting a hard timeout; it stops the WAITS, not the
// handler.
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
