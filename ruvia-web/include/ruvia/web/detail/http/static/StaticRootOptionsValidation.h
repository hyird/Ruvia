#pragma once

#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/field/HttpMediaType.h"
#include "ruvia/web/StaticFiles.h"
#include "ruvia/web/detail/http/static/StaticFileTypes.h"

namespace ruvia::detail {

[[nodiscard]] inline bool staticFileExtensionsEquivalent(std::string_view left, std::string_view right) noexcept {
    if (left.starts_with('.')) {
        left.remove_prefix(1);
    }
    if (right.starts_with('.')) {
        right.remove_prefix(1);
    }
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        auto leftCharacter = left[i];
        auto rightCharacter = right[i];
        if (leftCharacter >= 'A' && leftCharacter <= 'Z') {
            leftCharacter = static_cast<char>(leftCharacter + ('a' - 'A'));
        }
        if (rightCharacter >= 'A' && rightCharacter <= 'Z') {
            rightCharacter = static_cast<char>(rightCharacter + ('a' - 'A'));
        }
        if (leftCharacter != rightCharacter) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool staticRootServesDotfiles(StaticDotfilePolicy policy) {
    switch (policy) {
        case StaticDotfilePolicy::kDeny:
            return false;
        case StaticDotfilePolicy::kServe:
            return true;
        default:
            throw std::invalid_argument("invalid static dotfile policy");
    }
}

inline void validateStaticRootOptions(const StaticRootOptions& options) {
    (void)staticRootServesDotfiles(options.dotfiles);
    switch (options.rangeRequests) {
        case StaticRangeRequestPolicy::kIgnore:
        case StaticRangeRequestPolicy::kHonor:
            break;
        default:
            throw std::invalid_argument("invalid static range request policy");
    }
    switch (options.responseValidators) {
        case StaticResponseValidatorPolicy::kOmit:
        case StaticResponseValidatorPolicy::kEmit:
            break;
        default:
            throw std::invalid_argument("invalid static response validator policy");
    }
    if (!ruvia::isValidHttpHeaderValue(options.cacheControl) || (!options.defaultContentType.empty() && !ruvia::detail::isValidHttpContentTypeFieldValue(options.defaultContentType))) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (std::size_t i = 0; i < options.mimeTypes.size(); ++i) {
        const auto& mime = options.mimeTypes[i];
        if (!ruvia::detail::isValidStaticFileExtension(mime.extension) || mime.contentType.empty() || !ruvia::detail::isValidHttpContentTypeFieldValue(mime.contentType)) {
            throw std::invalid_argument("invalid static file mime type");
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (staticFileExtensionsEquivalent(options.mimeTypes[previous].extension, mime.extension)) {
                throw std::invalid_argument("duplicate static file mime extension");
            }
        }
    }
    switch (options.fileTypes.kind) {
        case StaticFileTypePolicy::Kind::kDefaults:
        case StaticFileTypePolicy::Kind::kAll:
            if (!options.fileTypes.extensions.empty()) {
                throw std::invalid_argument("static file type extensions require kOnly mode");
            }
            break;
        case StaticFileTypePolicy::Kind::kOnly:
            if (options.fileTypes.extensions.empty()) {
                throw std::invalid_argument("static file type allow-list must not be empty");
            }
            for (const auto& extension : options.fileTypes.extensions) {
                if (!isValidStaticFileExtension(extension)) {
                    throw std::invalid_argument("invalid static file type");
                }
            }
            break;
        default:
            throw std::invalid_argument("invalid static file type mode");
    }
    if (options.indexFile.find('/') != std::string_view::npos || options.indexFile.find('\\') != std::string_view::npos || options.indexFile == "." || options.indexFile == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

}  // namespace ruvia::detail
