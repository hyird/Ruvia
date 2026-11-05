#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_accept_encoding.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_date.h"
#include "ruvia/http/http_representation_response_plan.h"
#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "context/context_response_state.h"
#include "context/context_services.h"
#include "http/secure_token.h"
#include "http/static_file_metadata.h"
#include "http/static_file_variant.h"
#include "http/static_root_index.h"
#include "server/http_native_file.h"
#include "server/http_response_compression.h"
#include "server/http_server_options.h"
#include "server/response_file_source.h"

namespace ruvia {
namespace {

// Copies the file a borrowed path names into `output` in NUL-terminated
// native form: a borrowed std::filesystem::path's native string as is, text
// terminated and, on Windows, widened. False when the path cannot name a file
// (empty, an embedded NUL, or text that is not UTF-8 on Windows).
[[nodiscard]] bool assign_native_file_path(borrowed_file_path path, http_native_path_string_type& output) {
    if (const auto native = path.native(); !native.empty()) {
        output.assign(native);
        return true;
    }
    const auto text = path.text();
    if (text.empty() || text.find('\0') != std::string_view::npos) {
        return false;
    }
#if defined(_WIN32)
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const auto text_length = static_cast<int>(text.size());
    const auto wide_length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, nullptr, 0);
    if (wide_length <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(wide_length));
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), text_length, output.data(),
            wide_length) != wide_length) {
        return false;
    }
#else
    output.assign(text.data(), text.size());
#endif
    return true;
}

inline constexpr std::size_t file_response_header_reserve = 7;

[[nodiscard]] http_error static_file_changed_error() {
    return http_error({.status_ = ruvia::http_status::internal_server_error,
        .code_ = "static_file_changed",
        .message_ = "static file changed since its index was built"});
}

// context::file's open, run on the blocking pool. The path travels with it in
// the synchronized process resource -- an abandoned open frees it on the pool
// thread -- and comes back with the opened file, so the response is framed
// from the same copy.
struct opened_file_path final {
    http_native_path_string_type path_;
    detail::opened_response_file file_{};
    std::error_code error_{};
};

// Runs a file response's open (and, for an indexed file, its identity check)
// on the worker's blocking pool, never on the worker. The wait is bounded like
// the body reads the transport waits on later: the server write_timeout_ and
// the request's stop token. An abandoned open keeps running on the pool and
// frees what it owns there.
template <typename open_type>
[[nodiscard]] task<blocking_result<std::invoke_result_t<open_type&>>> run_file_open(
    context& context_value, open_type open) {
    return run_blocking(context_value.worker(),
        {.timeout_ = detail::context_access::services(context_value).file_io_timeout(),
            .stop_token_ = context_value.stop_token()},
        std::move(open));
}

// The open's value. An open that never completed is answered as a file body
// read that did not complete: 503 file_read_unavailable, after a real fault
// (no blocking pool, a stalled open) went to the connection failure outlet.
// The callable's own exception is rethrown unchanged.
template <typename value_type>
[[nodiscard]] value_type take_file_open(context& context_value, blocking_result<value_type>&& result) {
    if (result.status() != blocking_status::completed) {
        const auto failure = detail::response_file_failure::read_not_completed(result.status());
        detail::context_access::services(context_value)
            .connection_failure()
            .invoke(context_value.conn().remote().address(), failure.fault());
        std::rethrow_exception(failure.response_error());
    }
    return std::move(result).value();
}

// Names the response file by a borrowed native path that outlives
// make_file_response: a static_root index entry or context::file's opened
// copy. http_response copies the path into its own storage, so neither
// borrow reaches the returned body. The handle opened for the response goes
// with the body, so the transport reads it on the blocking pool and never
// reopens the path.
class file_response_path final {
public:
    // `path` must be NUL-terminated (a c_str() or path::native() view).
    file_response_path(http_native_path_view_type path, http_response_file_identity identity,
        http_response_file_handle opened)
        : path_(path),
          identity_(identity),
          opened_(std::move(opened)) {
        if (path_.empty() || !opened_) {
            throw std::logic_error("file response has no native path or opened handle");
        }
    }

