#pragma once

#include "ruvia/http/Streaming.h"

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
    using StreamScratch = ResponseStreamWriter::Scratch;
    using StreamAddTrailer = ResponseStreamWriter::AddTrailer;
    using StreamCommitted = ResponseStreamWriter::Committed;
    using StreamAborted = ResponseStreamWriter::Aborted;

    static void emplaceBodyReader(
        std::optional<BodyReader>& storage,
        void* target,
        BodyRead read) {
        storage.emplace(BodyReader::Token{}, target, read);
    }

    [[nodiscard]] static ResponseStreamWriter makeResponseStreamWriter(
        void* target,
        StreamWrite write,
        StreamEnd end,
        StreamSleep sleep,
        StreamBindContext bindContext,
        StreamScratch scratch,
        StreamAddTrailer addTrailer,
        StreamCommitted committed,
        StreamAborted aborted) noexcept {
        return ResponseStreamWriter(
            target, write, end, sleep, bindContext, scratch, addTrailer, committed, aborted);
    }

    [[nodiscard]] static SseWriter makeSseWriter(ResponseStreamWriter& writer) noexcept {
        return SseWriter(writer);
    }

    static void bindContext(ResponseStreamWriter& writer, Context& context) noexcept {
        writer.bindContext(context);
    }

    [[nodiscard]] static std::pmr::string& scratch(const ResponseStreamWriter& writer) noexcept {
        return writer.scratch();
    }

    [[nodiscard]] static bool committed(const ResponseStreamWriter& writer) noexcept {
        return writer.committed();
    }
};

}  // namespace ruvia::detail
