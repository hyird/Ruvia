#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_date.h"
#include "ruvia/http/http_representation_response_plan.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"

#include "context/context_response_state.h"
#include "context/context_services.h"
#include "http/secure_token.h"
#include "http/static_file_metadata.h"
#include "http/static_file_variant.h"
#include "http/static_path_normalization.h"
#include "http/static_root_index.h"
#include "server/http_native_file.h"

namespace ruvia {
namespace {

inline constexpr std::size_t file_response_header_reserve = 7;

// The path travels by value until http_response takes its own copy. static_root is
// a public value whose lifetime is not coupled to the returned response, so an
// indexed entry must never leak its internal native-path pointer into the body.
class file_response_path final {
public:
    [[nodiscard]] static file_response_path copying(
        std::filesystem::path path, http_response_file_identity identity) {
        return file_response_path(std::move(path), identity);
    }

    [[nodiscard]] static file_response_path copying_native(
        const ruvia::native_path_char_type* path, http_response_file_identity identity) {
        if (path == nullptr || *path == ruvia::native_path_char_type{}) {
            throw std::logic_error("static file entry has no native path");
        }
        return copying(std::filesystem::path(path), identity);
    }

    [[nodiscard]] std::string_view guessed_content_type() const noexcept {
        return detail::guess_static_file_content_type(path_);
    }

    [[nodiscard]] http_response_file_identity identity() const noexcept {
        return identity_;
    }

    void validate_current(std::uint64_t size) const {
        if (!identity_.requires_validation()) {
            return;
        }
        std::error_code ec;
        const auto snapshot = detail::snapshot_response_file(path_.c_str(), ec);
        if (ec || snapshot.identity_ != identity_ || snapshot.size_ != size) {
            throw http_error({.status_ = ruvia::http_status::internal_server_error,
                .code_ = "static_file_changed",
                .message_ = "static file changed since its index was built"});
        }
    }

    void set_body(
        http_response& response, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
        response.file_body(take_path(), size, offset, length, identity_);
    }

    void set_full_body(http_response& response, std::uint64_t size) {
        set_body(response, size, 0, size);
    }

    void set_multipart_body(http_response& response, std::uint64_t size,
        http_multipart_byte_range_plan&& plan) {
        response.multipart_file_body(take_path(), size, identity_, std::move(plan));
    }

private:
    explicit file_response_path(
        std::filesystem::path path, http_response_file_identity identity) noexcept
        : path_(std::move(path)),
          identity_(identity) {}

    [[nodiscard]] std::filesystem::path take_path() {
        if (consumed_) {
            throw std::logic_error("file response path was consumed more than once");
        }
        consumed_ = true;
        return std::move(path_);
    }

    std::filesystem::path path_;
    http_response_file_identity identity_;
    bool consumed_{false};
};

class file_response_body_source final {
public:
    [[nodiscard]] static file_response_body_source file(file_response_path path) {
        return file_response_body_source(std::move(path));
    }

    [[nodiscard]] static file_response_body_source bytes_value(std::string_view bytes_value) {
        return file_response_body_source(bytes_value);
    }

    [[nodiscard]] std::string_view guessed_content_type() const noexcept {
        if (const auto* path = std::get_if<file_response_path>(&value_)) {
            return path->guessed_content_type();
        }
        return "application/octet-stream";
    }

    [[nodiscard]] http_response_file_identity identity() const noexcept {
        if (const auto* path = std::get_if<file_response_path>(&value_)) {
            return path->identity();
        }
        return http_response_file_identity::unchecked();
    }

    void validate_current(std::uint64_t size) const {
        if (const auto* path = std::get_if<file_response_path>(&value_)) {
            path->validate_current(size);
        }
    }

    void set_body(http_response& response, std::pmr::memory_resource* resource, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length) {
        if (auto* path = std::get_if<file_response_path>(&value_)) {
            path->set_body(response, size, offset, length);
            return;
        }
        const auto bytes_value = std::get<std::string_view>(value_);
        if (offset > bytes_value.size() || length > bytes_value.size() - offset) {
            throw std::logic_error("static memory response slice is out of range");
        }
        std::pmr::string owned(
            bytes_value.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)),
            resource);
        response.owned_body(std::move(owned));
    }

