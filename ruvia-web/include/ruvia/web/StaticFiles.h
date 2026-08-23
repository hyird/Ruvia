#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ruvia {

class StaticRoot;

struct StaticMimeType final {
    std::string extension;
    std::string contentType;
};

struct StaticFileTypePolicy final {
    enum class Kind : std::uint8_t {
        kDefaults,
        kAll,
        kOnly,
    };

    Kind kind{Kind::kDefaults};
    std::vector<std::string> extensions;
};

enum class StaticRangeRequestPolicy : std::uint8_t {
    kIgnore,
    kHonor,
};

enum class StaticResponseValidatorPolicy : std::uint8_t {
    kOmit,
    kEmit,
};

enum class StaticDotfilePolicy : std::uint8_t {
    kDeny,
    kServe,
};

struct StaticRootOptions final {
    std::string cacheControl;
    std::string indexFile;
    std::string defaultContentType{"application/octet-stream"};
    std::vector<StaticMimeType> mimeTypes;
    StaticFileTypePolicy fileTypes;
    StaticRangeRequestPolicy rangeRequests{StaticRangeRequestPolicy::kHonor};
    StaticResponseValidatorPolicy responseValidators{StaticResponseValidatorPolicy::kEmit};
    // Serve files and directories whose name begins with '.' (dotfiles). Off by
    // default so a .env, .git/config, or .htpasswd sitting under the document
    // root is never exposed. Enable it only for a root that intentionally
    // publishes hidden paths (for example .well-known/ for ACME).
    StaticDotfilePolicy dotfiles{StaticDotfilePolicy::kDeny};
};

namespace detail {

class StaticRootAccess;
struct StaticRootState;

}  // namespace detail

// An immutable index of the document root, built once by this constructor and
// never refreshed directly. The Web runtime always rebuilds a configured
// DocumentRoot replacement off the worker and publishes it between requests.
// Each entry records the file's size, ETag, Last-Modified and an
// identity (device, inode, modification time); serving a request looks the file
// up in that index rather than touching the directory again, so the immutable
// path costs no directory syscalls.
//
// For a standalone StaticRoot, changing the tree does not take effect and is
// not silently tolerated either:
//
//   - A modified file fails its identity check when opened, and that request
//     errors out. Serving it from the stale index would mean sending the old
//     size for new content -- a truncated or misaligned body -- so the check
//     fails closed on purpose.
//   - A newly added file is not in the index and answers 404.
//   - A deleted file fails to open and errors out.
//   - If any filesystem operation fails while an index is being built, the
//     build fails as a whole. Refresh therefore keeps the previous complete
//     index instead of publishing a partial one.
//
// These states persist until a new StaticRoot is constructed. App document
// roots replace it after the next successful refresh (every second by default);
// a standalone StaticRoot does not.
// Standalone StaticRoot values never own refresh or compression policy.
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
