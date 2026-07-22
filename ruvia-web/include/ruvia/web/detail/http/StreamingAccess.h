#pragma once

#include "ruvia/web/Streaming.h"

#include <memory_resource>
#include <optional>
#include <string_view>

namespace ruvia::detail {

struct StreamingAccess final {
    using BodyRead = CallableRef<std::optional<std::string_view>>::Invoke;
    using StreamWrite = ResponseStreamWriter::Write;
    using StreamEnd = ResponseStreamWriter::End;
    using StreamSleep = ResponseStreamWriter::Sleep;
    using StreamBindContext = ResponseStreamWriter::BindContext;
    using StreamReleaseContext = ResponseStreamWriter::ReleaseContext;
    using StreamCommitted = ResponseStreamWriter::Committed;
    using StreamAborted = ResponseStreamWriter::Aborted;

    static void emplaceBodyReader(
        std::optional<BodyReader>& storage,
        void* target,
        BodyRead read) {
        storage.emplace(BodyReader::Token{}, target, read);
    }

    [[nodiscard]] static BodyReader makeBodyReader(
        void* target,
        BodyRead read) noexcept {
        return BodyReader(BodyReader::Token{}, target, read);
    }

    [[nodiscard]] static ResponseStreamWriter makeResponseStreamWriter(
        void* target,
        StreamWrite write,
        StreamEnd end,
        StreamSleep sleep,
        StreamBindContext bindContext,
        StreamReleaseContext releaseContext,
        StreamCommitted committed,
        StreamAborted aborted) noexcept {
        return ResponseStreamWriter(
            target,
            write,
            end,
            sleep,
            bindContext,
            releaseContext,
            committed,
            aborted);
    }

    [[nodiscard]] static SseWriter makeSseWriter(ResponseStreamWriter& writer) noexcept {
        return SseWriter(writer);
    }

    static void bindContext(
        ResponseStreamWriter& writer,
        Context& context,
        ResponseStreamWriter::StreamingHeadThunk streamingHead) {
        writer.bindContext(context, streamingHead);
    }

    static void releaseContext(ResponseStreamWriter& writer) noexcept {
        writer.releaseContext();
    }

    [[nodiscard]] static bool committed(const ResponseStreamWriter& writer) noexcept {
        return writer.committed();
    }
};

}  // namespace ruvia::detail
