#include "ruvia/web/detail/StaticFilesInternal.h"

#include "ruvia/http/detail/HttpDate.h"
#include "ruvia/web/detail/StaticFileMetadata.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia {
namespace {

inline constexpr std::size_t kStaticRootLinearLookupLimit = 8;

[[nodiscard]] bool validHeaderValue(std::string_view value) noexcept {
    for (const auto c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}

void validateOptions(const StaticRootOptions& options) {
    if (!validHeaderValue(options.cacheControl) || !validHeaderValue(options.defaultContentType)) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (const auto& mime : options.mimeTypes) {
        if (mime.extension.empty() || mime.contentType.empty() ||
            !validHeaderValue(mime.contentType)) {
            throw std::invalid_argument("invalid static file mime type");
        }
    }
    for (const auto& fileType : options.fileTypes) {
        if (fileType.empty() || fileType.find('/') != std::pmr::string::npos ||
            fileType.find('\\') != std::pmr::string::npos) {
            throw std::invalid_argument("invalid static file type");
        }
    }
    if (options.indexFile.find('/') != std::pmr::string::npos ||
        options.indexFile.find('\\') != std::pmr::string::npos ||
        options.indexFile == "." ||
        options.indexFile == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

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
    std::sort(mimeTypes.begin(), mimeTypes.end(), [](const StaticMimeType& left, const StaticMimeType& right) {
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
    std::sort(fileTypes.begin(), fileTypes.end());
    fileTypes.erase(std::unique(fileTypes.begin(), fileTypes.end()), fileTypes.end());
}

void applyDefaultFileTypes(std::pmr::vector<std::pmr::string>& fileTypes) {
    static constexpr std::string_view defaults[] = {
        "apng",
        "avif",
        "bmp",
        "css",
        "cur",
        "eot",
        "gif",
        "htm",
        "html",
        "ico",
        "jpeg",
        "jpg",
        "js",
        "json",
        "map",
        "mjs",
        "otf",
        "png",
        "svg",
        "ttf",
        "txt",
        "wasm",
        "webmanifest",
        "webp",
        "woff",
        "woff2",
        "xml",
        "xsl",
    };
    fileTypes.reserve(fileTypes.size() + sizeof(defaults) / sizeof(defaults[0]));
    for (const auto fileType : defaults) {
        fileTypes.emplace_back(fileType);
    }
}

bool fileTypeAllowed(
    std::string_view extension,
    const StaticRootOptions& options) {
    if (options.allowAll) {
        return true;
    }

    if (extension.empty() || extension == ".") {
        return false;
    }
    return std::binary_search(options.fileTypes.begin(), options.fileTypes.end(), extension.substr(1));
}

[[nodiscard]] const StaticMimeType* findStaticMimeType(
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

    const auto iter = std::lower_bound(
        mimeTypes.begin(),
        mimeTypes.end(),
        extension,
        [](const StaticMimeType& mime, std::string_view value) {
            return std::string_view(mime.extension) < value;
        });
    if (iter == mimeTypes.end() || std::string_view(iter->extension) != extension) {
        return nullptr;
    }
    return &*iter;
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

[[nodiscard]] std::unique_ptr<detail::StaticRootState, detail::StaticRootStateDeleter> makeStaticRootState() {
    auto* const resource = detail::processResource();
    return std::unique_ptr<detail::StaticRootState, detail::StaticRootStateDeleter>(
        detail::constructPmrObject<detail::StaticRootState>(resource, resource));
}

[[nodiscard]] const detail::StaticRootEntry* findStaticRootEntry(
    const std::pmr::vector<detail::StaticRootEntry>& entries,
    std::string_view relativePath) noexcept {
    if (entries.size() <= kStaticRootLinearLookupLimit) {
        for (const auto& entry : entries) {
            if (entry.relativePath == relativePath) {
                return &entry;
            }
        }
        return nullptr;
    }

    const auto iter = std::lower_bound(
        entries.begin(),
        entries.end(),
        relativePath,
        [](const detail::StaticRootEntry& entry, std::string_view value) {
            return std::string_view(entry.relativePath) < value;
        });
    if (iter == entries.end() || std::string_view(iter->relativePath) != relativePath) {
        return nullptr;
    }
    return &*iter;
}

[[nodiscard]] bool containsStaticDirectory(
    const std::pmr::vector<std::pmr::string>& directories,
    std::string_view relativePath) noexcept {
    if (directories.size() <= kStaticRootLinearLookupLimit) {
        for (const auto& directory : directories) {
            if (directory == relativePath) {
                return true;
            }
        }
        return false;
    }

    return std::binary_search(
        directories.begin(),
        directories.end(),
        relativePath,
        [](const auto& left, const auto& right) {
            return std::string_view(left) < std::string_view(right);
        });
}

// A precompressed sidecar (foo.js.br / .gz / .zst) is indexed when its base
// file's type is allowed, so it can be served as a Content-Encoding variant.
[[nodiscard]] bool isPrecompressedSidecarExtension(std::string_view extension) noexcept {
    return extension == ".br" || extension == ".gz" || extension == ".zst";
}

}  // namespace

std::string_view detail::StaticRootAccess::indexFile(const StaticRoot& root) noexcept {
    return root.state_->indexFile;
}

bool detail::StaticRootAccess::hasDirectoryIndex(const StaticRoot& root) noexcept {
    return !root.state_->indexFile.empty();
}

detail::StaticRootEntryView detail::StaticRootAccess::find(
    const StaticRoot& root,
    std::string_view relativePath) noexcept {
    const auto& state = *root.state_;
    const auto& entries = state.entries;
    const auto* const entry = findStaticRootEntry(entries, relativePath);
    if (entry == nullptr) {
        return {};
    }
    return detail::StaticRootEntryView{
        .filePath = entry->filePath.c_str(),
        .contentType = entry->contentType,
        .cacheControl = state.cacheControl,
        .etag = entry->etag,
        .lastModified = entry->lastModified,
        .size = entry->size,
        .modified = entry->modified,
        .enableRanges = state.enableRanges,
        .enableValidators = state.enableValidators};
}

bool detail::StaticRootAccess::isIndexedDirectory(
    const StaticRoot& root,
    std::string_view relativePath) noexcept {
    if (!hasDirectoryIndex(root)) {
        return false;
    }
    return containsStaticDirectory(root.state_->directories, relativePath);
}

StaticRoot::StaticRoot(const std::filesystem::path& root, StaticRootOptions options)
    : state_(makeStaticRootState()) {
    normalizeMimeTypes(options.mimeTypes);
    if (!options.allowAll) {
        applyDefaultFileTypes(options.fileTypes);
    }
    normalizeFileTypes(options.fileTypes);
    validateOptions(options);

    std::error_code ec;
    auto& state = *state_;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonicalRoot, ec)) {
        throw std::invalid_argument("static file root not found");
    }
    detail::assignNativePath(state.root, canonicalRoot);

    state.cacheControl = std::move(options.cacheControl);
    state.indexFile = std::move(options.indexFile);
    state.enableRanges = options.enableRanges;
    state.enableValidators = options.enableValidators;
    if (!state.indexFile.empty()) {
        state.directories.push_back({});
    }

    auto* const upstream = detail::processResource();
    for (std::filesystem::recursive_directory_iterator iter(canonicalRoot, ec), end; !ec && iter != end; iter.increment(ec)) {
        const auto& filePath = iter->path();
        const auto status = iter->symlink_status(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (std::filesystem::is_symlink(status)) {
            continue;
        }
        auto relative = filePath
            .lexically_relative(canonicalRoot)
            .generic_string<char, std::char_traits<char>, std::pmr::polymorphic_allocator<char>>(
                std::pmr::polymorphic_allocator<char>(upstream));
        if (relative.empty() || relative.starts_with("../")) {
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            if (!state.indexFile.empty()) {
                state.directories.push_back(std::move(relative));
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }
        const auto extension = detail::lowerStaticFileExtension(filePath, upstream);
        bool typeAllowed = fileTypeAllowed(extension, options);
        if (!typeAllowed && isPrecompressedSidecarExtension(extension)) {
            typeAllowed = fileTypeAllowed(
                detail::lowerStaticFileExtension(filePath.stem(), upstream), options);
        }
        if (!typeAllowed) {
            continue;
        }
        const auto size = std::filesystem::file_size(filePath, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto modified = std::filesystem::last_write_time(filePath, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto enableValidators = state.enableValidators;
        detail::StaticRootEntry entry(upstream);
        entry.relativePath = std::move(relative);
        detail::assignNativePath(entry.filePath, filePath);
        entry.contentType = contentTypeFor(filePath, extension, options, upstream);
        entry.size = static_cast<std::uint64_t>(size);
        entry.modified = modified;
        if (enableValidators) {
            entry.etag = detail::makeStaticFileEtag(
                upstream,
                static_cast<std::uint64_t>(size),
                modified);
            entry.lastModified = detail::httpFormatDate(
                upstream,
                detail::staticFileTimeToTimeT(modified));
        }
        state.entries.push_back(std::move(entry));
    }
    std::sort(state.entries.begin(), state.entries.end(), [](const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) {
        return left.relativePath < right.relativePath;
    });
    std::sort(state.directories.begin(), state.directories.end());
    state.directories.erase(std::unique(state.directories.begin(), state.directories.end()), state.directories.end());
}

StaticRoot::~StaticRoot() = default;

void detail::StaticRootStateDeleter::operator()(StaticRootState* state) const noexcept {
    destroyPmrObject(state, detail::processResource());
}

std::filesystem::path StaticRoot::path() const {
    return detail::makePathFromNativePath(state_->root);
}

}  // namespace ruvia