    void set_body(http_response& response, std::uint64_t size, std::uint64_t offset, std::uint64_t length) {
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
};

class file_response_body_source final {
public:
    [[nodiscard]] static file_response_body_source file(file_response_path path) {
        return file_response_body_source(std::move(path));
    }

    [[nodiscard]] static file_response_body_source bytes_value(std::string_view bytes_value) {
        return file_response_body_source(bytes_value);
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
        // Copied, not borrowed: the snapshot is not guaranteed to outlive the
        // response. A document-root lease ends when dispatch returns, before
        // the transport writes the body, and a caller's static_root carries no
        // lease at all. Borrowing would need a per-response reference on the
        // snapshot (shared ownership on the hot path) or a lease release hook
        // inside http_response.
        std::pmr::string owned(
            bytes_value.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length)),
            resource);
        response.file_bytes_body(std::move(owned));
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
        std::size_t total_size = 0;
        for (const auto& segment : plan.segments()) {
            total_size += segment.kind_ == http_multipart_byte_range_plan::segment_kind::file
                              ? static_cast<std::size_t>(segment.file_length_)
                              : segment.metadata_length_;
        }
        std::pmr::string body(resource);
        body.reserve(total_size);
        for (const auto& segment : plan.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                body.append(source_value.substr(static_cast<std::size_t>(segment.file_offset_),
                    static_cast<std::size_t>(segment.file_length_)));
            } else {
                body.append(plan.metadata().substr(segment.metadata_offset_, segment.metadata_length_));
            }
        }
        response.file_bytes_body(std::move(body));
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
// at a call site said none of that; designated initializers do. It is known
// before any file is opened, so the response plan is too.
struct file_response_source final {
    std::uint64_t size_{0};
    std::uint64_t modified_token_{0};
    std::time_t modified_seconds_{0};
    // What a computed ETag digests; unchecked for bytes served from memory.
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    // Never empty: the caller's type, the index entry's, or the one guessed
    // from the path.
    std::string_view content_type_;
    std::string_view cache_control_;
    static_range_request_policy range_requests_{static_range_request_policy::ignore};
    static_response_validator_policy response_validators_{static_response_validator_policy::omit};
    std::string_view precomputed_etag_;
    std::string_view precomputed_last_modified_;
    http_content_coding content_coding_{http_content_coding::identity};
    bool negotiates_encoding_{false};
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
    std::array<char, detail::static_file_etag_size>& etag_storage,
    std::array<char, http_imf_fixdate_size>& last_modified_storage) noexcept {
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
            etag_storage = detail::format_static_file_snapshot_etag(
                source_value.size_, source_value.modified_token_, source_value.identity_);
            validators.etag_ = std::string_view(etag_storage.data(), etag_storage.size());
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

[[nodiscard]] http_representation_response_plan plan_file_representation(const context& context_value,
    const file_response_source& source_value, const file_validators& validators) {
    return plan_http_representation_response(detail::context_access::request(context_value),
        http_selected_representation_metadata{
            .length_ = source_value.size_,
            .etag_ = validators.etag_,
            .last_modified_ = validators.last_modified_.empty() ? std::nullopt
                                                                : std::optional(validators.modified_seconds_),
            // Filesystem mtime cannot establish that a representation changed
            // at most once during the second in its Last-Modified value.
            .strong_date_validator_ = false,
            .content_coded_ = source_value.content_coding_ != http_content_coding::identity,
        },
        http_representation_response_options{
            .normal_status_ = detail::context_access::response_status(context_value),
            .range_policy_ = source_value.range_requests_ == static_range_request_policy::honor
                                 ? http_range_request_policy::honor_byte_ranges
                                 : http_range_request_policy::ignore,
        });
}

// The validators and the RFC 9110 response one file response's request
// selects, decided from the source alone, before any file is opened. The
// validator views may borrow this object's storage, so it stays where it was
// built.
class file_response_plan final {
public:
    file_response_plan(const context& context_value, const file_response_source& source_value)
        : validators_(compute_file_validators(source_value, etag_storage_, last_modified_storage_)),
          representation_(plan_file_representation(context_value, source_value, validators_)),
          head_(detail::context_access::request(context_value).known_method() == http_known_method::head) {}

    file_response_plan(const file_response_plan&) = delete;
    file_response_plan& operator=(const file_response_plan&) = delete;
    file_response_plan(file_response_plan&&) = delete;
    file_response_plan& operator=(file_response_plan&&) = delete;

    [[nodiscard]] const file_validators& validators() const noexcept {
        return validators_;
    }

    [[nodiscard]] const http_representation_response_plan& representation() const noexcept {
        return representation_;
    }

    // Whether the response sends the file's bytes. A HEAD, 304, 412 or 416
    // only describes them, so no transport read ever checks the file behind
    // such a response; its open has to.
    [[nodiscard]] bool sends_file_bytes() const noexcept {
        return !head_ && representation_.precondition_failed() == nullptr &&
               representation_.not_modified() == nullptr && representation_.range_unsatisfiable() == nullptr;
    }

private:
    std::array<char, detail::static_file_etag_size> etag_storage_{};
    std::array<char, http_imf_fixdate_size> last_modified_storage_{};
    file_validators validators_;
    http_representation_response_plan representation_;
    bool head_;
};

// Which representation metadata a response repeats. RFC 9110 15.4.5: a 304
// carries the validators and cache-guiding fields a 200 would, but no other
// representation metadata; Last-Modified guides the cache update only when no
// ETag is sent. RFC 9110 15.3.7: a 206 answering a matched If-Range SHOULD NOT
// repeat representation fields beyond the required ones (ETag, Cache-Control,
// Content-Range or the multipart Content-Type), because the client already
// holds them. Content-Encoding stays: it names the coding of the bytes this
// message carries, and the response coding policy reads it to keep a
// precompressed variant acceptable to a client that refuses identity.
// RFC 9110 15.5.17: a 416 has no content, so Content-Type and Content-Encoding
// would describe bytes that are not sent; it keeps Content-Range and the
// validator and cache fields.
enum class file_header_set : std::uint8_t {
    full,
    if_range_partial,
    not_modified,
    range_unsatisfied,
};

void add_file_headers(http_response& response, const file_response_source& source_value,
    const file_validators& validators, file_header_set header_set = file_header_set::full,
    bool emit_content_type = true) {
    const bool full = header_set == file_header_set::full;
    const bool carries_content =
        header_set == file_header_set::full || header_set == file_header_set::if_range_partial;
    response.reserve_headers(file_response_header_reserve);
    if (full && emit_content_type) {
        response.header("Content-Type", source_value.content_type_);
    }
    if (!source_value.cache_control_.empty()) {
        response.header("Cache-Control", source_value.cache_control_);
    }
    // A precompressed variant carries the original Content-Type with the
    // encoding declared here.
    if (carries_content && source_value.content_coding_ != http_content_coding::identity) {
        response.header_stable_view(
            "Content-Encoding", http_content_coding_token(source_value.content_coding_));
    }
    if (header_set != file_header_set::not_modified &&
        source_value.range_requests_ == static_range_request_policy::honor) {
        response.header_stable_view("Accept-Ranges", "bytes");
    }
    if (source_value.response_validators_ == static_response_validator_policy::emit) {
        response.header("ETag", validators.etag_);
        if ((full || header_set == file_header_set::range_unsatisfied) && !validators.last_modified_.empty()) {
            response.header("Last-Modified", validators.last_modified_);
        }
    }
}

[[nodiscard]] file_header_set partial_file_header_set(const http_representation_response_plan& plan) noexcept {
    return plan.authorized_by_if_range() ? file_header_set::if_range_partial : file_header_set::full;
}

void apply_file_response_state(context& context_value, const file_response_source& source_value,
    http_response& response, std::optional<http_status_code> status_code) {
    detail::context_access::apply_response_state(context_value, response, status_code);
    // Declare the negotiation dimension after context response metadata is
    // applied. A caller-provided Vary value must be merged, not allowed to
    // overwrite Accept-Encoding and make differently encoded variants share
    // one cache entry (RFC 9110 12.5.5 / RFC 9111 4.1). context::file does no
    // Accept-Encoding negotiation and stays Vary-free.
    if (source_value.negotiates_encoding_) {
        response.add_vary_token("Accept-Encoding");
    }
}

[[nodiscard]] http_response make_not_modified_response(context& context_value,
    const file_response_source& source_value, const file_validators& validators, http_status_code status_code) {
    http_response response(context_value.arena());
    add_file_headers(response, source_value, validators, file_header_set::not_modified);
    apply_file_response_state(context_value, source_value, response, status_code);
    return response;
}

[[nodiscard]] http_response make_full_file_response(context& context_value,
    const file_response_source& source_value, file_response_body_source& body, const file_validators& validators,
    std::optional<http_status_code> status_code) {
    http_response response(context_value.arena());
    add_file_headers(response, source_value, validators);
    body.set_full_body(response, context_value.arena(), source_value.size_);
    apply_file_response_state(context_value, source_value, response, status_code);
    return response;
}

[[nodiscard]] http_response make_multipart_response(context& context_value,
    const file_response_source& source_value, file_response_body_source& body, const file_validators& validators,
    const http_byte_range_set& ranges, file_header_set header_set) {
    std::array<char, detail::secure_token_size> boundary_token;
    const auto token = detail::generate_secure_token(boundary_token);
    http_response response(context_value.arena());
    add_file_headers(response, source_value, validators, header_set, false);
    apply_file_response_state(context_value, source_value, response, http_status::partial_content);
    const auto media_type = response.header("Content-Type");
    const auto selected_media_type = media_type.value_or(source_value.content_type_);
    auto multipart_plan = make_http_multipart_byte_range_plan(
        ranges, source_value.size_, selected_media_type, token, context_value.arena());

    // The outer representation is the multipart body, which is not
    // content-coded; coded representations never reach this path.
    response.header("Content-Type", multipart_plan.content_type());
    response.remove_header("Content-Range");
    response.remove_header("Content-Encoding");
    response.remove_header("Content-Length");
    body.set_multipart_body(response, context_value.arena(), source_value.size_, std::move(multipart_plan));
    return response;
}

// Builds the response `planned` selected. An indexed file behind a response
// that sends none of its bytes was already checked by its open.
[[nodiscard]] http_response make_file_response(context& context_value, const file_response_source& source_value,
    file_response_body_source& body, const file_response_plan& planned) {
    const auto& validators = planned.validators();
    const auto& plan = planned.representation();
    if (plan.precondition_failed()) {
        throw http_error({.status_ = plan.status(),
            .code_ = "precondition_failed",
            .message_ = "file precondition failed"});
    }
    if (plan.not_modified()) {
        return make_not_modified_response(context_value, source_value, validators, plan.status());
    }
    if (plan.range_unsatisfiable()) {
        http_response response(context_value.arena());
        response.content_range_unsatisfied(source_value.size_);
        add_file_headers(response, source_value, validators, file_header_set::range_unsatisfied);
        apply_file_response_state(context_value, source_value, response, plan.status());
        return response;
    }
    if (const auto* ranges = plan.multipart_ranges()) {
        return make_multipart_response(
            context_value, source_value, body, validators, *ranges, partial_file_header_set(plan));
    }
    if (const auto* range = plan.partial()) {
        http_response response(context_value.arena());
        add_file_headers(response, source_value, validators, partial_file_header_set(plan));
        response.content_range(range->offset_, range->length_, source_value.size_);
        body.set_body(response, context_value.arena(), source_value.size_, range->offset_, range->length_);
        apply_file_response_state(context_value, source_value, response, plan.status());
        return response;
    }
    return make_full_file_response(context_value, source_value, body, validators, plan.status());
}

[[nodiscard]] http_error file_not_found_error() {
    return http_error({.status_ = ruvia::http_status::not_found,
        .code_ = "not_found",
        .message_ = "file not found"});
}

// context::file once its inputs are owned: `native_path` in the process
// resource, since it travels to the pool thread, and `content_type` in the
// request pool. An empty `content_type` selects the type guessed from the
// path.
[[nodiscard]] task<http_response> serve_file(
    context& context_value, http_native_path_string_type native_path, std::pmr::string content_type) {
    const auto content_type_value = content_type.empty() ? detail::guess_static_file_content_type_from_path_view(
                                                               http_native_path_view_type(native_path))
                                                         : std::string_view(content_type);
    // Open once, on the blocking pool: the response is framed from this
    // handle's metadata, and the body comes from the same handle, so no path
    // is resolved twice.
    auto opened = take_file_open(context_value,
        co_await run_file_open(context_value, [path = std::move(native_path)]() mutable {
            opened_file_path result{.path_ = std::move(path)};
            result.file_ = detail::open_response_file(result.path_.c_str(), result.error_);
            return result;
        }));
    if (opened.error_) {
        throw file_not_found_error();
    }
    const auto& snapshot = opened.file_.snapshot_;
    const file_response_source source_value{
        .size_ = snapshot.size_,
        .modified_token_ = snapshot.modified_token_,
        .modified_seconds_ = snapshot.modified_seconds_,
        .identity_ = snapshot.identity_,
        .content_type_ = content_type_value,
        .cache_control_ = {},
        .range_requests_ = static_range_request_policy::honor,
        .response_validators_ = static_response_validator_policy::emit,
        .precomputed_etag_ = {},
        .precomputed_last_modified_ = {},
        .content_coding_ = http_content_coding::identity,
        .negotiates_encoding_ = false,
    };
    const file_response_plan planned(context_value, source_value);
    auto body = file_response_body_source::file(
        file_response_path(opened.path_, snapshot.identity_, std::move(opened.file_.handle_)));
    auto response = make_file_response(context_value, source_value, body, planned);
    detail::context_access::discard_draft(context_value);
    co_return response;
}

// static_file once its representation is selected. `served` and `base_entry`
// view `root`'s snapshot, which the caller keeps alive until this completes;
// `content_type` is owned in the request pool, and an empty one selects the
// base entry's type.
[[nodiscard]] task<http_response> serve_static_file(context& context_value, const static_root& root,
    static_file_representation served, detail::static_root_entry_view base_entry, std::pmr::string content_type) {
    const auto& served_entry = served.entry();
    const auto* const memory_variant = served.memory_variant();
    // A document-root snapshot keeps small files' bytes; they describe exactly
    // the indexed metadata, so the response is framed and sent from memory.
    const auto* const resident_bytes = memory_variant == nullptr ? served_entry.resident_bytes() : nullptr;
    const bool file_backed = memory_variant == nullptr && resident_bytes == nullptr;
    const file_response_source source_value{
        .size_ = memory_variant == nullptr ? served_entry.size() : memory_variant->size(),
        .modified_token_ = memory_variant == nullptr ? served_entry.modified_token() : memory_variant->modified_token(),
        .modified_seconds_ =
            memory_variant == nullptr ? served_entry.modified_seconds() : memory_variant->modified_seconds(),
        .identity_ = file_backed ? served_entry.identity() : http_response_file_identity::unchecked(),
        .content_type_ = content_type.empty() ? base_entry.content_type() : std::string_view(content_type),
        .cache_control_ = base_entry.get_cache_control(),
        .range_requests_ = base_entry.range_requests(),
        .response_validators_ = base_entry.response_validators(),
        .precomputed_etag_ = memory_variant == nullptr ? served_entry.etag() : memory_variant->etag(),
        .precomputed_last_modified_ =
            memory_variant == nullptr ? served_entry.last_modified() : memory_variant->last_modified(),
        .content_coding_ = served.content_coding(),
        // static_file negotiates the representation by Accept-Encoding.
        .negotiates_encoding_ = true,
    };
    const file_response_plan planned(context_value, source_value);

    // The indexed file is opened beneath its root on the blocking pool. One
    // that cannot be opened is gone or no longer where the index found it,
    // and its path is never resolved another way. A response that sends none
    // of its bytes checks the opened handle against the index there too.
    http_response_file_handle opened;
    if (file_backed) {
        opened = take_file_open(context_value,
            co_await run_file_open(context_value,
                detail::static_root_access::entry_open(root, served_entry, !planned.sends_file_bytes())));
        if (!opened) {
            throw static_file_changed_error();
        }
    }
    auto body = memory_variant != nullptr ? file_response_body_source::bytes_value(memory_variant->bytes())
                : resident_bytes != nullptr
                    ? file_response_body_source::bytes_value(*resident_bytes)
                    : file_response_body_source::file(file_response_path(
                          http_native_path_view_type(served_entry.file_path()), served_entry.identity(),
                          std::move(opened)));
    auto response = make_file_response(context_value, source_value, body, planned);
    detail::context_access::discard_draft(context_value);
    co_return response;
}

}  // namespace

