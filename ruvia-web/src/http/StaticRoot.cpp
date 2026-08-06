#include "ruvia/web/detail/http/static/StaticRootIndex.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
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

void hashBytes(std::uint64_t& hash, const char* bytes, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint8_t>(bytes[i]);
        hash *= UINT64_C(1099511628211);
    }
}

template <typename T>
void hashValue(std::uint64_t& hash, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    hashBytes(hash, reinterpret_cast<const char*>(&value), sizeof(value));
}

[[nodiscard]] std::uint64_t staticRootFingerprint(const detail::StaticRootState& state) noexcept {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    hashValue(hash, state.entries.size());
    for (const auto& entry : state.entries) {
        hashBytes(hash, entry.relativePath.data(), entry.relativePath.size());
        hashValue(hash, entry.size);
        hashValue(hash, entry.modifiedToken);
        hashValue(hash, entry.modifiedSeconds);
        hashValue(hash, entry.identity.requiresValidation());
        for (const auto word : entry.identity.words()) {
            hashValue(hash, word);
        }
        hashValue(hash, entry.directlyServable);
    }
    return hash;
}

[[nodiscard]] std::uint64_t nextStaticRootRevision() noexcept {
    static std::atomic<std::uint64_t> next{1};
    for (;;) {
        const auto revision = next.fetch_add(1, std::memory_order_relaxed);
        if (revision != 0) {
            return revision;
        }
    }
}