    void set_full_body(
        http_response& response, std::pmr::memory_resource* resource, std::uint64_t size) {
        set_body(response, resource, size, 0, size);
    }

    void set_multipart_body(http_response& response, std::pmr::memory_resource* resource,
        std::uint64_t size, http_multipart_byte_range_plan&& plan) {
        if (auto* path = std::get_if<file_response_path>(&value_)) {
            path->set_multipart_body(response, size, std::move(plan));
            return;
        }
        const auto source_value = std::get<std::string_view>(value_);
        std::pmr::string body(resource);
        for (const auto& segment : plan.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                body.append(source_value.substr(static_cast<std::size_t>(segment.file_offset_),
                    static_cast<std::size_t>(segment.file_length_)));
            } else {
                body.append(plan.metadata().substr(segment.metadata_offset_, segment.metadata_length_));
            }
        }
        response.owned_body(std::move(body));
    }

private:
    explicit file_response_body_source(file_response_path path) noexcept
        : value_(std::move(path)) {}

    explicit file_response_body_source(std::string_view bytes_value) noexcept
        : value_(bytes_value) {}

    std::variant<file_response_path, std::string_view> value_;
};

// What one file response describes: which bytes, when they last changed, and
// the policy the serving route attached to them. Fifteen positional arguments
// at a call site said none of that; designated initializers do.
struct file_response_source final {
    file_response_body_source body_;
    std::uint64_t size_{0};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    std::string_view content_type_;
    std::string_view cache_control_;
    static_range_request_policy range_requests_{static_range_request_policy::ignore};
    static_response_validator_policy response_validators_{static_response_validator_policy::omit};
    std::string_view precomputed_etag_;
    std::string_view precomputed_last_modified_;
    http_content_coding content_coding_{http_content_coding::identity};
    bool negotiates_encoding_{false};
    bool validate_indexed_file_for_bodyless_response_{false};
};

