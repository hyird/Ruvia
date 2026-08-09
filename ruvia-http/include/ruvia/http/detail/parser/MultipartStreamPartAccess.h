#pragma once

#include "ruvia/http/MultipartParser.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia::detail {

struct MultipartStreamPartAccess final {
    [[nodiscard]] static constexpr MultipartStreamPart make(std::string_view name, std::string_view filename, std::string_view contentType, std::string_view body, MultipartChunkPhase phase) noexcept {
        return make(name, filename, contentType, body, phase, !filename.empty());
    }

    [[nodiscard]] static constexpr MultipartStreamPart make(std::string_view name, std::string_view filename, std::string_view contentType, std::string_view body, MultipartChunkPhase phase, bool filenamePresent) noexcept {
        return MultipartStreamPart(name, filename, contentType, body, phase, filenamePresent);
    }

    template <HttpTemporaryOwningCharString Name>
    static MultipartStreamPart make(Name&&, std::string_view, std::string_view, std::string_view, MultipartChunkPhase) = delete;

    template <HttpTemporaryOwningCharString Filename>
    static MultipartStreamPart make(std::string_view, Filename&&, std::string_view, std::string_view, MultipartChunkPhase) = delete;

    template <HttpTemporaryOwningCharString ContentType>
    static MultipartStreamPart make(std::string_view, std::string_view, ContentType&&, std::string_view, MultipartChunkPhase) = delete;

    template <HttpTemporaryOwningCharString Body>
    static MultipartStreamPart make(std::string_view, std::string_view, std::string_view, Body&&, MultipartChunkPhase) = delete;

    template <HttpTemporaryOwningCharString Name>
    static MultipartStreamPart make(Name&&, std::string_view, std::string_view, std::string_view, MultipartChunkPhase, bool) = delete;

    template <HttpTemporaryOwningCharString Filename>
    static MultipartStreamPart make(std::string_view, Filename&&, std::string_view, std::string_view, MultipartChunkPhase, bool) = delete;

    template <HttpTemporaryOwningCharString ContentType>
    static MultipartStreamPart make(std::string_view, std::string_view, ContentType&&, std::string_view, MultipartChunkPhase, bool) = delete;

    template <HttpTemporaryOwningCharString Body>
    static MultipartStreamPart make(std::string_view, std::string_view, std::string_view, Body&&, MultipartChunkPhase, bool) = delete;
};

}  // namespace ruvia::detail
