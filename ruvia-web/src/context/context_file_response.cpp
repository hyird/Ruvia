#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
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

#include "context/context_access.h"
#include "context/context_response_state.h"
#include "context/context_services.h"
#include "http/secure_token.h"
#include "http/static_file_metadata.h"
#include "http/static_file_variant.h"
#include "http/static_path_normalization.h"
#include "http/static_root_index.h"
#include "server/http_native_file.h"
#include "server/http_response_compression.h"

namespace ruvia {
namespace {

// The NUL-terminated native form of a borrowed file path, or an empty view
// when the path cannot name a file (empty, an embedded NUL, or text that is
// not UTF-8 on Windows). A borrowed std::filesystem::path already is one; text
// is terminated, and on Windows widened, into `storage`.
[[nodiscard]] http_native_path_view_type native_file_path(
    borrowed_file_path path, http_native_path_string_type& storage) {
    if (const auto native = path.native(); !native.empty()) {
        return native;
    }
    const auto text = path.text();
    if (text.empty() || text.find('\0') != std::string_view::npos) {
        return {};
    }
#if defined(_WIN32)
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const auto text_length = static_cast<int>(text.size());
    const auto wide_length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, nullptr, 0);
    if (wide_length <= 0) {
        return {};
    }
    storage.resize(static_cast<std::size_t>(wide_length));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, storage.data(),
            wide_length) != wide_length) {
        return {};
    }
#else
    storage.assign(text.data(), text.size());
#endif
    return storage;
}

inline constexpr std::size_t file_response_header_reserve = 7;

// Names the response file by a borrowed native path that outlives
// make_file_response: a static_root index entry or context::file's options.
// http_response copies the path into its own storage, so neither borrow
// reaches the returned body. A response may also carry a handle opened for
// it; the body takes it so the transport never reopens the path.
class file_response_path final {
public:
    enum class inline_read : std::uint8_t {
        // The transport reads or sends the bytes and checks the handle (or the
        // file it opens) against the identity first.
        transport,
        // The handle's metadata is the snapshot this response was framed
        // from, so a small body is read from it right here, the handle is
        // closed, and the bytes travel with the response.
        framing_snapshot,
    };

    // `path` must be NUL-terminated (a c_str() or path::native() view).
    file_response_path(http_native_path_view_type path, http_response_file_identity identity,
        http_response_file_handle opened = {}, inline_read read_policy = inline_read::transport)
        : path_(path),
          identity_(identity),
          opened_(std::move(opened)),
          read_policy_(read_policy) {
        if (path_.empty()) {
            throw std::logic_error("file response has no native path");
        }
    }

    [[nodiscard]] std::string_view guessed_content_type() const noexcept {
        return detail::guess_static_file_content_type_from_path_view(path_);
    }

    [[nodiscard]] http_response_file_identity identity() const noexcept {
        return identity_;
    }

    void validate_current(std::uint64_t size) const {
        if (!identity_.requires_validation()) {
            return;
        }
        std::error_code ec;
#if defined(__unix__) || defined(_WIN32)
        const auto snapshot = opened_ ? detail::snapshot_native_file_handle(detail::borrowed_native_file(opened_), ec)
                                      : detail::snapshot_response_file(path_.data(), ec);
#else
        const auto snapshot = detail::snapshot_response_file(path_.data(), ec);
#endif
        if (ec || snapshot.identity_ != identity_ || snapshot.size_ != size) {
            throw http_error({.status_ = ruvia::http_status::internal_server_error,
                .code_ = "static_file_changed",
                .message_ = "static file changed since its index was built"});
        }
    }

