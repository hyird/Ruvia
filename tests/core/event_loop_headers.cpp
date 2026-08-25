#include <ruvia/core/EventLoop.h>

#include <concepts>

static_assert(std::copy_constructible<ruvia::EventLoop>);
static_assert(std::move_constructible<ruvia::EventLoopStopRegistration>);

#include <ruvia/core/EventLoopAttachment.h>

static_assert(std::move_constructible<ruvia::EventLoopAttachment>);
static_assert(!std::assignable_from<ruvia::EventLoopAttachment&, ruvia::EventLoopAttachment&&>);

#include <ruvia/core/EventLoopPool.h>

static_assert(!std::copy_constructible<ruvia::EventLoopPool>);

int main() {
    return 0;
}
