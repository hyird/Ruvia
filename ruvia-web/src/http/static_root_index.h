#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/worker_ref.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/error.h"
#include "ruvia/web/server_config.h"
#if defined(__unix__)
#include "server/connection_lifecycle.h"
#endif

namespace ruvia::detail {

[[nodiscard]] inline bool is_windows_drive_path(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[noreturn]] inline void throw_invalid_static_file_path() {
    throw http_error({.status_ = ruvia::http_status::forbidden,
        .code_ = "forbidden",
        .message_ = "invalid static file path"});
}

// Percent-decodes `input` in one pass into `output`, which holds at least
// input.size() bytes, and returns the decoded size. Percent mode fails on a
// malformed escape; form mode maps '+' to a space and keeps a '%' that starts
// no valid escape as a literal byte (WHATWG percent-decode). With
// `reject_encoded_separators`, an escape revealing '/' or '\\' fails too.
[[nodiscard]] inline std::optional<std::size_t> decode_url_bytes(
    std::string_view input, char* output, url_decode_mode mode, bool reject_encoded_separators) noexcept {
    std::size_t size = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (mode == url_decode_mode::form && c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (const auto byte = http_try_decode_percent_byte(input, i)) {
                if (reject_encoded_separators && (*byte == '/' || *byte == '\\')) {
                    return std::nullopt;
                }
                c = static_cast<char>(*byte);
                i += 2;
            } else if (mode != url_decode_mode::form) {
                return std::nullopt;
            }
        }
        output[size++] = c;
    }
    return size;
}

// Percent-decodes an encoded request path, or a wildcard tail of one, exactly
// once into the file path it names: the path itself when it holds no escape,
// otherwise the decoded bytes in `storage`. Routing and path-scoped middleware
// split request paths on raw '/' only, so an escape revealing '/' or '\\'
// would forge a segment boundary they never saw; that, like a malformed
// escape, yields nullopt.
[[nodiscard]] inline std::optional<std::string_view> decode_request_path(
    std::string_view path, std::string& storage) {
    if (!has_url_encoding(path, url_decode_mode::percent)) {
        return path;
    }
    storage.resize(path.size());
    const auto size = decode_url_bytes(path, storage.data(), url_decode_mode::percent, true);
    if (!size) {
        return std::nullopt;
    }
    storage.resize(*size);
    return std::string_view(storage);
}

// Web static-file path policy: validate a decoded file path relative to the
// configured document root. '/' and '\\' both separate segments; a "." or ".."
// segment, an empty interior segment, a NUL, or an absolute or drive path is
// rejected (403) rather than resolved. Requests carrying a dot segment never
// reach a route, and a decoded parameter could otherwise hide one behind an
// encoded separator ("sub%2F..%2Fsecret") that no route or path scope saw, so
// every caller fails closed instead of resolving it. A final separator names
// the directory itself and is dropped. The input is borrowed when it is
// already canonical; only a '\\' separator rebuilds it in `storage`.
[[nodiscard]] inline std::optional<std::string_view> try_normalize_static_relative_path(
    std::string_view input, std::string& storage) {
    if (input.empty()) {
        return input;
    }
    if (input.front() == '/' || input.front() == '\\' || is_windows_drive_path(input)) {
        return std::nullopt;
    }

    constexpr std::string_view stop_bytes("/\\\0", 3);
    bool backslash = false;
    std::size_t cursor_value = 0;
    for (;;) {
        const auto stop = input.find_first_of(stop_bytes, cursor_value);
        const auto end = stop == std::string_view::npos ? input.size() : stop;
        const auto segment = input.substr(cursor_value, end - cursor_value);
        if (segment == "." || segment == "..") {
            return std::nullopt;
        }
        if (stop == std::string_view::npos) {
            break;
        }
        if (input[stop] == '\0' || segment.empty()) {
            return std::nullopt;
        }
        backslash = backslash || input[stop] == '\\';
        cursor_value = stop + 1;
    }

    auto canonical = input;
    if (canonical.back() == '/' || canonical.back() == '\\') {
        canonical.remove_suffix(1);
    }
    if (!backslash) {
        return canonical;
    }
    storage.assign(canonical.data(), canonical.size());
    std::ranges::replace(storage, '\\', '/');
    return storage;
}