// Not a coroutine: the options are owned before the task is returned, so a
// stored task never reads the caller's path or content type.
task<http_response> context::file(file_response_options options) {
    http_native_path_string_type native_path(process_resource());
    if (!assign_native_file_path(options.path_, native_path)) {
        throw file_not_found_error();
    }
    return serve_file(*this, std::move(native_path), std::pmr::string(options.content_type_.view(), pool()));
}

// Not a coroutine: the path is resolved and the content type owned before the
// task is returned; only `root` stays borrowed.
task<http_response> context::static_file(const static_root& root, static_file_response_options options) {
    // relative_path_ is a decoded file path -- a wildcard capture from
    // req().param("*") is passed as is -- so it is never percent-decoded here:
    // a second decode would turn "%252E%252E" into a ".." no route or path
    // scope ever saw. normalize_static_relative_path rejects "." and ".."
    // segments, absolute paths and NUL, and the lookup is a byte-exact
    // index compare that never joins the path onto the filesystem.
    std::pmr::string relative_storage(pool());
    auto relative = options.relative_path_.view();
    const auto entry_value = detail::static_root_access::resolve(root, relative, relative_storage);
    if (!entry_value.has_value()) {
        throw file_not_found_error();
    }
    const auto mode = services().precompressed_static_files() ? detail::static_file_selection_mode::precompressed
                                                              : detail::static_file_selection_mode::identity_only;
    return detail::context_access::static_file_resolved(
        *this, root, relative, options.content_type_.view(), *entry_value, mode);
}

task<http_response> detail::context_access::static_file_resolved(
    context& context_value, const static_root& root, std::string_view relative,
    std::string_view content_type_value, detail::static_root_entry_view base_entry,
    detail::static_file_selection_mode mode) {
    // Serve a precompressed variant when the client accepts one; the bytes and
    // validators come from the variant, the Content-Type from the base entry.
    // Admission already folded Accept-Encoding; only a context created outside
    // admission negotiates from the request here.
    const auto served = [&]() -> std::optional<static_file_representation> {
        if (mode == detail::static_file_selection_mode::identity_only) {
            return static_file_representation(base_entry, http_content_coding::identity);
        }
        if (const auto* admitted = context_value.services().response_coding()) {
            return select_static_file_representation(root, relative, *admitted, context_value.pool(), base_entry);
        }
        return select_static_file_representation(
            root, relative, http_response_coding_selection_for(context_value.request_), context_value.pool(), base_entry);
    }();
    if (!served.has_value()) {
        throw http_error(detail::http_not_acceptable_coding_error());
    }
    return serve_static_file(
        context_value, root, *served, base_entry, std::pmr::string(content_type_value, context_value.pool()));
}

}  // namespace ruvia
