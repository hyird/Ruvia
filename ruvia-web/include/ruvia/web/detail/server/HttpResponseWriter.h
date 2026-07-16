#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <asio.hpp>

#include "ruvia/web/detail/server/HttpFileWrite.h"
#include "ruvia/web/detail/server/Http1BufferedResponseWrite.h"
#include "ruvia/http/detail/server/HttpResponseHead.h"
#include "ruvia/http/detail/server/HttpResponseHeadPolicy.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace ruvia::detail {

// Optimistic synchronous send for plain TCP. The socket is already
// non-blocking (the session always reads asynchronously before it writes), so
// write_some issues the same single send syscall async_write would -- but a
// full write completes without suspending or re-entering the reactor, which
// lets the session release its work set in the same tick instead of holding
// it across a queued completion. Under high connection counts that collapses
// the peak number of live work sets. Returns true when the attempt finished
// the write or hit a hard error (outcome in ec/bytesWritten); false when the
// remainder must go through async_write (would_block or a partial write).
template <typename ConstBufferSequence>
[[nodiscard]] inline bool tryPlainTcpSyncWrite(
    asio::ip::tcp::socket& socket,
    const ConstBufferSequence& buffers,
    std::size_t totalBytes,
    std::error_code& ec,
    std::size_t& bytesWritten) noexcept {
    ec.clear();
    bytesWritten = socket.write_some(buffers, ec);
    if (!ec) {
        return bytesWritten == totalBytes;
    }
    if (ec == asio::error::would_block || ec == asio::error::try_again ||
        ec == asio::error::interrupted) {
        ec.clear();
        return false;
    }
    return true;
}

template <typename Stream>
Task<Http1BufferedResponseWriteResult> writeResponseWithScratch(
    Stream& stream,
    WorkerMemory& memory,
    ResponseHeadBuffer& head,
    std::pmr::string* fileChunkBuffer,
    const HttpResponse& response,
    const Http1BufferedResponsePlan& responsePlan) {
    head.reset();
    appendResponseHead(
        response,
        head,
        responsePlan.headPlan());
    const auto responseHeadBytes = head.view().size();
    const auto& responseContent = responseBody(response);
    if (const auto fileBody = responseContent.file()) {
        auto writeCompletion = co_await asyncAsio<std::size_t>(
            [&stream, headView = head.view()](auto handler) mutable {
                asio::async_write(
                    stream,
                    asio::buffer(headView),
                    std::move(handler));
            });
        const auto ec = writeCompletion.errorCode();
        const auto bytesTransferred = writeCompletion.result();
        if (ec) {
            co_return classifyHttp1BufferedResponseWrite(
                responsePlan,
                responseHeadBytes,
                ec,
                bytesTransferred);
        }
        if (!responsePlan.sendBody()) {
            co_return classifyHttp1BufferedResponseWrite(
                responsePlan,
                responseHeadBytes,
                {},
                bytesTransferred);
        }

        const auto fileError = co_await writeHttpResponseFile(
            stream,
            memory,
            fileChunkBuffer,
            *fileBody);
        co_return classifyHttp1BufferedResponseWrite(
            responsePlan,
            responseHeadBytes,
            fileError,
            responseHeadBytes);
    }

    constexpr bool kPlainTcpStream = std::is_same_v<
        std::remove_cvref_t<Stream>,
        asio::ip::tcp::socket>;
    auto body = responsePlan.sendBody()
        ? responseContent.bytes()
        : std::string_view{};
    if (!body.empty() && head.canAppendOnStack(body.size())) {
        head.append(body);
        body = {};
    }
    if (body.empty()) {
        const auto headView = head.view();
        std::error_code writeEc;
        std::size_t writtenBytes = 0;
        bool writeDone = false;
        if constexpr (kPlainTcpStream) {
            writeDone = tryPlainTcpSyncWrite(
                stream, asio::buffer(headView), headView.size(), writeEc, writtenBytes);
        }
        if (!writeDone) {
            auto writeCompletion = co_await asyncAsio<std::size_t>(
                [&stream, remaining = headView.substr(writtenBytes)](auto handler) mutable {
                    asio::async_write(
                        stream,
                        asio::buffer(remaining),
                        std::move(handler));
                });
            writeEc = writeCompletion.errorCode();
            writtenBytes += writeCompletion.result();
        }
        co_return classifyHttp1BufferedResponseWrite(
            responsePlan,
            responseHeadBytes,
            writeEc,
            writtenBytes);
    }
    const auto headView = head.view();
    const std::array<asio::const_buffer, 2> buffers{asio::buffer(headView), asio::buffer(body)};
    const auto totalBytes = headView.size() + body.size();
    std::error_code writeEc;
    std::size_t writtenBytes = 0;
    bool writeDone = false;
    if constexpr (kPlainTcpStream) {
        writeDone = tryPlainTcpSyncWrite(
            stream, buffers, totalBytes, writeEc, writtenBytes);
    }
    if (!writeDone) {
        const std::array<asio::const_buffer, 2> remaining =
            writtenBytes < headView.size()
                ? std::array<asio::const_buffer, 2>{
                      asio::buffer(headView.substr(writtenBytes)),
                      asio::buffer(body)}
                : std::array<asio::const_buffer, 2>{
                      asio::buffer(body.substr(writtenBytes - headView.size())),
                      asio::const_buffer{}};
        auto writeCompletion = co_await asyncAsio<std::size_t>(
            [&stream, &remaining](auto handler) mutable {
                asio::async_write(stream, remaining, std::move(handler));
            });
        writeEc = writeCompletion.errorCode();
        writtenBytes += writeCompletion.result();
    }
    co_return classifyHttp1BufferedResponseWrite(
        responsePlan,
        responseHeadBytes,
        writeEc,
        writtenBytes);
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
