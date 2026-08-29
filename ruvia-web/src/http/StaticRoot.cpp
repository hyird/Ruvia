#include "ruvia/web/detail/http/static/StaticRootIndex.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/web/detail/http/static/StaticFileMetadata.h"
#include "ruvia/web/detail/http/static/StaticFileTypes.h"
#include "ruvia/web/detail/server/file/HttpNativeFile.h"
#include "ruvia/web/detail/http/static/StaticRootOptionsValidation.h"

// A document root indexed once at construction: the directory is walked, every
// servable file recorded with the metadata a response needs, and lookups after
// that touch only the index -- a request never stats the filesystem.

namespace ruvia {
namespace {

// Above this many indexed entries a lookup binary-searches instead of scanning.
inline constexpr std::size_t kStaticRootLinearLookupLimit = 8;

// A relative path (generic '/'-separated form) whose first component or any
// component after a '/' begins with '.' is hidden. Serving these by default
// leaks .env, .git/config, .htpasswd and similar secrets that happen to sit
// under a document root.
[[nodiscard]] bool hasHiddenPathSegment(std::string_view relativeGeneric) noexcept {
    return relativeGeneric.starts_with('.') || relativeGeneric.find("/.") != std::string_view::npos;
}

[[nodiscard]] detail::StaticRootState* makeStaticRootState(detail::StaticRootConfigStorage config) {
    auto* const resource = detail::processResource();
    if (resource == nullptr) {
        std::terminate();
    }
    return detail::constructPmrObject<detail::StaticRootState>(
        resource, resource, std::move(config));
}

[[nodiscard]] std::filesystem::path canonicalStaticRootPath(const std::filesystem::path& root) {
    std::error_code ec;
    auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonicalRoot, ec)) {
        throw std::invalid_argument("static file root not found");
    }
    return canonicalRoot;
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

    const auto iter = std::ranges::lower_bound(entries, relativePath, std::ranges::less{},
        [](const detail::StaticRootEntry& entry) noexcept {
            return std::string_view(entry.relativePath);
        });
    if (iter == entries.end() || std::string_view(iter->relativePath) != relativePath) {
        return nullptr;
    }
    return &*iter;
}

[[nodiscard]] bool containsStaticDirectory(
    const std::pmr::vector<std::pmr::string>& directories, std::string_view relativePath) noexcept {
    if (directories.size() <= kStaticRootLinearLookupLimit) {
        return std::ranges::find(directories, relativePath, [](const auto& directory) noexcept {
            return std::string_view(directory);
        }) != directories.end();
    }

    return std::ranges::binary_search(
        directories, relativePath, [](const auto& left, const auto& right) {
            return std::string_view(left) < std::string_view(right);
        });
}

// A precompressed sidecar (foo.js.br / .gz / .zst) is indexed when its base
// file's type is allowed, so it can be served as a Content-Encoding variant.
[[nodiscard]] bool isPrecompressedSidecarExtension(std::string_view extension) noexcept {
    return extension == ".br" || extension == ".gz" || extension == ".zst";
}

