#pragma once

#include <asio.hpp>

#include "../AsioAwait.h"
#include "ruvia/app/Task.h"

namespace ruvia::detail {

template <typename Stream>
Task<bool> writeContinue(Stream& stream) {
    const auto ec = co_await asyncError([&stream](auto handler) mutable {
        asio::async_write(
            stream,
            asio::buffer("HTTP/1.1 100 Continue\r\n\r\n", 25),
            std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