template <typename apply_response_state_type>
[[nodiscard]] http_response make_file_response(const context& context_value, const http_request& request,
    http_status_code normal_status, file_response_source source_value, apply_response_state_type apply_response_state) {
    std::pmr::string etag_storage(context_value.pool());
    std::array<char, http_imf_fixdate_size> last_modified_storage{};
    std::string_view etag;
    std::string_view last_modified;
    const bool honor_range_requests = source_value.range_requests_ == static_range_request_policy::honor;
    const bool emit_response_validators =
        source_value.response_validators_ == static_response_validator_policy::emit;
    // RFC 9110 §8.8.2.1 forbids an origin server from emitting a
    // Last-Modified value later than the message origination time. Filesystems
    // can legitimately contain future mtimes (clock skew, archives, or an
    // explicit timestamp), so use this response's current second for the wire
    // validator and every date precondition evaluated against it. The clamped
    // value is not the representation's actual validator and therefore cannot
    // be a strong If-Range validator (RFC 9110 §13.1.5).
    const auto response_seconds = std::time(nullptr);
    const bool has_response_time = response_seconds != std::time_t{-1};
    const bool last_modified_is_actual = has_response_time && source_value.modified_seconds_ <= response_seconds;
    const auto validator_modified_seconds =
        last_modified_is_actual ? source_value.modified_seconds_ : response_seconds;
    if (emit_response_validators) {
        if (source_value.precomputed_etag_.empty()) {
            etag_storage = detail::make_static_file_snapshot_etag(
                context_value.pool(), source_value.size_, source_value.modified_token_, source_value.body_.identity());
            etag = etag_storage;
        } else {
            etag = source_value.precomputed_etag_;
        }
    }
    // Date preconditions also apply when response validators are not emitted.
    // An unrepresentable date is unavailable, not a truncated wire validator.
    if (has_response_time) {
        if (source_value.precomputed_last_modified_.empty() || !last_modified_is_actual) {
            if (const auto date = format_http_date(validator_modified_seconds)) {
                last_modified_storage = *date;
                last_modified = std::string_view(last_modified_storage.data(), last_modified_storage.size());
            }
        } else {
            last_modified = source_value.precomputed_last_modified_;
        }
    }

    auto add_file_headers = [&](http_response& response) {
        response.reserve_headers(file_response_header_reserve);
        if (source_value.content_type_.empty()) {
            response.header("Content-Type", source_value.body_.guessed_content_type());
        } else {
            response.header("Content-Type", source_value.content_type_);
        }
        if (!source_value.cache_control_.empty()) {
            response.header("Cache-Control", source_value.cache_control_);
        }
        // A precompressed variant carries the original Content-Type with the
        // encoding declared here.
        if (source_value.content_coding_ != http_content_coding::identity) {
            response.header("Content-Encoding", http_content_coding_token(source_value.content_coding_));
        }
        if (honor_range_requests) {
            response.header("Accept-Ranges", "bytes");
        }
        if (emit_response_validators) {
            response.header("ETag", etag);
            if (!last_modified.empty()) {
                response.header("Last-Modified", last_modified);
            }
        }
    };
    auto apply_file_response_state = [&](http_response& response,
                                         std::optional<http_status_code> status_code) {
        apply_response_state(response, status_code);
        // Declare the negotiation dimension after context response metadata is
        // applied. A caller-provided Vary value must be merged, not allowed to
        // overwrite Accept-Encoding and make differently encoded variants share
        // one cache entry (RFC 9110 12.5.5 / RFC 9111 4.1). context::file does no
        // Accept-Encoding negotiation and stays Vary-free.
        if (source_value.negotiates_encoding_) {
            response.add_vary_token("Accept-Encoding");
        }
    };
    auto set_file_body = [&](http_response& response, std::uint64_t offset, std::uint64_t length) {
        source_value.body_.set_body(response, context_value.arena(), source_value.size_, offset, length);
    };
    auto set_full_file_body = [&](http_response& response) {
        source_value.body_.set_full_body(response, context_value.arena(), source_value.size_);
    };
    // Body writes validate the indexed identity when opening the file. Responses
    // without file bytes (HEAD, 304, 412, 416) need that check here instead.
    bool indexed_file_validated = false;
    auto validate_indexed_file_for_bodyless_response = [&] {
        if (source_value.validate_indexed_file_for_bodyless_response_ && !indexed_file_validated) {
            source_value.body_.validate_current(source_value.size_);
            indexed_file_validated = true;
        }
    };
    auto make_header_only_response = [&](std::optional<http_status_code> status_code) {
        validate_indexed_file_for_bodyless_response();
        http_response response({.resource_ = context_value.arena()});
        add_file_headers(response);
        apply_file_response_state(response, status_code);
        return response;
    };
    auto make_full_file_response = [&](std::optional<http_status_code> status_code) {
        http_response response({.resource_ = context_value.arena()});
        add_file_headers(response);
        set_full_file_body(response);
        apply_file_response_state(response, status_code);
        return response;
    };
    auto make_multipart_response = [&](const http_byte_range_set& ranges) {
        std::array<char, 48> boundary_token{};
        const auto token_result = detail::generate_secure_token(boundary_token);
        const auto* token = token_result.ready();
        if (token == nullptr) {
            throw std::runtime_error("secure multipart boundary generation failed");
        }
        std::pmr::string boundary(token->value(), context_value.arena());
        http_response response({.resource_ = context_value.arena()});
        add_file_headers(response);
        apply_file_response_state(response, http_status::partial_content);
        const auto media_type = response.header("Content-Type");
        const auto selected_media_type = media_type.value_or(source_value.body_.guessed_content_type());
        const auto content_encoding = source_value.content_coding_ == http_content_coding::identity
                                          ? std::string_view{}
                                          : http_content_coding_token(source_value.content_coding_);
        auto multipart_plan = make_http_multipart_byte_range_plan(ranges, source_value.size_,
            selected_media_type, boundary, content_encoding, context_value.arena());

        // The outer representation is multipart, not the encoded file bytes.
        // Carry the selected representation's coding on each part instead.
        response.header("Content-Type", multipart_plan.content_type());
        response.remove_header("Content-Range");
        response.remove_header("Content-Encoding");
        response.remove_header("Content-Length");
        source_value.body_.set_multipart_body(response, context_value.arena(), source_value.size_, std::move(multipart_plan));
        return response;
    };

    if (request.known_method() == http_known_method::head) {
        validate_indexed_file_for_bodyless_response();
    }
    const auto plan = plan_http_representation_response(request,
        http_selected_representation_metadata{
            .length_ = source_value.size_,
            .etag_ = etag,
            .last_modified_ = last_modified.empty() ? std::nullopt : std::optional(validator_modified_seconds),
            .strong_date_validator_ = emit_response_validators && last_modified_is_actual && !last_modified.empty(),
        },
        http_representation_response_options{
            .normal_status_ = normal_status,
            .range_policy_ = honor_range_requests ? http_range_request_policy::honor_byte_ranges
                                                  : http_range_request_policy::ignore,
        });
    if (plan.precondition_failed()) {
        validate_indexed_file_for_bodyless_response();
        throw http_error({.status_ = plan.status(),
            .code_ = "precondition_failed",
            .message_ = "file precondition failed"});
    }
    if (plan.not_modified()) {
        return make_header_only_response(plan.status());
    }
    if (plan.range_unsatisfiable()) {
        validate_indexed_file_for_bodyless_response();
        http_response response({.resource_ = context_value.arena()});
        response.content_range_unsatisfied(source_value.size_);
        add_file_headers(response);
        apply_file_response_state(response, plan.status());
        return response;
    }
    if (const auto* ranges = plan.multipart_ranges()) {
        return make_multipart_response(*ranges);
    }
    if (const auto* range = plan.partial()) {
        http_response response({.resource_ = context_value.arena()});
        add_file_headers(response);
        response.content_range(range->offset(), range->length(), source_value.size_);
        set_file_body(response, range->offset(), range->length());
        apply_file_response_state(response, plan.status());
        return response;
    }
    return make_full_file_response(plan.status());
}

}  // namespace

