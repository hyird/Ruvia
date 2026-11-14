#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <asio/bind_executor.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ssl/context.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/types.h>

#include "borrowed_text.h"
#include "connection_lifecycle.h"
#include "context.h"
#include "crypto.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http2_push.h"
#include "http_cache.h"
#include "http_client_ip.h"
#include "http_client_types.h"
#include "http_content_coding.h"
#include "http_content_negotiation.h"
#include "http_cookie.h"
#include "http_cors.h"
#include "http_header.h"
#include "http_priority.h"
#include "http_request.h"
#include "http_response.h"
#include "http_server.h"
#include "inbound_buffer.h"
#include "middleware.h"
#include "rate_limit.h"
#include "response_file.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "task_scope.h"
#include "tls.h"
#include "websocket_connection.h"
#include "worker_ref.h"
#include "worker_wait.h"

namespace ruvia::detail {

[[nodiscard]] inline bool is_windows_drive_path(std::string_view path) noexcept {
    if (path.size() < 2 || path[1] != ':') {
        return false;
    }
    const auto c = path.front();
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[noreturn]] inline void throw_invalid_static_file_path() {
    throw http_error({.status_ = ruvia::http_status::forbidden, .code_ = "forbidden", .message_ = "invalid static file path"});
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
[[nodiscard]] inline std::optional<std::string_view> try_normalize_static_relative_path(std::string_view input, std::string& storage) {
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

[[nodiscard]] inline std::string_view normalize_static_relative_path(std::string_view input, std::string& storage) {
    const auto normalized = try_normalize_static_relative_path(input, storage);
    if (!normalized.has_value()) {
        throw_invalid_static_file_path();
    }
    return *normalized;
}

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

[[nodiscard]] static_root_config_storage store_validated_static_root_config(const static_root_options& source_value);

[[nodiscard]] bool is_valid_static_file_extension(std::string_view extension) noexcept;

[[nodiscard]] bool file_type_allowed(std::string_view extension, const static_root_config_storage& config);

[[nodiscard]] std::string content_type_for(std::string_view extension, const static_root_config_storage& config);
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

    explicit static_root_state(static_root_config_storage configured_policy) noexcept :
        config_(std::move(configured_policy)) {}

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

    static_root_memory_variant_view(http_content_coding content_coding, std::string_view bytes_value, std::uint64_t modified_token, std::time_t modified_seconds, std::string_view etag, std::string_view last_modified) noexcept :
        content_coding_(content_coding),
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

    [[nodiscard]] std::optional<static_root_memory_variant_view> memory_variant(http_content_coding coding) const noexcept {
        if (memory_variants_ == nullptr) {
            return std::nullopt;
        }
        for (const auto& variant : *memory_variants_) {
            if (variant.content_coding_ == coding) {
                return static_root_memory_variant_view(variant.content_coding_, *variant.bytes_, variant.modified_token_, variant.modified_seconds_, variant.etag_, variant.last_modified_);
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

    static_root_entry_view(const http_native_path_char_type* file_path, std::string_view content_type_value, std::string_view cache_control_value, std::string_view etag, std::string_view last_modified, std::uint64_t size, http_response_file_identity identity, std::uint64_t modified_token, std::time_t modified_seconds, static_range_request_policy range_requests, static_response_validator_policy response_validators, bool directly_servable, const std::vector<static_root_memory_variant>* memory_variants, const std::string* resident_bytes) noexcept :
        file_path_(file_path),
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
    static_root_entry_open(std::shared_ptr<const native_file_descriptor> root_directory, http_native_path_string_type open_path, http_response_file_identity identity, std::uint64_t size, bool checked) noexcept :
        root_directory_(std::move(root_directory)),
        open_path_(std::move(open_path)),
        identity_(identity),
        size_(size),
        checked_(checked) {}
#else
    static_root_entry_open(http_native_path_string_type open_path, http_response_file_identity identity, std::uint64_t size, bool checked) noexcept :
        open_path_(std::move(open_path)),
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
    [[nodiscard]] static std::unique_ptr<static_root> make(const std::filesystem::path& root, const static_root_config_storage& config);
    [[nodiscard]] static std::unique_ptr<static_root> make(const std::filesystem::path& root, static_root_config_storage&& config);
    [[nodiscard]] static static_root_config_storage copy_config(const static_root& root);
    [[nodiscard]] static std::string_view index_file(const static_root& root) noexcept;
    [[nodiscard]] static bool has_directory_index(const static_root& root) noexcept;
    [[nodiscard]] static bool has_sidecars(const static_root& root) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find(const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static std::optional<static_root_entry_view> find_variant(const static_root& root, std::string_view relative_path) noexcept;
    [[nodiscard]] static bool is_indexed_directory(const static_root& root, std::string_view relative_path) noexcept;
    // Resolves a decoded relative file path the way a static file response
    // does: normalizes it, falls back to the directory index file, and leaves
    // the final relative path in `relative` (backed by `storage` when
    // rewritten). Throws the forbidden http_error for a path outside the root.
    [[nodiscard]] static std::optional<static_root_entry_view> resolve(const static_root& root, std::string_view& relative, std::string& storage);
    [[nodiscard]] static std::optional<static_root_entry_view> try_resolve(const static_root& root, std::string_view& relative, std::string& storage);
    [[nodiscard]] static std::uint64_t fingerprint(const static_root& root) noexcept;
    [[nodiscard]] static bool same_snapshot(const static_root& left, const static_root& right) noexcept;
    static void install_precompressed_variants(static_root& root, const static_root* previous, const static_root_precompression_options& options);
    // Reads eligible files into the snapshot, reusing `previous`'s bytes for
    // entries whose indexed metadata is unchanged. A file that cannot be read
    // as indexed stays file-backed and keeps its identity check.
    static void install_resident_bodies(static_root& root, const static_root* previous, const static_root_resident_options& options);
    // The open of the entry's file for one response, run by the caller on the
    // blocking pool. `checked` also verifies the opened handle against the
    // indexed identity and size, for a response that sends none of its bytes
    // and so is never checked by the transport.
    [[nodiscard]] static static_root_entry_open entry_open(const static_root& root, const static_root_entry_view& entry, bool checked);
};
// Above this many entries an index lookup binary-searches instead of scanning.
inline constexpr std::size_t static_root_linear_lookup_limit = 8;

// Looks a key up in an index vector sorted by `projection` (a string_view of
// each entry): a short vector is scanned, a longer one binary-searched.
template <typename entry_type, typename projection_type>
[[nodiscard]] const entry_type* find_static_index_entry(const std::vector<entry_type>& entries, std::string_view key, projection_type projection) noexcept {
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
[[nodiscard]] inline std::u8string static_file_utf8_path(const std::filesystem::path& path) {
    return path.generic_u8string();
}

[[nodiscard]] inline std::string static_file_utf8_string(const std::filesystem::path& path) {
    if constexpr (std::is_same_v<std::filesystem::path::value_type, char>) {
        return path.generic_string();
    } else {
        const auto utf8 = static_file_utf8_path(path);
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
}
template <typename char_value_type>
[[nodiscard]] inline std::basic_string_view<char_value_type> static_file_extension(std::basic_string_view<char_value_type> path) noexcept {
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

[[nodiscard]] inline std::string lower_static_file_extension_from_utf8(std::string_view utf8_path) {
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
[[nodiscard]] inline std::string_view guess_static_file_content_type_from_extension(std::string_view lower_extension) noexcept {
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
[[nodiscard]] inline std::string_view guess_static_file_content_type_from_path_view(std::basic_string_view<char_value_type> path) noexcept {
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
    return guess_static_file_content_type_from_extension(std::string_view(lower.data(), extension.size()));
}

// Strong validator for one file snapshot: a quoted 64-bit FNV-1a digest of the
// size, modification token, file identity and (for a stored non-identity
// representation) content coding, so any change to one of them yields a new
// ETag. Hashing keeps the field short and does not disclose the volume and
// file index the identity words carry.
inline constexpr std::size_t static_file_etag_size = 2U + 2U * sizeof(std::uint64_t);

[[nodiscard]] inline std::array<char, static_file_etag_size> format_static_file_snapshot_etag(std::uint64_t size, std::uint64_t modified_token, http_response_file_identity identity, http_content_coding coding = http_content_coding::identity) noexcept {
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

[[nodiscard]] inline std::string make_static_file_snapshot_etag(std::uint64_t size, std::uint64_t modified_token, http_response_file_identity identity, http_content_coding coding = http_content_coding::identity) {
    const auto formatted = format_static_file_snapshot_etag(size, modified_token, identity, coding);
    return std::string(formatted.data(), formatted.size());
}

[[nodiscard]] inline bool static_file_extensions_equivalent(std::string_view left, std::string_view right) noexcept {
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
    if (!ruvia::is_valid_http_header_value(options.cache_control_.view()) || (!options.default_content_type_.view().empty() && !ruvia::is_valid_http_content_type_field_value(options.default_content_type_.view()))) {
        throw std::invalid_argument("invalid static file header value");
    }
    for (std::size_t i = 0; i < options.mime_types_.size(); ++i) {
        const auto& mime = options.mime_types_[i];
        if (!ruvia::detail::is_valid_static_file_extension(mime.extension_.view()) || mime.content_type_.view().empty() || !ruvia::is_valid_http_content_type_field_value(mime.content_type_.view())) {
            throw std::invalid_argument("invalid static file mime type");
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (static_file_extensions_equivalent(options.mime_types_[previous].extension_.view(), mime.extension_.view())) {
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
    if ((index_file.find('/') != std::string_view::npos) || (index_file.find('\\') != std::string_view::npos) || index_file == "." || index_file == "..") {
        throw std::invalid_argument("invalid static file index name");
    }
}

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
    static_file_representation(detail::static_root_entry_view entry_value, http_content_coding content_coding) noexcept :
        entry_(entry_value),
        content_coding_(content_coding) {}

    static_file_representation(detail::static_root_entry_view entry_value, detail::static_root_memory_variant_view memory_variant, http_content_coding content_coding) noexcept :
        entry_(entry_value),
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
[[nodiscard]] std::optional<static_file_representation> select_static_file_representation(const static_root& root, std::string_view relative, const http_response_coding_selection& selection, detail::static_root_entry_view identity);

class http_client;
class http_client_ref;
class stop_token;
class operation_scope;

#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif

#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif
namespace detail {

class borrowed_clients;
class db_registry;
struct request_io_metrics;
class http_client_registry;
class redis_registry;

// A copyable view of the complete client registry set owned by one worker, or
// of the clients a router borrows for its dispatches. The only other state is
// detached: partial registry graphs are not representable. Detached views are
// only placeholders for contexts built without worker capabilities; an
// attached view never outlives its worker, which joins all posted work before
// destroying its registries, nor a router's dispatch.
class worker_client_registry_view final {
public:
    [[nodiscard]] static constexpr worker_client_registry_view detached() noexcept {
        return worker_client_registry_view(nullptr);
    }

    constexpr worker_client_registry_view(http_client_registry& http_clients
#ifdef RUVIA_ENABLE_DATABASE
        ,
        db_registry& databases
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_registry& redis
#endif
        ) noexcept :
        http_clients_(&http_clients)
#ifdef RUVIA_ENABLE_DATABASE
        ,
        databases_(&databases)
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_(&redis)
#endif
    {
    }

    explicit constexpr worker_client_registry_view(const borrowed_clients& borrowed) noexcept :
        borrowed_(&borrowed) {}

    [[nodiscard]] constexpr bool attached() const noexcept {
        return http_clients_ != nullptr || borrowed_ != nullptr;
    }

    // `io_metrics` is the request's counter block, or null outside a request.
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
    [[nodiscard]] db_session db(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
    [[nodiscard]] redis_session redis(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

    [[nodiscard]] http_client_ref client(::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] http_client_ref client(std::string_view alias, ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;

private:
    explicit constexpr worker_client_registry_view(std::nullptr_t) noexcept {}

    [[nodiscard]] const http_client& borrowed_http_clients(std::string_view alias) const;

    const borrowed_clients* borrowed_{nullptr};
    http_client_registry* http_clients_{nullptr};
#ifdef RUVIA_ENABLE_DATABASE
    db_registry* databases_{nullptr};
#endif
#ifdef RUVIA_ENABLE_REDIS
    redis_registry* redis_{nullptr};
#endif
};

}  // namespace detail
class body_reader;
class response_stream_writer;
class websocket;
class http_tunnel;

namespace detail {

class request_body_loader;
class context_request_body_source;
class context_response_output;

// A context always has exactly one request-body source. The buffered alternative
// means the complete bytes are already exposed by http_request; the other two
// borrow non-null runtime facades for the duration of dispatch.
class context_buffered_request_body_source final {
private:
    friend class context_request_body_source;
    constexpr context_buffered_request_body_source() noexcept = default;
};

class context_lazy_request_body_source final {
public:
    [[nodiscard]] constexpr request_body_loader& loader() const noexcept {
        return *loader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_lazy_request_body_source(request_body_loader& loader) noexcept :
        loader_(&loader) {}

    request_body_loader* loader_;
};

class context_streaming_request_body_source final {
public:
    [[nodiscard]] constexpr body_reader& reader() const noexcept {
        return *reader_;
    }

private:
    friend class context_request_body_source;

    explicit constexpr context_streaming_request_body_source(body_reader& reader_value) noexcept :
        reader_(&reader_value) {}

    body_reader* reader_;
};

class context_request_body_source final {
public:
    constexpr context_request_body_source() noexcept :
        value_(context_buffered_request_body_source{}) {}

    [[nodiscard]] static constexpr context_request_body_source lazy(request_body_loader& loader) noexcept {
        return context_request_body_source(context_lazy_request_body_source(loader));
    }

    [[nodiscard]] static constexpr context_request_body_source streaming(body_reader& reader_value) noexcept {
        return context_request_body_source(context_streaming_request_body_source(reader_value));
    }

    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const& noexcept {
        return std::get_if<context_lazy_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_lazy_request_body_source* lazy() const&& = delete;

    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const& noexcept {
        return std::get_if<context_streaming_request_body_source>(&value_);
    }
    [[nodiscard]] constexpr const context_streaming_request_body_source* streaming() const&& = delete;

private:
    using value_type = std::variant<context_buffered_request_body_source, context_lazy_request_body_source, context_streaming_request_body_source>;

    template <typename source>
    explicit constexpr context_request_body_source(source source_value) noexcept :
        value_(source_value) {}

    value_type value_;
};

// A context likewise has exactly one response output. The buffered alternative
// uses the ordinary http_response return path; long-lived outputs borrow one
// non-null runtime facade and cannot coexist in the same context. Every
// alternative but buffered and an uncommitted response stream means the
// response head is on the wire and no HTTP response can follow.
class context_buffered_response_output final {
private:
    friend class context_response_output;
    constexpr context_buffered_response_output() noexcept = default;
};

class context_response_stream_output final {
public:
    [[nodiscard]] constexpr response_stream_writer& writer() const noexcept {
        return *writer_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_response_stream_output(response_stream_writer& writer) noexcept :
        writer_(&writer) {}

    response_stream_writer* writer_;
};

class context_websocket_output final {
public:
    [[nodiscard]] constexpr websocket& get_websocket() const noexcept {
        return *websocket_;
    }

private:
    friend class context_response_output;

    explicit constexpr context_websocket_output(websocket& websocket_value) noexcept :
        websocket_(&websocket_value) {}

    websocket* websocket_;
};

class context_tunnel_output final {
public:
    [[nodiscard]] constexpr http_tunnel& tunnel() const noexcept {
        return *tunnel_;
    }

private:
    friend class context_response_output;
    explicit constexpr context_tunnel_output(http_tunnel& tunnel) noexcept :
        tunnel_(&tunnel) {}
    http_tunnel* tunnel_;
};

// The upgrade or CONNECT handshake head was handed to the transport. Holds
// while no websocket/tunnel facade is bound -- before the session binds one and
// after it restores this output -- so a buffered error built by middleware can
// never be mistaken for a sendable response.
class context_upgraded_response_output final {
private:
    friend class context_response_output;
    constexpr context_upgraded_response_output() noexcept = default;
};

class context_response_output final {
public:
    constexpr context_response_output() noexcept :
        value_(context_buffered_response_output{}) {}

    [[nodiscard]] static constexpr context_response_output response_stream(response_stream_writer& writer) noexcept {
        return context_response_output(context_response_stream_output(writer));
    }

    [[nodiscard]] static constexpr context_response_output websocket_value(websocket& websocket_value) noexcept {
        return context_response_output(context_websocket_output(websocket_value));
    }

    [[nodiscard]] static constexpr context_response_output tunnel(http_tunnel& tunnel) noexcept {
        return context_response_output(context_tunnel_output(tunnel));
    }

    [[nodiscard]] static constexpr context_response_output upgraded() noexcept {
        return context_response_output(context_upgraded_response_output{});
    }

    // Whether the response head already reached the transport.
    [[nodiscard]] bool committed() const noexcept;

    [[nodiscard]] constexpr const context_tunnel_output* tunnel() const& noexcept {
        return std::get_if<context_tunnel_output>(&value_);
    }
    const context_tunnel_output* tunnel() const&& = delete;
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const& noexcept {
        return std::get_if<context_buffered_response_output>(&value_);
    }
    [[nodiscard]] constexpr const context_buffered_response_output* buffered() const&& = delete;

    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const& noexcept {
        return std::get_if<context_response_stream_output>(&value_);
    }
    [[nodiscard]] constexpr const context_response_stream_output* response_stream() const&& = delete;

    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const& noexcept {
        return std::get_if<context_websocket_output>(&value_);
    }
    [[nodiscard]] constexpr const context_websocket_output* get_websocket() const&& = delete;

private:
    using value_type = std::variant<context_buffered_response_output, context_response_stream_output, context_websocket_output, context_tunnel_output, context_upgraded_response_output>;

    template <typename output_type>
    explicit constexpr context_response_output(output_type output) noexcept :
        value_(output) {}

    value_type value_;
};

}  // namespace detail
class http_request;
class http_request_trailers;
class env;
}  // namespace ruvia

namespace ruvia::detail {

class rate_limiter;
class http_interim_response_output;
class inbound_buffer_account;
class route_table;
struct connection_failure_sink;
class tls_served_certificate;
class worker_state_registry;

// Borrows the request's connection.
class http_push_output final {
public:
    using submit_type = bool (*)(void*, http_push_request_view);
    http_push_output(void* target, submit_type submit) noexcept :
        target_(target),
        submit_(submit) {}
    // Header-list bounds are enforced by the HTTP/2 submit path.
    [[nodiscard]] bool push(http_push_request_view request) {
        return submit_(target_, request);
    }

private:
    void* target_;
    submit_type submit_;
};

// Request-scoped typed access to the owning connection's advertisement lane.
class http_connection_advertisement_output final {
public:
    using origins_type = void (*)(void*, std::span<const std::string_view>);
    using alternative_service_type = void (*)(void*, std::string_view);
    http_connection_advertisement_output(void* target, origins_type origins, alternative_service_type service) noexcept :
        target_(target),
        origins_(origins),
        service_(service) {}
    void advertise_origins(std::span<const std::string_view> origins);
    void advertise_alternative_service(std::string_view value);

private:
    void* target_;
    origins_type origins_;
    alternative_service_type service_;
};

struct context_worker_services final {
    std::reference_wrapper<const worker_ref> worker_;
    worker_client_registry_view clients_{worker_client_registry_view::detached()};
    rate_limiter* rate_limiter_{};
    const ruvia::env* env_{};
    std::size_t max_decoded_body_bytes_{default_max_buffered_body_bytes};
    const worker_state_registry* states_{};
    bool precompressed_static_files_{};
    // Bounds each blocking-pool wait of a file response's open, as the body
    // reads the transport waits on are bounded: the server write_timeout_.
    std::optional<timeout_duration> file_io_timeout_{};
    // Where a file response's real faults (no blocking pool, a stalled open)
    // are reported; the worker's connection failure outlet, never null.
    const connection_failure_sink* connection_failure_{};
    const http_client_ip_extractor* trusted_proxies_{};
};

struct context_connection_services final {
    conn_info info_;
    inbound_buffer_account* inbound_buffers_{};
    const route_table* routes_{};
    rate_limiter* route_rate_limiter_{};
    // Whether the peer is a configured trusted proxy. Decided once per
    // connection: the peer address never changes, so its IP parse and block
    // match are not repeated per request.
    bool peer_trusted_{false};
    // The certificate a TLS connection presented; null for cleartext.
    tls_served_certificate* served_certificate_{};
    // The Alt-Svc field value (RFC 7838 §3) this connection's listener
    // advertises on every response; empty when it advertises none.
    std::string_view alternative_service_{};
    // Set for a listener_dispatch::request_stream connection: every request
    // goes to this callback instead of the routes.
    request_stream_callback_ref request_stream_{};
    connection_lifecycle* lifecycle_{};
    // A router's borrowed clients and worker states, bound for its dispatches
    // like routes_; null keeps the worker's own.
    const borrowed_clients* router_clients_{};
    const worker_state_registry* router_states_{};
};

// The code request_stream::reset() chose; monostate selects the protocol's
// cancellation code (HTTP/2 CANCEL, H3_REQUEST_CANCELLED).
using request_reset_code = std::variant<std::monostate, http2_error_code, http3_error_code>;

struct request_stream_control final {
    callback_ref<void(request_reset_code)> reset_;
    bool reset_requested_{false};
};

// Borrowed protocol channels outlive every context for this request.
struct context_request_channels final {
    http_interim_response_output* interim_output_{};
    http_connection_advertisement_output* connection_advertisements_{};
    http_push_output* push_output_{};
    const std::optional<http_priority>* priority_update_{};
};

struct context_request_services final {
    std::reference_wrapper<const stop_token> stop_token_;
    context_request_body_source body_source_{};
    const http_request_trailers* trailers_{};
    context_request_channels channels_{};
    context_response_output response_output_{};
    std::size_t dispatch_depth_{};
    // The matched route's declared body_limit; 0 when it declares none.
    std::size_t route_body_limit_{};
    request_io_metrics* io_metrics_{nullptr};
    // Admission's Accept-Encoding selection for this request; empty for a
    // context created outside admission.
    std::optional<http_response_coding_selection> response_coding_{};
    // The arrival instant the request's owner pinned for rate-limit decisions
    // (test_app); empty when those decisions read the steady clock.
    std::optional<std::chrono::steady_clock::time_point> pinned_arrival_{};
    bool early_data_{false};
    request_body_state body_state_{};
    request_stream_control* stream_control_{};
};

class context_services final {
public:
    context_services() = delete;

    context_services(const context_worker_services& worker_services, const stop_token& stop_token_value) :
        worker_services_(&worker_services),
        request_services_{.stop_token_ = stop_token_value} {
        if (!worker_services_->worker_.get().valid()) {
            throw std::invalid_argument("context services require a valid worker");
        }
    }
    context_services(context_worker_services&&, const stop_token&) = delete;
    context_services(const context_worker_services&, stop_token&&) = delete;

    [[nodiscard]] constexpr worker_client_registry_view client_registries() const noexcept {
        return connection_services_.router_clients_ != nullptr ? worker_client_registry_view(*connection_services_.router_clients_) : worker_services_->clients_;
    }

    // Either argument may be null to keep the worker's own capability.
    void bind_router_capabilities(const borrowed_clients* clients, const worker_state_registry* states) noexcept {
        connection_services_.router_clients_ = clients;
        connection_services_.router_states_ = states;
    }

    [[nodiscard]] detail::rate_limiter* rate_limiter() const noexcept {
        return connection_services_.route_rate_limiter_ != nullptr ? connection_services_.route_rate_limiter_ : worker_services_->rate_limiter_;
    }

    [[nodiscard]] const ruvia::env* env() const noexcept {
        return worker_services_->env_;
    }

    [[nodiscard]] inbound_buffer_account* inbound_buffers() const noexcept {
        return connection_services_.inbound_buffers_;
    }

    [[nodiscard]] context_services with_inbound_buffers(inbound_buffer_account& account) const noexcept {
        auto services = *this;
        services.connection_services_.inbound_buffers_ = &account;
        return services;
    }
    context_services with_inbound_buffers(inbound_buffer_account&&) const = delete;
    [[nodiscard]] constexpr std::size_t max_decoded_body_bytes() const noexcept {
        return worker_services_->max_decoded_body_bytes_;
    }

    // Ceiling for this request's decoded Content-Encoding representation. A
    // route's body_limit bounds what the route reads, not only the wire
    // bytes: a small compressed body must not inflate past it up to the
    // worker-wide bound.
    [[nodiscard]] constexpr std::size_t max_decoded_request_body_bytes() const noexcept {
        const auto route_limit = request_services_.route_body_limit_;
        return route_limit != 0 && route_limit < worker_services_->max_decoded_body_bytes_ ? route_limit : worker_services_->max_decoded_body_bytes_;
    }

    void bind_route_body_limit(std::size_t limit) noexcept {
        request_services_.route_body_limit_ = limit;
    }

    [[nodiscard]] request_io_metrics* io_metrics() const noexcept {
        return request_services_.io_metrics_;
    }

    void bind_io_metrics(request_io_metrics* metrics) noexcept {
        request_services_.io_metrics_ = metrics;
    }

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_services_->worker_.get();
    }

    [[nodiscard]] const stop_token& get_stop_token() const noexcept {
        return request_services_.stop_token_.get();
    }

    // A transport that can observe the client leaving binds a request stop
    // that also fires then, on a request-local copy; `token` must observe the previous one and
    // outlive every context dispatched with these services.
    void bind_stop_token(const stop_token& token) noexcept {
        request_services_.stop_token_ = token;
    }
    void bind_stop_token(stop_token&&) = delete;

    void bind_early_data(bool value) noexcept {
        request_services_.early_data_ = value;
    }
    [[nodiscard]] bool early_data() const noexcept {
        return request_services_.early_data_;
    }

    void bind_body_state(request_body_state state) noexcept {
        request_services_.body_state_ = state;
    }
    [[nodiscard]] request_body_state body_state() const noexcept {
        return request_services_.body_state_;
    }
    void bind_stream_control(request_stream_control& control) noexcept {
        request_services_.stream_control_ = &control;
    }
    [[nodiscard]] bool request_reset() const noexcept {
        const auto* control = request_services_.stream_control_;
        return control != nullptr && control->reset_requested_;
    }
    void reset_request(request_reset_code error) const {
        if (auto* control = request_services_.stream_control_) {
            if (!control->reset_requested_) {
                control->reset_requested_ = true;
                control->reset_(error);
            }
        } else {
            abort_connection();
        }
    }

    [[nodiscard]] const route_table* routes() const noexcept {
        return connection_services_.routes_;
    }

    // Every dispatch path hands the context the table it may re-enter, which
    // also selects the not-found handler for the request path when the
    // handler asks for one. The route table is server-owned and outlives every
    // dispatched request.
    void bind_routes(const route_table& routes_value) noexcept {
        connection_services_.routes_ = &routes_value;
    }
    void bind_routes(route_table&&) = delete;
    void bind_route_rate_limiter(detail::rate_limiter& limiter) noexcept {
        connection_services_.route_rate_limiter_ = &limiter;
    }

    [[nodiscard]] constexpr const context_request_body_source& request_body_source() const noexcept {
        return request_services_.body_source_;
    }

    // Bind on a request-local copy; the worker and connection views stay shared.
    void bind_streaming_request_body(body_reader& value) noexcept {
        streaming_access::bind_worker(value, worker());
        request_services_.body_source_ = context_request_body_source::streaming(value);
    }
    void bind_lazy_request_body(request_body_loader& value) noexcept {
        request_services_.body_source_ = context_request_body_source::lazy(value);
    }

    [[nodiscard]] const http_request_trailers* request_trailers() const noexcept {
        return request_services_.trailers_;
    }

    // Bound by whichever layer owns the request body, alongside its source.
    void bind_request_trailers(const http_request_trailers& trailers) noexcept {
        request_services_.trailers_ = &trailers;
    }
    void bind_request_trailers(http_request_trailers&&) = delete;

    [[nodiscard]] http_interim_response_output* interim_output() const noexcept {
        return request_services_.channels_.interim_output_;
    }

    [[nodiscard]] http_connection_advertisement_output* connection_advertisements() const noexcept {
        return request_services_.channels_.connection_advertisements_;
    }

    [[nodiscard]] http_push_output* push_output() const noexcept {
        return request_services_.channels_.push_output_;
    }

    [[nodiscard]] const std::optional<http_priority>* request_priority_update() const noexcept {
        return request_services_.channels_.priority_update_;
    }

    void bind_request_channels(const context_request_channels& channels) noexcept {
        request_services_.channels_ = channels;
    }

    void bind_interim_output(http_interim_response_output& output) noexcept {
        request_services_.channels_.interim_output_ = &output;
    }

    [[nodiscard]] constexpr context_response_output& response_output() noexcept {
        return request_services_.response_output_;
    }

    [[nodiscard]] constexpr const context_response_output& response_output() const noexcept {
        return request_services_.response_output_;
    }

    void bind_response_stream(response_stream_writer& value) noexcept {
        streaming_access::bind_worker(value, worker());
        request_services_.response_output_ = context_response_output::response_stream(value);
    }

    [[nodiscard]] std::size_t dispatch_depth() const noexcept {
        return request_services_.dispatch_depth_;
    }

    // Subrequests inherit the parent connection identity, one dispatch level deeper.
    [[nodiscard]] context_services for_subrequest(const stop_token& stop_token_value) const {
        context_services services(*worker_services_, stop_token_value);
        services.connection_services_.info_ = connection_services_.info_;
        services.connection_services_.inbound_buffers_ = connection_services_.inbound_buffers_;
        services.connection_services_.routes_ = connection_services_.routes_;
        services.connection_services_.route_rate_limiter_ = connection_services_.route_rate_limiter_;
        services.connection_services_.lifecycle_ = connection_services_.lifecycle_;
        services.connection_services_.router_clients_ = connection_services_.router_clients_;
        services.connection_services_.router_states_ = connection_services_.router_states_;
        services.request_services_.dispatch_depth_ = request_services_.dispatch_depth_ + 1;
        services.request_services_.io_metrics_ = request_services_.io_metrics_;
        services.request_services_.early_data_ = request_services_.early_data_;
        services.request_services_.pinned_arrival_ = request_services_.pinned_arrival_;
        return services;
    }
    context_services for_subrequest(stop_token&&) const = delete;

    // Server-owned static-file policy. When response compression is enabled,
    // static_file() may negotiate an indexed precompressed variant. It never
    // performs request-time file compression.
    [[nodiscard]] constexpr bool precompressed_static_files() const noexcept {
        return worker_services_->precompressed_static_files_;
    }

    [[nodiscard]] constexpr std::optional<timeout_duration> file_io_timeout() const noexcept {
        return worker_services_->file_io_timeout_;
    }

    [[nodiscard]] const connection_failure_sink& connection_failure() const noexcept {
        return *worker_services_->connection_failure_;
    }

    // The request's Accept-Encoding selection as admission folded it, or
    // null for a context created outside admission. A failed negotiation is a
    // selection whose acceptable() is false.
    [[nodiscard]] const http_response_coding_selection* response_coding() const noexcept {
        return request_services_.response_coding_ ? &*request_services_.response_coding_ : nullptr;
    }

    void bind_response_coding(const http_response_coding_selection& selection) noexcept {
        request_services_.response_coding_ = selection;
    }

    [[nodiscard]] const std::optional<std::chrono::steady_clock::time_point>& pinned_arrival() const noexcept {
        return request_services_.pinned_arrival_;
    }

    void pin_arrival(std::chrono::steady_clock::time_point arrival) noexcept {
        request_services_.pinned_arrival_ = arrival;
    }

    [[nodiscard]] const worker_state_registry* worker_states() const noexcept {
        return connection_services_.router_states_ != nullptr ? connection_services_.router_states_ : worker_services_->states_;
    }

    [[nodiscard]] constexpr const conn_info& get_conn_info() const noexcept {
        return connection_services_.info_;
    }

    // The connection metadata one request sees. Identical to get_conn_info() unless
    // the peer is a configured trusted proxy, in which case its forwarding
    // headers name the client. Resolved per request rather than per connection:
    // each keep-alive request may carry different forwarding headers. Admission
    // resolves it once and binds the result with bind_conn_info, so every
    // context created for the request reads get_conn_info().
    [[nodiscard]] conn_info resolve_conn_info(const http_request& request) const noexcept;

    void bind_conn_info(const conn_info& resolved) noexcept {
        connection_services_.info_ = resolved;
    }

    // The RFC 9110 §7.4 facts of the transport connection, never of a
    // forwarded hop: whether its peer is a trusted gateway, and the
    // certificate it presented (null for cleartext and for subrequests).
    [[nodiscard]] constexpr bool peer_trusted() const noexcept {
        return connection_services_.peer_trusted_;
    }

    [[nodiscard]] constexpr tls_served_certificate* served_certificate() const noexcept {
        return connection_services_.served_certificate_;
    }
    [[nodiscard]] connection_lifecycle* connection_lifecycle_value() const noexcept {
        return connection_services_.lifecycle_;
    }
    void note_connection_request() const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->note_request();
        }
    }
    void set_connection_close_reason(connection_close_reason reason) const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->set_reason(reason);
        }
    }
    [[nodiscard]] std::shared_ptr<void> connection_state() const noexcept {
        const auto* lifecycle = connection_services_.lifecycle_;
        return lifecycle == nullptr ? std::shared_ptr<void>{} : lifecycle->state();
    }
    void abort_connection() const noexcept {
        if (auto* lifecycle = connection_services_.lifecycle_) {
            lifecycle->abort();
        }
    }
    [[nodiscard]] bool connection_aborted() const noexcept {
        const auto* lifecycle = connection_services_.lifecycle_;
        return lifecycle != nullptr && lifecycle->aborted();
    }
    [[nodiscard]] std::string_view remote_address() const noexcept {
        return connection_services_.info_.remote_text_;
    }

    [[nodiscard]] context_services with_connection_lifecycle(connection_lifecycle& lifecycle) const noexcept {
        auto services = *this;
        services.connection_services_.lifecycle_ = &lifecycle;
        services.connection_services_.info_.local_ = lifecycle.local_endpoint();
        services.connection_services_.info_.remote_ = lifecycle.remote_endpoint();
        if (!services.connection_services_.info_.via_trusted_proxy_) {
            services.connection_services_.info_.client_ = lifecycle.remote_endpoint();
        }
        return services;
    }

    // Views borrow connection-owned storage and remain valid for every context
    // created while that connection is dispatched. `connection_id` is the
    // worker's conn_info::connection_id() for the connection, zero without one.
    [[nodiscard]] context_services with_plain_transport(std::uint64_t connection_id, std::string_view remote_address, std::uint16_t remote_port) const noexcept {
        auto services = *this;
        services.connection_services_.info_ = conn_info::plain(connection_id, remote_address);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ = trusts_peer(services.connection_services_.info_.remote_.address());
        services.connection_services_.served_certificate_ = nullptr;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_plain_transport(std::uint64_t, std::basic_string<char, traits_type, allocator_type>&&, std::uint16_t) const = delete;

    // `alpn` and `server_name` borrow the connection's SSL, which outlives it.
    [[nodiscard]] context_services with_tls_transport(std::uint64_t connection_id, std::string_view remote_address, std::optional<std::string_view> client_certificate_subject, std::string_view alpn, std::string_view server_name, std::uint16_t remote_port, tls_served_certificate& served_certificate) const noexcept {
        auto services = *this;
        services.connection_services_.info_ = conn_info::tls(connection_id, remote_address, client_certificate_subject, alpn, server_name);
        services.connection_services_.info_.set_remote_port(remote_port);
        services.connection_services_.peer_trusted_ = trusts_peer(services.connection_services_.info_.remote_.address());
        services.connection_services_.served_certificate_ = &served_certificate;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(std::uint64_t, std::basic_string<char, traits_type, allocator_type>&&, std::optional<std::string_view>, std::string_view, std::string_view, std::uint16_t, tls_served_certificate&) const = delete;

    template <typename traits_type, typename allocator_type>
    context_services with_tls_transport(std::uint64_t, std::string_view, std::basic_string<char, traits_type, allocator_type>&&, std::string_view, std::string_view, std::uint16_t, tls_served_certificate&) const = delete;

    [[nodiscard]] constexpr std::string_view alternative_service() const noexcept {
        return connection_services_.alternative_service_;
    }

    // Empty unless the connection's listener is a listener_dispatch::request_stream one.
    [[nodiscard]] constexpr request_stream_callback_ref request_stream() const noexcept {
        return connection_services_.request_stream_;
    }

    [[nodiscard]] context_services with_request_stream(request_stream_callback_ref callback) const noexcept {
        auto services = *this;
        services.connection_services_.request_stream_ = callback;
        return services;
    }

    // `value` borrows listener-owned storage that outlives the connection.
    [[nodiscard]] context_services with_alternative_service(std::string_view value) const noexcept {
        auto services = *this;
        services.connection_services_.alternative_service_ = value;
        return services;
    }

    template <typename traits_type, typename allocator_type>
    context_services with_alternative_service(std::basic_string<char, traits_type, allocator_type>&&) const = delete;

private:
    [[nodiscard]] bool trusts_peer(const ip_address& remote_address) const noexcept {
        return worker_services_->trusted_proxies_ != nullptr && worker_services_->trusted_proxies_->is_trusted_proxy(remote_address);
    }

    const context_worker_services* worker_services_;
    context_connection_services connection_services_{.info_ = conn_info::plain(0, {})};
    context_request_services request_services_;
};

// Adds the listener's Alt-Svc advertisement (RFC 7838 §3) to a final
// response. An application that set its own Alt-Svc field keeps it: the
// advertisement is a default, never a second, conflicting field. `value`
// borrows listener storage that outlives every response of the connection.
inline void apply_alternative_service(http_response& response, std::string_view value) {
    if (!value.empty() && !response.header("Alt-Svc").has_value()) {
        response.header_stable_view("Alt-Svc", value);
    }
}

}  // namespace ruvia::detail

namespace ruvia {
class session_middleware;
}

namespace ruvia::detail {

struct session_untouched final {};

struct session_unrecognized final {
    std::string id_;
};

struct session_loaded final {
    std::string id_;
    std::string payload_;
};

struct session_persist_new final {
    std::string payload_;
};

struct session_rotate final {
    std::string old_id_;
    std::string payload_;
};

struct session_clear final {
    std::optional<std::string> old_id_;
};

class context_session_state final {
public:
    context_session_state() noexcept = default;

    void bind(const session_middleware* owner);
    [[nodiscard]] bool available() const noexcept {
        return available_;
    }
    [[nodiscard]] const session_middleware* owner() const noexcept {
        return owner_;
    }
    // The first commit freezes all mutation before any asynchronous storage I/O.
    // A failed commit is terminal: its external writes cannot safely be replayed.
    [[nodiscard]] bool begin_commit();
    void finish_commit() noexcept;
    void fail_commit(std::exception_ptr exception) noexcept;
    void observe_presented_id(std::string_view id);
    void load_recognized(std::string_view payload);
    void set(std::string_view payload);
    void clear();
    void regenerate();

    [[nodiscard]] std::string_view payload() const& noexcept;
    [[nodiscard]] std::string_view payload() const&& = delete;

    [[nodiscard]] const session_untouched* untouched() const& noexcept {
        return std::get_if<session_untouched>(&value_);
    }
    [[nodiscard]] const session_untouched* untouched() const&& = delete;

    [[nodiscard]] const session_unrecognized* unrecognized() const& noexcept {
        return std::get_if<session_unrecognized>(&value_);
    }
    [[nodiscard]] const session_unrecognized* unrecognized() const&& = delete;

    [[nodiscard]] const session_loaded* loaded() const& noexcept {
        return std::get_if<session_loaded>(&value_);
    }
    [[nodiscard]] const session_loaded* loaded() const&& = delete;

    [[nodiscard]] const session_persist_new* persist_new() const& noexcept {
        return std::get_if<session_persist_new>(&value_);
    }
    [[nodiscard]] const session_persist_new* persist_new() const&& = delete;

    [[nodiscard]] const session_rotate* rotate() const& noexcept {
        return std::get_if<session_rotate>(&value_);
    }
    [[nodiscard]] const session_rotate* rotate() const&& = delete;

    [[nodiscard]] const session_clear* cleared() const& noexcept {
        return std::get_if<session_clear>(&value_);
    }
    [[nodiscard]] const session_clear* cleared() const&& = delete;

private:
    enum class phase_type : unsigned char { active,
        committing,
        committed,
        failed };
    void require_mutable() const;

    bool available_{false};
    const session_middleware* owner_{nullptr};
    phase_type phase_{phase_type::active};
    std::exception_ptr failure_;
    std::variant<session_untouched, session_unrecognized, session_loaded, session_persist_new, session_rotate, session_clear> value_;
};

// Privileged access to a context's session slot, used by the session middleware
// to load the stored blob and read what the handler left behind.
struct session_access final {
    static void bind(context& context_value, const session_middleware* owner) {
        context_value.session_state().bind(owner);
    }

    [[nodiscard]] static bool available(const context& context_value) noexcept {
        return context_value.session_state().available();
    }

    // No callback chain: the bound session_middleware owns this capability's
    // single response-commit stage for both buffered and long-lived routes.
    [[nodiscard]] static task<void> commit(context& context);

    static void observe_presented_id(context& context_value, std::string_view id) {
        context_value.session_state().observe_presented_id(id);
    }

    static void load(context& context_value, std::string_view data) {
        context_value.session_state().load_recognized(data);
    }

    [[nodiscard]] static const context_session_state& state(const context& context_value) noexcept {
        return context_value.session_state();
    }
};

inline constexpr std::string_view session_rotation_script =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "if not redis.call('SET', KEYS[2], ARGV[1], 'EX', ARGV[2], 'NX') then return 0 end "
    "redis.call('DEL', KEYS[1]) return 1";

[[nodiscard]] inline bool is_valid_session_id(std::string_view id) noexcept {
    if (id.size() != secure_token_size) {
        return false;
    }
    for (const char ch : id) {
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

inline void set_session_cookie(http_response_headers& headers, std::string_view cookie_name, std::string_view value, const cookie_options& options) {
    headers.set_cookie(set_cookie_plan(cookie_name, value, options));
}

inline void append_session_cookie_header(http_response_headers& headers, std::string_view cookie_name, std::string_view id, bool secure) {
    set_session_cookie(headers, cookie_name, id,
        {
            .same_site_ = cookie_same_site::lax,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

inline void append_expired_session_cookie_header(http_response_headers& headers, std::string_view cookie_name, bool secure) {
    set_session_cookie(headers, cookie_name, "",
        {
            .same_site_ = cookie_same_site::lax,
            .expires_ = cookie_deletion_expires,
            .http_only_ = cookie_attribute_policy::emit,
            .secure_ = secure ? cookie_attribute_policy::emit : cookie_attribute_policy::omit,
        });
}

class request_body_loader final {
public:
    // `operation` bounds the wait for body bytes; an interrupted call keeps what
    // arrived so far and a later call resumes it.
    using read_all_type = task<std::string_view> (*)(void*, operation_options);
    using discard_type = task<void> (*)(void*, operation_options);

    constexpr request_body_loader(void* target, read_all_type read_all, discard_type discard) noexcept :
        target_(target),
        read_all_(read_all),
        discard_(discard) {}

    request_body_loader(const request_body_loader&) = delete;
    request_body_loader& operator=(const request_body_loader&) = delete;

    [[nodiscard]] task<std::string_view> read_all(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
                co_return buffered_body_;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::discarded:
                throw std::logic_error("request body was discarded");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            buffered_body_ = co_await read_all_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::buffered;
        co_return buffered_body_;
    }

    task<void> discard(operation_options operation) {
        switch (state_) {
            case state_type::available:
                state_ = state_type::consuming;
                break;
            case state_type::buffered:
            case state_type::discarded:
                co_return;
            case state_type::consuming:
                throw std::logic_error("request body consumption is already in progress");
            case state_type::failed:
                throw std::logic_error("request body consumption previously failed");
        }
        const operation_steps steps(operation);
        try {
            co_await discard_(target_, std::move(operation));
        } catch (...) {
            settle_failure(steps);
            throw;
        }
        state_ = state_type::discarded;
    }

private:
    enum class state_type : std::uint8_t {
        available,
        consuming,
        buffered,
        discarded,
        failed,
    };

    // Called while handling a consumption failure: an interruption by the
    // operation's own bound returns the body to `available`, so a later call
    // resumes it; any other failure is final.
    void settle_failure(const operation_steps& steps) noexcept {
        state_ = state_type::failed;
        try {
            throw;
        } catch (const std::system_error& error) {
            if (steps.interrupted(error)) {
                state_ = state_type::available;
            }
        } catch (...) {
        }
    }

    void* target_;
    read_all_type read_all_;
    discard_type discard_;
    std::string_view buffered_body_;
    state_type state_{state_type::available};
};

template <typename reader_type>
[[nodiscard]] task<std::optional<std::span<const std::byte>>> body_reader_read_thunk(void* target, operation_options operation) {
    return static_cast<reader_type*>(target)->read(std::move(operation));
}

template <typename reader_type>
[[nodiscard]] body_reader make_body_reader_facade(reader_type& reader_value) noexcept {
    return streaming_access::make_body_reader(&reader_value, &body_reader_read_thunk<reader_type>);
}

template <typename reader_type>
class body_reader_binding final {
public:
    template <typename... args_type>
    explicit body_reader_binding(args_type&&... args) :
        reader_(std::forward<args_type>(args)...),
        facade_(make_body_reader_facade(reader_)) {}

    body_reader_binding(const body_reader_binding&) = delete;
    body_reader_binding& operator=(const body_reader_binding&) = delete;
    body_reader_binding(body_reader_binding&&) = delete;
    body_reader_binding& operator=(body_reader_binding&&) = delete;

    [[nodiscard]] reader_type& reader() noexcept {
        return reader_;
    }
    [[nodiscard]] body_reader& facade() noexcept {
        return facade_;
    }

private:
    reader_type reader_;
    body_reader facade_;
};

template <typename loader_type>
[[nodiscard]] task<std::string_view> request_body_loader_read_all_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->read_all(std::move(operation));
}

template <typename loader_type>
task<void> request_body_loader_discard_thunk(void* target, operation_options operation) {
    return static_cast<loader_type*>(target)->discard(std::move(operation));
}

template <typename loader_type>
[[nodiscard]] request_body_loader make_request_body_loader_facade(loader_type& loader) noexcept {
    return request_body_loader(&loader, &request_body_loader_read_all_thunk<loader_type>, &request_body_loader_discard_thunk<loader_type>);
}

template <typename loader_type>
class request_body_loader_binding final {
public:
    template <typename... args_type>
    explicit request_body_loader_binding(args_type&&... args) :
        loader_(std::forward<args_type>(args)...),
        facade_(make_request_body_loader_facade(loader_)) {}

    request_body_loader_binding(const request_body_loader_binding&) = delete;
    request_body_loader_binding& operator=(const request_body_loader_binding&) = delete;
    request_body_loader_binding(request_body_loader_binding&&) = delete;
    request_body_loader_binding& operator=(request_body_loader_binding&&) = delete;

    [[nodiscard]] loader_type& loader() noexcept {
        return loader_;
    }
    [[nodiscard]] const loader_type& loader() const noexcept {
        return loader_;
    }
    [[nodiscard]] request_body_loader& facade() noexcept {
        return facade_;
    }

private:
    loader_type loader_;
    request_body_loader facade_;
};

}  // namespace ruvia::detail

namespace ruvia {
class context;
}

namespace ruvia::detail {

// One request's typed interim output lane. The protocol driver outlives this
// object; operations own their fields until completion.
class http_interim_response_output final {
public:
    using write_type = task<void> (*)(void*, const http_interim_response_head&);
    http_interim_response_output(void* target, write_type write) noexcept :
        target_(target),
        write_(write) {}

    // An interrupted inform() resets the request of `context_value`.
    [[nodiscard]] task<void> inform(const http_interim_response_head& response, operation_options operation, context& context_value);
    // Answers a 100-continue expectation before the body is read, on the same
    // lane as inform() so the two heads never interleave. A final response
    // that has started already answered the expectation, and RFC 9110 §15.2
    // forbids any 1xx after it, so then nothing is written.
    task<void> send_continue();
    void commit_final() {
        if (output_active_) {
            throw std::logic_error("interim response output is still active");
        }
        final_committed_ = true;
    }

private:
    [[nodiscard]] task<void> write_owned(http_status_code status, std::vector<http_header> fields, operation_options operation, context& context_value);
    void* target_;
    write_type write_;
    bool final_committed_{};
    bool output_active_{};
    operation_interrupt_source interrupts_;
    ::ruvia::operation_scope scope_;
};
// A lazily built index for queries(name). Values stay in request order within
// each name; groups are sorted only for lookup and are never exposed publicly.
class request_query_values final {
public:
    explicit request_query_values(const request_name_value_list& fields) {
        if (fields.empty()) {
            return;
        }
        static constexpr std::size_t inline_order_capacity = 32;
        std::array<std::size_t, inline_order_capacity> inline_order;
        std::vector<std::size_t> heap_order;
        std::span<std::size_t> order;
        if (fields.size() <= inline_order_capacity) {
            order = std::span<std::size_t>(inline_order.data(), fields.size());
        } else {
            heap_order.resize(fields.size());
            order = heap_order;
        }
        for (std::size_t i = 0; i < fields.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
            const auto left_name = fields[left].name();
            const auto right_name = fields[right].name();
            return left_name == right_name ? left < right : left_name < right_name;
        });
        std::size_t group_count = 1;
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (fields[order[i]].name() != fields[order[i - 1]].name()) {
                ++group_count;
            }
        }
        groups_.reserve(group_count);
        values_.reserve(fields.size());
        for (const auto index : order) {
            const auto name = fields[index].name();
            if (groups_.empty() || groups_.back().name_ != name) {
                groups_.push_back({name, values_.size(), 0});
            }
            values_.push_back(fields[index].value());
            ++groups_.back().count_;
        }
    }

    [[nodiscard]] std::span<const std::string_view> values(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND {
        const auto group = std::ranges::lower_bound(groups_, name, {}, &group_type::name_);
        if (group == groups_.end() || group->name_ != name) {
            return {};
        }
        return std::span<const std::string_view>(values_).subspan(group->begin_, group->count_);
    }
    std::span<const std::string_view> values(std::string_view) const&& = delete;

private:
    struct group_type final {
        std::string_view name_;
        std::size_t begin_;
        std::size_t count_;
    };
    std::vector<group_type> groups_;
    std::vector<std::string_view> values_;
};

// Field views borrow the request query string or decoded_bytes_, so neither
// moves once published.
class request_query_cache final {
public:
    request_query_cache() noexcept :
        fields_(request_name_value_list_access::make()) {}

    [[nodiscard]] std::string& decoded_bytes() noexcept {
        return decoded_bytes_;
    }
    [[nodiscard]] request_name_value_list& mutable_fields() noexcept {
        return fields_;
    }

    [[nodiscard]] const request_name_value_list& fields() const& noexcept {
        return fields_;
    }
    const request_name_value_list& fields() const&& = delete;

    [[nodiscard]] const request_query_values& values() & {
        if (!values_) {
            values_.emplace(fields_);
        }
        return *values_;
    }
    const request_query_values& values() && = delete;

private:
    std::string decoded_bytes_;
    request_name_value_list fields_;
    std::optional<request_query_values> values_;
};

// Draft and selected response have disjoint ownership. Building a response
// consumes the draft; selecting or replacing a response never merges metadata.
class context_response_state final {
public:
    context_response_state() noexcept = default;

    [[nodiscard]] bool has_draft() const noexcept {
        return draft_.has_value();
    }
    [[nodiscard]] http_status_code draft_status() const noexcept {
        return draft_ ? draft_->status() : http_status::ok;
    }
    void reset_draft() noexcept {
        draft_.reset();
    }
    [[nodiscard]] http_response take_draft_or_new() noexcept {
        if (!draft_) {
            return http_response{};
        }
        auto response = std::move(*draft_);
        draft_.reset();
        return response;
    }
    [[nodiscard]] http_response& draft() {
        if (!draft_) {
            draft_.emplace();
        }
        return *draft_;
    }
    [[nodiscard]] http_response* final() noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] const http_response* final() const noexcept {
        return final_ ? &*final_ : nullptr;
    }
    [[nodiscard]] http_response& policy() {
        if (!policy_) {
            policy_.emplace();
        }
        return *policy_;
    }
    [[nodiscard]] http_response& defaults() {
        if (!defaults_) {
            defaults_.emplace();
        }
        return *defaults_;
    }
    void policy_status(http_status_code status) {
        policy_status_ = status;
    }
    void apply_policy(http_response& response) const {
        if (defaults_ && !defaults_->headers().empty()) {
            response.transfer_headers_from(*defaults_, http_response_header_transfer::merge);
        }
        if (policy_ && !policy_->headers().empty()) {
            response.transfer_headers_from(*policy_, http_response_header_transfer::apply);
        }
        if (policy_status_) {
            response.status(*policy_status_);
        }
    }

    void finalize(http_response&& response) {
        final_.emplace(std::move(response));
    }

    [[nodiscard]] http_response&& release_final() & {
        assert(final_);
        apply_policy(*final_);
        return std::move(*final_);
    }
    http_response&& release_final() && = delete;

private:
    std::optional<http_response> draft_;
    std::optional<http_response> policy_;
    std::optional<http_response> defaults_;
    std::optional<http_status_code> policy_status_;
    std::optional<http_response> final_;
};

struct aligned_storage_deleter final {
    std::size_t alignment_{alignof(std::max_align_t)};
    void operator()(std::byte* storage) const noexcept {
        ::operator delete[](storage, std::align_val_t{alignment_});
    }
};

// Owns every request-local context value that is not a borrowed pointer: lazy
// caches, response/session machines, typed bindings, and middleware next() hop
// storage. Synchronous construction borrows the input view, then retains one
// services value here before returning; no reference to the input survives.
class context_request_storage final {
public:
    explicit context_request_storage(const context_services& service_view) noexcept :
        services_(service_view),
        decoded_body_reservation_(services_.inbound_buffers()) {}

    context_request_storage(const context_request_storage&) = delete;
    context_request_storage& operator=(const context_request_storage&) = delete;

    [[nodiscard]] std::byte* allocate_raw(std::size_t size, std::size_t alignment) {
        auto* storage = static_cast<std::byte*>(::operator new[](size, std::align_val_t{alignment}));
        try {
            owned_blocks_.emplace_back(storage, aligned_storage_deleter{alignment});
        } catch (...) {
            ::operator delete[](storage, std::align_val_t{alignment});
            throw;
        }
        return storage;
    }

    context_services services_;
    context_response_state response_state_;
    context_session_state session_state_;
    request_bindings request_bindings_;

    std::optional<std::string> decoded_body_;
    inbound_buffer_reservation decoded_body_reservation_;
    std::optional<request_name_value_list> headers_;
    std::optional<request_query_cache> query_;
    std::optional<request_name_value_list> cookies_;
    std::string decoded_route_params_;
    // Views borrow route storage, the request path, or decoded_route_params_.
    std::optional<request_name_value_list> route_params_;
    std::vector<std::unique_ptr<std::byte[], aligned_storage_deleter>> owned_blocks_;
    bool route_params_invalid_{false};
    bool route_params_unencoded_{false};
};

class context_websocket_binding;
class context_tunnel_binding;

struct context_access final {
    // An interrupted output operation cannot withdraw what it wrote, so it
    // resets the request: the HTTP/2 or HTTP/3 stream, or the HTTP/1 connection.
    static void reset_interrupted_request(void* raw_context) noexcept {
        auto& request_services = static_cast<context*>(raw_context)->services();
        try {
            request_services.reset_request({});
        } catch (...) {
            request_services.abort_connection();
        }
    }

    // A request-stream router borrows the already-bound transport context.
    // Restore every borrowed route view before its resolution frame retires;
    // retain the output state, since a stream may have committed in that scope.
    class route_binding final {
    public:
        route_binding(context& value, std::string_view path, std::span<const std::string_view> names, std::span<const std::string_view> values, std::uintptr_t scope) :
            context_(value),
            services_(value.services()),
            path_(value.route_path_),
            names_(value.param_names_),
            values_(value.param_values_),
            count_(value.param_count_),
            scope_(value.route_rate_limit_scope_),
            worker_states_(value.capabilities_.worker_states()) {
            value.route_path_ = path;
            value.param_names_ = names.data();
            value.param_values_ = values.data();
            value.param_count_ = values.size();
            value.route_rate_limit_scope_ = scope;
        }
        route_binding(const route_binding&) = delete;
        route_binding& operator=(const route_binding&) = delete;
        ~route_binding() {
            const auto output = context_.services().response_output();
            context_.services() = services_;
            context_.services().response_output() = output;
            context_.route_path_ = path_;
            context_.param_names_ = names_;
            context_.param_values_ = values_;
            context_.param_count_ = count_;
            context_.route_rate_limit_scope_ = scope_;
            context_.capabilities_.bind_worker_states(worker_states_);
        }

        // Points the context's worker-state lookups at the registry its
        // services now name; the destructor restores the previous one.
        void bind_worker_states() noexcept {
            context_.capabilities_.bind_worker_states(context_.services().worker_states());
        }

    private:
        context& context_;
        context_services services_;
        std::string_view path_;
        const std::string_view* names_;
        const std::string_view* values_;
        std::size_t count_;
        std::uintptr_t scope_;
        const worker_state_registry* worker_states_;
    };

    [[nodiscard]] static context make(const http_request& request, const context_services& services) {
        return context(request, services);
    }

    [[nodiscard]] static context make(const http_request& request, std::string_view route_path, const std::string_view* param_names, const std::string_view* param_values, std::size_t param_count, std::uintptr_t route_rate_limit_scope, const context_services& services) {
        return context(request, route_path, param_names, param_values, param_count, route_rate_limit_scope, services);
    }

    [[nodiscard]] static std::byte* allocate_request_storage(context& context_value, std::size_t size, std::size_t alignment) {
        return context_value.request_storage().allocate_raw(size, alignment);
    }

    // The response for an indexed entry already resolved below `root`. The
    // representation is selected and the content type copied before the task
    // is returned, so `relative` and `content_type_value` are read only by
    // this call; `root` and `base_entry` (a view into it) must outlive the
    // task, which opens a file-backed entry on the blocking pool.
    [[nodiscard]] static task<http_response> static_file_resolved(context& context_value, const static_root& root, std::string_view relative, std::string_view content_type_value, static_root_entry_view base_entry, static_file_selection_mode mode);

    [[nodiscard]] static http_response make_static_json_response(context& context_value, std::string_view json_body) {
        auto response = context_value.response_state().take_draft_or_new();
        if (!response.header("Content-Type")) {
            response.content_type(http_content_type::application_json);
        }
        response.static_body(json_body);
        return response;
    }

    [[nodiscard]] static http_response make_owned_json_response(context& context_value, std::string& json_body) {
        return context_value.json_serialized(json_body);
    }

    [[nodiscard]] static http_interim_response_output* interim_output(context& context_value) noexcept {
        return context_value.services().interim_output();
    }

    [[nodiscard]] static const http_request& request(const context& context_value) noexcept {
        return context_value.request_;
    }

    [[nodiscard]] static const context_request_body_source& request_body_source(const context& context_value) noexcept {
        return context_value.request_body_source();
    }

    [[nodiscard]] static context_services& services(context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static const context_services& services(const context& context_value) noexcept {
        return context_value.services();
    }

    [[nodiscard]] static std::optional<std::string_view> decoded_body(const context& context_value) noexcept {
        if (!context_value.body_decoded_) {
            return std::nullopt;
        }
        return std::string_view(*context_value.request_storage_->decoded_body_);
    }

    [[nodiscard]] static bool body_identity(const context& context_value) noexcept {
        return context_value.body_identity_;
    }

    static void mark_body_identity(const context& context_value) noexcept {
        context_value.body_identity_ = true;
    }

    [[nodiscard]] static std::string_view store_decoded_body(const context& context_value, std::string&& decoded_content) {
        auto& storage = context_value.request_storage();
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded_content));
        auto& decoded = context_value.decoded_body();
        decoded = std::move(decoded_content);
        storage.decoded_body_reservation_.resize_to(inbound_string_heap_bytes(decoded));
        context_value.body_decoded_ = true;
        return std::string_view(decoded);
    }

    [[nodiscard]] static detail::rate_limiter* rate_limiter(context& context_value) noexcept {
        return context_value.services().rate_limiter();
    }

    [[nodiscard]] static const std::optional<std::chrono::steady_clock::time_point>& rate_limit_arrival(context& context_value) noexcept {
        return context_value.services().pinned_arrival();
    }

    [[nodiscard]] static std::uintptr_t route_rate_limit_scope(const context& context_value) noexcept {
        return context_value.route_rate_limit_scope_;
    }

    static void set_response(context& context_value, http_response&& response) {
        context_value.store_response(std::move(response));
    }

    [[nodiscard]] static const http_response_headers& handshake_headers(context& context_value) {
        auto& state = context_value.response_state();
        state.apply_policy(state.draft());
        return state.draft().headers();
    }
    [[nodiscard]] static http_response& response_policy(context& context_value) {
        return context_value.response_state().policy();
    }
    static void discard_draft(context& context_value) {
        context_value.response_state().reset_draft();
    }
    static void apply_response_policy(context& context_value, http_response& response) {
        context_value.response_state().apply_policy(response);
    }
    static void security_header(context& context_value, std::string_view name, std::string_view value, bool replace) {
        auto& state = context_value.response_state();
        auto& response = replace ? state.policy() : state.defaults();
        response.header_stable_view(name, value);
    }

    // The status the handler set; a file response sends it for the full body.
    [[nodiscard]] static http_status_code response_status(const context& context_value) noexcept {
        return context_value.response_state().draft_status();
    }

    static void apply_response_state(context& context_value, http_response& response, std::optional<http_status_code> status_code) {
        context_value.apply_response_state(response, status_code);
    }

    // Called by a driver right before it hands the upgrade/CONNECT handshake
    // head to the transport: from here on no HTTP response can follow, and no
    // 1xx head either -- the stream now carries the upgraded protocol. Throws,
    // before anything is written, while an interim head is still being written.
    static void commit_upgrade(context& context_value) {
        if (auto* interim = context_value.services().interim_output()) {
            interim->commit_final();
        }
        context_value.response_output() = context_response_output::upgraded();
    }

    [[nodiscard]] static bool output_committed(const context& context_value) noexcept {
        return context_value.response_output().committed();
    }

    static void set_error(context& context_value, std::exception_ptr exception) noexcept {
        context_value.store_error(std::move(exception));
    }

    [[nodiscard]] static bool has_response(const context& context_value) noexcept {
        return context_value.has_response();
    }

    // The final response, for the dispatcher to return; see
    // context_response_state::release_final.
    [[nodiscard]] static http_response&& release_response(context& context_value) {
        return context_value.response_state().release_final();
    }

    [[nodiscard]] static http_response streaming_head(context& context_value) {
        return context_value.streaming_head();
    }

    // Commits the bound session, then builds the head a response stream sends before its first byte.
    [[nodiscard]] static task<http_response> commit_streaming_head(context& context_value);

private:
    friend class context_websocket_binding;
    friend class context_tunnel_binding;
    [[nodiscard]] static context_response_output bind_tunnel(context& context_value, http_tunnel& tunnel) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::tunnel(tunnel);
        return previous;
    }

    [[nodiscard]] static context_response_output bind_websocket(context& context_value, websocket& websocket_value) noexcept {
        auto previous = context_value.response_output();
        context_value.response_output() = context_response_output::websocket_value(websocket_value);
        return previous;
    }

    static void restore_response_output(context& context_value, context_response_output output) noexcept {
        context_value.response_output() = output;
    }
};

class context_tunnel_binding final {
public:
    context_tunnel_binding(context& context_value, http_tunnel& tunnel) noexcept :
        context_(context_value),
        previous_(context_access::bind_tunnel(context_value, tunnel)) {}
    context_tunnel_binding(const context_tunnel_binding&) = delete;
    context_tunnel_binding& operator=(const context_tunnel_binding&) = delete;
    ~context_tunnel_binding() {
        context_access::restore_response_output(context_, previous_);
    }

private:
    context& context_;
    context_response_output previous_;
};

// The facade borrowed by context is valid only while the established session
// owns its connection. Restoring the previous output capability on every exit
// prevents onion middleware post-processing from observing a dangling facade.
class context_websocket_binding final {
public:
    context_websocket_binding(context& context_value, websocket& websocket_value) noexcept :
        context_(&context_value),
        previous_(context_access::bind_websocket(context_value, websocket_value)) {}

    context_websocket_binding(const context_websocket_binding&) = delete;
    context_websocket_binding& operator=(const context_websocket_binding&) = delete;
    context_websocket_binding(context_websocket_binding&&) = delete;
    context_websocket_binding& operator=(context_websocket_binding&&) = delete;

    ~context_websocket_binding() {
        context_access::restore_response_output(*context_, previous_);
    }

private:
    context* context_;
    context_response_output previous_;
};

[[nodiscard]] http_response make_default_error_response(http_error_info error);

// The default 404 answer for a request no route matched.
[[nodiscard]] http_response make_route_not_found_response();

[[nodiscard]] task<http_response> invoke_error_handler(context& context_value, http_error_info error, http_error_handler_ref handler);

// Web-runtime signal for an otherwise valid request whose complete
// Content-Encoding stack cannot be decoded. router dispatch owns the 415 JSON
// mapping and the RFC 9110 Accept-Encoding response advertisement, which names
// what the failed read accepts: a streaming read decodes nothing.
class unsupported_request_content_coding final : public std::exception {
public:
    enum class read_type { decoded,
        streaming };

    explicit unsupported_request_content_coding(read_type read) noexcept :
        read_(read) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "request Content-Encoding is not supported";
    }

    // An Accept-Encoding field value with static storage.
    [[nodiscard]] std::string_view accepted_codings() const noexcept {
        return read_ == read_type::decoded ? http_supported_request_content_codings() : "identity";
    }

private:
    read_type read_;
};

}  // namespace ruvia::detail

namespace ruvia {

class static_root;

namespace detail {

// One immutable document-root snapshot. The application's refresher builds it
// once and every worker shares it; reference counting happens only when a
// snapshot is published, adopted, or retired, never per request.
using document_root_snapshot = std::shared_ptr<const static_root>;

// One worker's adopted snapshot plus that worker's request leases on it. Only
// the owning worker touches the counter, so leasing stays a plain increment.
// The worker keeps a superseded generation until its leases drain, which keeps
// the snapshot alive for requests that still borrow it.
class document_root_generation final {
public:
    explicit document_root_generation(document_root_snapshot root) noexcept :
        root_(std::move(root)) {}

    ~document_root_generation() {
        if (leases_ != 0) {
            // A binding still points into this generation. Destroying it would
            // leave the request with a dangling root and counter.
            std::terminate();
        }
    }

    document_root_generation(const document_root_generation&) = delete;
    document_root_generation& operator=(const document_root_generation&) = delete;

    [[nodiscard]] const static_root* root() const noexcept {
        return root_.get();
    }

    [[nodiscard]] bool leased() const noexcept {
        return leases_ != 0;
    }

private:
    friend class document_root_binding;

    document_root_snapshot root_;
    std::size_t leases_{0};
};

// A request-time view of the configured document root. The object is
// deliberately not default-constructible or aggregate-initializable: callers
// choose between no root and a worker's current generation. It is move-only
// because a configured binding is also the request's lease on that generation;
// copying the view would make that ownership boundary ambiguous and could
// reintroduce a dangling static_root after a refresh.
class document_root_binding final {
public:
    [[nodiscard]] static document_root_binding none() noexcept {
        return document_root_binding(nullptr);
    }

    [[nodiscard]] static document_root_binding configured(document_root_generation& generation) noexcept {
        ++generation.leases_;
        return document_root_binding(&generation);
    }

    ~document_root_binding() {
        reset();
    }

    document_root_binding(const document_root_binding&) = delete;
    document_root_binding& operator=(const document_root_binding&) = delete;

    document_root_binding(document_root_binding&& other) noexcept :
        generation_(std::exchange(other.generation_, nullptr)) {}

    document_root_binding& operator=(document_root_binding&& other) noexcept {
        if (this != &other) {
            reset();
            generation_ = std::exchange(other.generation_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] const static_root* root() const noexcept {
        return generation_ == nullptr ? nullptr : generation_->root();
    }

private:
    explicit document_root_binding(document_root_generation* generation) noexcept :
        generation_(generation) {}

    void reset() noexcept {
        auto* const generation = std::exchange(generation_, nullptr);
        if (generation != nullptr) {
            if (generation->leases_ == 0) {
                std::terminate();
            }
            --generation->leases_;
        }
    }

    document_root_generation* generation_;
};

}  // namespace detail
}  // namespace ruvia

namespace ruvia::detail {

using stream_encoder_lease = http_stream_encoder_lease;

class worker_codecs final {
public:
    worker_codecs() noexcept = default;
    ~worker_codecs() = default;
    worker_codecs(const worker_codecs&) = delete;
    worker_codecs& operator=(const worker_codecs&) = delete;

    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding) {
        return content_codecs_.encoder(coding);
    }

    [[nodiscard]] stream_encoder_lease acquire_stream_encoder(http_content_coding coding) {
        return content_codecs_.acquire_stream_encoder(coding);
    }

    [[nodiscard]] websocket_deflate_codecs& websocket_deflate() noexcept {
        return websocket_deflate_;
    }

private:
    http_worker_codecs content_codecs_;
    websocket_deflate_codecs websocket_deflate_;
};

class document_root_publisher;
class inbound_buffer_budget;
class worker_codecs;

struct access_log_sink final {
    access_log_callback_ref callback_;

    void invoke(const access_log_record& record) const noexcept {
        callback_(record);
    }
};

// The one place access-log records are built: from a live request, or from
// fields a pipelined response kept after its request storage was reused.
struct access_log_record_access final {
    using fields_type = access_log_record::fields_type;

    [[nodiscard]] static constexpr access_log_record make(const fields_type& fields) noexcept {
        return access_log_record(fields);
    }

    // Microseconds from `start` to now, clamped at zero.
    [[nodiscard]] static std::uint64_t duration_since(std::chrono::steady_clock::time_point start) noexcept {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        return micros < 0 ? 0 : static_cast<std::uint64_t>(micros);
    }
};

struct connection_failure_record_access final {
    [[nodiscard]] static connection_failure_record make(ip_address remote_address, std::exception_ptr exception) noexcept {
        return connection_failure_record(remote_address, std::move(exception));
    }
};

// The one place server observation records are built.
struct server_observation_access final {
    using tls_client_hello_fields = tls_client_hello_record::fields_type;

    [[nodiscard]] static tls_client_hello_record tls_client_hello(const tls_client_hello_fields& fields) noexcept {
        return tls_client_hello_record(fields);
    }

    [[nodiscard]] static constexpr http2_input_record http2_input(std::uint64_t connection_id, std::span<const std::byte> bytes) noexcept {
        return http2_input_record(connection_id, bytes);
    }
#if defined(RUVIA_ENABLE_HTTP2)
    [[nodiscard]] static constexpr http2_frame_record http2_frame(std::uint64_t connection_id, const http2_frame_metadata& metadata) noexcept {
        return http2_frame_record(connection_id, metadata);
    }
    [[nodiscard]] static constexpr http2_headers_record http2_headers(std::uint64_t connection_id, const http2_header_metadata& metadata) noexcept {
        return http2_headers_record(connection_id, metadata);
    }
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    [[nodiscard]] static constexpr http3_frame_record http3_frame(std::uint64_t connection_id, const http3_frame_metadata& metadata) noexcept {
        return http3_frame_record(connection_id, metadata);
    }
    [[nodiscard]] static constexpr http3_field_record http3_field(std::uint64_t connection_id, const http3_field_metadata& metadata) noexcept {
        return http3_field_record(connection_id, metadata);
    }
#endif
};

// The connection level's failure outlet. An unset callback still reports: the
// failure goes to the shared last-resort reporter rather than being dropped
// with the connection that produced it.
//
// A connection that ends because the peer reset or closed it, or because this
// server cancelled its I/O (stop, drain, or a timeout that closes the socket),
// is a normal end of connection rather than a failure: browsers close SSE
// tabs with a reset, and drain aborts pending reads. Such endings are neither
// reported nor counted, so the sink stays reserved for real faults. Only an
// inbound_transport_error -- thrown by I/O on the connection's own transport,
// possibly propagated through a handler -- qualifies; any other exception is
// reported whatever error code it carries.
struct connection_failure_sink final {
    connection_failure_callback_ref callback_;
    // Owned by the web_worker_runtime this sink was configured for; null before one
    // claims it. Counting here rather than at each reporting site keeps the
    // count and the callback from drifting apart as new sites are added.
    std::atomic<std::size_t>* counter_{nullptr};

    void invoke(ip_address remote_address, std::exception_ptr exception) const noexcept;
    void invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept;
};

struct worker_failure_sink final {
    using invoke_type = void (*)(void*, const std::exception_ptr&) noexcept;

    void* target_{nullptr};
    invoke_type invoke_{nullptr};

    void notify(const std::exception_ptr& failure) const noexcept {
        if (invoke_ != nullptr) {
            invoke_(target_, failure);
        }
    }
};

// Fully normalized, worker-owned runtime options. Public callers configure application
// with http_server.h values; only the Web runtime constructs this state.
struct http_server_options final {
    // The application's document root: one app-scoped publisher shared by
    // every worker, plus this worker's current generation. Validated
    // configuration carries only the publisher; each worker's
    // static_root_runtime publishes its own generation before serving.
    class document_root_state final {
    public:
        document_root_state() noexcept = default;

        [[nodiscard]] static document_root_state refreshing(document_root_publisher& publisher) noexcept {
            return document_root_state(&publisher);
        }

        [[nodiscard]] document_root_publisher* publisher() const noexcept {
            return publisher_;
        }

        // Points new request bindings at the worker's adopted generation. Only
        // the owning worker calls this; earlier bindings keep their own lease.
        void publish(document_root_generation& generation) noexcept {
            if (publisher_ == nullptr) {
                std::terminate();
            }
            current_ = &generation;
        }

        [[nodiscard]] document_root_binding binding() const noexcept {
            return current_ == nullptr ? document_root_binding::none() : document_root_binding::configured(*current_);
        }

    private:
        explicit document_root_state(document_root_publisher* publisher) noexcept :
            publisher_(publisher) {}

        document_root_publisher* publisher_{nullptr};
        document_root_generation* current_{nullptr};
    };

    // The validated application server configuration, one copy per worker.
    server_config server_{};
    // Borrowed from the worker runtime, whose shutdown joins every connection;
    // each connection charges its inbound_buffer_account against it.
    inbound_buffer_budget* inbound_budget_{nullptr};
    // Presence enables the policy; absence bypasses it without retaining an
    // inactive configuration state.
    std::optional<compression_config> compression_{};
    // Borrowed from the worker runtime, which always sets it and outlives every
    // request and connection. Inline response compression and server WebSocket
    // connections (shared no-context-takeover deflate) borrow it; worker-thread
    // synchronous use only.
    worker_codecs* codecs_{nullptr};
    std::optional<http_cors_policy> cors_{};
    document_root_state document_root_{};
    // Peers whose forwarding headers may be believed. Empty by default, so an
    // unconfigured server treats every direct peer as the client.
    http_client_ip_extractor trusted_proxies_{};
    access_log_sink access_log_{};
    const env* env_{nullptr};
    worker_failure_sink failure_{};
    connection_failure_sink connection_failure_{};
    // Empty observers keep the connection paths free of observation work.
    tls_client_hello_callback_ref tls_client_hello_{};
    http2_input_callback_ref http2_input_{};
#if defined(RUVIA_ENABLE_HTTP2)
    http2_frame_callback_ref http2_frame_{};
    http2_headers_callback_ref http2_headers_{};
#endif
#if defined(RUVIA_ENABLE_HTTP3)
    http3_frame_callback_ref http3_frame_{};
    http3_field_callback_ref http3_field_{};
#endif
    connection_admission_callback_ref connection_admission_{};
    connection_open_callback_ref connection_open_{};
    connection_close_callback_ref connection_close_{};
    // Serves every listener_dispatch::connection listener; run() requires it
    // when one exists.
    connection_callback_ref connection_{};
    // Serves every listener_dispatch::request_stream listener; run() requires
    // it when one exists.
    request_stream_callback_ref request_stream_{};
    std::optional<rate_limit_rule> default_rate_limit_per_worker_{};
    std::size_t rate_limit_capacity_per_worker_{default_rate_limit_capacity_per_worker};

    // Validates the whole aggregate, including its compression and CORS
    // policies, before publishing any of it.
    void set_server(const server_config& config);
    void set_rate_limit(const rate_limit_config& config);
    void clear_rate_limit() noexcept {
        default_rate_limit_per_worker_.reset();
        rate_limit_capacity_per_worker_ = default_rate_limit_capacity_per_worker;
    }
    void set_trusted_proxies(const trusted_proxy_config& config);
    void clear_trusted_proxies() noexcept {
        trusted_proxies_ = http_client_ip_extractor();
    }
};

// Runtime capability is deliberately separate from Accept-Encoding
// negotiation. A precompressed file sidecar can satisfy a selected coding
// without this process owning an incremental encoder.
enum class http_response_coding_availability : std::uint8_t {
    identity_only,
    identity_and_compression,
};

// Static-file representations remain owned by the static response path.
enum class http_response_compression_source : std::uint8_t {
    buffered,
    stream,
};

enum class http_response_compression_decision : std::uint8_t {
    fixed_representation,
    negotiated_identity,
    encode,
};

// Compression policy and encoder execution are separate outcomes. A response
// may intentionally remain identity (body too small while identity is
// acceptable, no-transform, integrity digest fields, an incompressible media
// type, or an unsupported runtime capability), or the encoder may actually
// fail. Those cases have different meanings when the client has forbidden
// identity and must not collapse back to a bool.
enum class http_response_compression_status : std::uint8_t {
    compressed,
    not_applicable,
    unavailable,
    failed,
};

// The 406 every coding-policy path answers when the client accepts none of the
// codings a response can be sent with. Options rather than http_error_info so
// a throw site can build an http_error from the same value.
[[nodiscard]] constexpr http_error_info_options http_not_acceptable_coding_error() noexcept {
    return {.status_ = http_status::not_acceptable, .code_ = "not_acceptable", .message_ = "no acceptable response content coding"};
}

// Return true when the response cannot satisfy the client's coding policy:
// the negotiation failed, a framework-selected coding fell back to forbidden
// identity, or a handler supplied a pre-encoded representation the client
// excluded. Bodyless statuses are representation-free and therefore never
// need a content-coding fallback.
[[nodiscard]] inline bool http_response_coding_fallback_forbidden(const http_response_coding_selection& selection, http_known_method request_method, const http_response& response, const http_request* request = nullptr) {
    if (!plan_http_response_body(request_method, response.status()).status_allows_body()) {
        return false;
    }
    // An empty 416 carries no content and so no coding to negotiate; its
    // Content-Range describes the selected representation (RFC 9110 15.5.17).
    if (response.status() == http_status::range_not_satisfiable && response.body_bytes().empty()) {
        return false;
    }
    switch (evaluate_http_response_content_coding(selection, response, request)) {
        case http_response_content_coding_status::accepted:
            return false;
        case http_response_content_coding_status::not_acceptable:
        case http_response_content_coding_status::invalid:
            return true;
        case http_response_content_coding_status::absent:
            break;
    }
    if (!selection.acceptable()) {
        return true;
    }
    if (selection.coding() == http_content_coding::identity || selection.identity_accepted()) {
        return false;
    }
    // This guard targets only the framework's silent identity fallback.
    return true;
}

// Outcome of the synchronous compression phase for an in-memory body. An
// eligible body above sync_bytes_ on a worker with a blocking pool is left as
// identity and flagged for offload; every other outcome is final here.
struct http_buffered_compression_step final {
    http_response_compression_status status_;
    bool offload_;
};

// Applies the buffered compression policy without suspending: eligibility,
// size bounds, and encoding up to sync_bytes_. sync_bytes_ is the inline
// ceiling: a larger body is flagged for offload when `offload_available`, and
// otherwise stays identity (not_applicable). Only an offload result requires
// compress_response_offloaded(), so identity and ineligible responses never
// create a coroutine frame. The inline encoder is the worker's reusable one:
// its state stays in worker and only the encoded body comes from the
// response's resource.
[[nodiscard]] http_buffered_compression_step apply_response_compression(const http_response_coding_selection& selection, http_known_method request_method, http_response& response, const compression_config& options, worker_codecs& codecs, bool offload_available);

// Encodes the body flagged by apply_response_compression() on the worker's
// bounded blocking pool with a codec of its own, since worker codecs never leave the
// worker. Rejection by the pool is an intentional identity fallback;
// encoder/commit failures remain typed failures so a client that forbids
// identity receives a terminal error.
[[nodiscard]] task<http_response_compression_status> compress_response_offloaded(http_response_coding_selection selection, http_response& response, const worker_ref& worker_value, const stop_token& stop_token_value = {});

// The single decimal Content-Length a handler declared, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> declared_content_length(const http_response& response) noexcept;

// The size rule for a body of `declared` bytes, shared by buffered bodies,
// HEAD metadata and streams whose head declares a length: below min_bytes_
// (or empty) stays identity unless identity is refused, and above max_bytes_
// always stays identity.
[[nodiscard]] bool declared_length_permits_encoding(const http_response_coding_selection& selection, const compression_config& options, std::uint64_t declared) noexcept;

// Response compression policy. Fixed representations do not vary by coding,
// except an application-encoded body, whose 200-or-406 outcome does; eligible
// representations are annotated before deciding between identity and an
// encoder. Encoder availability never changes Accept-Encoding parsing.
[[nodiscard]] http_response_compression_decision prepare_response_compression(const http_response_coding_selection& selection, http_known_method request_method, http_response& response, http_response_compression_source source_value, http_response_coding_availability availability);

class http_response_coding_policy_disabled final {};

// The response lifecycle has a state that is different from HTTP negotiation
// failure: after a generated terminal error, coding application is deliberately
// disabled. That state stays distinct from the negotiation result, so "406
// negotiation" and "do not transform this error again" are never conflated.
class http_response_coding_policy final {
public:
    [[nodiscard]] static http_response_coding_policy disabled() noexcept {
        return http_response_coding_policy(http_response_coding_policy_disabled{});
    }

    // A failed negotiation stays alive in the selection until a buffered
    // handler has produced its response status: a 204/304 has no response
    // content, so rejecting it before dispatch would turn a representation-free
    // response into a false 406. The selection's identity coding is then only
    // a placeholder that never becomes a silent identity fallback for a
    // response that actually has a body.
    [[nodiscard]] static http_response_coding_policy selected(http_response_coding_selection selection) noexcept {
        return http_response_coding_policy(selection);
    }

    [[nodiscard]] const http_response_coding_selection* selection() const& noexcept {
        return std::get_if<http_response_coding_selection>(&value_);
    }
    const http_response_coding_selection* selection() const&& = delete;

private:
    explicit http_response_coding_policy(http_response_coding_policy_disabled disabled) noexcept :
        value_(disabled) {}

    explicit http_response_coding_policy(http_response_coding_selection selection) noexcept :
        value_(selection) {}

    using value_type = std::variant<http_response_coding_selection, http_response_coding_policy_disabled>;
    value_type value_;
};

// A selected non-identity coding is only a promise until the response policy
// actually installs Content-Encoding. In particular, no-transform,
// incompressible media, or an existing file body can leave the response as
// identity. If the client explicitly excluded identity, that policy fallback
// is a 406 outcome; an encoder failure is surfaced separately as a 500.
[[nodiscard]] inline bool http_response_needs_not_acceptable(const http_response_coding_policy& policy, const http_request& request, const http_response& response) noexcept {
    const auto* selection = policy.selection();
    if (selection == nullptr) {
        return false;
    }
    return http_response_coding_fallback_forbidden(*selection, request.known_method(), response, &request);
}

[[nodiscard]] inline std::optional<http_error_info> http_buffered_response_preparation_error(const http_response_coding_policy& policy, const http_request& request, const http_response& response, http_response_compression_status compression) noexcept {
    const auto* selection = policy.selection();
    if (selection != nullptr && selection->coding() != http_content_coding::identity && !selection->identity_accepted() && plan_http_response_body(request.known_method(), response.status()).status_allows_body()) {
        if (compression == http_response_compression_status::failed) {
            return http_error_info({.status_ = http_status::internal_server_error, .code_ = "response_compression_failed", .message_ = "response compression failed"});
        }
        if (compression == http_response_compression_status::unavailable) {
            return http_error_info({.status_ = http_status::service_unavailable, .code_ = "blocking_pool_unavailable", .message_ = "blocking pool cannot accept the operation"});
        }
    }
    if (http_response_needs_not_acceptable(policy, request, response)) {
        return http_error_info(http_not_acceptable_coding_error());
    }
    return std::nullopt;
}

enum class buffered_response_recovery_action { ready,
    respond_error,
    prepare_terminal };

struct buffered_response_recovery_step final {
    buffered_response_recovery_action action_{buffered_response_recovery_action::ready};
    std::optional<http_error_info> error_;
};

// A secondary failure preserves the error handler's representation and
// disables coding instead of invoking it again. An error response (4xx/5xx)
// goes straight to that terminal path: it is not a representation of the
// target resource (RFC 9110 15.5.7), so the server may disregard
// Accept-Encoding for it (RFC 9110 12.4.1). Replacing it with a 406 would
// disguise the original failure and run the error handler a second time.
class buffered_response_recovery final {
public:
    [[nodiscard]] bool recovered() const noexcept {
        return stage_ != stage::application;
    }

    [[nodiscard]] buffered_response_recovery_step advance(http_response_coding_policy& policy, const http_request& request, const http_response& response, http_response_compression_status compression) noexcept {
        if (stage_ == stage::terminal) {
            return {};
        }
        auto error = http_buffered_response_preparation_error(policy, request, response, compression);
        if (!error.has_value()) {
            return {};
        }
        if (stage_ == stage::application && !response.status().is_error()) {
            stage_ = stage::negotiated_error;
            return {buffered_response_recovery_action::respond_error, std::move(error)};
        }
        policy = http_response_coding_policy::disabled();
        stage_ = stage::terminal;
        return {buffered_response_recovery_action::prepare_terminal, std::nullopt};
    }

private:
    enum class stage { application,
        negotiated_error,
        terminal };
    stage stage_{stage::application};
};

struct prepared_application_response final {
    http_buffered_response_write_plan write_plan_;
    bool recovered_;
};

}  // namespace ruvia::detail
