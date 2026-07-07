#pragma once

#include "ruvia/http/HttpCommon.h"

#include <memory_resource>
#include <string_view>

#include "HeaderTokenUtils.h"

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
};

struct RequestNameValueViewAccess final {
    [[nodiscard]] static constexpr RequestNameValueView make(
        std::string_view name,
        std::string_view value) noexcept {
        return RequestNameValueView(name, value);
    }
};

}  // namespace ruvia::detail