[[nodiscard]] bool sameStaticRootEntry(const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) noexcept {
    return left.relativePath == right.relativePath &&
           left.filePath == right.filePath &&
           left.contentType == right.contentType &&
           left.size == right.size &&
           left.identity == right.identity &&
           left.modifiedToken == right.modifiedToken &&
           left.modifiedSeconds == right.modifiedSeconds &&
           left.etag == right.etag &&
           left.lastModified == right.lastModified &&
           left.directlyServable == right.directlyServable;
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

StaticRootOptions detail::StaticRootAccess::options(const StaticRoot& root) {
    const auto& state = *root.state_;
    StaticRootOptions result;
    result.cacheControl = std::pmr::string(state.cacheControl, detail::processResource());
    result.indexFile = std::pmr::string(state.indexFile, detail::processResource());
    result.defaultContentType = std::pmr::string(state.defaultContentType, detail::processResource());
    result.mimeTypes = std::pmr::vector<StaticMimeType>(detail::processResource());
    result.mimeTypes.reserve(state.mimeTypes.size());
    for (const auto& mime : state.mimeTypes) {
        result.mimeTypes.push_back(StaticMimeType{
            .extension = std::pmr::string(mime.extension, detail::processResource()),
            .contentType = std::pmr::string(mime.contentType, detail::processResource()),
        });
    }
    switch (state.fileTypeKind) {
        case StaticFileTypePolicy::Kind::kDefaults:
            result.fileTypes = StaticFileTypePolicy::defaults();
            break;
        case StaticFileTypePolicy::Kind::kAll:
            result.fileTypes = StaticFileTypePolicy::all();
            break;
        case StaticFileTypePolicy::Kind::kOnly: {
            std::pmr::vector<std::pmr::string> extensions(detail::processResource());
            extensions.reserve(state.fileTypeExtensions.size());
            for (const auto& extension : state.fileTypeExtensions) {
                extensions.emplace_back(extension);
            }
            std::pmr::vector<std::string_view> views(detail::processResource());
            views.reserve(extensions.size());
            for (const auto& extension : extensions) {
                views.push_back(extension);
            }
            result.fileTypes = StaticFileTypePolicy::only(views);
            break;
        }
    }
    result.enableRanges = state.enableRanges;
    result.enableValidators = state.enableValidators;
    result.serveDotfiles = state.serveDotfiles;
    return result;
}

std::uint64_t detail::StaticRootAccess::fingerprint(const StaticRoot& root) noexcept {
    return root.state_->fingerprint;
}

std::uint64_t detail::StaticRootAccess::revision(const StaticRoot& root) noexcept {
    return root.state_->revision;
}

void detail::StaticRootAccess::acquireBinding(const StaticRoot& root) noexcept {
    ++root.state_->activeBindings;
}

void detail::StaticRootAccess::releaseBinding(const StaticRoot& root) noexcept {
    if (root.state_->activeBindings == 0) {
        std::terminate();
    }
    --root.state_->activeBindings;
}

bool detail::StaticRootAccess::hasActiveBindings(const StaticRoot& root) noexcept {
    return root.state_->activeBindings != 0;
}

bool detail::StaticRootAccess::sameSnapshot(const StaticRoot& left, const StaticRoot& right) noexcept {
    const auto& lhs = *left.state_;
    const auto& rhs = *right.state_;
    if (lhs.root != rhs.root ||
        lhs.indexFile != rhs.indexFile ||
        lhs.cacheControl != rhs.cacheControl ||
        lhs.defaultContentType != rhs.defaultContentType ||
        lhs.fileTypeKind != rhs.fileTypeKind ||
        lhs.enableRanges != rhs.enableRanges ||
        lhs.enableValidators != rhs.enableValidators ||
        lhs.serveDotfiles != rhs.serveDotfiles ||
        lhs.fileTypeExtensions != rhs.fileTypeExtensions ||
        lhs.directories != rhs.directories ||
        lhs.mimeTypes.size() != rhs.mimeTypes.size() ||
        lhs.entries.size() != rhs.entries.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.mimeTypes.size(); ++i) {
        if (lhs.mimeTypes[i].extension != rhs.mimeTypes[i].extension || lhs.mimeTypes[i].contentType != rhs.mimeTypes[i].contentType) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.entries.size(); ++i) {
        if (!sameStaticRootEntry(lhs.entries[i], rhs.entries[i])) {
            return false;
        }
    }
    return true;
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

    auto* const upstream = detail::processResource();
    state.cacheControl = std::move(options.cacheControl);
    state.indexFile = std::move(options.indexFile);
    state.defaultContentType = std::move(options.defaultContentType);
    state.mimeTypes.reserve(options.mimeTypes.size());
    for (auto& mime : options.mimeTypes) {
        // StaticMimeType is an aggregate containing nested pmr strings; its
        // implicit move constructor cannot receive the destination vector's
        // allocator. Rebuild both strings explicitly so the long-lived index
        // never retains a caller-owned resource.
        state.mimeTypes.push_back(StaticMimeType{
            .extension = std::pmr::string(mime.extension, upstream),
            .contentType = std::pmr::string(mime.contentType, upstream),
        });
    }
    state.fileTypeKind = options.fileTypes.kind();
    if (state.fileTypeKind == StaticFileTypePolicy::Kind::kOnly) {
        state.fileTypeExtensions.reserve(options.fileTypes.extensions().size());
        for (const auto& extension : options.fileTypes.extensions()) {
            state.fileTypeExtensions.emplace_back(extension);
        }
    }
    state.enableRanges = options.enableRanges;
    state.enableValidators = options.enableValidators;
    state.serveDotfiles = options.serveDotfiles;
    if (!state.indexFile.empty()) {
        state.directories.push_back({});
    }

    // Index construction is transactional. A directory may change while it is
    // being walked, but publishing a partial snapshot would turn one transient
    // filesystem error into arbitrary 404s. Let the caller keep the previous
    // root during polling, and fail startup when there is no previous snapshot.
    std::filesystem::recursive_directory_iterator iter(canonicalRoot, ec);
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonicalRoot, ec);
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iter != end; iter.increment(ec)) {
        const auto& filePath = iter->path();
        ec.clear();
        const auto status = iter->symlink_status(ec);
        if (ec) {
            throw std::filesystem::filesystem_error("inspect static file root entry", filePath, ec);
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
        ec.clear();
        const auto snapshot = detail::snapshotResponseFile(filePath.c_str(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error("snapshot static file root entry", filePath, ec);
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
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonicalRoot, ec);
    }
    std::ranges::sort(state.entries, [](const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) { return left.relativePath < right.relativePath; });
    std::ranges::sort(state.directories);
    state.directories.erase(std::ranges::unique(state.directories).begin(), state.directories.end());
    state.fingerprint = staticRootFingerprint(state);
    state.revision = nextStaticRootRevision();
}

StaticRoot::~StaticRoot() = default;

void StaticRoot::StateDeleter::operator()(detail::StaticRootState* state) const noexcept {
    if (state->activeBindings != 0) {
        // A configured binding is the lifetime lease for this immutable
        // snapshot. Destroying the state first would leave its move-only
        // binding with a dangling pointer and make the eventual release
        // undefined behavior.
        std::terminate();
    }
    detail::destroyPmrObject(state, detail::processResource());
}

std::filesystem::path StaticRoot::path() const {
    return detail::makePathFromNativePath(state_->root);
}

}  // namespace ruvia
