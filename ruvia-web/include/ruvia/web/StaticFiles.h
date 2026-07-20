#pragma once

#include <filesystem>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace ruvia {

class StaticRoot;

struct StaticMimeType final {
    std::pmr::string extension;
    std::pmr::string contentType;
};

class StaticFileTypePolicy final {
public:
    enum class Kind : std::uint8_t {
        kDefaults,
        kAll,
        kOnly,
    };

    [[nodiscard]] static StaticFileTypePolicy defaults();
    [[nodiscard]] static StaticFileTypePolicy all();
    [[nodiscard]] static StaticFileTypePolicy only(
        std::span<const std::string_view> extensions);
    [[nodiscard]] static StaticFileTypePolicy only(
        std::initializer_list<std::string_view> extensions) {
        return only(std::span<const std::string_view>(extensions.begin(), extensions.size()));
    }

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::span<const std::pmr::string> extensions() const & noexcept {
        return extensions_;
    }
    std::span<const std::pmr::string> extensions() const && = delete;

private:
    explicit StaticFileTypePolicy(Kind kind) : kind_(kind) {}

    Kind kind_;
    std::pmr::vector<std::pmr::string> extensions_;
};

struct StaticRootOptions final {
    std::pmr::string cacheControl;
    std::pmr::string indexFile;
    std::pmr::string defaultContentType{"application/octet-stream"};
    std::pmr::vector<StaticMimeType> mimeTypes;
    StaticFileTypePolicy fileTypes{StaticFileTypePolicy::defaults()};
    bool enableRanges{true};
    bool enableValidators{true};
    // Serve files and directories whose name begins with '.' (dotfiles). Off by
    // default so a .env, .git/config, or .htpasswd sitting under the document
    // root is never exposed. Enable it only for a root that intentionally
    // publishes hidden paths (for example .well-known/ for ACME).
    bool serveDotfiles{false};
};

namespace detail {

class StaticRootAccess;
struct StaticRootState;

}  // namespace detail

class StaticRoot final {
public:
    explicit StaticRoot(const std::filesystem::path& root, StaticRootOptions options = {});
    ~StaticRoot();

    StaticRoot(const StaticRoot&) = delete;
    StaticRoot& operator=(const StaticRoot&) = delete;
    StaticRoot(StaticRoot&&) = delete;
    StaticRoot& operator=(StaticRoot&&) = delete;

    [[nodiscard]] std::filesystem::path path() const;

private:
    struct StateDeleter final {
        void operator()(detail::StaticRootState* state) const noexcept;
    };

    friend class detail::StaticRootAccess;

    std::unique_ptr<detail::StaticRootState, StateDeleter> state_;
};

}  // namespace ruvia
