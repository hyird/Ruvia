#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/MultipartParser.h"
#include "ruvia/http/detail/HeaderTokenUtils.h"

namespace ruvia::detail {

struct MultipartPartAccess final {
    // name/filename arrive quote-trimmed; decode their RFC 7230 §3.2.6 quoted-pairs
    // into part-owned storage (they may differ from the raw bytes). contentType/body
    // stay borrowed views into the request body.
    [[nodiscard]] static MultipartPart make(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body,
        std::pmr::memory_resource* resource) {
        std::pmr::string decodedName(resource);
        httpAppendDecodedQuotedPairs(decodedName, name);
        std::pmr::string decodedFilename(resource);
        httpAppendDecodedQuotedPairs(decodedFilename, filename);
        return MultipartPart(std::move(decodedName), std::move(decodedFilename), contentType, body);
    }

    [[nodiscard]] static MultipartPart makeDecoded(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body,
        std::pmr::memory_resource* resource) {
        return MultipartPart(
            std::pmr::string(name, resource),
            std::pmr::string(filename, resource),
            contentType,
            body);
    }
};

}  // namespace ruvia::detail
