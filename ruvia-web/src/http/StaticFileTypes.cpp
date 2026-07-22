#include "ruvia/web/detail/http/static/StaticFileTypes.h"

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/http/static/StaticFileMetadata.h"

namespace ruvia {

namespace {

inline constexpr std::size_t kStaticRootLinearLookupLimit = 8;

inline constexpr std::string_view kDefaultStaticFileTypes[] = {
    "apng", "avif", "bmp", "css", "cur", "eot", "gif", "htm", "html", "ico",
    "jpeg", "jpg", "js", "json", "map", "mjs", "otf", "png", "svg", "ttf",
    "txt", "wasm", "webmanifest", "webp", "woff", "woff2", "xml", "xsl",
};

const StaticMimeType* findStaticMimeType(
    const std::pmr::vector<StaticMimeType>& mimeTypes,
    std::string_view extension) noexcept {
    if (mimeTypes.size() <= kStaticRootLinearLookupLimit) {
        for (const auto& mime : mimeTypes) {
            if (mime.extension == extension) {
                return &mime;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(
        mimeTypes,
        extension,
        std::ranges::less{},
        [](const StaticMimeType& mime) noexcept {
            return std::string_view(mime.extension);
        });
    if (iter == mimeTypes.end() || std::string_view(iter->extension) != extension) {
        return nullptr;
    }
    return &*iter;
}

}  // namespace

StaticFileTypePolicy StaticFileTypePolicy::defaults() {
    return StaticFileTypePolicy(Kind::kDefaults);
}

StaticFileTypePolicy StaticFileTypePolicy::all() {
    return StaticFileTypePolicy(Kind::kAll);
}

StaticFileTypePolicy StaticFileTypePolicy::only(
    std::span<const std::string_view> extensions) {
    if (extensions.empty()) {
        throw std::invalid_argument("static file type allow-list must not be empty");
    }
    StaticFileTypePolicy result(Kind::kOnly);
    result.extensions_.reserve(extensions.size());
    for (const auto extension : extensions) {
        if (extension.empty() || extension.find('/') != std::string_view::npos || extension.find('\\') != std::string_view::npos) {
            throw std::invalid_argument("invalid static file type");
        }
        result.extensions_.emplace_back(extension);
    }
    detail::normalizeFileTypes(result.extensions_);
    if (result.extensions_.front().empty()) {
        throw std::invalid_argument("invalid static file type");
    }
    return result;
}

namespace detail {

void normalizeMimeTypes(std::pmr::vector<StaticMimeType>& mimeTypes) {
    for (auto& mime : mimeTypes) {
        if (!mime.extension.starts_with('.')) {
            mime.extension.insert(mime.extension.begin(), '.');
        }
        for (auto& c : mime.extension) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
    }
    std::ranges::sort(mimeTypes, [](const StaticMimeType& left, const StaticMimeType& right) {
        return left.extension < right.extension;
    });
}

void normalizeFileTypes(std::pmr::vector<std::pmr::string>& fileTypes) {
    for (auto& fileType : fileTypes) {
        if (fileType.starts_with('.')) {
            fileType.erase(fileType.begin());
        }
        for (auto& c : fileType) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
    }
    std::ranges::sort(fileTypes);
    fileTypes.erase(std::ranges::unique(fileTypes).begin(), fileTypes.end());
}

bool fileTypeAllowed(
    std::string_view extension,
    const StaticRootOptions& options) {
    if (options.fileTypes.kind() == StaticFileTypePolicy::Kind::kAll) {
        return true;
    }

    if (extension.empty() || extension == ".") {
        return false;
    }
    const auto value = extension.substr(1);
    if (options.fileTypes.kind() == StaticFileTypePolicy::Kind::kDefaults) {
        return std::ranges::binary_search(kDefaultStaticFileTypes, value);
    }
    const auto extensions = options.fileTypes.extensions();
    return std::ranges::binary_search(extensions, value);
}

std::pmr::string contentTypeFor(
    const std::filesystem::path& path,
    std::string_view extension,
    const StaticRootOptions& options,
    std::pmr::memory_resource* resource) {
    if (const auto* const mime = findStaticMimeType(options.mimeTypes, extension); mime != nullptr) {
        return std::pmr::string(mime->contentType, resource);
    }

    const auto guessed = detail::guessStaticFileContentType(path);
    if (guessed != std::string_view("application/octet-stream") || options.defaultContentType.empty()) {
        return std::pmr::string(guessed, resource);
    }
    return std::pmr::string(options.defaultContentType, resource);
}

}  // namespace detail
}  // namespace ruvia
