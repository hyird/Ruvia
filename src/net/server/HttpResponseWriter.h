#pragma once

#include <array>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <asio.hpp>

#include "HttpFileFallback.h"
#include "HttpFileZeroCopy.h"
#include "HttpResponseHead.h"
#include "../../http/HttpResponseBodyAccess.h"
#include "../../http/HttpResponseFileAccess.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
Task<void> writeResponseWithScratch(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer& head,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    bool skipBody,
    std::error_code& ec) {
    head.reset();
    const auto policy = responseWritePolicy(response.statusCode());
    appendResponseHead(response, head, policy);
    if (responseHasFileBody(response)) {
        const auto fileBody = responseFileBody(response);
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        if (ec || skipBody || !policy.bodyAllowed || fileBody.length == 0) {
            co_return;
        }

        if (co_await tryWriteFileZeroCopy(stream, fileBody, ec)) {
            co_return;
        }

        co_await writeFileFallback(stream, memory, fileChunkBuffer, fileBody, ec);
        co_return;
    }

    const auto body = skipBody || !policy.bodyAllowed ? std::string_view{} : responseBodyBytes(response);
    if (body.empty()) {
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        co_return;
    }
    if (head.canAppendOnStack(body.size())) {
        head.append(body);
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        co_return;
    }
    const auto headView = head.view();
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(headView), asio::buffer(body)};
    ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
}

template <typename Stream>
Task<void> writeResponseWithLocalHead(
    Stream& stream,
    WorkerMemory& memory,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    bool skipBody,
    std::error_code& ec) {
    ResponseHeadBuffer localHead(memory.allocator<char>());
    co_await writeResponseWithScratch(stream, memory, localHead, fileChunkBuffer, response, skipBody, ec);
}

template <typename Stream>
Task<void> writeResponse(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer* reusableHead,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    bool skipBody,
    std::error_code& ec) {
    if (reusableHead != nullptr) {
        return writeResponseWithScratch(stream, memory, *reusableHead, fileChunkBuffer, response, skipBody, ec);
    }
    return writeResponseWithLocalHead(stream, memory, fileChunkBuffer, response, skipBody, ec);
}

}  // namespace ruvia::detail
