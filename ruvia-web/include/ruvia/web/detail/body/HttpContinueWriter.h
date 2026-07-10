#pragma once

#include <asio.hpp>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/detail/http1/Http1ServerSemantics.h"

namespace ruvia::detail {

template <typename Stream>
Task<bool> writeContinue(Stream& stream) {
    const auto ec = co_await asyncError([&stream](auto handler) mutable {
        asio::async_write(
            stream,
            asio::buffer(kHttp1ContinueResponse),
            std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