http_response context::file(file_response_options options) const {
    std::error_code ec;
    const auto snapshot = detail::snapshot_response_file(options.path_.c_str(), ec);
    if (ec) {
        throw http_error({.status_ = ruvia::http_status::not_found,
            .code_ = "not_found",
            .message_ = "file not found"});
    }

    const auto content_type_value = options.content_type_.view();
    const auto apply_state = [this](
                                 http_response& response, std::optional<http_status_code> status_code) {
        apply_response_state(response, status_code);
    };
    return make_file_response(*this, request_, response_state().active_response().status(),
        file_response_source{
            .body_ = file_response_body_source::file(
                file_response_path::copying(std::move(options.path_), snapshot.identity_)),
            .size_ = snapshot.size_,
            .modified_token_ = snapshot.modified_token_,
            .modified_seconds_ = snapshot.modified_seconds_,
            .content_type_ = content_type_value,
            .cache_control_ = {},
            .range_requests_ = static_range_request_policy::honor,
            .response_validators_ = static_response_validator_policy::emit,
            .precomputed_etag_ = {},
            .precomputed_last_modified_ = {},
            .content_coding_ = http_content_coding::identity,
            .negotiates_encoding_ = false,
        },
        apply_state);
}

http_response context::static_file(const static_root& root, static_file_response_options options) const {
    const auto mode = services().precompressed_static_files() ? detail::static_file_selection_mode::precompressed
                                                              : detail::static_file_selection_mode::identity_only;
    return static_file(root, options, mode);
}

