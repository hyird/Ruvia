#pragma once

#include <stdexcept>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/static/StaticFileTypes.h"

namespace ruvia::detail {

inline void validateStaticRootOptions(const StaticRootOptions& options) {
    if (!ruvia::isValidHttpHeaderValue(options.cacheControl) || (!options.defaultContentType.empty() && !ruvia::detail::isValidHttpContentTypeFieldValue(options.defaultContentType))) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (const auto& mime : options.mimeTypes) {
        if (!ruvia::detail::isValidStaticFileExtension(mime.extension) || mime.contentType.empty() || !ruvia::detail::isValidHttpContentTypeFieldValue(mime.contentType)) {
            throw std::invalid_argument("invalid static file mime type");
        }
    }
    if (options.indexFile.find('/') != std::string_view::npos || options.indexFile.find('\\') != std::string_view::npos || options.indexFile == "." || options.indexFile == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

}  // namespace ruvia::detail
