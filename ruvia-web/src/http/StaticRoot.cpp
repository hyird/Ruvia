#include "ruvia/web/detail/http/static/StaticRootIndex.h"

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

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/web/detail/http/static/StaticFileMetadata.h"
#include "ruvia/web/detail/http/static/StaticFileTypes.h"
#include "ruvia/web/detail/server/file/HttpNativeFile.h"

// A document root indexed once at construction: the directory is walked, every
// servable file recorded with the metadata a response needs, and lookups after
// that touch only the index -- a request never stats the filesystem.

namespace ruvia {
namespace {

// Above this many indexed entries a lookup binary-searches instead of scanning.
inline constexpr std::size_t kStaticRootLinearLookupLimit = 8;

[[nodiscard]] bool validHeaderValue(std::string_view value) noexcept {
    return std::ranges::none_of(value, [](char c) noexcept { return c == '\r' || c == '\n' || c == '\0'; });
}

void validateOptions(const StaticRootOptions& options) {
    if (!validHeaderValue(options.cacheControl) || !validHeaderValue(options.defaultContentType)) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (const auto& mime : options.mimeTypes) {
        if (mime.extension.empty() || mime.contentType.empty() || !validHeaderValue(mime.contentType)) {
            throw std::invalid_argument("invalid static file mime type");
        }
    }
    if (options.indexFile.find('/') != std::string_view::npos || options.indexFile.find('\\') != std::string_view::npos || options.indexFile == "." || options.indexFile == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

// A relative path (generic '/'-separated form) whose first component or any
// component after a '/' begins with '.' is hidden. Serving these by default
// leaks .env, .git/config, .htpasswd and similar secrets that happen to sit
// under a document root.
[[nodiscard]] bool hasHiddenPathSegment(std::string_view relativeGeneric) noexcept {
    return relativeGeneric.starts_with('.') || relativeGeneric.find("/.") != std::string_view::npos;
}

[[nodiscard]] detail::StaticRootState* makeStaticRootState() {
    auto* const resource = detail::processResource();
    return detail::constructPmrObject<detail::StaticRootState>(resource, resource);
}

[[nodiscard]] const detail::StaticRootEntry* findStaticRootEntry(const std::pmr::vector<detail::StaticRootEntry>& entries, std::string_view relativePath) noexcept {
    if (entries.size() <= kStaticRootLinearLookupLimit) {
        for (const auto& entry : entries) {
            if (entry.relativePath == relativePath) {
                return &entry;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(entries, relativePath, std::ranges::less{}, [](const detail::StaticRootEntry& entry) noexcept { return std::string_view(entry.relativePath); });
    if (iter == entries.end() || std::string_view(iter->relativePath) != relativePath) {
        return nullptr;
    }
    return &*iter;
}

[[nodiscard]] bool containsStaticDirectory(const std::pmr::vector<std::pmr::string>& directories, std::string_view relativePath) noexcept {
    if (directories.size() <= kStaticRootLinearLookupLimit) {
        return std::ranges::find(directories, relativePath, [](const auto& directory) noexcept { return std::string_view(directory); }) != directories.end();
    }

    return std::ranges::binary_search(directories, relativePath, [](const auto& left, const auto& right) { return std::string_view(left) < std::string_view(right); });
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

std::optional<detail::StaticRootEntryView> detail::StaticRootAccess::find(const StaticRoot& root, std::string_view relativePath) noexcept {
    auto entry = findVariant(root, relativePath);
    if (!entry.has_value() || !entry->directlyServable_) {
        return std::nullopt;
    }
    return entry;
}

std::optional<detail::StaticRootEntryView> detail::StaticRootAccess::findVariant(const StaticRoot& root, std::string_view relativePath) noexcept {
    const auto& state = *root.state_;
    const auto& entries = state.entries;
    const auto* const entry = findStaticRootEntry(entries, relativePath);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return detail::StaticRootEntryView(entry->filePath.c_str(), entry->contentType, state.cacheControl, entry->etag, entry->lastModified, entry->size, entry->identity, entry->modifiedToken, entry->modifiedSeconds, state.enableRanges, state.enableValidators, entry->directlyServable);
}

bool detail::StaticRootAccess::isIndexedDirectory(const StaticRoot& root, std::string_view relativePath) noexcept {
    if (!hasDirectoryIndex(root)) {
        return false;
    }
    return containsStaticDirectory(root.state_->directories, relativePath);
}

StaticRoot::StaticRoot(const std::filesystem::path& root, StaticRootOptions options)
    : state_(makeStaticRootState()) {
    detail::normalizeMimeTypes(options.mimeTypes);
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
        auto relative = filePath.lexically_relative(canonicalRoot).generic_string<char, std::char_traits<char>, std::pmr::polymorphic_allocator<char>>(std::pmr::polymorphic_allocator<char>(upstream));
        if (relative.empty() || relative.starts_with("../")) {
            continue;
        }
        // Default-deny hidden paths: skip dotfiles and do not descend into
        // dot-directories (.git, .ssh, ...) so their contents are never indexed.
        if (!options.serveDotfiles && hasHiddenPathSegment(relative)) {
            if (std::filesystem::is_directory(status)) {
                iter.disable_recursion_pending();
            }
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
        const bool directlyServable = detail::fileTypeAllowed(extension, options);
        bool usableAsSidecar = false;
        if (!directlyServable && isPrecompressedSidecarExtension(extension)) {
            usableAsSidecar = detail::fileTypeAllowed(detail::lowerStaticFileExtension(filePath.stem(), upstream), options);
        }
        if (!directlyServable && !usableAsSidecar) {
            continue;
        }
        const auto snapshot = detail::snapshotResponseFile(filePath.c_str(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const auto enableValidators = state.enableValidators;
        detail::StaticRootEntry entry(upstream);
        entry.relativePath = std::move(relative);
        detail::assignNativePath(entry.filePath, filePath);
        entry.contentType = detail::contentTypeFor(filePath, extension, options, upstream);
        entry.size = snapshot.size;
        entry.identity = snapshot.identity;
        entry.modifiedToken = snapshot.modifiedToken;
        entry.modifiedSeconds = snapshot.modifiedSeconds;
        entry.directlyServable = directlyServable;
        if (enableValidators) {
            entry.etag = detail::makeStaticFileSnapshotEtag(upstream, snapshot.size, snapshot.modifiedToken, snapshot.identity);
            entry.lastModified = detail::httpFormatDate(upstream, snapshot.modifiedSeconds);
        }
        state.entries.push_back(std::move(entry));
    }
    std::ranges::sort(state.entries, [](const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) { return left.relativePath < right.relativePath; });
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
