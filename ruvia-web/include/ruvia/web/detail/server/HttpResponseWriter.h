#pragma once

#include <array>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <asio.hpp>

#include "ruvia/web/detail/server/HttpFileFallback.h"
#include "ruvia/web/detail/server/HttpFileZeroCopy.h"
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
Task<void> writeResponseWithScratch(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer& head,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    const Http1BufferedResponsePlan& responsePlan,
    std::error_code& ec) {
    const auto& writePlan = responsePlan.writePlan();
    head.reset();
    appendResponseHead(
        response,
        head,
        responsePlan.headPlan());
    const auto& responseContent = responseBody(response);
    if (const auto fileBody = responseContent.file()) {
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        if (ec || !writePlan.sendBody()) {
            co_return;
        }

        if constexpr (std::is_same_v<std::remove_cvref_t<Stream>, asio::ip::tcp::socket>) {
            co_await writeFileZeroCopy(stream, *fileBody, ec);
            if (ec != asio::error::operation_not_supported) {
                co_return;
            }
        }

        co_await writeFileFallback(stream, memory, fileChunkBuffer, *fileBody, ec);
        co_return;
    }

    const auto body = writePlan.sendBody()
        ? responseContent.bytes()
        : std::string_view{};
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
    const Http1BufferedResponsePlan& responsePlan,
    std::error_code& ec) {
    ResponseHeadBuffer localHead(memory.allocator<char>());
    co_await writeResponseWithScratch(
        stream, memory, localHead, fileChunkBuffer, response, responsePlan, ec);
}

template <typename Stream>
Task<void> writeResponse(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer* reusableHead,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    const Http1BufferedResponsePlan& responsePlan,
    std::error_code& ec) {
    if (reusableHead != nullptr) {
        return writeResponseWithScratch(
            stream,
            memory,
            *reusableHead,
            fileChunkBuffer,
            response,
            responsePlan,
            ec);
    }
    return writeResponseWithLocalHead(
        stream, memory, fileChunkBuffer, response, responsePlan, ec);
}

}  // namespace ruvia::detail
