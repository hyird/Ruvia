#pragma once

#include <array>
#include <stdexcept>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/Http1InterimResponseWriter.h"

namespace ruvia::detail {

template <typename Stream>
Task<void> writeHttp1Continue(Stream& stream) {
    std::array<char, 32> headBuffer{};
    const HttpInterimResponseHead response(100);
    const auto result = Http1InterimResponseWriter().prepare(
        response, headBuffer);
    const auto* const prepared = result.prepared();
    if (prepared == nullptr) {
        throw std::logic_error("failed to prepare HTTP/1 100 Continue");
    }

    const auto writeCompletion = co_await asyncAsio(
        [&stream, head = prepared->head()](auto handler) mutable {
            asio::async_write(
                stream,
                asio::buffer(head),
                std::move(handler));
        });
    const auto ec = writeCompletion.errorCode();
    if (ec) {
        throw std::system_error(ec, "failed to write HTTP/1 100 Continue");
    }
}

}  // namespace ruvia::detail
