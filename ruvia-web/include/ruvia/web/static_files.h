#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

class static_root;

struct static_mime_type final {
    borrowed_text extension_{};
    borrowed_text content_type_{};
};

struct static_file_type_policy final {
    enum class kind : std::uint8_t {
        defaults,
        all,
        only,
    };

    kind kind_{kind::defaults};
    std::vector<std::string> extensions_{};
};

enum class static_range_request_policy : std::uint8_t {
    ignore,
    honor,
};

enum class static_response_validator_policy : std::uint8_t {
    omit,
    emit,
};

enum class static_dotfile_policy : std::uint8_t {
    deny,
    serve,
};

struct static_root_options final {
    borrowed_text cache_control_{};
    borrowed_text index_file_{"index.html"};
    borrowed_text default_content_type_{"application/octet-stream"};
    std::vector<static_mime_type> mime_types_{};
    static_file_type_policy file_types_{};
    static_range_request_policy range_requests_{static_range_request_policy::honor};
    static_response_validator_policy response_validators_{static_response_validator_policy::emit};
    // Serve files and directories whose name begins with '.' (dotfiles). Off by
    // default so a .env, .git/config, or .htpasswd sitting under the document
    // root is never exposed. Enable it only for a root that intentionally
    // publishes hidden paths (for example .well-known/ for ACME).
    static_dotfile_policy dotfiles_{static_dotfile_policy::deny};
    // Keep the bytes of files up to resident_max_bytes_, at most
    // resident_budget_bytes_ in total (in index order). Unspecified values
    // resolve to zero for a standalone static_root, or 64 KiB / 16 MiB for
    // application::document_root. An explicit zero for either disables it.
    // Resident files are served as indexed until the next snapshot, even
    // after a disk edit; a standalone root is never refreshed.
    std::optional<std::size_t> resident_max_bytes_{};
    std::optional<std::size_t> resident_budget_bytes_{};
};

namespace detail {

class static_root_access;
struct static_root_config_storage;
struct static_root_state;

}  // namespace detail

// An immutable index of the document root, built once by this constructor and
// never refreshed directly. For a configured document root the application's
// one refresher rebuilds a replacement off the workers, and each worker adopts
// it between requests.
// Relative URL keys use the generic UTF-8 form on every platform; filesystem
// I/O keeps native paths.
// Each entry records the file's size, ETag, Last-Modified and an
// identity (device, inode, modification time); serving a request looks the file
// up in that index rather than touching the directory again, so the immutable
// path costs no directory syscalls.
//
// For a standalone static_root, changing the tree does not take effect and is
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
// These states persist until a new static_root is constructed. application document
// roots replace it after the next successful refresh (every second by default);
// a standalone static_root does not. Resident files are served from the
// snapshot's memory until that refresh instead of failing closed on a disk
// edit. Both owners configure residency through static_root_options;
// a standalone static_root never owns refresh or compression.
class static_root final {
public:
    explicit static_root(const std::filesystem::path& root, static_root_options options = {});
    ~static_root();

    static_root(const static_root&) = delete;
    static_root& operator=(const static_root&) = delete;
    static_root(static_root&&) = delete;
    static_root& operator=(static_root&&) = delete;

    [[nodiscard]] std::filesystem::path path() const;

    struct state_deleter_type {
        void operator()(detail::static_root_state* state) const noexcept;
    };

private:
    explicit static_root(std::unique_ptr<detail::static_root_state, state_deleter_type> state) noexcept;
    friend class detail::static_root_access;

    std::unique_ptr<detail::static_root_state, state_deleter_type> state_;
};

}  // namespace ruvia
