#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/http/Http1ChunkedFraming.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia::detail {

using ::ruvia::Http1ChunkHeader;
using ::ruvia::kHttp1ChunkDataTerminator;
using ::ruvia::kHttp1LastChunkPrefix;
using ::ruvia::kHttp1TrailerSectionTerminator;

inline void appendHttp1TrailerSection(
    std::pmr::string& output, const HttpResponseTrailerSection& section) {
    for (const auto& trailer : section.fields()) {
        output.append(trailer.name().data(), trailer.name().size());
        output.append(": ");
        output.append(trailer.value().data(), trailer.value().size());
        output.append(kHttp1ChunkDataTerminator);
    }
}

}  // namespace ruvia::detail
