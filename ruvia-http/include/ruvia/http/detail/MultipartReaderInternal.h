#pragma once

#include "ruvia/http/MultipartParser.h"

namespace ruvia::detail {

struct MultipartStreamPartAccess final {
    [[nodiscard]] static constexpr MultipartStreamPart make(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body,
        MultipartChunkPhase phase) noexcept {
        return MultipartStreamPart(name, filename, contentType, body, phase);
    }
};

}  // namespace ruvia::detail