[[nodiscard]] inline std::string_view normalize_static_relative_path(
    std::string_view input, std::string& storage) {
    const auto normalized = try_normalize_static_relative_path(input, storage);
    if (!normalized.has_value()) {
        throw_invalid_static_file_path();
    }
    return *normalized;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

struct static_root_mime_type_storage final {
    friend bool operator==(const static_root_mime_type_storage&, const static_root_mime_type_storage&) = default;

    std::string extension_;
    std::string content_type_;
};

// Validated, normalized, owning static-root policy.
struct static_root_config_storage final {
    static_root_config_storage() = default;
    static_root_config_storage(const static_root_config_storage&) = default;
    static_root_config_storage(static_root_config_storage&&) noexcept = default;
    static_root_config_storage& operator=(const static_root_config_storage&) = delete;
    static_root_config_storage& operator=(static_root_config_storage&&) noexcept = default;

    friend bool operator==(const static_root_config_storage&, const static_root_config_storage&) = default;

    std::string cache_control_;
    std::string index_file_;
    std::string default_content_type_{"application/octet-stream"};
    std::vector<static_root_mime_type_storage> mime_types_;
    static_file_type_policy::kind file_type_kind_{static_file_type_policy::kind::defaults};
    std::vector<std::string> file_type_extensions_;
    static_range_request_policy range_requests_{static_range_request_policy::honor};
    static_response_validator_policy response_validators_{static_response_validator_policy::emit};
    static_dotfile_policy dotfiles_{static_dotfile_policy::deny};
};

[[nodiscard]] static_root_config_storage store_validated_static_root_config(
    const static_root_options& source_value);

[[nodiscard]] bool is_valid_static_file_extension(std::string_view extension) noexcept;

[[nodiscard]] bool file_type_allowed(
    std::string_view extension, const static_root_config_storage& config);

[[nodiscard]] std::string content_type_for(std::string_view extension,
    const static_root_config_storage& config);
}  // namespace ruvia::detail

namespace ruvia::detail {

struct static_root_precompression_options final {
    bool gzip_{false};
    bool brotli_{false};
    bool zstd_{false};
    std::size_t min_bytes_{1024};
    std::size_t max_bytes_{std::size_t{256} * 1024};

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return gzip_ || brotli_ || zstd_;
    }

    [[nodiscard]] constexpr bool enabled(http_content_coding coding) const noexcept {
        switch (coding) {
            case http_content_coding::gzip:
                return gzip_;
            case http_content_coding::brotli:
                return brotli_;
            case http_content_coding::zstd:
                return zstd_;
            default:
                return false;
        }
    }
};

// Document-root snapshots keep the bytes of files up to max_file_bytes_, within
// budget_bytes_ for the whole snapshot, so those responses never touch the
// filesystem. Zero for either disables it.
struct static_root_resident_options final {
    std::size_t max_file_bytes_{0};
    std::size_t budget_bytes_{0};

    [[nodiscard]] constexpr bool enabled() const noexcept {
        return max_file_bytes_ != 0 && budget_bytes_ != 0;
    }
};

// Snapshots built by one refresher reuse an unchanged file's encoded bytes
// instead of copying them: the buffer is immutable and shared by every
// snapshot that indexes the same file version.
struct static_root_memory_variant final {
    http_content_coding content_coding_{http_content_coding::identity};
    std::shared_ptr<const std::string> bytes_;
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    std::string etag_;
    std::string last_modified_;
};

