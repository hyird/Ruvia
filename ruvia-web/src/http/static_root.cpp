#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_accept_match.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_date.h"
#include "ruvia/web/detail/util/fnv1a.h"

#include "http/static_file_metadata.h"
#include "http/static_file_types.h"
#include "http/static_path_normalization.h"
#include "http/static_root_index.h"
#include "http/static_root_options_validation.h"
#include "server/http_file_open.h"
#include "server/http_native_file.h"

// A document root indexed once at construction: the directory is walked, every
// servable file recorded with the metadata a response needs, and lookups after
// that touch only the index -- a request never stats the filesystem.

namespace ruvia {
namespace {

// A relative path (generic '/'-separated form) whose first component or any
// component after a '/' begins with '.' is hidden. Serving these by default
// leaks .env, .git/config, .htpasswd and similar secrets that happen to sit
// under a document root.
[[nodiscard]] bool has_hidden_path_segment(std::string_view relative_generic) noexcept {
    return relative_generic.starts_with('.') || (relative_generic.find("/.") != std::string_view::npos);
}

[[nodiscard]] detail::static_root_state* make_static_root_state(detail::static_root_config_storage config) {
    auto* const resource = process_resource();
    if (resource == nullptr) {
        std::terminate();
    }
    return construct_pmr_object<detail::static_root_state>(
        resource, resource, std::move(config));
}

[[nodiscard]] std::filesystem::path canonical_static_root_path(const std::filesystem::path& root) {
    std::error_code ec;
    auto canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec || !std::filesystem::is_directory(canonical_root, ec)) {
        throw std::invalid_argument("static file root not found");
    }
    return canonical_root;
}

[[nodiscard]] const detail::static_root_entry* find_static_root_entry(
    const std::pmr::vector<detail::static_root_entry>& entries,
    std::string_view relative_path) noexcept {
    return detail::find_static_index_entry(entries, relative_path,
        [](const detail::static_root_entry& entry_value) noexcept {
            return std::string_view(entry_value.relative_path_);
        });
}

[[nodiscard]] bool contains_static_directory(
    const std::pmr::vector<std::pmr::string>& directories, std::string_view relative_path) noexcept {
    return detail::find_static_index_entry(directories, relative_path,
               [](const std::pmr::string& directory) noexcept { return std::string_view(directory); }) != nullptr;
}

[[nodiscard]] bool media_type_starts_with(
    std::string_view media_type, std::string_view prefix) noexcept {
    return media_type.size() >= prefix.size() &&
           http_ascii_equals_ignore_case(media_type.substr(0, prefix.size()), prefix);
}

[[nodiscard]] bool media_type_ends_with(std::string_view media_type, std::string_view suffix) noexcept {
    return media_type.size() >= suffix.size() &&
           http_ascii_equals_ignore_case(
               media_type.substr(media_type.size() - suffix.size()), suffix);
}

[[nodiscard]] bool static_content_type_eligible_for_precompression(
    std::string_view content_type_value) noexcept {
    const auto media_type = http_media_type_only(content_type_value);
    if (media_type.empty()) {
        return false;
    }
    return media_type_starts_with(media_type, "text/") ||
           http_ascii_equals_ignore_case(media_type, "application/json") ||
           http_ascii_equals_ignore_case(media_type, "application/javascript") ||
           http_ascii_equals_ignore_case(media_type, "application/x-javascript") ||
           http_ascii_equals_ignore_case(media_type, "application/wasm") ||
           http_ascii_equals_ignore_case(media_type, "application/xml") ||
           http_ascii_equals_ignore_case(media_type, "application/xhtml+xml") ||
           http_ascii_equals_ignore_case(media_type, "image/svg+xml") ||
           media_type_ends_with(media_type, "+json") || media_type_ends_with(media_type, "+xml");
}

[[nodiscard]] detail::static_file_version static_root_entry_version(
    const detail::static_root_entry& entry_value) noexcept {
    return {entry_value.modified_seconds_, entry_value.modified_token_};
}

[[nodiscard]] bool same_static_root_entry_metadata(
    const detail::static_root_entry& left, const detail::static_root_entry& right) noexcept {
    return left.relative_path_ == right.relative_path_ && left.file_path_ == right.file_path_ &&
           left.content_type_ == right.content_type_ && left.size_ == right.size_ &&
           left.identity_ == right.identity_ && left.modified_token_ == right.modified_token_ &&
           left.modified_seconds_ == right.modified_seconds_ && left.etag_ == right.etag_ &&
           left.last_modified_ == right.last_modified_ &&
           left.directly_servable_ == right.directly_servable_;
}

[[nodiscard]] const detail::static_root_entry* find_fresh_sidecar_entry(
    const detail::static_root_state& state_value, const detail::static_root_entry& identity,
    std::string_view suffix) {
    static constexpr std::size_t inline_path_capacity = 256;
    std::array<char, inline_path_capacity> inline_path{};
    std::pmr::string variant_path(process_resource());
    const std::size_t total_size = identity.relative_path_.size() + suffix.size();
    std::string_view lookup_path;
    if (total_size <= inline_path.size()) {
        if (!identity.relative_path_.empty()) {
            std::memcpy(inline_path.data(), identity.relative_path_.data(), identity.relative_path_.size());
        }
        std::memcpy(inline_path.data() + identity.relative_path_.size(), suffix.data(), suffix.size());
        lookup_path = std::string_view(inline_path.data(), total_size);
    } else {
        variant_path.reserve(total_size);
        variant_path.append(identity.relative_path_.data(), identity.relative_path_.size());
        variant_path.append(suffix.data(), suffix.size());
        lookup_path = variant_path;
    }
    const auto* variant = find_static_root_entry(state_value.entries_, lookup_path);
    if (variant == nullptr ||
        !static_root_entry_version(*variant).at_least_as_new_as(static_root_entry_version(identity))) {
        return nullptr;
    }
    return variant;
}

[[nodiscard]] const detail::static_root_memory_variant* find_memory_variant(
    const detail::static_root_entry& entry_value, http_content_coding coding) noexcept {
    for (const auto& variant : entry_value.memory_variants_) {
        if (variant.content_coding_ == coding) {
            return &variant;
        }
    }
    return nullptr;
}

// Reuses an unchanged file's encoded bytes from the previous snapshot. The
// buffer is immutable, so the new snapshot shares it instead of copying it.
void share_memory_variant(detail::static_root_entry& entry_value,
    const detail::static_root_memory_variant& source_value, std::pmr::memory_resource* resource) {
    auto& stored = entry_value.memory_variants_.emplace_back(resource);
    stored.content_coding_ = source_value.content_coding_;
    stored.bytes_ = source_value.bytes_;
    stored.modified_token_ = source_value.modified_token_;
    stored.modified_seconds_ = source_value.modified_seconds_;
    stored.etag_ = source_value.etag_;
    stored.last_modified_ = source_value.last_modified_;
}

// A file that changed (or vanished) since it was indexed yields nothing: only
// that entry loses its variants and stays file-backed, validated per request.
[[nodiscard]] std::optional<std::pmr::string> read_static_root_entry_bytes(
    const detail::static_root_entry& entry_value, std::pmr::memory_resource* resource) {
    if (entry_value.size_ > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("static file is too large to buffer");
    }
    const auto size = static_cast<std::size_t>(entry_value.size_);
    std::pmr::string bytes(resource);
    bytes.resize(size);
    // Read and revalidate through one handle: the kept bytes are exactly those
    // of the indexed snapshot, with no path re-resolution in between.
    detail::response_file_input input(http_response_file_view(
        entry_value.file_path_.c_str(), entry_value.size_, 0, entry_value.size_, entry_value.identity_));
    if (!input || !detail::read_exact(input, bytes.data(), size) ||
        !input.matches_snapshot(entry_value.identity_, entry_value.size_)) {
        return std::nullopt;
    }
    return bytes;
}

void store_precompressed_variant(detail::static_root_entry& entry_value, http_content_coding coding,
    std::pmr::string encoded, bool emit_response_validators, std::pmr::memory_resource* resource) {
    auto bytes = std::allocate_shared<std::pmr::string>(
        std::pmr::polymorphic_allocator<std::pmr::string>(resource), std::move(encoded));
    auto& variant = entry_value.memory_variants_.emplace_back(resource);
    variant.content_coding_ = coding;
    variant.modified_token_ = entry_value.modified_token_;
    variant.modified_seconds_ = entry_value.modified_seconds_;
    variant.bytes_ = std::move(bytes);
    if (emit_response_validators) {
        variant.etag_ = detail::make_static_file_snapshot_etag(
            resource, variant.bytes_->size(), entry_value.modified_token_, entry_value.identity_, coding);
        variant.last_modified_ = entry_value.last_modified_;
    }
}

template <typename t_type>
void hash_value(std::uint64_t& hash, t_type value) noexcept {
    static_assert(std::is_integral_v<t_type>);
    hash = detail::fnv1a_accumulate_integer(hash, static_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t static_root_fingerprint(const detail::static_root_state& state_value) noexcept {
    std::uint64_t hash = detail::fnv1a_offset_basis;
#if defined(__unix__)
    // A root directory replaced by another one with an identical index is
    // still a different snapshot: requests open entries relative to it.
    hash_value(hash, state_value.root_identity_.device_);
    hash_value(hash, state_value.root_identity_.inode_);
#endif
    hash_value(hash, state_value.entries_.size());
    for (const auto& entry : state_value.entries_) {
        hash = detail::fnv1a_accumulate(hash, entry.relative_path_);
        hash_value(hash, entry.size_);
        hash_value(hash, entry.modified_token_);
        hash_value(hash, entry.modified_seconds_);
        hash_value(hash, entry.identity_.requires_validation());
        for (const auto word : entry.identity_.words()) {
            hash_value(hash, word);
        }
        hash_value(hash, entry.directly_servable_);
    }
    return hash;
}

}  // namespace

detail::static_root_config_storage detail::static_root_access::copy_config(
    const static_root& root, std::pmr::memory_resource* resource) {
    return static_root_config_storage(root.state_->config_, resource);
}

std::string_view detail::static_root_access::index_file(const static_root& root) noexcept {
    return root.state_->config_.index_file_;
}

bool detail::static_root_access::has_directory_index(const static_root& root) noexcept {
    return !root.state_->config_.index_file_.empty();
}

std::optional<detail::static_root_entry_view> detail::static_root_access::find(
    const static_root& root, std::string_view relative_path) noexcept {
    auto entry_value = find_variant(root, relative_path);
    if (!entry_value.has_value() || !entry_value->directly_servable_) {
        return std::nullopt;
    }
    return entry_value;
}

std::optional<detail::static_root_entry_view> detail::static_root_access::find_variant(
    const static_root& root, std::string_view relative_path) noexcept {
    const auto& state_value = *root.state_;
    const auto& entries = state_value.entries_;
    const auto* const entry_value = find_static_root_entry(entries, relative_path);
    if (entry_value == nullptr) {
        return std::nullopt;
    }
    return detail::static_root_entry_view(entry_value->file_path_.c_str(), entry_value->content_type_,
        state_value.config_.cache_control_, entry_value->etag_, entry_value->last_modified_, entry_value->size_, entry_value->identity_,
        entry_value->modified_token_, entry_value->modified_seconds_, state_value.config_.range_requests_,
        state_value.config_.response_validators_, entry_value->directly_servable_, &entry_value->memory_variants_,
        entry_value->resident_bytes_.get());
}

bool detail::static_root_access::is_indexed_directory(
    const static_root& root, std::string_view relative_path) noexcept {
    if (!has_directory_index(root)) {
        return false;
    }
    return contains_static_directory(root.state_->directories_, relative_path);
}

namespace {

[[nodiscard]] std::optional<detail::static_root_entry_view> resolve_normalized_static_root_entry(
    const static_root& root, bool directory_slash, std::string_view& relative, std::pmr::string& storage) {
    auto entry_value =
        directory_slash ? std::optional<detail::static_root_entry_view>{} : detail::static_root_access::find(root, relative);
    if (!entry_value.has_value() && detail::static_root_access::is_indexed_directory(root, relative)) {
        // A normalized path never ends with '/'.
        const auto index = detail::static_root_access::index_file(root);
        storage.reserve(relative.size() + (relative.empty() ? 0U : 1U) + index.size());
        if (relative.data() != storage.data()) {
            storage.assign(relative.data(), relative.size());
        }
        if (!storage.empty()) {
            storage.push_back('/');
        }
        storage.append(index.data(), index.size());
        relative = storage;
        entry_value = detail::static_root_access::find(root, relative);
    }
    return entry_value;
}

}  // namespace

std::optional<detail::static_root_entry_view> detail::static_root_access::resolve(
    const static_root& root, std::string_view& relative, std::pmr::string& storage) {
    const bool directory_slash =
        !relative.empty() && (relative.back() == '/' || relative.back() == '\\');
    relative = normalize_static_relative_path(relative, storage);
    if (relative.empty() && !has_directory_index(root)) {
        throw_invalid_static_file_path();
    }
    return resolve_normalized_static_root_entry(root, directory_slash, relative, storage);
}

std::optional<detail::static_root_entry_view> detail::static_root_access::try_resolve(
    const static_root& root, std::string_view& relative, std::pmr::string& storage) {
    const bool directory_slash =
        !relative.empty() && (relative.back() == '/' || relative.back() == '\\');
    const auto normalized = try_normalize_static_relative_path(relative, storage);
    if (!normalized.has_value()) {
        return std::nullopt;
    }
    relative = *normalized;
    if (relative.empty() && !has_directory_index(root)) {
        return std::nullopt;
    }
    return resolve_normalized_static_root_entry(root, directory_slash, relative, storage);
}

std::uint64_t detail::static_root_access::fingerprint(const static_root& root) noexcept {
    return root.state_->fingerprint_;
}

void detail::static_root_access::install_precompressed_variants(
    static_root& root, const static_root* previous, const static_root_precompression_options& options) {
    if (!options.enabled()) {
        return;
    }
    if (options.min_bytes_ == 0 || options.max_bytes_ < options.min_bytes_) {
        throw std::invalid_argument("invalid static root precompression options");
    }

    auto& state_value = *root.state_;
    auto* const resource = process_resource();
    const auto* previous_state = previous == nullptr ? nullptr : previous->state_.get();
    const bool emit_response_validators =
        state_value.config_.response_validators_ == static_response_validator_policy::emit;

    for (auto& entry : state_value.entries_) {
        if (!entry.directly_servable_ || entry.size_ < options.min_bytes_ ||
            entry.size_ > options.max_bytes_ ||
            !static_content_type_eligible_for_precompression(entry.content_type_)) {
            continue;
        }

        const detail::static_root_entry* previous_entry = nullptr;
        if (previous_state != nullptr) {
            previous_entry = find_static_root_entry(previous_state->entries_, entry.relative_path_);
            if (previous_entry != nullptr && !same_static_root_entry_metadata(entry, *previous_entry)) {
                previous_entry = nullptr;
            }
        }

        std::optional<std::pmr::string> plain;
        for (const auto& sidecar : detail::static_precompression_sidecars) {
            const auto coding = sidecar.content_coding_;
            if (!options.enabled(coding) || find_fresh_sidecar_entry(state_value, entry, sidecar.suffix_) != nullptr) {
                continue;
            }
            if (previous_entry != nullptr) {
                if (const auto* previous_variant = find_memory_variant(*previous_entry, coding);
                    previous_variant != nullptr) {
                    share_memory_variant(entry, *previous_variant, resource);
                    continue;
                }
            }
            if (!plain.has_value()) {
                plain = read_static_root_entry_bytes(entry, resource);
                if (!plain.has_value()) {
                    entry.memory_variants_.clear();
                    break;
                }
            }
            if (plain->empty()) {
                continue;
            }
            auto encoded = encode_http_content(
                coding, *plain, {.max_encoded_bytes_ = plain->size() - 1, .resource_ = resource});
            if (encoded.has_value()) {
                store_precompressed_variant(entry, coding, std::move(*encoded),
                    emit_response_validators, resource);
            }
        }
    }
}

void detail::static_root_access::install_resident_bodies(
    static_root& root, const static_root* previous, const static_root_resident_options& options) {
    if (!options.enabled()) {
        return;
    }
    auto& state_value = *root.state_;
    auto* const resource = process_resource();
    const auto* previous_state = previous == nullptr ? nullptr : previous->state_.get();
    std::size_t used = 0;
    for (auto& entry : state_value.entries_) {
        if (entry.size_ > options.max_file_bytes_ || entry.size_ > options.budget_bytes_ - used) {
            continue;
        }
        const auto size = static_cast<std::size_t>(entry.size_);
        if (previous_state != nullptr) {
            const auto* previous_entry = find_static_root_entry(previous_state->entries_, entry.relative_path_);
            if (previous_entry != nullptr && previous_entry->resident_bytes_ != nullptr &&
                same_static_root_entry_metadata(entry, *previous_entry)) {
                entry.resident_bytes_ = previous_entry->resident_bytes_;
                used += size;
                continue;
            }
        }
        auto bytes = read_static_root_entry_bytes(entry, resource);
        if (!bytes.has_value()) {
            continue;
        }
        entry.resident_bytes_ = std::allocate_shared<std::pmr::string>(
            std::pmr::polymorphic_allocator<std::pmr::string>(resource), std::move(*bytes));
        used += size;
    }
}

http_response_file_handle detail::static_root_access::open_entry(
    const static_root& root, const static_root_entry_view& entry) noexcept {
#if defined(__unix__)
    const auto& state_value = *root.state_;
    if (state_value.root_directory_ < 0) {
        return {};
    }
    // Every entry path is the canonical root joined with its relative path,
    // so the relative part starts right after the root and its separator.
    const auto& root_path = state_value.root_;
    const auto prefix = root_path.size() + (root_path.ends_with('/') ? 0 : 1);
    std::error_code ec;
    return open_response_file_at(state_value.root_directory_, entry.file_path() + prefix, ec);
#else
    static_cast<void>(root);
    static_cast<void>(entry);
    return {};
#endif
}

bool detail::static_root_access::same_snapshot(
    const static_root& left, const static_root& right) noexcept {
    const auto& lhs = *left.state_;
    const auto& rhs = *right.state_;
    const auto& lhs_config = lhs.config_;
    const auto& rhs_config = rhs.config_;
    if (lhs.root_ != rhs.root_ || lhs_config.index_file_ != rhs_config.index_file_ ||
        lhs_config.cache_control_ != rhs_config.cache_control_ ||
        lhs_config.default_content_type_ != rhs_config.default_content_type_ ||
        lhs_config.file_type_kind_ != rhs_config.file_type_kind_ ||
        lhs_config.range_requests_ != rhs_config.range_requests_ ||
        lhs_config.response_validators_ != rhs_config.response_validators_ ||
        lhs_config.dotfiles_ != rhs_config.dotfiles_ ||
        lhs_config.file_type_extensions_ != rhs_config.file_type_extensions_ ||
        lhs.directories_ != rhs.directories_ ||
        lhs_config.mime_types_.size() != rhs_config.mime_types_.size() ||
        lhs.entries_.size() != rhs.entries_.size()) {
        return false;
    }
#if defined(__unix__)
    if (lhs.root_identity_ != rhs.root_identity_) {
        return false;
    }
#endif
    for (std::size_t i = 0; i < lhs_config.mime_types_.size(); ++i) {
        if (lhs_config.mime_types_[i].extension_ != rhs_config.mime_types_[i].extension_ ||
            lhs_config.mime_types_[i].content_type_ != rhs_config.mime_types_[i].content_type_) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.entries_.size(); ++i) {
        if (!same_static_root_entry_metadata(lhs.entries_[i], rhs.entries_[i])) {
            return false;
        }
    }
    return true;
}

namespace {

[[nodiscard]] std::unique_ptr<detail::static_root_state, static_root::state_deleter_type>
build_static_root_state(const std::filesystem::path& root, detail::static_root_config_storage config_storage) {
    const auto canonical_root = canonical_static_root_path(root);
    std::unique_ptr<detail::static_root_state, static_root::state_deleter_type> state(
        make_static_root_state(std::move(config_storage)));
    std::error_code ec;
    auto& state_value = *state;
    http_assign_native_path(state_value.root_, canonical_root);
#if defined(__unix__)
    {
        auto root_directory = detail::open_native_directory(state_value.root_.c_str(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error("open static file root", canonical_root, ec);
        }
        state_value.root_identity_ = detail::native_directory_handle_identity(root_directory.get(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error("inspect static file root", canonical_root, ec);
        }
        state_value.root_directory_ = root_directory.release();
    }
#endif

    auto* const upstream = process_resource();
    const auto& config = state_value.config_;
    const auto serve_dotfiles = detail::static_root_serves_dotfiles(config.dotfiles_);
    if (!config.index_file_.empty()) {
        state_value.directories_.push_back({});
    }

    // Index construction is transactional. A directory may change while it is
    // being walked, but publishing a partial snapshot would turn one transient
    // filesystem error into arbitrary 404s. Let the caller keep the previous
    // root during polling, and fail startup when there is no previous snapshot.
    std::filesystem::recursive_directory_iterator iter(canonical_root, ec);
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonical_root, ec);
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iter != end; iter.increment(ec)) {
        const auto& file_path = iter->path();
        ec.clear();
        const auto status = iter->symlink_status(ec);
        if (ec) {
            throw std::filesystem::filesystem_error("inspect static file root entry", file_path, ec);
        }
        if (std::filesystem::is_symlink(status)) {
            continue;
        }
        auto relative = detail::static_file_utf8_string(
            file_path.lexically_relative(canonical_root), upstream);
        if (relative.empty() || relative.starts_with("../")) {
            continue;
        }
        // Default-deny hidden paths: skip dotfiles and do not descend into
        // dot-directories (.git, .ssh, ...) so their contents are never indexed.
        if (!serve_dotfiles && has_hidden_path_segment(relative)) {
            if (std::filesystem::is_directory(status)) {
                iter.disable_recursion_pending();
            }
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            if (!config.index_file_.empty()) {
                state_value.directories_.push_back(std::move(relative));
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            continue;
        }
        const auto extension = detail::lower_static_file_extension_from_utf8(relative, upstream);
        const bool directly_servable = detail::file_type_allowed(extension, config);
        bool usable_as_sidecar = false;
        if (!directly_servable && detail::is_precompressed_sidecar_extension(extension)) {
            const auto stem_view =
                std::string_view(relative).substr(0, relative.size() - extension.size());
            usable_as_sidecar = detail::file_type_allowed(
                detail::lower_static_file_extension_from_utf8(stem_view, upstream), config);
        }
        if (!directly_servable && !usable_as_sidecar) {
            continue;
        }
        ec.clear();
        const auto snapshot = detail::snapshot_response_file(file_path.c_str(), ec);
        if (ec) {
            throw std::filesystem::filesystem_error(
                "snapshot static file root entry", file_path, ec);
        }
        const auto emit_response_validators =
            config.response_validators_ == static_response_validator_policy::emit;
        detail::static_root_entry entry(upstream);
        entry.relative_path_ = std::move(relative);
        http_assign_native_path(entry.file_path_, file_path);
        entry.content_type_ = detail::content_type_for(file_path, extension, config, upstream);
        entry.size_ = snapshot.size_;
        entry.identity_ = snapshot.identity_;
        entry.modified_token_ = snapshot.modified_token_;
        entry.modified_seconds_ = snapshot.modified_seconds_;
        entry.directly_servable_ = directly_servable;
        if (emit_response_validators) {
            entry.etag_ = detail::make_static_file_snapshot_etag(
                upstream, snapshot.size_, snapshot.modified_token_, snapshot.identity_);
            if (const auto date = format_http_date(snapshot.modified_seconds_)) {
                entry.last_modified_.assign(date->data(), date->size());
            }
        }
        if (state_value.entries_.empty()) {
            state_value.entries_.reserve(8);
        }
        state_value.entries_.push_back(std::move(entry));
    }
    if (ec) {
        throw std::filesystem::filesystem_error("iterate static file root", canonical_root, ec);
    }
    std::ranges::sort(state_value.entries_,
        [](const detail::static_root_entry& left, const detail::static_root_entry& right) {
            return left.relative_path_ < right.relative_path_;
        });
    std::ranges::sort(state_value.directories_);
    state_value.directories_.erase(
        std::ranges::unique(state_value.directories_).begin(), state_value.directories_.end());
#if defined(__unix__)
    // The walk resolved the root by path. Publish only when that path still
    // names the retained directory, so requests open entries in the indexed
    // directory rather than in one swapped in while the index was built.
    if (detail::native_directory_path_identity(state_value.root_.c_str(), ec) != state_value.root_identity_ || ec) {
        throw std::filesystem::filesystem_error("static file root changed while it was indexed", canonical_root,
            ec ? ec : std::make_error_code(std::errc::state_not_recoverable));
    }
#endif
    state_value.fingerprint_ = static_root_fingerprint(state_value);
    return state;
}

}  // namespace

std::unique_ptr<static_root, pmr_object_deleter<static_root>> detail::static_root_access::make(
    std::pmr::memory_resource* object_resource, const std::filesystem::path& root,
    const static_root_config_storage& config) {
    auto state = build_static_root_state(root, detail::static_root_config_storage(config, process_resource()));
    auto* const resource = pmr_resource_or_default(object_resource);
    auto* const storage = resource->allocate(sizeof(static_root), alignof(static_root));
    try {
        auto* const result_value = ::new (storage) static_root(std::move(state));
        return std::unique_ptr<static_root, pmr_object_deleter<static_root>>(
            result_value, pmr_object_deleter<static_root>{resource});
    } catch (...) {
        resource->deallocate(storage, sizeof(static_root), alignof(static_root));
        throw;
    }
}

std::unique_ptr<static_root, pmr_object_deleter<static_root>> detail::static_root_access::make(
    std::pmr::memory_resource* object_resource, const std::filesystem::path& root,
    static_root_config_storage&& config) {
    auto stored = config.cache_control_.get_allocator().resource() == process_resource()
                      ? std::move(config)
                      : detail::static_root_config_storage(config, process_resource());
    auto state = build_static_root_state(root, std::move(stored));
    auto* const resource = pmr_resource_or_default(object_resource);
    auto* const storage = resource->allocate(sizeof(static_root), alignof(static_root));
    try {
        auto* const result_value = ::new (storage) static_root(std::move(state));
        return std::unique_ptr<static_root, pmr_object_deleter<static_root>>(
            result_value, pmr_object_deleter<static_root>{resource});
    } catch (...) {
        resource->deallocate(storage, sizeof(static_root), alignof(static_root));
        throw;
    }
}

static_root::static_root(std::unique_ptr<detail::static_root_state, state_deleter_type> state) noexcept
    : state_(std::move(state)) {}

static_root::static_root(const std::filesystem::path& root, static_root_options options)
    : state_([&] {
          detail::validate_static_root_options(options);
          return build_static_root_state(
              root, detail::store_validated_static_root_config(options, process_resource()));
      }()) {
    detail::static_root_access::install_resident_bodies(*this, nullptr,
        {.max_file_bytes_ = options.resident_max_bytes_.value_or(0),
            .budget_bytes_ = options.resident_budget_bytes_.value_or(0)});
}

static_root::~static_root() = default;

detail::static_root_state::~static_root_state() {
#if defined(__unix__)
    if (root_directory_ >= 0) {
        ::close(root_directory_);
    }
#endif
}

void static_root::state_deleter_type::operator()(detail::static_root_state* state_value) const noexcept {
    destroy_pmr_object(state_value, process_resource());
}

std::filesystem::path static_root::path() const {
    return http_make_path_from_native_path(state_->root_);
}

}  // namespace ruvia