    void set_body(http_response& response, std::pmr::memory_resource* resource, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length) {
#if defined(__unix__) || defined(_WIN32)
        if (read_policy_ == inline_read::framing_snapshot && opened_ &&
            length <= detail::inline_response_file_max_bytes) {
            std::pmr::string bytes(resource);
            bytes.resize(static_cast<std::size_t>(length));
            if (detail::read_native_file_at(
                    detail::borrowed_native_file(opened_), offset, bytes.data(), bytes.size())) {
                throw http_error({.status_ = ruvia::http_status::internal_server_error,
                    .code_ = "file_read_failed",
                    .message_ = "file could not be read"});
            }
            // The head was framed from this handle's earlier snapshot: publish
            // the bytes only if the same handle still matches it after the read.
            validate_current(size);
            opened_ = {};
            response.owned_body(std::move(bytes));
            return;
        }
#else
        static_cast<void>(resource);
#endif
        response.file_body(path_, size, offset, length, identity_, std::move(opened_));
    }

    void set_multipart_body(http_response& response, std::uint64_t size,
        http_multipart_byte_range_plan&& plan) {
        response.multipart_file_body(path_, size, identity_, std::move(plan), std::move(opened_));
    }

private:
    http_native_path_view_type path_;
    http_response_file_identity identity_;
    http_response_file_handle opened_;
    inline_read read_policy_;
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
            path->set_body(response, resource, size, offset, length);
            return;
        }
        const auto bytes_value = std::get<std::string_view>(value_);
        if (offset > bytes_value.size() || length > bytes_value.size() - offset) {
            throw std::logic_error("static memory response slice is out of range");
        }
        // Copied, not borrowed: the snapshot is not guaranteed to outlive the
        // response. A document-root lease ends when dispatch returns, before
        // the transport writes the body, and a caller's static_root carries no
        // lease at all. Borrowing would need a per-response reference on the
        // snapshot (shared ownership on the hot path) or a lease release hook
        // inside http_response.
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

// The validators one file response sends and evaluates preconditions
// against. The views borrow the caller's storage or the source's precomputed
// values.
struct file_validators final {
    std::string_view etag_;
    // Empty when the date is unavailable.
    std::string_view last_modified_;
    std::time_t modified_seconds_{0};
};

[[nodiscard]] file_validators compute_file_validators(const file_response_source& source_value,
    std::pmr::string& etag_storage, std::array<char, http_imf_fixdate_size>& last_modified_storage) {
    file_validators validators;
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
    validators.modified_seconds_ = last_modified_is_actual ? source_value.modified_seconds_ : response_seconds;
    if (source_value.response_validators_ == static_response_validator_policy::emit) {
        if (source_value.precomputed_etag_.empty()) {
            etag_storage = detail::make_static_file_snapshot_etag(etag_storage.get_allocator().resource(),
                source_value.size_, source_value.modified_token_, source_value.body_.identity());
            validators.etag_ = etag_storage;
        } else {
            validators.etag_ = source_value.precomputed_etag_;
        }
    }
    // Date preconditions also apply when response validators are not emitted.
    // An unrepresentable date is unavailable, not a truncated wire validator.
    if (has_response_time) {
        if (source_value.precomputed_last_modified_.empty() || !last_modified_is_actual) {
            if (const auto date = format_http_date(validators.modified_seconds_)) {
                last_modified_storage = *date;
                validators.last_modified_ =
                    std::string_view(last_modified_storage.data(), last_modified_storage.size());
            }
        } else {
            validators.last_modified_ = source_value.precomputed_last_modified_;
        }
    }
    return validators;
}

