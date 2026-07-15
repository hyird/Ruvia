#include "ruvia/web/detail/server/Http2BufferedResponseWrite.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/server/HttpFileChunkBuffer.h"
#include "ruvia/web/detail/server/HttpFileOpen.h"

namespace ruvia::detail {

Http2BufferedResponseWriter::Http2BufferedResponseWriter(
    Http2Connection& connection,
    Http2SansIoStreamRuntimeTable& streamRuntimes,
    WorkerMemory& worker,
    WorkerSignal& writeSignal) noexcept
    : connection_(&connection),
      streamRuntimes_(&streamRuntimes),
      worker_(&worker),
      writeSignal_(&writeSignal) {}

void Http2BufferedResponseWriter::wakeWriter() noexcept {
    writeSignal_->notify();
}

Task<bool> Http2BufferedResponseWriter::awaitSendWindow(
    std::uint32_t streamId) {
    auto* runtime = streamRuntimes_->find(streamId);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    for (;;) {
        auto* stream = connection_->stream(streamId);
        if (stream == nullptr || stream->isAborted()) {
            co_return false;
        }
        if (!connection_->hasQueuedData(streamId)) {
            co_return true;
        }
        if (signal == nullptr || signal->ended()) {
            co_return false;
        }
        co_await signal->wait();
    }
}

Task<Http2BufferedResponseWriter::DataWriteResult>
Http2BufferedResponseWriter::writeData(
    std::uint32_t streamId,
    std::string_view chunk,
    Http2EndStream endStream) {
    // kQueued transfers the unsent suffix to the core; kBackpressured retains
    // caller ownership and must retry this exact stable view after the older
    // queued input drains.
    for (;;) {
        const auto result = connection_->submitData(
            streamId, chunk, endStream);
        wakeWriter();
        if (result == Http2DataSubmitStatus::kAccepted) {
            co_return DataWriteResult::kCompleted;
        }
        if (result == Http2DataSubmitStatus::kClosed) {
            co_return DataWriteResult::kPeerAborted;
        }
        if (result == Http2DataSubmitStatus::kInvalidState ||
            result == Http2DataSubmitStatus::kContentLengthExceeded ||
            result == Http2DataSubmitStatus::kContentLengthIncomplete) {
            co_return DataWriteResult::kFailed;
        }
        if (!(co_await awaitSendWindow(streamId))) {
            co_return DataWriteResult::kPeerAborted;
        }
        if (result == Http2DataSubmitStatus::kQueued) {
            co_return DataWriteResult::kCompleted;
        }
    }
}

Task<Http2BufferedResponseWriteResult>
Http2BufferedResponseWriter::write(
    std::uint32_t streamId,
    const HttpResponse& response,
    HttpBufferedResponseWritePlan writePlan) {
    auto* stream = connection_->stream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        co_return
            Http2BufferedResponseWriteResult::makePeerAbortedBeforeCommit();
    }

    const auto headResult = connection_->submitResponseHead(
        streamId,
        response,
        std::move(writePlan));
    const auto* submittedHead = headResult.submitted();
    if (submittedHead == nullptr) {
        const auto error = headResult.failure()->error();
        if (error == Http2ResponseHeadSubmitError::kClosed) {
            co_return
                Http2BufferedResponseWriteResult::makePeerAbortedBeforeCommit();
        }
        // Invalid final metadata cannot leave an open peer stream waiting for a
        // response that the transactional head submission rejected.
        (void)connection_->submitReset(
            streamId,
            Http2ErrorCode::kInternalError);
        wakeWriter();
        co_return Http2BufferedResponseWriteResult::makeFailedBeforeCommit();
    }

    wakeWriter();
    const auto committedStatus = submittedHead->responseStatus();
    if (!submittedHead->sendBody()) {
        co_return Http2BufferedResponseWriteResult::makeCompleted(
            committedStatus);
    }

    const auto& content = responseBody(response);
    if (const auto fileBody = content.file()) {
        auto input = openResponseFileInput(*fileBody);
        bool ready = static_cast<bool>(input);
        if (ready) {
            input.seekg(
                static_cast<std::streamoff>(fileBody->offset()),
                std::ios::beg);
            ready = static_cast<bool>(input);
        }
        if (!ready) {
            // The committed Content-Length can no longer be honoured.
            (void)connection_->submitReset(
                streamId,
                Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return Http2BufferedResponseWriteResult::makeFailedAfterCommit(
                committedStatus);
        }

        std::pmr::string fileChunk(worker_->allocator<char>());
        ensureFileChunkBuffer(fileChunk);
        std::uint64_t remaining = fileBody->length();
        while (remaining > 0) {
            auto* live = connection_->stream(streamId);
            if (live == nullptr || live->isAborted()) {
                co_return Http2BufferedResponseWriteResult::makePeerAbortedAfterCommit(
                    committedStatus);
            }
            const auto next = static_cast<std::size_t>(
                std::min<std::uint64_t>(fileChunk.size(), remaining));
            input.read(
                fileChunk.data(),
                static_cast<std::streamsize>(next));
            const auto readBytes = input.gcount();
            if (readBytes <= 0) {
                (void)connection_->submitReset(
                    streamId,
                    Http2ErrorCode::kInternalError);
                wakeWriter();
                co_return Http2BufferedResponseWriteResult::makeFailedAfterCommit(
                    committedStatus);
            }
            remaining -= static_cast<std::uint64_t>(readBytes);
            const auto result = co_await writeData(
                streamId,
                std::string_view(
                    fileChunk.data(),
                    static_cast<std::size_t>(readBytes)),
                remaining == 0
                    ? Http2EndStream::kEndStream
                    : Http2EndStream::kKeepOpen);
            if (result == DataWriteResult::kPeerAborted) {
                co_return Http2BufferedResponseWriteResult::makePeerAbortedAfterCommit(
                    committedStatus);
            }
            if (result == DataWriteResult::kFailed) {
                (void)connection_->submitReset(
                    streamId,
                    Http2ErrorCode::kInternalError);
                wakeWriter();
                co_return Http2BufferedResponseWriteResult::makeFailedAfterCommit(
                    committedStatus);
            }
        }
        co_return Http2BufferedResponseWriteResult::makeCompleted(
            committedStatus);
    }

    // Bound the core-owned window-blocked remainder to one frame-sized slice.
    const auto body = content.bytes();
    constexpr std::size_t kSliceBytes = 16 * 1024;
    std::size_t offset = 0;
    while (offset < body.size()) {
        const auto size = std::min<std::size_t>(
            kSliceBytes,
            body.size() - offset);
        const auto result = co_await writeData(
            streamId,
            body.substr(offset, size),
            offset + size == body.size()
                ? Http2EndStream::kEndStream
                : Http2EndStream::kKeepOpen);
        if (result == DataWriteResult::kPeerAborted) {
            co_return Http2BufferedResponseWriteResult::makePeerAbortedAfterCommit(
                committedStatus);
        }
        if (result == DataWriteResult::kFailed) {
            (void)connection_->submitReset(
                streamId,
                Http2ErrorCode::kInternalError);
            wakeWriter();
            co_return Http2BufferedResponseWriteResult::makeFailedAfterCommit(
                committedStatus);
        }
        offset += size;
    }

    // An empty write plan committed END_STREAM with the response head above.
    co_return Http2BufferedResponseWriteResult::makeCompleted(
        committedStatus);
}

}  // namespace ruvia::detail
