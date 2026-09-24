#include "ruvia/web/detail/http2/Http2BufferedResponseWrite.h"

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
#include "ruvia/http/HttpResponseServer.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/web/detail/http2/Http2SansIoSendWindow.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamRuntime.h"
#include "ruvia/web/detail/server/file/HttpFileChunkBuffer.h"
#include "ruvia/web/detail/server/file/HttpFileOpen.h"

namespace ruvia::detail {
namespace {

void submitResetNoThrow(
    ruvia::Http2Connection& connection, std::uint32_t streamId, Http2ErrorCode error) noexcept {
    try {
        (void)connection.submitReset(streamId, error);
    } catch (...) {
        // Failure to serialize RESET_STREAM is already rolled back by
        // Http2Connection. This writer reports the local failure through its typed
        // result; the owning session may still retry during stream cleanup.
    }
}

}  // namespace

Http2BufferedResponseWriter::Http2BufferedResponseWriter(ruvia::Http2Connection& connection,
    Http2SansIoStreamRuntimeTable& streamRuntimes, WorkerMemory& worker,
    WorkerSignal& writeSignal, Http2DataOutputBudget& outputBudget) noexcept
    : connection_(connection),
      streamRuntimes_(streamRuntimes),
      worker_(worker),
      writeSignal_(writeSignal),
      outputBudget_(&outputBudget) {}

Http2BufferedResponseWriter::Http2BufferedResponseWriter(
    ruvia::Http2Connection& connection, Http2SansIoStreamRuntimeTable& streamRuntimes,
    WorkerMemory& worker, WorkerSignal& writeSignal) noexcept
    : connection_(connection), streamRuntimes_(streamRuntimes), worker_(worker),
      writeSignal_(writeSignal) {}

void Http2BufferedResponseWriter::wakeWriter() noexcept {
    writeSignal_.notify();
}

Task<Http2BufferedResponseWriter::DataWriteResult> Http2BufferedResponseWriter::writeData(
    std::uint32_t streamId, std::string_view chunk, Http2EndStream endStream) {
    // kQueued transfers the unsent suffix to the core; kBackpressured retains
    // caller ownership and must retry this exact stable view after the older
    // queued input drains.
    for (;;) {
        auto* runtime = streamRuntimes_.find(streamId);
        auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
        if (signal != nullptr && signal->terminated()) {
            co_return DataWriteResult::kFailed;
        }
        if (signal == nullptr) {
            co_return DataWriteResult::kPeerAborted;
        }
        if (outputBudget_ != nullptr) {
            for (;;) {
                const auto window = connection_.sendWindowState(streamId);
                if (!window) {
                    co_return DataWriteResult::kPeerAborted;
                }
                if (window->available != 0) {
                    break;
                }
                co_await outputBudget_->waitForChange();
                if (signal->terminated()) {
                    co_return DataWriteResult::kPeerAborted;
                }
            }
            if (!(co_await outputBudget_->acquire(streamId, *signal))) {
                co_return DataWriteResult::kPeerAborted;
            }
        }
        const auto result = connection_.submitData(streamId, chunk, endStream);
        if (outputBudget_ != nullptr &&
            (result == Http2DataSubmitStatus::kAccepted || result == Http2DataSubmitStatus::kQueued)) {
            outputBudget_->noteDataSubmitted(streamId, chunk.size());
        }
        wakeWriter();
        if (result != Http2DataSubmitStatus::kAccepted &&
            result != Http2DataSubmitStatus::kQueued && outputBudget_ != nullptr) {
            outputBudget_->release(streamId);
        }
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
        const auto waitResult = co_await awaitHttp2SendWindow(connection_, streamId, signal);
        if (waitResult.aborted() != nullptr) {
            co_return DataWriteResult::kPeerAborted;
        }
        if (result == Http2DataSubmitStatus::kQueued) {
            co_return DataWriteResult::kCompleted;
        }
    }
}

Task<Http2BufferedResponseWriteResult> Http2BufferedResponseWriter::write(
    std::uint32_t streamId, const HttpResponse& response, HttpServerBufferedResponseWritePlan writePlan) {
    if (connection_.streamAborted(streamId)) {
        co_return Http2BufferedResponseWriteResult::makePeerAbortedBeforeCommit();
    }
    auto* runtime = streamRuntimes_.find(streamId);
    auto* signal = runtime != nullptr ? runtime->signal() : nullptr;
    if (signal != nullptr && signal->terminated()) {
        co_return Http2BufferedResponseWriteResult::makeFailedBeforeCommit();
    }

    const auto headResult =
        connection_.submitResponseHead(streamId, response, std::move(writePlan));
    const auto* submittedHead = headResult.submitted();
    if (submittedHead == nullptr) {
        if (headResult.failure()->error() == Http2ResponseHeadSubmitError::kClosed) {
            co_return Http2BufferedResponseWriteResult::makePeerAbortedBeforeCommit();
        }
        // Invalid final metadata cannot leave an open peer stream waiting for a
        // response that the transactional head submission rejected.
        submitResetNoThrow(connection_, streamId, Http2ErrorCode::kInternalError);
        wakeWriter();
        co_return Http2BufferedResponseWriteResult::makeFailedBeforeCommit();
    }

    wakeWriter();
    const auto committedStatus = submittedHead->responseStatus();
    const auto failAfterCommit = [this, streamId, committedStatus]() noexcept {
        submitResetNoThrow(connection_, streamId, Http2ErrorCode::kInternalError);
        wakeWriter();
        return Http2BufferedResponseWriteResult::makeFailedAfterCommit(committedStatus);
    };
    if (!submittedHead->sendBody()) {
        co_return Http2BufferedResponseWriteResult::makeCompleted(committedStatus);
    }

    if (const auto fileBody = response.fileBody()) {
        auto input = openResponseFileInput(*fileBody);
        bool ready = static_cast<bool>(input);
        if (ready) {
            input.seekg(static_cast<std::streamoff>(fileBody->offset()), std::ios::beg);
            ready = static_cast<bool>(input);
        }
        if (!ready) {
            // The committed Content-Length can no longer be honoured.
            co_return failAfterCommit();
        }

        std::pmr::string fileChunk(worker_.allocator<char>());
        ensureFileChunkBuffer(fileChunk);
        std::uint64_t remaining = fileBody->length();
        while (remaining > 0) {
            if (connection_.streamAborted(streamId)) {
                co_return Http2BufferedResponseWriteResult::makePeerAbortedAfterCommit(
                    committedStatus);
            }
            constexpr std::uint64_t kDataCreditBytes = kHttp2DataOutputCreditBytes;
            const auto next = static_cast<std::size_t>(
                std::min<std::uint64_t>(kDataCreditBytes, remaining));
            input.read(fileChunk.data(), static_cast<std::streamsize>(next));
            const auto readBytes = input.gcount();
            if (readBytes <= 0) {
                co_return failAfterCommit();
            }
            remaining -= static_cast<std::uint64_t>(readBytes);
            const auto result = co_await writeData(streamId,
                std::string_view(fileChunk.data(), static_cast<std::size_t>(readBytes)),
                remaining == 0 ? Http2EndStream::kEndStream : Http2EndStream::kKeepOpen);
            if (result == DataWriteResult::kPeerAborted) {
                co_return Http2BufferedResponseWriteResult::makePeerAbortedAfterCommit(
                    committedStatus);
            }
            if (result == DataWriteResult::kFailed) {
                co_return failAfterCommit();
            }
        }
        co_return Http2BufferedResponseWriteResult::makeCompleted(committedStatus);
    }

    // Bound the core-owned window-blocked remainder to one frame-sized slice.
    const auto body = response.bodyBytes();
    constexpr std::size_t kSliceBytes = kHttp2DataOutputCreditBytes;
    std::size_t offset = 0;
    while (offset < body.size()) {
        const auto size = std::min<std::size_t>(kSliceBytes, body.size() - offset);
        const auto result = co_await writeData(streamId, body.substr(offset, size),
            offset + size == body.size() ? Http2EndStream::kEndStream : Http2EndStream::kKeepOpen);
        if (result == DataWriteResult::kPeerAborted) {
            co_return Http2BufferedResponseWriteResult::makePeerAbortedAfterCommit(committedStatus);
        }
        if (result == DataWriteResult::kFailed) {
            co_return failAfterCommit();
        }
        offset += size;
    }

    // An empty write plan committed END_STREAM with the response head above.
    co_return Http2BufferedResponseWriteResult::makeCompleted(committedStatus);
}

}  // namespace ruvia::detail