// RFC 9110 15.4.5: a 304 carries the validators and cache-guiding fields a
// 200 would, but no other representation metadata. Last-Modified guides the
// cache update only when no ETag is sent.
void add_file_headers(http_response& response, const file_response_source& source_value,
    const file_validators& validators, bool not_modified = false) {
    response.reserve_headers(file_response_header_reserve);
    if (!not_modified) {
        if (source_value.content_type_.empty()) {
            response.header("Content-Type", source_value.body_.guessed_content_type());
        } else {
            response.header("Content-Type", source_value.content_type_);
        }
    }
    if (!source_value.cache_control_.empty()) {
        response.header("Cache-Control", source_value.cache_control_);
    }
    // A precompressed variant carries the original Content-Type with the
    // encoding declared here.
    if (!not_modified && source_value.content_coding_ != http_content_coding::identity) {
        response.header("Content-Encoding", http_content_coding_token(source_value.content_coding_));
    }
    if (!not_modified && source_value.range_requests_ == static_range_request_policy::honor) {
        response.header("Accept-Ranges", "bytes");
    }
    if (source_value.response_validators_ == static_response_validator_policy::emit) {
        response.header("ETag", validators.etag_);
        if (!not_modified && !validators.last_modified_.empty()) {
            response.header("Last-Modified", validators.last_modified_);
        }
    }
}

[[nodiscard]] http_response make_file_response(const context& context_value, file_response_source source_value) {
    const auto& request = detail::context_access::request(context_value);
    std::pmr::string etag_storage(context_value.pool());
    std::array<char, http_imf_fixdate_size> last_modified_storage{};
    const auto validators = compute_file_validators(source_value, etag_storage, last_modified_storage);
    const bool honor_range_requests = source_value.range_requests_ == static_range_request_policy::honor;
    auto apply_file_response_state = [&](http_response& response,
                                         std::optional<http_status_code> status_code) {
        detail::context_access::apply_response_state(context_value, response, status_code);
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
    auto make_not_modified_response = [&](http_status_code status_code) {
        validate_indexed_file_for_bodyless_response();
        http_response response({.resource_ = context_value.arena()});
        add_file_headers(response, source_value, validators, true);
        apply_file_response_state(response, status_code);
        return response;
    };
    auto make_full_file_response = [&](std::optional<http_status_code> status_code) {
        http_response response({.resource_ = context_value.arena()});
        add_file_headers(response, source_value, validators);
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
        add_file_headers(response, source_value, validators);
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
            .etag_ = validators.etag_,
            .last_modified_ = validators.last_modified_.empty() ? std::nullopt
                                                                : std::optional(validators.modified_seconds_),
            // Filesystem mtime cannot establish that a representation changed
            // at most once during the second in its Last-Modified value.
            .strong_date_validator_ = false,
        },
        http_representation_response_options{
            .normal_status_ = detail::context_access::response_status(context_value),
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
        return make_not_modified_response(plan.status());
    }
    if (plan.range_unsatisfiable()) {
        validate_indexed_file_for_bodyless_response();
        http_response response({.resource_ = context_value.arena()});
        response.content_range_unsatisfied(source_value.size_);
        add_file_headers(response, source_value, validators);
        apply_file_response_state(response, plan.status());
        return response;
    }
    if (const auto* ranges = plan.multipart_ranges()) {
        return make_multipart_response(*ranges);
    }
    if (const auto* range = plan.partial()) {
        http_response response({.resource_ = context_value.arena()});
        add_file_headers(response, source_value, validators);
        response.content_range(range->offset(), range->length(), source_value.size_);
        set_file_body(response, range->offset(), range->length());
        apply_file_response_state(response, plan.status());
        return response;
    }
    return make_full_file_response(plan.status());
}

}  // namespace

