#pragma once

#include <array>
#include <charconv>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <asio.hpp>

#include "HttpFileFallback.h"
#include "HttpFileZeroCopy.h"
#include "HttpResponseHead.h"
#include "HttpResponseHeadPolicy.h"
#include "HttpResponseBodyAccess.h"
#include "HttpResponseFileAccess.h"
#include "HttpResponseHeaderBits.h"
#include "HttpResponseHeaderState.h"
#include "runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

// Writes a response whose body is a pull-based HttpBodyStream (BodyKind::kStream): a normal route
// can return such a response and it is streamed here -- HTTP/1.1 with chunked framing, HTTP/1.0
// close-delimited. This is what a reverse proxy (Context::proxy) returns. keep-alive / Connection
// were already finalized on `response` by finalizeBufferedRouteResponse before this runs.
template <typename Stream, typename ScannerEntry>
Task<void> writeStreamingResponse(
    Stream& stream,
    ResponseHeadBuffer& head,
    ScannerEntry& scannerEntry,
    HttpResponse& response,
    bool http11,
    bool skipBody,
    std::error_code& ec) {
    const auto policy = responseWritePolicy(response.status());
    const bool chunked = http11 && policy.transferEncodingAllowed();
    if (chunked && !responseHasKnownHeader(response, kResponseHeaderTransferEncoding)) {
        setResponseHeaderStableView(response, "Transfer-Encoding", "chunked");
    }
    head.reset();
    // suppressAutoContentLength: a streamed body has no known length, so never emit Content-Length.
    appendResponseHead(response, head, policy, true);
    ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
        asio::async_write(stream, asio::buffer(headView), std::move(handler));
    });
    if (ec) {
        co_return;
    }
    scannerEntry.touch();
    if (skipBody || !policy.bodyAllowed()) {
        co_return;
    }

    auto& body = HttpResponseBodyAccess::stream(response);
    for (;;) {
        std::string_view chunk;
        try {
            chunk = co_await body.nextChunk();
        } catch (...) {
            // The head is already committed, so a mid-body failure (e.g. a truncated upstream) can
            // only drop the connection; report an error so the caller closes it.
            ec = asio::error::make_error_code(asio::error::connection_aborted);
            co_return;
        }
        if (chunk.empty()) {
            break;
        }
        if (chunked) {
            std::array<char, 32> sizeBuffer;
            const auto [ptr, cec] = std::to_chars(
                sizeBuffer.data(), sizeBuffer.data() + sizeBuffer.size(), chunk.size(), 16);
            if (cec != std::errc{}) {
                ec = asio::error::make_error_code(asio::error::connection_aborted);
                co_return;
            }
            const auto sizeView = std::string_view(
                sizeBuffer.data(), static_cast<std::size_t>(ptr - sizeBuffer.data()));
            constexpr std::string_view crlf = "\r\n";
            const std::array<asio::const_buffer, 4> buffers{
                asio::buffer(sizeView), asio::buffer(crlf), asio::buffer(chunk), asio::buffer(crlf)};
            ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
                asio::async_write(stream, buffers, std::move(handler));
            });
        } else {
            ec = co_await asyncError([&stream, chunk](auto handler) mutable {
                asio::async_write(stream, asio::buffer(chunk), std::move(handler));
            });
        }
        if (ec) {
            co_return;
        }
        scannerEntry.touch();
    }
    if (chunked) {
        constexpr std::string_view lastChunk = "0\r\n\r\n";  // last-chunk + empty trailer section
        ec = co_await asyncError([&stream, lastChunk](auto handler) mutable {
            asio::async_write(stream, asio::buffer(lastChunk), std::move(handler));
        });
    }
}

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
    const auto policy = responseWritePolicy(response.status());
    appendResponseHead(response, head, policy);
    if (responseHasFileBody(response)) {
        const auto fileBody = responseFileBody(response);
        ec = co_await asyncError([&stream, headView = head.view()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(headView), std::move(handler));
        });
        if (ec || skipBody || !policy.bodyAllowed() || fileBody.length == 0) {
            co_return;
        }

        if constexpr (std::is_same_v<std::remove_cvref_t<Stream>, asio::ip::tcp::socket>) {
            co_await writeFileZeroCopy(stream, fileBody, ec);
            if (ec != asio::error::operation_not_supported) {
                co_return;
            }
        }

        co_await writeFileFallback(stream, memory, fileChunkBuffer, fileBody, ec);
        co_return;
    }

    const auto body = skipBody || !policy.bodyAllowed() ? std::string_view{} : responseBodyBytes(response);
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
