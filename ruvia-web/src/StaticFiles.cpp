#include "ruvia/web/detail/StaticFilesInternal.h"

#include "ruvia/http/detail/HttpDate.h"
#include "ruvia/web/detail/StaticFileMetadata.h"
#include "ruvia/web/detail/server/HttpNativeFile.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ruvia {
namespace {

inline constexpr std::size_t kStaticRootLinearLookupLimit = 8;

inline constexpr std::string_view kDefaultStaticFileTypes[] = {
    "apng", "avif", "bmp", "css", "cur", "eot", "gif", "htm", "html", "ico",
    "jpeg", "jpg", "js", "json", "map", "mjs", "otf", "png", "svg", "ttf",
    "txt", "wasm", "webmanifest", "webp", "woff", "woff2", "xml", "xsl",
};

[[nodiscard]] bool validHeaderValue(std::string_view value) noexcept {
    return std::ranges::none_of(value, [](char c) noexcept {
        return c == '\r' || c == '\n' || c == '\0';
    });
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
    if (options.indexFile.contains('/') ||
        options.indexFile.contains('\\') ||
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

[[nodiscard]] detail::StaticRootState* makeStaticRootState() {
    auto* const resource = detail::processResource();
    return detail::constructPmrObject<detail::StaticRootState>(resource, resource);
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

    const auto iter = std::ranges::lower_bound(
        entries,
        relativePath,
        std::ranges::less{},
        [](const detail::StaticRootEntry& entry) noexcept {
            return std::string_view(entry.relativePath);
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
        return std::ranges::contains(
            directories,
            relativePath,
            [](const auto& directory) noexcept {
                return std::string_view(directory);
            });
    }

    return std::ranges::binary_search(
        directories,
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
        if (extension.empty() || extension.contains('/') || extension.contains('\\')) {
            throw std::invalid_argument("invalid static file type");
        }
        result.extensions_.emplace_back(extension);
    }
    normalizeFileTypes(result.extensions_);
    if (result.extensions_.front().empty()) {
        throw std::invalid_argument("invalid static file type");
    }
    return result;
}

std::string_view detail::StaticRootAccess::indexFile(const StaticRoot& root) noexcept {
    return root.state_->indexFile;
}

bool detail::StaticRootAccess::hasDirectoryIndex(const StaticRoot& root) noexcept {
    return !root.state_->indexFile.empty();
}

std::optional<detail::StaticRootEntryView> detail::StaticRootAccess::find(
    const StaticRoot& root,
    std::string_view relativePath) noexcept {
    auto entry = findVariant(root, relativePath);
    if (!entry.has_value() || !entry->directlyServable_) {
        return std::nullopt;
    }
    return entry;
}

std::optional<detail::StaticRootEntryView> detail::StaticRootAccess::findVariant(
    const StaticRoot& root,
    std::string_view relativePath) noexcept {
    const auto& state = *root.state_;
    const auto& entries = state.entries;
    const auto* const entry = findStaticRootEntry(entries, relativePath);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return detail::StaticRootEntryView(
        entry->filePath.c_str(),
        entry->contentType,
        state.cacheControl,
        entry->etag,
        entry->lastModified,
        entry->size,
        entry->identity,
        entry->modifiedToken,
        entry->modifiedSeconds,
        state.enableRanges,
        state.enableValidators,
        entry->directlyServable);
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
        const bool directlyServable = fileTypeAllowed(extension, options);
        bool usableAsSidecar = false;
        if (!directlyServable && isPrecompressedSidecarExtension(extension)) {
            usableAsSidecar = fileTypeAllowed(
                detail::lowerStaticFileExtension(filePath.stem(), upstream), options);
        }
        if (!directlyServable && !usableAsSidecar) {
            continue;
        }
        const auto snapshot = detail::snapshotResponseFile(
            filePath.c_str(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto enableValidators = state.enableValidators;
        detail::StaticRootEntry entry(upstream);
        entry.relativePath = std::move(relative);
        detail::assignNativePath(entry.filePath, filePath);
        entry.contentType = contentTypeFor(filePath, extension, options, upstream);
        entry.size = snapshot.size;
        entry.identity = snapshot.identity;
        entry.modifiedToken = snapshot.modifiedToken;
        entry.modifiedSeconds = snapshot.modifiedSeconds;
        entry.directlyServable = directlyServable;
        if (enableValidators) {
            entry.etag = detail::makeStaticFileSnapshotEtag(
                upstream,
                snapshot.size,
                snapshot.modifiedToken,
                snapshot.identity);
            entry.lastModified = detail::httpFormatDate(
                upstream,
                snapshot.modifiedSeconds);
        }
        state.entries.push_back(std::move(entry));
    }
    std::ranges::sort(state.entries, [](const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) {
        return left.relativePath < right.relativePath;
    });
    std::ranges::sort(state.directories);
    state.directories.erase(std::ranges::unique(state.directories).begin(), state.directories.end());
}

StaticRoot::~StaticRoot() = default;

void StaticRoot::StateDeleter::operator()(detail::StaticRootState* state) const noexcept {
    detail::destroyPmrObject(state, detail::processResource());
}

std::filesystem::path StaticRoot::path() const {
    return detail::makePathFromNativePath(state_->root);
}

}  // namespace ruvia
