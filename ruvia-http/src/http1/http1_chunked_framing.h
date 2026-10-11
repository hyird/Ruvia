#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http1_chunked_framing.h"

namespace ruvia::detail {

inline void append_http1_trailer_section(
    std::pmr::string& output, const http_response_trailer_section& section) {
    for (const auto& trailer : section.fields()) {
        output.append(trailer.name().data(), trailer.name().size());
        output.append(": ");
        output.append(trailer.value().data(), trailer.value().size());
        output.append(http1_chunk_data_terminator);
    }
}

}  // namespace ruvia::detail