[[nodiscard]] bool mediaTypeStartsWith(
    std::string_view mediaType, std::string_view prefix) noexcept {
    return mediaType.size() >= prefix.size() &&
           detail::httpAsciiEqualsIgnoreCase(mediaType.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool mediaTypeEndsWith(std::string_view mediaType, std::string_view suffix) noexcept {
    return mediaType.size() >= suffix.size() &&
           detail::httpAsciiEqualsIgnoreCase(
               mediaType.substr(mediaType.size() - suffix.size()), suffix);
}

[[nodiscard]] bool staticContentTypeEligibleForPrecompression(
    std::string_view contentType) noexcept {
    const auto semicolon = contentType.find(';');
    const auto mediaType = detail::httpTrimOws(
        semicolon == std::string_view::npos ? contentType : contentType.substr(0, semicolon));
    if (mediaType.empty()) {
        return false;
    }
    return mediaTypeStartsWith(mediaType, "text/") ||
           detail::httpAsciiEqualsIgnoreCase(mediaType, "application/json") ||
           detail::httpAsciiEqualsIgnoreCase(mediaType, "application/javascript") ||
           detail::httpAsciiEqualsIgnoreCase(mediaType, "application/x-javascript") ||
           detail::httpAsciiEqualsIgnoreCase(mediaType, "application/wasm") ||
           detail::httpAsciiEqualsIgnoreCase(mediaType, "application/xml") ||
           detail::httpAsciiEqualsIgnoreCase(mediaType, "application/xhtml+xml") ||
           detail::httpAsciiEqualsIgnoreCase(mediaType, "image/svg+xml") ||
           mediaTypeEndsWith(mediaType, "+json") || mediaTypeEndsWith(mediaType, "+xml");
}

[[nodiscard]] std::string_view staticPrecompressionSuffix(HttpContentCoding coding) noexcept {
    switch (coding) {
        case HttpContentCoding::kGzip:
            return ".gz";
        case HttpContentCoding::kBrotli:
            return ".br";
        case HttpContentCoding::kZstd:
            return ".zst";
        default:
            return {};
    }
}

[[nodiscard]] std::string_view staticPrecompressionEtagToken(HttpContentCoding coding) noexcept {
    switch (coding) {
        case HttpContentCoding::kGzip:
            return "gzip";
        case HttpContentCoding::kBrotli:
            return "br";
        case HttpContentCoding::kZstd:
            return "zstd";
        default:
            return "identity";
    }
}

[[nodiscard]] std::pmr::string makeStaticFileEncodedSnapshotEtag(
    std::pmr::memory_resource* resource, std::uint64_t encodedSize, std::uint64_t modifiedToken,
    detail::ResponseFileIdentity identity, HttpContentCoding coding) {
    std::pmr::string output(resource);
    output.reserve(144);
    output.push_back('"');
    detail::appendStaticFileUnsigned(output, encodedSize);
    output.push_back('-');
    detail::appendStaticFileUnsigned(output, modifiedToken);
    for (const auto word : identity.words()) {
        output.push_back('-');
        detail::appendStaticFileUnsigned(output, word);
    }
    output.push_back('-');
    const auto token = staticPrecompressionEtagToken(coding);
    output.append(token.data(), token.size());
    output.push_back('"');
    return output;
}

[[nodiscard]] bool sameStaticRootFileSnapshot(
    const detail::StaticRootEntry& entry, const detail::ResponseFileSnapshot& snapshot) noexcept {
    return entry.size == snapshot.size && entry.identity == snapshot.identity &&
           entry.modifiedToken == snapshot.modifiedToken &&
           entry.modifiedSeconds == snapshot.modifiedSeconds;
}

[[nodiscard]] bool sameStaticRootEntryMetadata(
    const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) noexcept {
    return left.relativePath == right.relativePath && left.filePath == right.filePath &&
           left.contentType == right.contentType && left.size == right.size &&
           left.identity == right.identity && left.modifiedToken == right.modifiedToken &&
           left.modifiedSeconds == right.modifiedSeconds && left.etag == right.etag &&
           left.lastModified == right.lastModified &&
           left.directlyServable == right.directlyServable;
}

[[nodiscard]] bool precompressedVariantIsAtLeastAsNew(
    const detail::StaticRootEntry& identity, const detail::StaticRootEntry& variant) noexcept {
    if (variant.modifiedSeconds != identity.modifiedSeconds) {
        return variant.modifiedSeconds > identity.modifiedSeconds;
    }
    return variant.modifiedToken >= identity.modifiedToken;
}

[[nodiscard]] const detail::StaticRootEntry* findFreshSidecarEntry(
    const detail::StaticRootState& state, const detail::StaticRootEntry& identity,
    HttpContentCoding coding) {
    const auto suffix = staticPrecompressionSuffix(coding);
    if (suffix.empty()) {
        return nullptr;
    }
    std::pmr::string variantPath(detail::processResource());
    variantPath.reserve(identity.relativePath.size() + suffix.size());
    variantPath.append(identity.relativePath.data(), identity.relativePath.size());
    variantPath.append(suffix.data(), suffix.size());
    const auto* variant = findStaticRootEntry(state.entries, variantPath);
    if (variant == nullptr || !precompressedVariantIsAtLeastAsNew(identity, *variant)) {
        return nullptr;
    }
    return variant;
}

[[nodiscard]] const detail::StaticRootMemoryVariant* findMemoryVariant(
    const detail::StaticRootEntry& entry, HttpContentCoding coding) noexcept {
    for (const auto& variant : entry.memoryVariants) {
        if (variant.contentCoding == coding) {
            return &variant;
        }
    }
    return nullptr;
}

void copyMemoryVariant(detail::StaticRootEntry& entry,
    const detail::StaticRootMemoryVariant& source, std::pmr::memory_resource* resource) {
    auto& stored = entry.memoryVariants.emplace_back(resource);
    stored.contentCoding = source.contentCoding;
    stored.bytes = source.bytes;
    stored.modifiedToken = source.modifiedToken;
    stored.modifiedSeconds = source.modifiedSeconds;
    stored.etag = source.etag;
    stored.lastModified = source.lastModified;
}

[[nodiscard]] std::pmr::string readStableStaticRootEntryBytes(
    const detail::StaticRootEntry& entry, std::pmr::memory_resource* resource) {
    if (entry.size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("static file is too large to precompress");
    }
    const auto size = static_cast<std::size_t>(entry.size);
    std::pmr::string bytes(resource);
    bytes.resize(size);
    const std::filesystem::path path(entry.filePath.c_str());
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::filesystem::filesystem_error("open static file for precompression", path,
            std::make_error_code(std::errc::no_such_file_or_directory));
    }
    if (size != 0) {
        input.read(bytes.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) {
            throw std::runtime_error("static file changed while it was being precompressed");
        }
    }
    std::error_code ec;
    const auto after = detail::snapshotResponseFile(entry.filePath.c_str(), ec);
    if (ec || !sameStaticRootFileSnapshot(entry, after)) {
        throw std::runtime_error("static file changed while it was being precompressed");
    }
    return bytes;
}

void storePrecompressedVariant(detail::StaticRootEntry& entry, HttpContentCoding coding,
    std::pmr::string encoded, bool emitResponseValidators, std::pmr::memory_resource* resource) {
    auto& variant = entry.memoryVariants.emplace_back(resource);
    variant.contentCoding = coding;
    variant.modifiedToken = entry.modifiedToken;
    variant.modifiedSeconds = entry.modifiedSeconds;
    variant.bytes = std::move(encoded);
    if (emitResponseValidators) {
        variant.etag = makeStaticFileEncodedSnapshotEtag(
            resource, variant.bytes.size(), entry.modifiedToken, entry.identity, coding);
        variant.lastModified = entry.lastModified;
    }
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

[[nodiscard]] bool sameStaticRootEntry(
    const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) noexcept {
    return sameStaticRootEntryMetadata(left, right);
}

}  // namespace

detail::StaticRootConfigStorage detail::StaticRootAccess::copyConfig(
    const StaticRoot& root, std::pmr::memory_resource* resource) {
    return StaticRootConfigStorage(root.state_->config, resource);
}

std::string_view detail::StaticRootAccess::indexFile(const StaticRoot& root) noexcept {
    return root.state_->config.indexFile;
}

bool detail::StaticRootAccess::hasDirectoryIndex(const StaticRoot& root) noexcept {
    return !root.state_->config.indexFile.empty();
}

std::optional<detail::StaticRootEntryView> detail::StaticRootAccess::find(
    const StaticRoot& root, std::string_view relativePath) noexcept {
    auto entry = findVariant(root, relativePath);
    if (!entry.has_value() || !entry->directlyServable_) {
        return std::nullopt;
    }
    return entry;
}

std::optional<detail::StaticRootEntryView> detail::StaticRootAccess::findVariant(
    const StaticRoot& root, std::string_view relativePath) noexcept {
    const auto& state = *root.state_;
    const auto& entries = state.entries;
    const auto* const entry = findStaticRootEntry(entries, relativePath);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return detail::StaticRootEntryView(entry->filePath.c_str(), entry->contentType,
        state.config.cacheControl, entry->etag, entry->lastModified, entry->size, entry->identity,
        entry->modifiedToken, entry->modifiedSeconds, state.config.rangeRequests,
        state.config.responseValidators, entry->directlyServable, &entry->memoryVariants);
}

bool detail::StaticRootAccess::isIndexedDirectory(
    const StaticRoot& root, std::string_view relativePath) noexcept {
    if (!hasDirectoryIndex(root)) {
        return false;
    }
    return containsStaticDirectory(root.state_->directories, relativePath);
}

std::uint64_t detail::StaticRootAccess::fingerprint(const StaticRoot& root) noexcept {
    return root.state_->fingerprint;
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

void detail::StaticRootAccess::installPrecompressedVariants(
    StaticRoot& root, const StaticRoot* previous, const StaticRootPrecompressionOptions& options) {
    if (!options.enabled()) {
        return;
    }
    if (options.minBytes == 0 || options.maxBytes < options.minBytes) {
        throw std::invalid_argument("invalid static root precompression options");
    }

    auto& state = *root.state_;
    auto* const resource = detail::processResource();
    const std::array<HttpContentCoding, 3> codings{
        HttpContentCoding::kBrotli,
        HttpContentCoding::kZstd,
        HttpContentCoding::kGzip,
    };
    const bool enabled[] = {
        options.brotli,
        options.zstd,
        options.gzip,
    };
    const auto* previousState = previous == nullptr ? nullptr : previous->state_.get();
    const bool emitResponseValidators =
        state.config.responseValidators == StaticResponseValidatorPolicy::kEmit;

    for (auto& entry : state.entries) {
        if (!entry.directlyServable || entry.size < options.minBytes ||
            entry.size > options.maxBytes ||
            !staticContentTypeEligibleForPrecompression(entry.contentType)) {
            continue;
        }

        const detail::StaticRootEntry* previousEntry = nullptr;
        if (previousState != nullptr) {
            previousEntry = findStaticRootEntry(previousState->entries, entry.relativePath);
            if (previousEntry != nullptr && !sameStaticRootEntryMetadata(entry, *previousEntry)) {
                previousEntry = nullptr;
            }
        }

        std::optional<std::pmr::string> plain;
        for (std::size_t i = 0; i < codings.size(); ++i) {
            if (!enabled[i]) {
                continue;
            }
            const auto coding = codings[i];
            if (findFreshSidecarEntry(state, entry, coding) != nullptr) {
                continue;
            }
            if (previousEntry != nullptr) {
                if (const auto* previousVariant = findMemoryVariant(*previousEntry, coding);
                    previousVariant != nullptr) {
                    copyMemoryVariant(entry, *previousVariant, resource);
                    continue;
                }
            }
            if (!plain.has_value()) {
                plain.emplace(readStableStaticRootEntryBytes(entry, resource));
            }
            if (plain->empty()) {
                continue;
            }
            auto encoded = encodeHttpContent(
                coding, *plain, {.maxEncodedBytes = plain->size() - 1, .resource = resource});
            if (auto* content = encoded.encoded(); content != nullptr) {
                storePrecompressedVariant(entry, coding, std::move(*content).takeBytes(),
                    emitResponseValidators, resource);
            }
        }
    }
}

bool detail::StaticRootAccess::sameSnapshot(
    const StaticRoot& left, const StaticRoot& right) noexcept {
    const auto& lhs = *left.state_;
    const auto& rhs = *right.state_;
    const auto& lhsConfig = lhs.config;
    const auto& rhsConfig = rhs.config;
    if (lhs.root != rhs.root || lhsConfig.indexFile != rhsConfig.indexFile ||
        lhsConfig.cacheControl != rhsConfig.cacheControl ||
        lhsConfig.defaultContentType != rhsConfig.defaultContentType ||
        lhsConfig.fileTypeKind != rhsConfig.fileTypeKind ||
        lhsConfig.rangeRequests != rhsConfig.rangeRequests ||
        lhsConfig.responseValidators != rhsConfig.responseValidators ||
        lhsConfig.dotfiles != rhsConfig.dotfiles ||
        lhsConfig.fileTypeExtensions != rhsConfig.fileTypeExtensions ||
        lhs.directories != rhs.directories ||
        lhsConfig.mimeTypes.size() != rhsConfig.mimeTypes.size() ||
        lhs.entries.size() != rhs.entries.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhsConfig.mimeTypes.size(); ++i) {
        if (lhsConfig.mimeTypes[i].extension != rhsConfig.mimeTypes[i].extension ||
            lhsConfig.mimeTypes[i].contentType != rhsConfig.mimeTypes[i].contentType) {
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

struct StaticRoot::PreparedConstruction final {
    std::filesystem::path canonicalRoot;
    detail::StaticRootConfigStorage config;
};

StaticRoot::PreparedConstruction StaticRoot::prepareConstruction(
    const std::filesystem::path& root, StaticRootOptions options) {
    detail::validateStaticRootOptions(options);
    return PreparedConstruction{
        .canonicalRoot = canonicalStaticRootPath(root),
        .config = detail::storeValidatedStaticRootConfig(options, detail::processResource()),
    };
}

StaticRoot::PreparedConstruction StaticRoot::prepareConstruction(
    const std::filesystem::path& root, const detail::StaticRootConfigStorage& config) {
    return PreparedConstruction{
        .canonicalRoot = canonicalStaticRootPath(root),
        .config = detail::StaticRootConfigStorage(config, detail::processResource()),
    };
}

StaticRoot::PreparedConstruction StaticRoot::prepareConstruction(
    const std::filesystem::path& root, detail::StaticRootConfigStorage&& config) {
    auto canonicalRoot = canonicalStaticRootPath(root);
    if (config.cacheControl.get_allocator().resource() != detail::processResource()) {
        return PreparedConstruction{
            .canonicalRoot = std::move(canonicalRoot),
            .config = detail::StaticRootConfigStorage(config, detail::processResource()),
        };
    }
    return PreparedConstruction{
        .canonicalRoot = std::move(canonicalRoot),
        .config = std::move(config),
    };
}

std::unique_ptr<StaticRoot, detail::PmrObjectDeleter<StaticRoot>>
detail::StaticRootAccess::construct(
    std::pmr::memory_resource* objectResource, StaticRoot::PreparedConstruction prepared) {
    auto* const resource = pmrResourceOrDefault(objectResource);
    auto* const storage = resource->allocate(sizeof(StaticRoot), alignof(StaticRoot));
    try {
        auto* const result = ::new (storage) StaticRoot(std::move(prepared));
        return std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>>(
            result, PmrObjectDeleter<StaticRoot>{resource});
    } catch (...) {
        resource->deallocate(storage, sizeof(StaticRoot), alignof(StaticRoot));
        throw;
    }
}

std::unique_ptr<StaticRoot, detail::PmrObjectDeleter<StaticRoot>> detail::StaticRootAccess::make(
    std::pmr::memory_resource* objectResource, const std::filesystem::path& root,
    const StaticRootConfigStorage& config) {
    return construct(objectResource, StaticRoot::prepareConstruction(root, config));
}

std::unique_ptr<StaticRoot, detail::PmrObjectDeleter<StaticRoot>> detail::StaticRootAccess::make(
    std::pmr::memory_resource* objectResource, const std::filesystem::path& root,
    StaticRootConfigStorage&& config) {
    return construct(objectResource, StaticRoot::prepareConstruction(root, std::move(config)));
}

std::unique_ptr<StaticRoot, detail::PmrObjectDeleter<StaticRoot>> detail::StaticRootAccess::clone(
    std::pmr::memory_resource* objectResource, const StaticRoot& source) {
    return make(objectResource, source.path(), source.state_->config);
}

StaticRoot::StaticRoot(const std::filesystem::path& root, StaticRootOptions options)
    : StaticRoot(prepareConstruction(root, std::move(options))) {}

StaticRoot::StaticRoot(PreparedConstruction prepared)
    : state_(makeStaticRootState(std::move(prepared.config))) {
    const auto& canonicalRoot = prepared.canonicalRoot;
    std::error_code ec;
    auto& state = *state_;
    detail::assignNativePath(state.root, canonicalRoot);

    auto* const upstream = detail::processResource();
    const auto& config = state.config;
    const auto serveDotfiles = detail::staticRootServesDotfiles(config.dotfiles);
    if (!config.indexFile.empty()) {
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
        auto relative = filePath.lexically_relative(canonicalRoot)
                            .generic_string<char, std::char_traits<char>,
                                std::pmr::polymorphic_allocator<char>>(
                                std::pmr::polymorphic_allocator<char>(upstream));
        if (relative.empty() || relative.starts_with("../")) {
            continue;
        }
        // Default-deny hidden paths: skip dotfiles and do not descend into
        // dot-directories (.git, .ssh, ...) so their contents are never indexed.
        if (!serveDotfiles && hasHiddenPathSegment(relative)) {
            if (std::filesystem::is_directory(status)) {
                iter.disable_recursion_pending();
            }
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            if (!config.indexFile.empty()) {
                state.directories.push_back(std::move(relative));
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }
        const auto extension = detail::lowerStaticFileExtension(filePath, upstream);
        const bool directlyServable = detail::fileTypeAllowed(extension, config);
        bool usableAsSidecar = false;
        if (!directlyServable && isPrecompressedSidecarExtension(extension)) {
            usableAsSidecar = detail::fileTypeAllowed(
                detail::lowerStaticFileExtension(filePath.stem(), upstream), config);
        }
        if (!directlyServable && !usableAsSidecar) {
            continue;
        }
        ec.clear();
        const auto snapshot = detail::snapshotResponseFile(filePath.c_str(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error(
                "snapshot static file root entry", filePath, ec);
        }
        const auto emitResponseValidators =
            config.responseValidators == StaticResponseValidatorPolicy::kEmit;
        detail::StaticRootEntry entry(upstream);
        entry.relativePath = std::move(relative);
        detail::assignNativePath(entry.filePath, filePath);
        entry.contentType = detail::contentTypeFor(filePath, extension, config, upstream);
        entry.size = snapshot.size;
        entry.identity = snapshot.identity;
        entry.modifiedToken = snapshot.modifiedToken;
        entry.modifiedSeconds = snapshot.modifiedSeconds;
        entry.directlyServable = directlyServable;
        if (emitResponseValidators) {
            entry.etag = detail::makeStaticFileSnapshotEtag(
                upstream, snapshot.size, snapshot.modifiedToken, snapshot.identity);
            entry.lastModified = detail::httpFormatDate(upstream, snapshot.modifiedSeconds);
        }
        state.entries.push_back(std::move(entry));
    }
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonicalRoot, ec);
    }
    std::ranges::sort(state.entries,
        [](const detail::StaticRootEntry& left, const detail::StaticRootEntry& right) {
            return left.relativePath < right.relativePath;
        });
    std::ranges::sort(state.directories);
    state.directories.erase(
        std::ranges::unique(state.directories).begin(), state.directories.end());
    state.fingerprint = staticRootFingerprint(state);
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