struct static_root_entry final {
    std::string relative_path_;
    http_native_path_string_type file_path_;
    std::string content_type_;
    std::uint64_t size_{0};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    std::string etag_;
    std::string last_modified_;
    bool directly_servable_{true};
    std::vector<static_root_memory_variant> memory_variants_;
    // The file's bytes as indexed, when the snapshot keeps it resident. Shared
    // with later snapshots that index the same file version.
    std::shared_ptr<const std::string> resident_bytes_;
};

struct static_root_state final {
    http_native_path_string_type root_;
    static_root_config_storage config_;
    std::vector<static_root_entry> entries_;
    std::vector<std::string> directories_;
    std::uint64_t fingerprint_{0};
    bool has_sidecars_{false};
#if defined(__unix__)
    // The indexed root, opened once: the index snapshot and each request
    // open entries beneath it. Shared with every static_root_entry_open, so
    // an open still running on the blocking pool after its request gave up
    // never uses a descriptor this snapshot already closed.
    std::shared_ptr<const native_file_descriptor> root_directory_;
    // The directory instance root_directory_ names; the index was built from
    // the same one.
    native_directory_identity root_identity_{};
#endif

    explicit static_root_state(static_root_config_storage configured_policy) noexcept
        : config_(std::move(configured_policy)) {}

    static_root_state(const static_root_state&) = delete;
    static_root_state& operator=(const static_root_state&) = delete;
    static_root_state(static_root_state&&) = delete;
    static_root_state& operator=(static_root_state&&) = delete;
};

class static_root_memory_variant_view final {
public:
    [[nodiscard]] http_content_coding content_coding() const noexcept {
        return content_coding_;
    }

    [[nodiscard]] std::string_view bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    [[nodiscard]] std::uint64_t modified_token() const noexcept {
        return modified_token_;
    }

    [[nodiscard]] std::time_t modified_seconds() const noexcept {
        return modified_seconds_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view last_modified() const noexcept {
        return last_modified_;
    }

private:
    friend class static_root_entry_view;

    static_root_memory_variant_view(http_content_coding content_coding, std::string_view bytes_value,
        std::uint64_t modified_token, std::time_t modified_seconds, std::string_view etag,
        std::string_view last_modified) noexcept
        : content_coding_(content_coding),
          bytes_(bytes_value),
          modified_token_(modified_token),
          modified_seconds_(modified_seconds),
          etag_(etag),
          last_modified_(last_modified) {}

    http_content_coding content_coding_;
    std::string_view bytes_;
    std::uint64_t modified_token_;
    std::time_t modified_seconds_;
    std::string_view etag_;
    std::string_view last_modified_;
};

class static_root_entry_view final {
public:
    [[nodiscard]] const http_native_path_char_type* file_path() const noexcept {
        return file_path_;
    }

    [[nodiscard]] std::string_view content_type() const noexcept {
        return content_type_;
    }

    [[nodiscard]] std::string_view get_cache_control() const noexcept {
        return cache_control_;
    }

    [[nodiscard]] std::string_view etag() const noexcept {
        return etag_;
    }

    [[nodiscard]] std::string_view last_modified() const noexcept {
        return last_modified_;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] http_response_file_identity identity() const noexcept {
        return identity_;
    }

    [[nodiscard]] std::uint64_t modified_token() const noexcept {
        return modified_token_;
    }

    [[nodiscard]] std::time_t modified_seconds() const noexcept {
        return modified_seconds_;
    }

    [[nodiscard]] static_range_request_policy range_requests() const noexcept {
        return range_requests_;
    }

    [[nodiscard]] static_response_validator_policy response_validators() const noexcept {
        return response_validators_;
    }

    [[nodiscard]] bool has_memory_variants() const noexcept {
        return memory_variants_ != nullptr && !memory_variants_->empty();
    }

    [[nodiscard]] std::optional<static_root_memory_variant_view> memory_variant(
        http_content_coding coding) const noexcept {
        if (memory_variants_ == nullptr) {
            return std::nullopt;
        }
        for (const auto& variant : *memory_variants_) {
            if (variant.content_coding_ == coding) {
                return static_root_memory_variant_view(variant.content_coding_, *variant.bytes_,
                    variant.modified_token_, variant.modified_seconds_, variant.etag_,
                    variant.last_modified_);
            }
        }
        return std::nullopt;
    }