http_response context::file(file_response_options options) const {
    const auto not_found = [] {
        return http_error({.status_ = ruvia::http_status::not_found,
            .code_ = "not_found",
            .message_ = "file not found"});
    };
    // Text paths are terminated (and widened) in the arena; it outlives the
    // response construction below, and the response copies its path.
    http_native_path_string_type text_path(arena());
    const auto native_path = native_file_path(options.path_, text_path);
    if (native_path.empty()) {
        throw not_found();
    }
    // Open once: the response is framed from this handle's metadata, and the
    // body comes from the same handle, so no path is resolved twice. A small
    // body is read right away and the handle closed; HEAD sends no bytes.
    const auto read_policy = request_.known_method() == http_known_method::head
                                 ? file_response_path::inline_read::transport
                                 : file_response_path::inline_read::framing_snapshot;
    std::error_code ec;
    auto opened = detail::open_response_file(native_path.data(), ec);
    if (ec) {
        throw not_found();
    }
    const auto& snapshot = opened.snapshot_;

    const auto content_type_value = options.content_type_.view();
    return make_file_response(*this,
        file_response_source{
            .body_ = file_response_body_source::file(file_response_path(
                native_path, snapshot.identity_, std::move(opened.handle_), read_policy)),
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
        });
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
    // An encoded '/' or '\' would split one request path segment into several
    // file path segments, so the file served would differ from the path every
    // route and path-scoped middleware matched; reject it like the NUL.
    std::optional<std::pmr::string> decoded_path;
    if (has_url_encoding(relative_path, url_decode_mode::percent)) {
        decoded_path = decode_url_component(
            relative_path, {.mode_ = url_decode_mode::percent, .resource_ = pool()});
    }
    const std::string_view lookup_path =
        decoded_path.has_value() ? std::string_view(*decoded_path) : relative_path;
    std::pmr::string relative_storage(pool());
    const auto normalized = detail::normalize_static_relative_path(lookup_path, relative_storage);
    if (decoded_path.has_value() &&
        normalized.separator_count_ != static_cast<std::size_t>(std::ranges::count_if(
                                           relative_path, [](char c) noexcept { return c == '/' || c == '\\'; }))) {
        detail::throw_invalid_static_file_path();
    }
    auto relative = normalized.path_;

    if (relative.empty() && !detail::static_root_access::has_directory_index(root)) {
        detail::throw_invalid_static_file_path();
    }

    auto entry_value = detail::static_root_access::find(root, relative);
    if (!entry_value.has_value() && detail::static_root_access::is_indexed_directory(root, relative)) {
        // A normalized path never ends with '/'.
        if (relative.data() != relative_storage.data()) {
            relative_storage.assign(relative.data(), relative.size());
        }
        if (!relative_storage.empty()) {
            relative_storage.push_back('/');
        }
        const auto index_file = detail::static_root_access::index_file(root);
        relative_storage.append(index_file.data(), index_file.size());
        relative = relative_storage;
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
        throw http_error(detail::http_not_acceptable_coding_error());
    }
    const auto& served_entry = served->entry();
    const auto* const memory_variant = served->memory_variant();
    // A document-root snapshot keeps small files' bytes; they describe exactly
    // the indexed metadata, so the response is framed and sent from memory.
    const auto* const resident_bytes = memory_variant == nullptr ? served_entry.resident_bytes() : nullptr;
    const auto response_size = memory_variant == nullptr ? served_entry.size() : memory_variant->size();
    const auto response_modified_token =
        memory_variant == nullptr ? served_entry.modified_token() : memory_variant->modified_token();
    const auto response_modified_seconds =
        memory_variant == nullptr ? served_entry.modified_seconds() : memory_variant->modified_seconds();
    const auto response_etag = memory_variant == nullptr ? served_entry.etag() : memory_variant->etag();
    const auto response_last_modified =
        memory_variant == nullptr ? served_entry.last_modified() : memory_variant->last_modified();

    return make_file_response(*this,
        file_response_source{
            .body_ = memory_variant != nullptr ? file_response_body_source::bytes_value(memory_variant->bytes())
                     : resident_bytes != nullptr
                         ? file_response_body_source::bytes_value(*resident_bytes)
                         : file_response_body_source::file(
                               file_response_path(http_native_path_view_type(served_entry.file_path()),
                                   served_entry.identity(), detail::static_root_access::open_entry(root, served_entry))),
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
            .validate_indexed_file_for_bodyless_response_ = memory_variant == nullptr && resident_bytes == nullptr,
        });
}

}  // namespace ruvia
