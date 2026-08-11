#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/MultipartParser.h"
#include "ruvia/http/detail/util/BorrowedView.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"

namespace ruvia::detail {

struct MultipartPartAccess final {
    // name/filename arrive quote-trimmed; decode their RFC 7230 §3.2.6 quoted-pairs
    // into part-owned storage (they may differ from the raw bytes). contentType/body
    // stay borrowed views into the request body.
    [[nodiscard]] static MultipartPart make(std::string_view name, std::string_view filename, std::string_view contentType, std::string_view body, std::pmr::memory_resource* resource) {
        return make(name, filename, contentType, body, !filename.empty(), resource);
    }

    [[nodiscard]] static MultipartPart make(std::string_view name, std::string_view filename, std::string_view contentType, std::string_view body, bool filenamePresent, std::pmr::memory_resource* resource) {
        std::pmr::string decodedName(resource);
        httpAppendDecodedQuotedPairs(decodedName, name);
        std::pmr::string decodedFilename(resource);
        httpAppendDecodedQuotedPairs(decodedFilename, filename);
        return MultipartPart(std::move(decodedName), std::move(decodedFilename), contentType, body, filenamePresent);
    }

    template <HttpTemporaryOwningCharString ContentType>
    static MultipartPart make(std::string_view, std::string_view, ContentType&&, std::string_view, std::pmr::memory_resource*) = delete;

    template <HttpTemporaryOwningCharString Body>
    static MultipartPart make(std::string_view, std::string_view, std::string_view, Body&&, std::pmr::memory_resource*) = delete;

    template <HttpTemporaryOwningCharString ContentType>
    static MultipartPart make(std::string_view, std::string_view, ContentType&&, std::string_view, bool, std::pmr::memory_resource*) = delete;

    template <HttpTemporaryOwningCharString Body>
    static MultipartPart make(std::string_view, std::string_view, std::string_view, Body&&, bool, std::pmr::memory_resource*) = delete;

    [[nodiscard]] static MultipartPart makeDecoded(std::string_view name, std::string_view filename, std::string_view contentType, std::string_view body, std::pmr::memory_resource* resource) {
        return makeDecoded(name, filename, contentType, body, !filename.empty(), resource);
    }

    [[nodiscard]] static MultipartPart makeDecoded(std::string_view name, std::string_view filename, std::string_view contentType, std::string_view body, bool filenamePresent, std::pmr::memory_resource* resource) {
        return MultipartPart(std::pmr::string(name, resource), std::pmr::string(filename, resource), contentType, body, filenamePresent);
    }

    template <HttpTemporaryOwningCharString ContentType>
    static MultipartPart makeDecoded(std::string_view, std::string_view, ContentType&&, std::string_view, std::pmr::memory_resource*) = delete;

    template <HttpTemporaryOwningCharString Body>
    static MultipartPart makeDecoded(std::string_view, std::string_view, std::string_view, Body&&, std::pmr::memory_resource*) = delete;

    template <HttpTemporaryOwningCharString ContentType>
    static MultipartPart makeDecoded(std::string_view, std::string_view, ContentType&&, std::string_view, bool, std::pmr::memory_resource*) = delete;

    template <HttpTemporaryOwningCharString Body>
    static MultipartPart makeDecoded(std::string_view, std::string_view, std::string_view, Body&&, bool, std::pmr::memory_resource*) = delete;
};

}  // namespace ruvia::detail
