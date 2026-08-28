#include "ruvia/web/detail/http/static/StaticRootConfigStorage.h"

#include <algorithm>
#include <string_view>

#include "ruvia/web/detail/http/static/StaticRootOptionsValidation.h"

namespace ruvia::detail {

namespace {

void lowercaseAscii(std::pmr::string& value) noexcept {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
}

}  // namespace

StaticRootConfigStorage::StaticRootConfigStorage(
    const StaticRootConfigStorage& source, std::pmr::memory_resource* resource)
    : StaticRootConfigStorage(resource) {
    cacheControl = source.cacheControl;
    indexFile = source.indexFile;
    defaultContentType = source.defaultContentType;
    auto* const storedResource = cacheControl.get_allocator().resource();
    mimeTypes.reserve(source.mimeTypes.size());
    for (const auto& mime : source.mimeTypes) {
        auto& stored = mimeTypes.emplace_back(storedResource);
        stored.extension = mime.extension;
        stored.contentType = mime.contentType;
    }
    fileTypeKind = source.fileTypeKind;
    fileTypeExtensions.reserve(source.fileTypeExtensions.size());
    for (const auto& extension : source.fileTypeExtensions) {
        fileTypeExtensions.emplace_back(extension);
    }
    rangeRequests = source.rangeRequests;
    responseValidators = source.responseValidators;
    dotfiles = source.dotfiles;
}

StaticRootConfigStorage makeStaticRootConfigStorage(
    const StaticRootOptions& source, std::pmr::memory_resource* resource) {
    validateStaticRootOptions(source);
    return storeValidatedStaticRootConfig(source, resource);
}

StaticRootConfigStorage storeValidatedStaticRootConfig(
    const StaticRootOptions& source, std::pmr::memory_resource* resource) {
    StaticRootConfigStorage result(resource);
    result.cacheControl = source.cacheControl;
    result.indexFile = source.indexFile;
    result.defaultContentType = source.defaultContentType;
    auto* const storedResource = result.cacheControl.get_allocator().resource();
    result.mimeTypes.reserve(source.mimeTypes.size());
    for (const auto& mime : source.mimeTypes) {
        auto& stored = result.mimeTypes.emplace_back(storedResource);
        if (!mime.extension.starts_with('.')) {
            stored.extension.push_back('.');
        }
        stored.extension.append(mime.extension);
        lowercaseAscii(stored.extension);
        stored.contentType = mime.contentType;
    }
    std::ranges::sort(result.mimeTypes,
        [](const StaticRootMimeTypeStorage& left, const StaticRootMimeTypeStorage& right) {
            return left.extension < right.extension;
        });

    result.fileTypeKind = source.fileTypes.kind;
    if (result.fileTypeKind == StaticFileTypePolicy::Kind::kOnly) {
        result.fileTypeExtensions.reserve(source.fileTypes.extensions.size());
        for (std::string_view extension : source.fileTypes.extensions) {
            if (extension.starts_with('.')) {
                extension.remove_prefix(1);
            }
            auto& stored = result.fileTypeExtensions.emplace_back(extension);
            lowercaseAscii(stored);
        }
        std::ranges::sort(result.fileTypeExtensions);
        result.fileTypeExtensions.erase(std::ranges::unique(result.fileTypeExtensions).begin(),
            result.fileTypeExtensions.end());
    }
    result.rangeRequests = source.rangeRequests;
    result.responseValidators = source.responseValidators;
    result.dotfiles = source.dotfiles;
    return result;
}

}  // namespace ruvia::detail
