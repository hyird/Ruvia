#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_date.h"
#include "ruvia/http/http_media_type.h"

#include "http/static_file_metadata.h"
#include "http/static_file_types.h"
#include "http/static_root_index.h"
#include "http/static_root_options_validation.h"
#include "server/http_native_file.h"

// A document root indexed once at construction: the directory is walked, every
// servable file recorded with the metadata a response needs, and lookups after
// that touch only the index -- a request never stats the filesystem.

namespace ruvia {
namespace {

// Above this many indexed entries a lookup binary-searches instead of scanning.
inline constexpr std::size_t static_root_linear_lookup_limit = 8;

// A relative path (generic '/'-separated form) whose first component or any
// component after a '/' begins with '.' is hidden. Serving these by default
// leaks .env, .git/config, .htpasswd and similar secrets that happen to sit
// under a document root.
[[nodiscard]] bool has_hidden_path_segment(std::string_view relative_generic) noexcept {
    return relative_generic.starts_with('.') || (relative_generic.find("/.") != std::string_view::npos);
}

[[nodiscard]] detail::static_root_state* make_static_root_state(detail::static_root_config_storage config) {
    auto* const resource = detail::process_resource();
    if (resource == nullptr) {
        std::terminate();
    }
    return detail::construct_pmr_object<detail::static_root_state>(
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
    if (entries.size() <= static_root_linear_lookup_limit) {
        for (const auto& entry : entries) {
            if (entry.relative_path_ == relative_path) {
                return &entry;
            }
        }
        return nullptr;
    }

    const auto iter = std::ranges::lower_bound(entries, relative_path, std::ranges::less{},
        [](const detail::static_root_entry& entry_value) noexcept {
            return std::string_view(entry_value.relative_path_);
        });
    if (iter == entries.end() || std::string_view(iter->relative_path_) != relative_path) {
        return nullptr;
    }
    return &*iter;
}

[[nodiscard]] bool contains_static_directory(
    const std::pmr::vector<std::pmr::string>& directories, std::string_view relative_path) noexcept {
    if (directories.size() <= static_root_linear_lookup_limit) {
        return std::ranges::find(directories, relative_path, [](const auto& directory) noexcept {
            return std::string_view(directory);
        }) != directories.end();
    }

    return std::ranges::binary_search(
        directories, relative_path, [](const auto& left, const auto& right) {
            return std::string_view(left) < std::string_view(right);
        });
}

// A precompressed sidecar (foo.js.br / .gz / .zst) is indexed when its base
// file's type is allowed, so it can be served as a Content-Encoding variant.
[[nodiscard]] bool is_precompressed_sidecar_extension(std::string_view extension) noexcept {
    return extension == ".br" || extension == ".gz" || extension == ".zst";
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

[[nodiscard]] std::string_view static_precompression_suffix(http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::gzip:
            return ".gz";
        case http_content_coding::brotli:
            return ".br";
        case http_content_coding::zstd:
            return ".zst";
        default:
            return {};
    }
}

[[nodiscard]] std::string_view static_precompression_etag_token(http_content_coding coding) noexcept {
    switch (coding) {
        case http_content_coding::gzip:
            return "gzip";
        case http_content_coding::brotli:
            return "br";
        case http_content_coding::zstd:
            return "zstd";
        default:
            return "identity";
    }
}

[[nodiscard]] std::pmr::string make_static_file_encoded_snapshot_etag(
    std::pmr::memory_resource* resource, std::uint64_t encoded_size, std::uint64_t modified_token,
    http_response_file_identity identity, http_content_coding coding) {
    std::pmr::string output(resource);
    output.reserve(144);
    output.push_back('"');
    detail::append_static_file_unsigned(output, encoded_size);
    output.push_back('-');
    detail::append_static_file_unsigned(output, modified_token);
    for (const auto word : identity.words()) {
        output.push_back('-');
        detail::append_static_file_unsigned(output, word);
    }
    output.push_back('-');
    const auto token = static_precompression_etag_token(coding);
    output.append(token.data(), token.size());
    output.push_back('"');
    return output;
}

[[nodiscard]] bool same_static_root_file_snapshot(
    const detail::static_root_entry& entry_value, const detail::response_file_snapshot& snapshot) noexcept {
    return entry_value.size_ == snapshot.size_ && entry_value.identity_ == snapshot.identity_ &&
           entry_value.modified_token_ == snapshot.modified_token_ &&
           entry_value.modified_seconds_ == snapshot.modified_seconds_;
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

[[nodiscard]] bool precompressed_variant_is_at_least_as_new(
    const detail::static_root_entry& identity, const detail::static_root_entry& variant) noexcept {
    if (variant.modified_seconds_ != identity.modified_seconds_) {
        return variant.modified_seconds_ > identity.modified_seconds_;
    }
    return variant.modified_token_ >= identity.modified_token_;
}

[[nodiscard]] const detail::static_root_entry* find_fresh_sidecar_entry(
    const detail::static_root_state& state_value, const detail::static_root_entry& identity,
    http_content_coding coding) {
    const auto suffix = static_precompression_suffix(coding);
    if (suffix.empty()) {
        return nullptr;
    }
    std::pmr::string variant_path(detail::process_resource());
    variant_path.reserve(identity.relative_path_.size() + suffix.size());
    variant_path.append(identity.relative_path_.data(), identity.relative_path_.size());
    variant_path.append(suffix.data(), suffix.size());
    const auto* variant = find_static_root_entry(state_value.entries_, variant_path);
    if (variant == nullptr || !precompressed_variant_is_at_least_as_new(identity, *variant)) {
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

void copy_memory_variant(detail::static_root_entry& entry_value,
    const detail::static_root_memory_variant& source_value, std::pmr::memory_resource* resource) {
    auto& stored = entry_value.memory_variants_.emplace_back(resource);
    stored.content_coding_ = source_value.content_coding_;
    stored.bytes_ = source_value.bytes_;
    stored.modified_token_ = source_value.modified_token_;
    stored.modified_seconds_ = source_value.modified_seconds_;
    stored.etag_ = source_value.etag_;
    stored.last_modified_ = source_value.last_modified_;
}

[[nodiscard]] std::pmr::string read_stable_static_root_entry_bytes(
    const detail::static_root_entry& entry_value, std::pmr::memory_resource* resource) {
    if (entry_value.size_ > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("static file is too large to precompress");
    }
    const auto size = static_cast<std::size_t>(entry_value.size_);
    std::pmr::string bytes(resource);
    bytes.resize(size);
    const std::filesystem::path path(entry_value.file_path_.c_str());
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
    const auto after = detail::snapshot_response_file(entry_value.file_path_.c_str(), ec);
    if (ec || !same_static_root_file_snapshot(entry_value, after)) {
        throw std::runtime_error("static file changed while it was being precompressed");
    }
    return bytes;
}

void store_precompressed_variant(detail::static_root_entry& entry_value, http_content_coding coding,
    std::pmr::string encoded, bool emit_response_validators, std::pmr::memory_resource* resource) {
    auto& variant = entry_value.memory_variants_.emplace_back(resource);
    variant.content_coding_ = coding;
    variant.modified_token_ = entry_value.modified_token_;
    variant.modified_seconds_ = entry_value.modified_seconds_;
    variant.bytes_ = std::move(encoded);
    if (emit_response_validators) {
        variant.etag_ = make_static_file_encoded_snapshot_etag(
            resource, variant.bytes_.size(), entry_value.modified_token_, entry_value.identity_, coding);
        variant.last_modified_ = entry_value.last_modified_;
    }
}

void hash_bytes(std::uint64_t& hash, const char* bytes_value, std::size_t size) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint8_t>(bytes_value[i]);
        hash *= UINT64_C(1099511628211);
    }
}

template <typename t_type>
void hash_value(std::uint64_t& hash, const t_type& value) noexcept {
    static_assert(std::is_trivially_copyable_v<t_type>);
    hash_bytes(hash, reinterpret_cast<const char*>(&value), sizeof(value));
}

[[nodiscard]] std::uint64_t static_root_fingerprint(const detail::static_root_state& state_value) noexcept {
    std::uint64_t hash = UINT64_C(1469598103934665603);
    hash_value(hash, state_value.entries_.size());
    for (const auto& entry : state_value.entries_) {
        hash_bytes(hash, entry.relative_path_.data(), entry.relative_path_.size());
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

[[nodiscard]] bool same_static_root_entry(
    const detail::static_root_entry& left, const detail::static_root_entry& right) noexcept {
    return same_static_root_entry_metadata(left, right);
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
        state_value.config_.response_validators_, entry_value->directly_servable_, &entry_value->memory_variants_);
}

bool detail::static_root_access::is_indexed_directory(
    const static_root& root, std::string_view relative_path) noexcept {
    if (!has_directory_index(root)) {
        return false;
    }
    return contains_static_directory(root.state_->directories_, relative_path);
}

std::uint64_t detail::static_root_access::fingerprint(const static_root& root) noexcept {
    return root.state_->fingerprint_;
}

void detail::static_root_access::acquire_binding(const static_root& root) noexcept {
    ++root.state_->active_bindings_;
}

void detail::static_root_access::release_binding(const static_root& root) noexcept {
    if (root.state_->active_bindings_ == 0) {
        std::terminate();
    }
    --root.state_->active_bindings_;
}

bool detail::static_root_access::has_active_bindings(const static_root& root) noexcept {
    return root.state_->active_bindings_ != 0;
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
    auto* const resource = detail::process_resource();
    const std::array<http_content_coding, 3> codings{
        http_content_coding::brotli,
        http_content_coding::zstd,
        http_content_coding::gzip,
    };
    const bool enabled[] = {
        options.brotli_,
        options.zstd_,
        options.gzip_,
    };
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
        for (std::size_t i = 0; i < codings.size(); ++i) {
            if (!enabled[i]) {
                continue;
            }
            const auto coding = codings[i];
            if (find_fresh_sidecar_entry(state_value, entry, coding) != nullptr) {
                continue;
            }
            if (previous_entry != nullptr) {
                if (const auto* previous_variant = find_memory_variant(*previous_entry, coding);
                    previous_variant != nullptr) {
                    copy_memory_variant(entry, *previous_variant, resource);
                    continue;
                }
            }
            if (!plain.has_value()) {
                plain.emplace(read_stable_static_root_entry_bytes(entry, resource));
            }
            if (plain->empty()) {
                continue;
            }
            auto encoded = encode_http_content(
                coding, *plain, {.max_encoded_bytes_ = plain->size() - 1, .resource_ = resource});
            if (auto* content = encoded.encoded(); content != nullptr) {
                store_precompressed_variant(entry, coding, std::move(*content).take_bytes(),
                    emit_response_validators, resource);
            }
        }
    }
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
    for (std::size_t i = 0; i < lhs_config.mime_types_.size(); ++i) {
        if (lhs_config.mime_types_[i].extension_ != rhs_config.mime_types_[i].extension_ ||
            lhs_config.mime_types_[i].content_type_ != rhs_config.mime_types_[i].content_type_) {
            return false;
        }
    }
    for (std::size_t i = 0; i < lhs.entries_.size(); ++i) {
        if (!same_static_root_entry(lhs.entries_[i], rhs.entries_[i])) {
            return false;
        }
    }
    return true;
}

struct static_root::prepared_construction_type final {
    std::filesystem::path canonical_root_;
    detail::static_root_config_storage config_;
};

static_root::prepared_construction_type static_root::prepare_construction(
    const std::filesystem::path& root, static_root_options options) {
    detail::validate_static_root_options(options);
    return prepared_construction_type{
        .canonical_root_ = canonical_static_root_path(root),
        .config_ = detail::store_validated_static_root_config(options, detail::process_resource()),
    };
}

static_root::prepared_construction_type static_root::prepare_construction(
    const std::filesystem::path& root, const detail::static_root_config_storage& config) {
    return prepared_construction_type{
        .canonical_root_ = canonical_static_root_path(root),
        .config_ = detail::static_root_config_storage(config, detail::process_resource()),
    };
}

static_root::prepared_construction_type static_root::prepare_construction(
    const std::filesystem::path& root, detail::static_root_config_storage&& config) {
    auto canonical_root = canonical_static_root_path(root);
    if (config.cache_control_.get_allocator().resource() != detail::process_resource()) {
        return prepared_construction_type{
            .canonical_root_ = std::move(canonical_root),
            .config_ = detail::static_root_config_storage(config, detail::process_resource()),
        };
    }
    return prepared_construction_type{
        .canonical_root_ = std::move(canonical_root),
        .config_ = std::move(config),
    };
}

std::unique_ptr<static_root, detail::pmr_object_deleter<static_root>>
detail::static_root_access::construct(
    std::pmr::memory_resource* object_resource, static_root::prepared_construction_type prepared) {
    auto* const resource = pmr_resource_or_default(object_resource);
    auto* const storage = resource->allocate(sizeof(static_root), alignof(static_root));
    try {
        auto* const result_value = ::new (storage) static_root(std::move(prepared));
        return std::unique_ptr<static_root, pmr_object_deleter<static_root>>(
            result_value, pmr_object_deleter<static_root>{resource});
    } catch (...) {
        resource->deallocate(storage, sizeof(static_root), alignof(static_root));
        throw;
    }
}

std::unique_ptr<static_root, detail::pmr_object_deleter<static_root>> detail::static_root_access::make(
    std::pmr::memory_resource* object_resource, const std::filesystem::path& root,
    const static_root_config_storage& config) {
    return construct(object_resource, static_root::prepare_construction(root, config));
}

std::unique_ptr<static_root, detail::pmr_object_deleter<static_root>> detail::static_root_access::make(
    std::pmr::memory_resource* object_resource, const std::filesystem::path& root,
    static_root_config_storage&& config) {
    return construct(object_resource, static_root::prepare_construction(root, std::move(config)));
}

std::unique_ptr<static_root, detail::pmr_object_deleter<static_root>> detail::static_root_access::clone(
    std::pmr::memory_resource* object_resource, const static_root& source_value) {
    return make(object_resource, source_value.path(), source_value.state_->config_);
}

static_root::static_root(const std::filesystem::path& root, static_root_options options)
    : static_root(prepare_construction(root, std::move(options))) {}

static_root::static_root(prepared_construction_type prepared)
    : state_(make_static_root_state(std::move(prepared.config_))) {
    const auto& canonical_root = prepared.canonical_root_;
    std::error_code ec;
    auto& state_value = *state_;
    ruvia::assign_native_path(state_value.root_, canonical_root);

    auto* const upstream = detail::process_resource();
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
        const auto relative_utf8 = detail::static_file_utf8_path(
            file_path.lexically_relative(canonical_root), upstream);
        std::pmr::string relative(as_chars(std::as_bytes(std::span(relative_utf8))), upstream);
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
        const auto extension = detail::lower_static_file_extension(file_path, upstream);
        const bool directly_servable = detail::file_type_allowed(extension, config);
        bool usable_as_sidecar = false;
        if (!directly_servable && is_precompressed_sidecar_extension(extension)) {
            usable_as_sidecar = detail::file_type_allowed(
                detail::lower_static_file_extension(file_path.stem(), upstream), config);
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
        ruvia::assign_native_path(entry.file_path_, file_path);
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
    state_value.fingerprint_ = static_root_fingerprint(state_value);
}

static_root::~static_root() = default;

void static_root::state_deleter_type::operator()(detail::static_root_state* state_value) const noexcept {
    if (state_value->active_bindings_ != 0) {
        // A configured binding is the lifetime lease for this immutable
        // snapshot. Destroying the state first would leave its move-only
        // binding with a dangling pointer and make the eventual release
        // undefined behavior.
        std::terminate();
    }
    detail::destroy_pmr_object(state_value, detail::process_resource());
}

std::filesystem::path static_root::path() const {
    return ruvia::make_path_from_native_path(state_->root_);
}

}  // namespace ruvia
