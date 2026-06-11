#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ruvia/http/FileResponse.h"
#include "ruvia/http/HttpTypes.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia {

struct StaticMimeType final {
    std::pmr::string extension;
    std::pmr::string contentType;
};

struct StaticRootOptions final {
    std::pmr::string cacheControl;
    std::pmr::string indexFile;
    std::pmr::string defaultContentType{"application/octet-stream"};
    std::pmr::vector<StaticMimeType> mimeTypes;
    std::pmr::vector<std::pmr::string> fileTypes;
    bool allowAll{false};
    bool enableRanges{true};
    bool enableValidators{true};
};

class StaticRoot final {
public:
    struct Entry final {
        std::pmr::string relativePath;
        FileToken file;
        std::pmr::string contentType;
        std::pmr::string cacheControl;
        std::uint64_t size{0};
        std::filesystem::file_time_type modified{};
        std::pmr::string etag;
        std::pmr::string lastModified;
        bool enableRanges{true};
        bool enableValidators{true};
    };

    explicit StaticRoot(const std::filesystem::path& root, StaticRootOptions options = {}) {
        normalizeMimeTypes(options.mimeTypes);
        normalizeFileTypes(options.fileTypes);
        if (!options.allowAll) {
            applyDefaultFileTypes(options.fileTypes);
            normalizeFileTypes(options.fileTypes);
        }
        validateOptions(options);

        std::error_code ec;
        root_ = std::filesystem::weakly_canonical(root, ec);
        if (ec || !std::filesystem::is_directory(root_, ec)) {
            throw std::invalid_argument("static file root not found");
        }

        options_.cacheControl = std::move(options.cacheControl);
        options_.indexFile = std::move(options.indexFile);
        options_.defaultContentType = std::move(options.defaultContentType);
        options_.mimeTypes = std::move(options.mimeTypes);
        options_.fileTypes = std::move(options.fileTypes);
        options_.allowAll = options.allowAll;
        options_.enableRanges = options.enableRanges;
        options_.enableValidators = options.enableValidators;
        if (!options_.indexFile.empty()) {
            directories_.push_back({});
        }

        for (std::filesystem::recursive_directory_iterator iter(root_, ec), end; !ec && iter != end; iter.increment(ec)) {
            const auto status = iter->symlink_status(ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (std::filesystem::is_symlink(status)) {
                continue;
            }
            const auto relative = iter->path().lexically_relative(root_).generic_string();
            if (relative.empty() || relative.starts_with("../")) {
                continue;
            }
            if (std::filesystem::is_directory(status)) {
                if (!options_.indexFile.empty()) {
                    directories_.push_back(std::pmr::string(relative.data(), relative.size()));
                }
                continue;
            }
            if (!std::filesystem::is_regular_file(status)) {
                continue;
            }
            auto* const upstream = ProcessMemory::instance().upstreamResource();
            const auto extension = detail::httpLowerFileExtension(iter->path(), upstream);
            if (!fileTypeAllowed(extension, options_)) {
                continue;
            }
            const auto size = std::filesystem::file_size(iter->path(), ec);
            if (ec) {
                ec.clear();
                continue;
            }
            const auto modified = std::filesystem::last_write_time(iter->path(), ec);
            if (ec) {
                ec.clear();
                continue;
            }
            const auto enableValidators = options_.enableValidators;
            Entry entry;
            entry.relativePath.assign(relative.data(), relative.size());
            entry.file = FileToken(iter->path());
            entry.contentType = contentTypeFor(iter->path(), extension, options_);
            entry.cacheControl = options_.cacheControl;
            entry.size = static_cast<std::uint64_t>(size);
            entry.modified = modified;
            if (enableValidators) {
                auto* const resource = ProcessMemory::instance().upstreamResource();
                const auto etag = detail::httpMakeFileEtag(
                    resource,
                    static_cast<std::uint64_t>(size),
                    modified);
                const auto lastModified = detail::httpFormatDate(
                    resource,
                    detail::httpFileTimeToTimeT(modified));
                entry.etag.assign(etag.data(), etag.size());
                entry.lastModified.assign(lastModified.data(), lastModified.size());
            }
            entry.enableRanges = options_.enableRanges;
            entry.enableValidators = enableValidators;
            entries_.push_back(std::move(entry));
        }
        std::sort(entries_.begin(), entries_.end(), [](const Entry& left, const Entry& right) {
            return left.relativePath < right.relativePath;
        });
        std::sort(directories_.begin(), directories_.end());
        directories_.erase(std::unique(directories_.begin(), directories_.end()), directories_.end());
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return root_;
    }

    [[nodiscard]] std::string_view indexFile() const noexcept {
        return options_.indexFile;
    }

    [[nodiscard]] bool hasDirectoryIndex() const noexcept {
        return !options_.indexFile.empty();
    }

    [[nodiscard]] const Entry* find(std::string_view relativePath) const noexcept {
        const auto iter = std::lower_bound(
            entries_.begin(),
            entries_.end(),
            relativePath,
            [](const Entry& entry, std::string_view value) {
                return entry.relativePath < value;
            });
        if (iter == entries_.end() || iter->relativePath != relativePath) {
            return nullptr;
        }
        return &*iter;
    }

    [[nodiscard]] bool isIndexedDirectory(std::string_view relativePath) const noexcept {
        if (!hasDirectoryIndex()) {
            return false;
        }
        return std::binary_search(
            directories_.begin(),
            directories_.end(),
            relativePath,
            [](const auto& left, const auto& right) {
                return std::string_view(left) < std::string_view(right);
            });
    }

private:
    [[nodiscard]] static bool validHeaderValue(std::string_view value) noexcept {
        for (const auto c : value) {
            if (c == '\r' || c == '\n' || c == '\0') {
                return false;
            }
        }
        return true;
    }

    static void validateOptions(const StaticRootOptions& options) {
        if (!validHeaderValue(options.cacheControl) || !validHeaderValue(options.defaultContentType)) {
            throw std::invalid_argument("invalid static file header value");
        }
        for (const auto& mime : options.mimeTypes) {
            if (mime.extension.empty() || !validHeaderValue(mime.contentType)) {
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

    static void normalizeMimeTypes(std::pmr::vector<StaticMimeType>& mimeTypes) {
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

    static void normalizeFileTypes(std::pmr::vector<std::pmr::string>& fileTypes) {
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

    static void applyDefaultFileTypes(std::pmr::vector<std::pmr::string>& fileTypes) {
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

    [[nodiscard]] static bool fileTypeAllowed(
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

    [[nodiscard]] static std::pmr::string contentTypeFor(
        const std::filesystem::path& path,
        std::string_view extension,
        const StaticRootOptions& options) {
        auto* const resource = ProcessMemory::instance().upstreamResource();
        const auto iter = std::lower_bound(
            options.mimeTypes.begin(),
            options.mimeTypes.end(),
            extension,
            [](const StaticMimeType& mime, std::string_view value) {
                return mime.extension < value;
            });
        if (iter != options.mimeTypes.end() && std::string_view(iter->extension) == std::string_view(extension)) {
            return std::pmr::string(iter->contentType, resource);
        }

        const auto guessed = detail::httpGuessContentType(path);
        if (guessed != std::string_view("application/octet-stream") || options.defaultContentType.empty()) {
            return std::pmr::string(guessed, resource);
        }
        return std::pmr::string(options.defaultContentType, resource);
    }

    std::filesystem::path root_;
    StaticRootOptions options_;
    std::pmr::vector<Entry> entries_;
    std::pmr::vector<std::pmr::string> directories_;
};

}  // namespace ruvia
