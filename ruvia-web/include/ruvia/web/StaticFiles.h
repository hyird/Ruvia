#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace ruvia {

class StaticRoot;

struct StaticMimeType final {
    std::string extension;
    std::string contentType;
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
    [[nodiscard]] static StaticFileTypePolicy only(std::span<const std::string_view> extensions);
    [[nodiscard]] static StaticFileTypePolicy only(std::initializer_list<std::string_view> extensions) {
        return only(std::span<const std::string_view>(extensions.begin(), extensions.size()));
    }

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] constexpr std::span<const std::string> extensions() const& noexcept {
        return extensions_;
    }
    std::span<const std::string> extensions() const&& = delete;

private:
    explicit StaticFileTypePolicy(Kind kind)
        : kind_(kind) {}

    Kind kind_;
    std::vector<std::string> extensions_;
};

struct StaticRootOptions final {
    std::string cacheControl;
    std::string indexFile;
    std::string defaultContentType{"application/octet-stream"};
    std::vector<StaticMimeType> mimeTypes;
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

// An immutable index of the document root, built once by this constructor and
// never refreshed directly. The Web runtime's DocumentRootRuntimeOptions may
// rebuild a complete replacement off the worker and publish it between
// requests. Each entry records the file's size, ETag, Last-Modified and an
// identity (device, inode, modification time); serving a request looks the file
// up in that index rather than touching the directory again, so the immutable
// path costs no directory syscalls.
//
// The consequence is a deliberate one, and the reason this is spelled out:
// **changing the tree under a live server does not take effect, and is not
// silently tolerated either.**
//
//   - A modified file fails its identity check when opened, and that request
//     errors out. Serving it from the stale index would mean sending the old
//     size for new content -- a truncated or misaligned body -- so the check
//     fails closed on purpose.
//   - A newly added file is not in the index and answers 404.
//   - A deleted file fails to open and errors out.
//   - If any filesystem operation fails while an index is being built, the
//     build fails as a whole. Polling therefore keeps the previous complete
//     index instead of publishing a partial one.
//
// All three persist until a new StaticRoot is constructed, which in an App
// means a restart unless its document-root runtime policy enables polling.
// Standalone StaticRoot values never own refresh, compression, or browser reload
// policy.
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