http_response context::static_file(const static_root& root, static_file_response_options options,
    detail::static_file_selection_mode mode) const {
    const auto relative_path = options.relative_path_.view();
    const auto content_type_value = options.content_type_.view();
    // Percent-decode the request path before matching it against the static index,
    // whose keys are the real (decoded) on-disk names -- so a file whose name holds
    // an encoded octet (a space "%20", UTF-8, parentheses, ...) resolves instead of
    // 404ing, per RFC 3986 2.1 / 6.2.2.2 percent-encoding equivalence. Decoding is
    // safe here: normalize_static_relative_path still clamps ".." at the root and
    // rejects absolute paths, and the lookup is a byte-exact index compare that
    // never joins the client path onto the filesystem, so the worst case is a miss
    // (404). A "%00" would inject a NUL that cannot occur in a filename, so reject
    // it; a malformed escape falls back to the raw bytes (which simply miss).
    std::optional<std::pmr::string> decoded_path;
    if (has_url_encoding(relative_path, url_decode_mode::percent)) {
        decoded_path = decode_url_component(
            relative_path, {.mode_ = url_decode_mode::percent, .resource_ = pool()});
    }
    const std::string_view lookup_path =
        decoded_path.has_value() ? std::string_view(*decoded_path) : relative_path;
    if ((lookup_path.find('\0') != std::string_view::npos)) {
        throw http_error({.status_ = ruvia::http_status::forbidden,
            .code_ = "forbidden",
            .message_ = "invalid static file path"});
    }
    auto relative = detail::normalize_static_relative_path(
        lookup_path, std::pmr::polymorphic_allocator<char>(pool()));

    if (relative.empty() && !detail::static_root_access::has_directory_index(root)) {
        throw http_error({.status_ = ruvia::http_status::forbidden,
            .code_ = "forbidden",
            .message_ = "invalid static file path"});
    }

    auto entry_value = detail::static_root_access::find(root, relative);
    if (!entry_value.has_value() && detail::static_root_access::is_indexed_directory(root, relative)) {
        if (!relative.empty() && relative.back() != '/') {
            relative.push_back('/');
        }
        const auto index_file = detail::static_root_access::index_file(root);
        relative.append(index_file.data(), index_file.size());
        entry_value = detail::static_root_access::find(root, relative);
    }
    if (!entry_value.has_value()) {
        throw http_error({.status_ = ruvia::http_status::not_found,
            .code_ = "not_found",
            .message_ = "file not found"});
    }
    const auto& base_entry = *entry_value;

    // Serve a precompressed variant when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    const auto served =
        select_static_file_representation(root, relative, request_, pool(), base_entry, mode);
    if (!served.has_value()) {
        throw http_error({.status_ = ruvia::http_status::not_acceptable,
            .code_ = "not_acceptable",
            .message_ = "no acceptable response content coding"});
    }
    const auto& served_entry = served->entry();
    const auto* const memory_variant = served->memory_variant();
    const auto response_size = memory_variant == nullptr ? served_entry.size() : memory_variant->size();
    const auto response_modified_token =
        memory_variant == nullptr ? served_entry.modified_token() : memory_variant->modified_token();
    const auto response_modified_seconds =
        memory_variant == nullptr ? served_entry.modified_seconds() : memory_variant->modified_seconds();
    const auto response_etag = memory_variant == nullptr ? served_entry.etag() : memory_variant->etag();
    const auto response_last_modified =
        memory_variant == nullptr ? served_entry.last_modified() : memory_variant->last_modified();

    const auto apply_state = [this](
                                 http_response& response, std::optional<http_status_code> status_code) {
        apply_response_state(response, status_code);
    };
    return make_file_response(*this, request_, response_state().active_response().status(),
        file_response_source{
            .body_ = memory_variant == nullptr
                         ? file_response_body_source::file(file_response_path::copying_native(
                               served_entry.file_path(), served_entry.identity()))
                         : file_response_body_source::bytes_value(memory_variant->bytes()),
            .size_ = response_size,
            .modified_token_ = response_modified_token,
            .modified_seconds_ = response_modified_seconds,
            .content_type_ = content_type_value.empty() ? base_entry.content_type() : content_type_value,
            .cache_control_ = base_entry.get_cache_control(),
            .range_requests_ = base_entry.range_requests(),
            .response_validators_ = base_entry.response_validators(),
            .precomputed_etag_ = response_etag,
            .precomputed_last_modified_ = response_last_modified,
            .content_coding_ = served->content_coding(),
            // static_file negotiates the representation by Accept-Encoding.
            .negotiates_encoding_ = true,
            .validate_indexed_file_for_bodyless_response_ = memory_variant == nullptr,
        },
        apply_state);
}

}  // namespace ruvia
