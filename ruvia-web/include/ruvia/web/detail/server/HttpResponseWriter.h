#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <asio.hpp>

#include "ruvia/web/detail/server/HttpFileFallback.h"
#include "ruvia/web/detail/server/HttpFileZeroCopy.h"
#include "ruvia/web/detail/server/Http1BufferedResponseWrite.h"
#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
Task<Http1BufferedResponseWriteResult> writeResponseWithScratch(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer& head,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    const Http1BufferedResponsePlan& responsePlan) {
    const auto& writePlan = responsePlan.writePlan();
    head.reset();
    appendResponseHead(
        response,
        head,
        responsePlan.headPlan());
    const auto responseHeadBytes = head.view().size();
    const auto& responseContent = responseBody(response);
    if (const auto fileBody = responseContent.file()) {
        auto [ec, bytesTransferred] = co_await asyncResult<std::size_t>(
            [&stream, headView = head.view()](auto handler) mutable {
                asio::async_write(
                    stream,
                    asio::buffer(headView),
                    std::move(handler));
            });
        if (ec) {
            co_return classifyHttp1BufferedResponseWrite(
                responsePlan,
                responseHeadBytes,
                ec,
                bytesTransferred);
        }
        if (!writePlan.sendBody()) {
            co_return classifyHttp1BufferedResponseWrite(
                responsePlan,
                responseHeadBytes,
                {},
                bytesTransferred);
        }

        if constexpr (std::is_same_v<std::remove_cvref_t<Stream>, asio::ip::tcp::socket>) {
            std::error_code fileError;
            co_await writeFileZeroCopy(stream, *fileBody, fileError);
            if (fileError != asio::error::operation_not_supported) {
                co_return classifyHttp1BufferedResponseWrite(
                    responsePlan,
                    responseHeadBytes,
                    fileError,
                    responseHeadBytes);
            }
        }

        std::error_code fileError;
        co_await writeFileFallback(
            stream,
            memory,
            fileChunkBuffer,
            *fileBody,
            fileError);
        co_return classifyHttp1BufferedResponseWrite(
            responsePlan,
            responseHeadBytes,
            fileError,
            responseHeadBytes);
    }

    const auto body = writePlan.sendBody()
        ? responseContent.bytes()
        : std::string_view{};
    if (body.empty()) {
        auto [ec, bytesTransferred] = co_await asyncResult<std::size_t>(
            [&stream, headView = head.view()](auto handler) mutable {
                asio::async_write(
                    stream,
                    asio::buffer(headView),
                    std::move(handler));
            });
        co_return classifyHttp1BufferedResponseWrite(
            responsePlan,
            responseHeadBytes,
            ec,
            bytesTransferred);
    }
    if (head.canAppendOnStack(body.size())) {
        head.append(body);
        auto [ec, bytesTransferred] = co_await asyncResult<std::size_t>(
            [&stream, headView = head.view()](auto handler) mutable {
                asio::async_write(
                    stream,
                    asio::buffer(headView),
                    std::move(handler));
            });
        co_return classifyHttp1BufferedResponseWrite(
            responsePlan,
            responseHeadBytes,
            ec,
            bytesTransferred);
    }
    const auto headView = head.view();
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(headView), asio::buffer(body)};
    auto [ec, bytesTransferred] = co_await asyncResult<std::size_t>(
        [&stream, &buffers](auto handler) mutable {
            asio::async_write(stream, buffers, std::move(handler));
        });
    co_return classifyHttp1BufferedResponseWrite(
        responsePlan,
        responseHeadBytes,
        ec,
        bytesTransferred);
}

template <typename Stream>
Task<Http1BufferedResponseWriteResult> writeResponseWithLocalHead(
    Stream& stream,
    WorkerMemory& memory,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    const Http1BufferedResponsePlan& responsePlan) {
    ResponseHeadBuffer localHead(memory.allocator<char>());
    co_return co_await writeResponseWithScratch(
        stream, memory, localHead, fileChunkBuffer, response, responsePlan);
}

template <typename Stream>
Task<Http1BufferedResponseWriteResult> writeResponse(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer* reusableHead,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    const Http1BufferedResponsePlan& responsePlan) {
    if (reusableHead != nullptr) {
        return writeResponseWithScratch(
            stream,
            memory,
            *reusableHead,
            fileChunkBuffer,
            response,
            responsePlan);
    }
    return writeResponseWithLocalHead(
        stream, memory, fileChunkBuffer, response, responsePlan);
}

}  // namespace ruvia::detail