    // The file's indexed bytes when the snapshot keeps them in memory.
    [[nodiscard]] const std::string* resident_bytes() const noexcept {
        return resident_bytes_;
    }

private:
    friend class static_root_access;

    static_root_entry_view(const http_native_path_char_type* file_path, std::string_view content_type_value,
        std::string_view cache_control_value, std::string_view etag, std::string_view last_modified,
        std::uint64_t size, http_response_file_identity identity, std::uint64_t modified_token,
        std::time_t modified_seconds, static_range_request_policy range_requests,
        static_response_validator_policy response_validators, bool directly_servable,
        const std::vector<static_root_memory_variant>* memory_variants,
        const std::string* resident_bytes) noexcept
        : file_path_(file_path),
          content_type_(content_type_value),
          cache_control_(cache_control_value),
          etag_(etag),
          last_modified_(last_modified),
          size_(size),
          identity_(identity),
          modified_token_(modified_token),
          modified_seconds_(modified_seconds),
          range_requests_(range_requests),
          response_validators_(response_validators),
          directly_servable_(directly_servable),
          memory_variants_(memory_variants),
          resident_bytes_(resident_bytes) {}

    const http_native_path_char_type* file_path_;
    std::string_view content_type_;
    std::string_view cache_control_;
    std::string_view etag_;
    std::string_view last_modified_;
    std::uint64_t size_;
    http_response_file_identity identity_;
    std::uint64_t modified_token_;
    std::time_t modified_seconds_;
    static_range_request_policy range_requests_;
    static_response_validator_policy response_validators_;
    bool directly_servable_;
    const std::vector<static_root_memory_variant>* memory_variants_;
    const std::string* resident_bytes_;
};

// One indexed file's open for a response, owning everything it reads: it runs
// on the blocking pool and may outlive the request -- and the snapshot -- that
// started it. On POSIX it opens beneath the snapshot's retained root directory
// without following a symlink in any component; on Windows it opens the
// canonical path, the identity check against the index being its guard.
class static_root_entry_open final {
public:
    // The opened handle; empty when the open failed or, for a checked open,
    // when the handle no longer holds the indexed identity and size.
    [[nodiscard]] http_response_file_descriptor operator()() const noexcept;

private:
    friend class static_root_access;

#if defined(__unix__)
    static_root_entry_open(std::shared_ptr<const native_file_descriptor> root_directory,
        http_native_path_string_type open_path, http_response_file_identity identity, std::uint64_t size,
        bool checked) noexcept
        : root_directory_(std::move(root_directory)),
          open_path_(std::move(open_path)),
          identity_(identity),
          size_(size),
          checked_(checked) {}
#else
    static_root_entry_open(http_native_path_string_type open_path, http_response_file_identity identity,
        std::uint64_t size, bool checked) noexcept
        : open_path_(std::move(open_path)),
          identity_(identity),
          size_(size),
          checked_(checked) {}
#endif

#if defined(__unix__)
    std::shared_ptr<const native_file_descriptor> root_directory_;
#endif
    // Beneath the root on POSIX, the canonical path on Windows; in the
    // synchronized process resource, since the pool thread may free it.
    http_native_path_string_type open_path_;
    http_response_file_identity identity_;
    std::uint64_t size_;
    bool checked_;
};

class static_root_access final {
public:
    [[nodiscard]] static std::unique_ptr<static_root> make(
        const std::filesystem::path& root, const static_root_config_storage& config);
    [[nodiscard]] static std::unique_ptr<static_root> make(
        const std::filesystem::path& root, static_root_config_storage&& config);
    [[nodiscard]] static static_root_config_storage copy_config(const static_root& root);
    [[nodiscard]] static std::string_view index_file(const static_root& root) noexcept;
    [[nodiscard]] static bool has_directory_index(const static_root& root) noexcept;
    [[nodiscard]] static bool has_sidecars(const static_root& root) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find(
        const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find_variant(
        const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static bool is_indexed_directory(
        const static_root& root, std::string_view relative_path) noexcept;
    // Resolves a decoded relative file path the way a static file response
    // does: normalizes it, falls back to the directory index file, and leaves
    // the final relative path in `relative` (backed by `storage` when
    // rewritten). Throws the forbidden http_error for a path outside the root.
    [[nodiscard]] static std::optional<static_root_entry_view> resolve(
        const static_root& root, std::string_view& relative, std::string& storage);
    [[nodiscard]] static std::optional<static_root_entry_view> try_resolve(
        const static_root& root, std::string_view& relative, std::string& storage);
    [[nodiscard]] static std::uint64_t fingerprint(const static_root& root) noexcept;
    [[nodiscard]] static bool same_snapshot(
        const static_root& left, const static_root& right) noexcept;
    static void install_precompressed_variants(static_root& root, const static_root* previous,
        const static_root_precompression_options& options);
    // Reads eligible files into the snapshot, reusing `previous`'s bytes for
    // entries whose indexed metadata is unchanged. A file that cannot be read
    // as indexed stays file-backed and keeps its identity check.
    static void install_resident_bodies(static_root& root, const static_root* previous,
        const static_root_resident_options& options);
    // The open of the entry's file for one response, run by the caller on the
    // blocking pool. `checked` also verifies the opened handle against the
    // indexed identity and size, for a response that sends none of its bytes
    // and so is never checked by the transport.
    [[nodiscard]] static static_root_entry_open entry_open(
        const static_root& root, const static_root_entry_view& entry, bool checked);
};
}  // namespace ruvia::detail

namespace ruvia::detail {

// Above this many entries an index lookup binary-searches instead of scanning.
inline constexpr std::size_t static_root_linear_lookup_limit = 8;

// Looks a key up in an index vector sorted by `projection` (a string_view of
// each entry): a short vector is scanned, a longer one binary-searched.
template <typename entry_type, typename projection_type>
[[nodiscard]] const entry_type* find_static_index_entry(
    const std::vector<entry_type>& entries, std::string_view key, projection_type projection) noexcept {
    if (entries.size() <= static_root_linear_lookup_limit) {
        for (const auto& entry : entries) {
            if (projection(entry) == key) {
                return &entry;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(entries, key, std::ranges::less{}, projection);
    if (iter == entries.end() || projection(*iter) != key) {
        return nullptr;
    }
    return &*iter;
}

// Precompressed sidecars (foo.js.br / .zst / .gz) in selection preference
// order: an Accept-Encoding tie resolves br > zstd > gzip. Indexing,
// precompression and representation selection all read this one table.
struct static_precompression_sidecar final {
    std::string_view suffix_;
    http_content_coding content_coding_;
};

inline constexpr std::array<static_precompression_sidecar, 3> static_precompression_sidecars{{
    {".br", http_content_coding::brotli},
    {".zst", http_content_coding::zstd},
    {".gz", http_content_coding::gzip},
}};

[[nodiscard]] constexpr bool is_precompressed_sidecar_extension(std::string_view extension) noexcept {
    for (const auto& sidecar : static_precompression_sidecars) {
        if (sidecar.suffix_ == extension) {
            return true;
        }
    }
    return false;
}

// Orders two indexed snapshots of one file by (mtime, change token). A sidecar
// older than its identity file is never served: presence alone cannot prove
// its decoded bytes still describe the current resource.
struct static_file_version final {
    std::time_t modified_seconds_;
    std::uint64_t modified_token_;

    [[nodiscard]] constexpr bool at_least_as_new_as(const static_file_version& identity) const noexcept {
        if (modified_seconds_ != identity.modified_seconds_) {
            return modified_seconds_ > identity.modified_seconds_;
        }
        return modified_token_ >= identity.modified_token_;
    }
};

// URL index keys and extension policy use UTF-8 on every platform rather than
// the system narrow encoding of filesystem::path::generic_string<char>().
[[nodiscard]] inline std::u8string static_file_utf8_path(
    const std::filesystem::path& path) {
    return path.generic_u8string();
}

[[nodiscard]] inline std::string static_file_utf8_string(
    const std::filesystem::path& path) {
    if constexpr (std::is_same_v<std::filesystem::path::value_type, char>) {
        return path.generic_string();
    } else {
        const auto utf8 = static_file_utf8_path(path);
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
}
template <typename char_value_type>
[[nodiscard]] inline std::basic_string_view<char_value_type> static_file_extension(
    std::basic_string_view<char_value_type> path) noexcept {
    for (std::size_t i = path.size(); i > 0; --i) {
        const auto c = path[i - 1];
        if (c == static_cast<char_value_type>('/') || c == static_cast<char_value_type>('\\')) {
            return {};
        }
        if (c == static_cast<char_value_type>('.')) {
            return path.substr(i - 1);
        }
    }
    return {};
}

[[nodiscard]] inline std::string lower_static_file_extension_from_utf8(
    std::string_view utf8_path) {
    const auto source_value = static_file_extension(utf8_path);
    if (source_value.empty()) {
        return {};
    }
    std::string extension;
    extension.reserve(source_value.size());
    for (const char c : source_value) {
        auto out = c;
        if (out >= 'A' && out <= 'Z') {
            out = static_cast<char>(out + ('a' - 'A'));
        }
        extension.push_back(out);
    }
    return extension;
}
[[nodiscard]] inline std::string_view guess_static_file_content_type_from_extension(
    std::string_view lower_extension) noexcept {
    switch (lower_extension.size()) {
        case 3U:
            if (lower_extension == ".js") {
                return "text/javascript; charset=utf-8";
            }
            break;
        case 4U:
            if (lower_extension == ".htm") {
                return "text/html; charset=utf-8";
            }
            if (lower_extension == ".css") {
                return "text/css; charset=utf-8";
            }
            if (lower_extension == ".mjs") {
                return "text/javascript; charset=utf-8";
            }
            if (lower_extension == ".txt" || lower_extension == ".log") {
                return "text/plain; charset=utf-8";
            }
            if (lower_extension == ".png") {
                return "image/png";
            }
            if (lower_extension == ".jpg") {
                return "image/jpeg";
            }
            if (lower_extension == ".gif") {
                return "image/gif";
            }
            if (lower_extension == ".svg") {
                return "image/svg+xml";
            }
            if (lower_extension == ".bmp") {
                return "image/bmp";
            }
            if (lower_extension == ".ico" || lower_extension == ".cur") {
                return "image/vnd.microsoft.icon";
            }
            if (lower_extension == ".ttf") {
                return "font/ttf";
            }
            if (lower_extension == ".otf") {
                return "font/otf";
            }
            if (lower_extension == ".eot") {
                return "application/vnd.ms-fontobject";
            }
            if (lower_extension == ".xml" || lower_extension == ".xsl") {
                return "text/xml; charset=utf-8";
            }
            if (lower_extension == ".map") {
                return "application/json; charset=utf-8";
            }
            break;
        case 5U:
            if (lower_extension == ".html") {
                return "text/html; charset=utf-8";
            }
            if (lower_extension == ".json") {
                return "application/json; charset=utf-8";
            }
            if (lower_extension == ".jpeg") {
                return "image/jpeg";
            }
            if (lower_extension == ".wasm") {
                return "application/wasm";
            }
            if (lower_extension == ".apng") {
                return "image/apng";
            }
            if (lower_extension == ".avif") {
                return "image/avif";
            }
            if (lower_extension == ".webp") {
                return "image/webp";
            }
            if (lower_extension == ".woff") {
                return "font/woff";
            }
            break;
        case 6U:
            if (lower_extension == ".woff2") {
                return "font/woff2";
            }
            break;
        case 12U:
            if (lower_extension == ".webmanifest") {
                return "application/manifest+json; charset=utf-8";
            }
            break;
        default:
            break;
    }
    return "application/octet-stream";
}

template <typename char_value_type>
[[nodiscard]] inline std::string_view guess_static_file_content_type_from_path_view(
    std::basic_string_view<char_value_type> path) noexcept {
    const auto extension = static_file_extension(path);
    std::array<char, 12> lower;
    if (extension.size() < 3U || extension.size() > lower.size()) {
        return "application/octet-stream";
    }
    for (std::size_t i = 0; i < extension.size(); ++i) {
        const auto c = extension[i];
        if (c < static_cast<char_value_type>(0) || c > static_cast<char_value_type>(0x7f)) {
            return "application/octet-stream";
        }
        auto out = static_cast<char>(c);
        if (out >= 'A' && out <= 'Z') {
            out = static_cast<char>(out + ('a' - 'A'));
        }
        lower[i] = out;
    }
    return guess_static_file_content_type_from_extension(
        std::string_view(lower.data(), extension.size()));
}

// Strong validator for one file snapshot: a quoted 64-bit FNV-1a digest of the
// size, modification token, file identity and (for a stored non-identity
// representation) content coding, so any change to one of them yields a new
// ETag. Hashing keeps the field short and does not disclose the volume and
// file index the identity words carry.
inline constexpr std::size_t static_file_etag_size = 2U + 2U * sizeof(std::uint64_t);

[[nodiscard]] inline std::array<char, static_file_etag_size> format_static_file_snapshot_etag(
    std::uint64_t size, std::uint64_t modified_token, http_response_file_identity identity,
    http_content_coding coding = http_content_coding::identity) noexcept {
    auto hash = fnv1a_accumulate_integer(fnv1a_offset_basis, size);
    hash = fnv1a_accumulate_integer(hash, modified_token);
    for (const auto word : identity.words()) {
        hash = fnv1a_accumulate_integer(hash, word);
    }
    if (coding != http_content_coding::identity) {
        hash = fnv1a_accumulate(hash, http_content_coding_token(coding));
    }
    std::array<unsigned char, sizeof(hash)> digest{};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        digest[i] = static_cast<unsigned char>(hash >> ((digest.size() - 1 - i) * 8U));
    }
    std::array<char, static_file_etag_size> output{};
    output.front() = '"';
    http_encode_lower_hex(output.data() + 1, digest);
    output.back() = '"';
    return output;
}

[[nodiscard]] inline std::string make_static_file_snapshot_etag(
    std::uint64_t size, std::uint64_t modified_token,
    http_response_file_identity identity, http_content_coding coding = http_content_coding::identity) {
    const auto formatted = format_static_file_snapshot_etag(size, modified_token, identity, coding);
    return std::string(formatted.data(), formatted.size());
}

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline bool static_file_extensions_equivalent(
    std::string_view left, std::string_view right) noexcept {
    if (left.starts_with('.')) {
        left.remove_prefix(1);
    }
    if (right.starts_with('.')) {
        right.remove_prefix(1);
    }
    return http_ascii_equals_ignore_case(left, right);
}

[[nodiscard]] inline bool static_root_serves_dotfiles(static_dotfile_policy policy) {
    switch (policy) {
        case static_dotfile_policy::deny:
            return false;
        case static_dotfile_policy::serve:
            return true;
        default:
            throw std::invalid_argument("invalid static dotfile policy");
    }
}

inline void validate_static_root_options(const static_root_options& options) {
    (void)static_root_serves_dotfiles(options.dotfiles_);
    switch (options.range_requests_) {
        case static_range_request_policy::ignore:
        case static_range_request_policy::honor:
            break;
        default:
            throw std::invalid_argument("invalid static range request policy");
    }
    switch (options.response_validators_) {
        case static_response_validator_policy::omit:
        case static_response_validator_policy::emit:
            break;
        default:
            throw std::invalid_argument("invalid static response validator policy");
    }
    if (!ruvia::is_valid_http_header_value(options.cache_control_.view()) ||
        (!options.default_content_type_.view().empty() &&
            !ruvia::is_valid_http_content_type_field_value(options.default_content_type_.view()))) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (std::size_t i = 0; i < options.mime_types_.size(); ++i) {
        const auto& mime = options.mime_types_[i];
        if (!ruvia::detail::is_valid_static_file_extension(mime.extension_.view()) ||
            mime.content_type_.view().empty() ||
            !ruvia::is_valid_http_content_type_field_value(mime.content_type_.view())) {
            throw std::invalid_argument("invalid static file mime type");
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (static_file_extensions_equivalent(
                    options.mime_types_[previous].extension_.view(), mime.extension_.view())) {
                throw std::invalid_argument("duplicate static file mime extension");
            }
        }
    }
    switch (options.file_types_.kind_) {
        case static_file_type_policy::kind::defaults:
        case static_file_type_policy::kind::all:
            if (!options.file_types_.extensions_.empty()) {
                throw std::invalid_argument("static file type extensions require only mode");
            }
            break;
        case static_file_type_policy::kind::only:
            if (options.file_types_.extensions_.empty()) {
                throw std::invalid_argument("static file type allow-list must not be empty");
            }
            for (const auto& extension : options.file_types_.extensions_) {
                if (!is_valid_static_file_extension(extension)) {
                    throw std::invalid_argument("invalid static file type");
                }
            }
            break;
        default:
            throw std::invalid_argument("invalid static file type mode");
    }
    const auto index_file = options.index_file_.view();
    if ((index_file.find('/') != std::string_view::npos) || (index_file.find('\\') != std::string_view::npos) || index_file == "." ||
        index_file == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
enum class static_file_selection_mode : std::uint8_t {
    identity_only,
    precompressed,
};

}  // namespace ruvia::detail

namespace ruvia {

// Which bytes a static route actually serves: the identity file, a
// precompressed sidecar, or a refresh-built in-memory variant the client
// accepts.
class static_file_representation final {
public:
    static_file_representation(
        detail::static_root_entry_view entry_value, http_content_coding content_coding) noexcept
        : entry_(entry_value),
          content_coding_(content_coding) {}

    static_file_representation(detail::static_root_entry_view entry_value,
        detail::static_root_memory_variant_view memory_variant, http_content_coding content_coding) noexcept
        : entry_(entry_value),
          memory_variant_(memory_variant),
          content_coding_(content_coding) {}

    [[nodiscard]] const detail::static_root_entry_view& entry() const& noexcept RUVIA_LIFETIMEBOUND {
        return entry_;
    }
    const detail::static_root_entry_view& entry() const&& = delete;

    [[nodiscard]] const detail::static_root_memory_variant_view* memory_variant() const& noexcept RUVIA_LIFETIMEBOUND {
        return memory_variant_.has_value() ? &*memory_variant_ : nullptr;
    }
    const detail::static_root_memory_variant_view* memory_variant() const&& = delete;
    [[nodiscard]] http_content_coding content_coding() const noexcept {
        return content_coding_;
    }

private:
    detail::static_root_entry_view entry_;
    std::optional<detail::static_root_memory_variant_view> memory_variant_;
    http_content_coding content_coding_;
};

// Selects the best precompressed sidecar (foo.js.br / .gz / .zst) or
// refresh-built in-memory variant the request's Accept-Encoding selection
// accepts -- highest q-value wins, ties resolve br > zstd > gzip. The served
// bytes are the variant's, so its size/etag/modified describe the wire
// representation; the caller keeps the original Content-Type. Index lookups
// only (no per-request filesystem stat). nullopt means no acceptable
// representation exists.
[[nodiscard]] std::optional<static_file_representation> select_static_file_representation(
    const static_root& root, std::string_view relative,
    const http_response_coding_selection& selection,
    detail::static_root_entry_view identity);

}  // namespace ruvia
