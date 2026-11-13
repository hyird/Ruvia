#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
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

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

// Platform-native path text (wchar_t on Windows, char elsewhere), as carried
// by response file bodies.
using http_native_path_char_type = std::filesystem::path::value_type;
using http_native_path_string_type = std::basic_string<http_native_path_char_type>;
using http_native_path_view_type = std::basic_string_view<http_native_path_char_type>;

[[nodiscard]] inline http_native_path_view_type http_native_path_view(const std::filesystem::path& path) noexcept {
    return http_native_path_view_type(path.native());
}
http_native_path_view_type http_native_path_view(const std::filesystem::path&&) = delete;

inline void http_assign_native_path(http_native_path_string_type& output, const std::filesystem::path& path) {
    const auto native = http_native_path_view(path);
    output.assign(native.data(), native.size());
}

[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_char_type* path) {
    return path == nullptr ? std::filesystem::path{} : std::filesystem::path(path);
}

[[nodiscard]] inline std::filesystem::path http_make_path_from_native_path(const http_native_path_string_type& path) {
    return http_make_path_from_native_path(path.c_str());
}

}  // namespace ruvia

namespace ruvia {

// Opaque runtime-supplied identity of the file whose metadata was used to
// frame a response. HTTP transports this token; the runtime validates it after
// opening the actual file handle.
class http_response_file_identity final {
public:
    [[nodiscard]] static constexpr http_response_file_identity unchecked() noexcept {
        return http_response_file_identity({}, false);
    }

    [[nodiscard]] static constexpr http_response_file_identity checked(
        std::array<std::uint64_t, 4> words) noexcept {
        return http_response_file_identity(words, true);
    }

    [[nodiscard]] constexpr bool requires_validation() const noexcept {
        return checked_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const& noexcept {
        return words_;
    }
    [[nodiscard]] constexpr const std::array<std::uint64_t, 4>& words() const&& = delete;

    friend constexpr bool operator==(
        const http_response_file_identity&, const http_response_file_identity&) noexcept = default;

private:
    constexpr http_response_file_identity(std::array<std::uint64_t, 4> words, bool checked) noexcept
        : words_(words),
          checked_(checked) {}

    std::array<std::uint64_t, 4> words_{};
    bool checked_{false};
};

// An open native file a runtime already framed a response from, carried by
// the response body so the transport sends from that same descriptor instead of
// resolving the path again. HTTP performs no I/O on it: it stores the native
// value and invokes the runtime-supplied release function exactly once when
// the owning body is destroyed or replaced.
class http_response_file_descriptor final {
public:
    using native_type = std::uintptr_t;
    using release_type = void (*)(native_type) noexcept;

    constexpr http_response_file_descriptor() noexcept = default;
    constexpr http_response_file_descriptor(native_type native_value, release_type release) noexcept
        : native_(native_value),
          release_(release) {}

    http_response_file_descriptor(const http_response_file_descriptor&) = delete;
    http_response_file_descriptor& operator=(const http_response_file_descriptor&) = delete;

    constexpr http_response_file_descriptor(http_response_file_descriptor&& other) noexcept
        : native_(std::exchange(other.native_, native_type{})),
          release_(std::exchange(other.release_, nullptr)) {}

    http_response_file_descriptor& operator=(http_response_file_descriptor&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        http_response_file_descriptor retired(std::move(other));
        std::swap(native_, retired.native_);
        std::swap(release_, retired.release_);
        // Commit both owners and the return reference before release can reenter.
        return *this;
    }

    ~http_response_file_descriptor() {
        reset();
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return release_ != nullptr;
    }

    // Borrowed while this descriptor is alive; meaningful only when engaged.
    [[nodiscard]] constexpr native_type native() const noexcept {
        return native_;
    }

private:
    void reset() noexcept {
        if (release_ != nullptr) {
            std::exchange(release_, nullptr)(std::exchange(native_, native_type{}));
        }
    }

    native_type native_{};
    release_type release_{nullptr};
};

// Read-only descriptor for a response file. Its path and any opened file are
// borrowed and remain valid for the lifetime of the response body from which
// it was obtained.
class http_response_file_view final {
public:
    constexpr http_response_file_view(const http_native_path_char_type* native_path, std::uint64_t size,
        std::uint64_t offset, std::uint64_t length, http_response_file_identity identity,
        const http_response_file_descriptor* opened = nullptr) noexcept
        : native_path_(native_path),
          opened_(opened != nullptr && *opened ? opened : nullptr),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity) {}

    [[nodiscard]] constexpr const http_native_path_char_type* native_path_c_str() const noexcept {
        return native_path_;
    }
    [[nodiscard]] std::filesystem::path to_path() const {
        return std::filesystem::path(native_path_);
    }
    [[nodiscard]] constexpr std::uint64_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }
    [[nodiscard]] constexpr http_response_file_identity identity() const noexcept {
        return identity_;
    }
    // The descriptor the response was framed from, or null when the transport
    // must open native_path_c_str() and validate identity() itself.
    [[nodiscard]] constexpr const http_response_file_descriptor* opened_file() const noexcept {
        return opened_;
    }

private:
    const http_native_path_char_type* native_path_;
    const http_response_file_descriptor* opened_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
};

// A borrowed view of one response-body segment. Any bytes and file path remain
// valid only while the owning http_response is alive and unmodified.
struct http_response_body_segment_view final {
    std::string_view bytes_{};
    std::optional<http_response_file_view> file_{};
};

}  // namespace ruvia

namespace ruvia {

// A byte range already resolved against the selected representation.
struct http_byte_range final {
    std::uint64_t offset_{};
    std::uint64_t length_{};

    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return offset_;
    }

    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }
};
enum class http_byte_range_order_policy : std::uint8_t {
    coalesce,
    strict_ascending_disjoint,
};

struct http_byte_range_options final {
    std::size_t max_ranges_{16};
    http_byte_range_order_policy order_policy_{http_byte_range_order_policy::coalesce};
    bool strict_order_{false};
};

// Bounded result for a byte-range set. Inline storage covers up to 16 ranges
// without allocation; larger configured max_ranges_ spill into heap storage.
class http_byte_range_set final {
public:
    static constexpr std::size_t capacity = 16;

    using range = http_byte_range;

    http_byte_range_set() noexcept = default;

    [[nodiscard]] constexpr bool ignored() const noexcept {
        return ignored_;
    }
    [[nodiscard]] constexpr bool unsatisfiable() const noexcept {
        return !ignored_ && count_ == 0;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] std::span<const range> ranges() const& noexcept RUVIA_LIFETIMEBOUND {
        return overflow_.empty() ? std::span<const range>(ranges_.data(), count_)
                                 : std::span<const range>(overflow_.data(), count_);
    }
    std::span<const range> ranges() const&& = delete;

    [[nodiscard]] const range* begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return ranges().data();
    }
    const range* begin() const&& = delete;

    [[nodiscard]] const range* end() const& noexcept RUVIA_LIFETIMEBOUND {
        return ranges().data() + count_;
    }
    const range* end() const&& = delete;

    // Throws std::out_of_range for index >= size().
    [[nodiscard]] const range& operator[](std::size_t index) const& RUVIA_LIFETIMEBOUND {
        if (index >= count_) {
            throw std::out_of_range("HTTP byte range index out of range");
        }
        return overflow_.empty() ? ranges_[index] : overflow_[index];
    }
    const range& operator[](std::size_t) const&& = delete;

private:
    friend http_byte_range_set resolve_http_byte_range_set(
        std::string_view, http_content_length, http_byte_range_options);

    range& mutable_at(std::size_t index) noexcept {
        return overflow_.empty() ? ranges_[index] : overflow_[index];
    }

    void append_slot(range value) {
        if (overflow_.empty() && count_ < capacity) {
            ranges_[count_++] = value;
            return;
        }
        if (overflow_.empty()) {
            overflow_.reserve(capacity * 2);
            overflow_.insert(overflow_.end(), ranges_.begin(), ranges_.begin() + count_);
        }
        overflow_.push_back(value);
        count_ = overflow_.size();
    }

    void resize_slots(std::size_t new_count) noexcept {
        count_ = new_count;
        if (!overflow_.empty()) {
            overflow_.resize(new_count);
        }
    }

    std::array<range, capacity> ranges_{};
    std::vector<range> overflow_;
    std::size_t count_{};
    bool ignored_{true};
};

[[nodiscard]] http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value,
    http_byte_range_options options);

[[nodiscard]] inline http_byte_range_set resolve_http_byte_range_set(
    std::string_view field_value, http_content_length representation_length_value) noexcept {
    return resolve_http_byte_range_set(field_value, representation_length_value, {});
}

// Parsed Content-Range: "bytes first-last/total" (RFC 9110 section 14.4).
struct http_content_range final {
    std::uint64_t first_{};
    std::uint64_t last_{};
    std::uint64_t total_{};

    [[nodiscard]] constexpr std::uint64_t first() const noexcept {
        return first_;
    }
    [[nodiscard]] constexpr std::uint64_t last() const noexcept {
        return last_;
    }
    [[nodiscard]] constexpr std::uint64_t total() const noexcept {
        return total_;
    }
    [[nodiscard]] constexpr std::uint64_t offset() const noexcept {
        return first_;
    }
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return last_ - first_ + 1;
    }
    [[nodiscard]] constexpr http_byte_range range() const noexcept {
        return {first_, last_ - first_ + 1};
    }

    friend constexpr bool operator==(const http_content_range&, const http_content_range&) noexcept = default;
};

[[nodiscard]] std::optional<http_content_range> parse_http_content_range(
    std::string_view field_value) noexcept;

[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(
    std::span<const http_byte_range> ranges, http_content_length representation_length,
    std::string_view media_type = {}, std::string_view boundary = {}) noexcept;
[[nodiscard]] std::optional<std::uint64_t> compute_http_byte_range_content_length(
    const http_byte_range_set& ranges, http_content_length representation_length,
    std::string_view media_type = {}, std::string_view boundary = {}) noexcept;

// Streaming byte-range slicer for single-range 206 bodies and multi-range
// multipart/byteranges streams. Accepts sequential representation chunks (or
// partial chunks starting at a specified representation offset) and appends the
// framed output bytes to the caller's buffer.
class http_byte_range_slicer final {
public:
    http_byte_range_slicer(const http_byte_range_set& ranges,
        http_content_length representation_length, std::string_view media_type = {},
        std::string_view boundary = {});
    http_byte_range_slicer(std::span<const http_byte_range> ranges,
        http_content_length representation_length, std::string_view media_type = {},
        std::string_view boundary = {});

    [[nodiscard]] bool multipart() const noexcept {
        return ranges_.size() > 1;
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        return content_type_;
    }
    std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view content_range() const& noexcept RUVIA_LIFETIMEBOUND {
        return content_range_;
    }
    std::string_view content_range() const&& = delete;
    [[nodiscard]] std::uint64_t current_offset() const noexcept {
        return current_offset_;
    }
    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }

    void write(std::string_view chunk, std::string& output);
    void write(std::uint64_t chunk_offset, std::string_view chunk, std::string& output);
    void finish(std::string& output);
    void reset(std::uint64_t start_offset = 0) noexcept;

private:
    void emit_part_header(std::size_t index, std::string& output) const;

    std::vector<http_byte_range> ranges_;
    std::string media_type_;
    std::string boundary_;
    std::string content_type_;
    std::string content_range_;
    std::uint64_t representation_length_{0};
    std::uint64_t content_length_{0};
    std::uint64_t current_offset_{0};
    std::uint64_t range_progress_{0};
    std::size_t range_index_{0};
    bool complete_{false};
};

}  // namespace ruvia

namespace ruvia {

// Owns copied framing metadata and ordered file-slice descriptors. Observed
// views borrow the plan. Construction and clone propagate allocation failures.
class http_multipart_byte_range_plan final {
public:
    enum class segment_kind : std::uint8_t {
        metadata,
        file,
    };

    struct segment final {
        segment_kind kind_{};
        std::size_t metadata_offset_{};
        std::size_t metadata_length_{};
        std::uint64_t file_offset_{};
        std::uint64_t file_length_{};
    };

    http_multipart_byte_range_plan(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan& operator=(const http_multipart_byte_range_plan&) = delete;
    http_multipart_byte_range_plan(http_multipart_byte_range_plan&&) noexcept = default;
    http_multipart_byte_range_plan& operator=(http_multipart_byte_range_plan&&) = delete;

    [[nodiscard]] std::string_view content_type() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_ == nullptr ? std::string_view{} : std::string_view(state_->content_type_);
    }
    [[nodiscard]] std::string_view content_type() const&& = delete;
    [[nodiscard]] std::string_view metadata() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_ == nullptr ? std::string_view{} : std::string_view(state_->metadata_);
    }
    [[nodiscard]] std::string_view metadata() const&& = delete;
    /// Ordered wire fragments, not multipart parts. Adjacent metadata may share a
    /// descriptor; consumers must not assume a fixed segment count per range.
    [[nodiscard]] std::span<const segment> segments() const& noexcept RUVIA_LIFETIMEBOUND {
        return state_ == nullptr
                   ? std::span<const segment>{}
                   : std::span<const segment>(state_->segments_.data(), state_->segment_count_);
    }
    [[nodiscard]] std::span<const segment> segments() const&& = delete;
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return state_ == nullptr ? 0 : state_->content_length_;
    }
    [[nodiscard]] http_multipart_byte_range_plan clone() const;

private:
    friend http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
        const http_byte_range_set&, http_content_length, std::string_view, std::string_view);

    struct storage final {
        static constexpr std::size_t max_segments = http_byte_range_set::capacity * 2 + 1;

        storage() = default;

        std::string content_type_;
        std::string metadata_;
        std::array<segment, max_segments> segments_{};
        std::size_t segment_count_{};
        std::uint64_t content_length_{};
    };

    http_multipart_byte_range_plan()
        : state_(std::make_unique<storage>()) {}

    std::unique_ptr<storage> state_;
};

// Each body part carries the selected representation's Content-Type and its
// Content-Range (RFC 9110 §15.3.7.2). Parts never carry Content-Encoding: a
// slice of a content-coded representation is not itself a coded body, so
// plan_http_representation_response() answers multiple ranges of a coded
// representation with the full representation instead.
[[nodiscard]] http_multipart_byte_range_plan make_http_multipart_byte_range_plan(
    const http_byte_range_set& ranges, http_content_length representation_length,
    std::string_view media_type, std::string_view boundary);

}  // namespace ruvia

namespace ruvia {

class http_request;

// Metadata for an already selected, existing origin representation. This plan
// does not describe cache misses or select a representation. ETag text is
// borrowed only during planning; the resulting plan retains no input views.
struct http_selected_representation_metadata final {
    http_content_length length_{};
    borrowed_text etag_{};
    std::optional<std::time_t> last_modified_{};
    // Only authorizes a date-form If-Range when last_modified is also present.
    // Ordinary date preconditions do not require a strong validator.
    bool strong_date_validator_{false};
    // The representation carries a content coding (Content-Encoding). Body
    // parts of multipart/byteranges cannot declare the coding of the whole
    // representation they slice, and a header-section Content-Encoding would
    // claim the multipart body itself is coded (RFC 9110 §8.4, §14.6), so a
    // request for several ranges of such a representation is answered with
    // the full representation (ignoring Range is permitted, RFC 9110 §14.2).
    // A single range remains a single-part 206.
    bool content_coded_{false};
};

enum class http_range_request_policy {
    ignore,
    honor_byte_ranges,
};

struct http_representation_response_options final {
    // Origin response status before evaluating preconditions or Range.
    http_status_code normal_status_{http_status::ok};
    http_range_request_policy range_policy_{http_range_request_policy::ignore};
};

// An exclusive origin response selection. Owns only status/slicing values;
// response body permission and wire framing are decided at write submission.
class http_representation_response_plan final {
public:
    struct full_type final {
        http_status_code status_;
    };
    struct not_modified_type final {};
    struct precondition_failed_type final {};
    struct range_unsatisfiable_type final {};
    using partial_type = http_byte_range;
    using multipart_type = http_byte_range_set;

    [[nodiscard]] const full_type* full() const& noexcept;
    [[nodiscard]] const full_type* full() const&& = delete;
    [[nodiscard]] const not_modified_type* not_modified() const& noexcept;
    [[nodiscard]] const not_modified_type* not_modified() const&& = delete;
    [[nodiscard]] const precondition_failed_type* precondition_failed() const& noexcept;
    [[nodiscard]] const precondition_failed_type* precondition_failed() const&& = delete;
    [[nodiscard]] const range_unsatisfiable_type* range_unsatisfiable() const& noexcept;
    [[nodiscard]] const range_unsatisfiable_type* range_unsatisfiable() const&& = delete;
    [[nodiscard]] const partial_type* partial() const& noexcept;
    [[nodiscard]] const partial_type* partial() const&& = delete;
    [[nodiscard]] const multipart_type* multipart_ranges() const& noexcept;
    [[nodiscard]] const multipart_type* multipart_ranges() const&& = delete;
    [[nodiscard]] http_status_code status() const noexcept;
    // True when a partial() or multipart_ranges() outcome answers a request
    // whose If-Range matched. RFC 9110 §15.3.7: such a 206 SHOULD NOT repeat
    // representation header fields beyond the required ones (Date,
    // Cache-Control, ETag, Expires, Content-Location, Vary, plus Content-Range
    // or the multipart Content-Type), since the client already holds them.
    // Without If-Range a 206 MUST carry every representation field a 200 would.
    [[nodiscard]] bool authorized_by_if_range() const noexcept {
        return authorized_by_if_range_;
    }

private:
    friend http_representation_response_plan plan_http_representation_response(
        const http_request&, http_selected_representation_metadata,
        http_representation_response_options);

    using value_type = std::variant<full_type, not_modified_type, precondition_failed_type, range_unsatisfiable_type, partial_type, multipart_type>;

    template <typename alternative_type>
    explicit http_representation_response_plan(alternative_type alternative, bool authorized_by_if_range = false) noexcept
        : value_(alternative),
          authorized_by_if_range_(authorized_by_if_range) {}

    value_type value_;
    bool authorized_by_if_range_{false};
};

// Call after normal request checks, before processing request content or
// performing the method, with the target resource's current representation.
// The method must be supported and allowed. For extensions, the caller must
// establish that it selects or modifies that representation; non-representation
// extensions and extensions redefining conditional semantics are outside this API.
// CONNECT, OPTIONS, and TRACE ignore conditions, as do ineligible normal statuses.
[[nodiscard]] http_representation_response_plan plan_http_representation_response(
    const http_request& request, http_selected_representation_metadata representation,
    http_representation_response_options options = {});

}  // namespace ruvia

namespace ruvia {

enum class http_connect_udp_error : std::uint8_t;
class http1_response_head_plan;
class http_request;
class http_response;
class http_response_headers;

enum class http_response_content_semantics : std::uint8_t {
    informational,
    protocol_switch,
    connect_tunnel,
    without_content,
    with_content,
};

class http_response_body_plan final {
public:
    [[nodiscard]] constexpr http_known_method request_method() const noexcept {
        return request_method_;
    }
    [[nodiscard]] constexpr http_status_code response_status() const noexcept {
        return response_status_;
    }
    [[nodiscard]] constexpr http_response_content_semantics content_semantics() const noexcept {
        return semantics_;
    }
    [[nodiscard]] constexpr bool status_allows_body() const noexcept {
        return status_allows_body_;
    }
    [[nodiscard]] constexpr bool body_suppressed() const noexcept {
        return body_suppressed_;
    }
    [[nodiscard]] constexpr bool auto_content_length_allowed() const noexcept {
        return auto_content_length_allowed_;
    }
    [[nodiscard]] constexpr bool explicit_content_length_allowed() const noexcept {
        return explicit_content_length_allowed_;
    }
    [[nodiscard]] constexpr bool transfer_encoding_allowed() const noexcept {
        return transfer_encoding_allowed_;
    }
    [[nodiscard]] std::uint64_t buffered_representation_length(const http_response& response) const noexcept;

private:
    friend http_response_body_plan plan_http_response_body(http_known_method, http_status_code) noexcept;
    constexpr http_response_body_plan(http_known_method method, http_status_code status, http_response_content_semantics semantics,
        bool status_allows_body, bool body_suppressed, bool auto_content_length_allowed,
        bool explicit_content_length_allowed, bool transfer_encoding_allowed) noexcept
        : request_method_(method),
          response_status_(status),
          semantics_(semantics),
          status_allows_body_(status_allows_body),
          body_suppressed_(body_suppressed),
          auto_content_length_allowed_(auto_content_length_allowed),
          explicit_content_length_allowed_(explicit_content_length_allowed),
          transfer_encoding_allowed_(transfer_encoding_allowed) {}

    http_known_method request_method_;
    http_status_code response_status_;
    http_response_content_semantics semantics_;
    bool status_allows_body_;
    bool body_suppressed_;
    bool auto_content_length_allowed_;
    bool explicit_content_length_allowed_;
    bool transfer_encoding_allowed_;
};

[[nodiscard]] http_response_body_plan plan_http_response_body(
    http_known_method request_method, http_status_code response_status) noexcept;

namespace detail {

// A buffered response sends payload bytes only when its method/status permit a
// body and the representation is non-empty.
[[nodiscard]] constexpr bool http_buffered_response_sends_body(
    const http_response_body_plan& body_plan, std::uint64_t content_length) noexcept {
    return !body_plan.body_suppressed() && content_length != 0;
}

}  // namespace detail

class http_buffered_response_write_plan final {
public:
    [[nodiscard]] http_known_method request_method() const noexcept {
        return body_plan_.request_method();
    }
    [[nodiscard]] http_status_code response_status() const noexcept {
        return body_plan_.response_status();
    }
    [[nodiscard]] http_response_body_plan body_plan() const noexcept {
        return body_plan_;
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return content_length_;
    }
    [[nodiscard]] bool body_suppressed() const noexcept {
        return body_plan_.body_suppressed();
    }
    [[nodiscard]] bool status_allows_body() const noexcept {
        return body_plan_.status_allows_body();
    }
    [[nodiscard]] bool auto_content_length_allowed() const noexcept {
        return body_plan_.auto_content_length_allowed();
    }
    [[nodiscard]] bool explicit_content_length_allowed() const noexcept {
        return body_plan_.explicit_content_length_allowed();
    }
    [[nodiscard]] bool transfer_encoding_allowed() const noexcept {
        return body_plan_.transfer_encoding_allowed();
    }
    [[nodiscard]] bool send_body() const noexcept {
        return detail::http_buffered_response_sends_body(body_plan_, content_length_);
    }
    [[nodiscard]] bool matches_response(const http_response& response) const noexcept;

private:
    friend http_buffered_response_write_plan plan_buffered_http_response_write(
        http_known_method, const http_response&) noexcept;
    http_buffered_response_write_plan(http_response_body_plan body_plan, std::uint64_t content_length) noexcept
        : body_plan_(body_plan),
          content_length_(content_length) {}

    http_response_body_plan body_plan_;
    std::uint64_t content_length_;
};

[[nodiscard]] http_buffered_response_write_plan plan_buffered_http_response_write(
    http_known_method request_method, const http_response& response) noexcept;

class set_cookie_plan;

enum class http_response_header_mode : std::uint8_t {
    replace,
    append,
};

enum class http_response_header_transfer : std::uint8_t {
    merge,
    assign,
    apply,
};

// Content-Type values whose field lines are constants of this library. Setting
// one shares a static descriptor: no allocation and no grammar re-validation.
enum class http_content_type : std::uint8_t {
    text_plain_utf8,   // text/plain; charset=UTF-8
    text_html_utf8,    // text/html; charset=UTF-8
    application_json,  // application/json
};

struct http_response_header_options {
    http_response_header_mode mode_{http_response_header_mode::replace};
};

namespace detail {

struct http_response_body_access;
struct http_response_header_access;
struct http_response_header_state_access;

}  // namespace detail

struct http_response_header {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return bytes_ == nullptr ? std::string_view{} : std::string_view(bytes_, name_size_);
    }

    [[nodiscard]] std::string_view value() const noexcept {
        return bytes_ == nullptr ? std::string_view{}
                                 : std::string_view(bytes_ + name_size_, value_size_);
    }

private:
    friend class http_response;
    friend class http_response_headers;
    friend struct detail::http_response_header_access;

    http_response_header() noexcept = default;

    const char* bytes_{nullptr};
    std::uint32_t name_size_{0};
    std::uint32_t value_size_{0};
    std::uint32_t known_bit_{0};
    bool owned_{false};
    bool append_{false};
};

static_assert(std::is_trivially_copyable_v<http_response_header>);
static_assert(sizeof(http_response_header) <= 24);

// Owns a response header block: descriptor storage, the known-header index, every
// field mutation rule, and descriptor staging. A clone is a header-only
// transaction: edit it around external operations, then publish it with
// http_response::commit_headers.
class http_response_headers final {
public:
    using value_type = http_response_header;
    using const_iterator = const http_response_header*;

    // Moving transfers every descriptor and the index. The source remains
    // usable, independent, and empty.
    http_response_headers(http_response_headers&& other) noexcept;
    http_response_headers(const http_response_headers&) = delete;
    http_response_headers& operator=(const http_response_headers&) = delete;
    http_response_headers& operator=(http_response_headers&&) = delete;
    ~http_response_headers();

    [[nodiscard]] const_iterator begin() const& noexcept RUVIA_LIFETIMEBOUND {
        return data();
    }
    [[nodiscard]] const_iterator begin() const&& = delete;

    [[nodiscard]] const_iterator end() const& noexcept RUVIA_LIFETIMEBOUND {
        return data() + size();
    }
    [[nodiscard]] const_iterator end() const&& = delete;

    [[nodiscard]] const_iterator cbegin() const& noexcept RUVIA_LIFETIMEBOUND {
        return begin();
    }
    [[nodiscard]] const_iterator cbegin() const&& = delete;

    [[nodiscard]] const_iterator cend() const& noexcept RUVIA_LIFETIMEBOUND {
        return end();
    }
    [[nodiscard]] const_iterator cend() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return spilled_ ? heap_.size() : size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // Inputs may borrow current header storage. Refresh views after mutation.
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    // Validated like header(). Name and value may be retained as borrowed views
    // without copying. Keep their bytes valid and unchanged while this block or
    // any clone retains them.
    void header_stable_view(std::string_view key, std::string_view value);
    // Replaces Content-Type with a library constant.
    void content_type(http_content_type type);
    void remove_header(std::string_view key);
    // Replace cookies with the same name, Domain/Path scope, and Partitioned
    // state; partitioned and unpartitioned cookies may coexist.
    void set_cookie(const set_cookie_plan& plan);
    void allow_methods(
        std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void content_range(
        http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve(std::size_t count);
    void transfer_from(const http_response_headers& source, http_response_header_transfer mode);
    // Owned bytes are copied so either block may change or be destroyed;
    // stable views are shared. Reserves room for additional_headers more fields.
    // Throws length_error before cloning if the total field count cannot fit.
    [[nodiscard]] http_response_headers clone(std::size_t additional_headers = 0) const;

private:
    friend class http_response;
    friend struct detail::http_response_header_state_access;

    using iterator = http_response_header*;

    static constexpr std::size_t known_header_count = 22;
    static constexpr std::size_t inline_capacity = 8;
    struct inline_storage_type {
        alignas(http_response_header) std::byte bytes_[sizeof(http_response_header)];
    };

    // Stages up to three owned descriptors without touching the table and
    // publishes them later without allocation or failure.
    class descriptor_stage;

    http_response_headers() noexcept;

    // Mutable iteration for the owning block and its friends. Named apart from
    // the public const begin()/end() so a non-const block still iterates
    // through the public overloads.
    [[nodiscard]] iterator mutable_begin() & noexcept {
        return data();
    }

    [[nodiscard]] iterator mutable_end() & noexcept {
        return data() + size();
    }

    // Header-only transaction publication. `staged` is left empty and independent.
    void commit(http_response_headers&& staged) noexcept;
    // Appends the source's fields in place when none can interact with an
    // existing field. Returns false, leaving this block unchanged, otherwise.
    [[nodiscard]] bool try_append_disjoint(const http_response_headers& source, http_response_header_transfer mode);
    // Replaces the field's line with `prepared`, or appends it; ownership of
    // `prepared` transfers either way.
    void publish_header(http_response_header prepared);
    // Releases `existing`, adopts `prepared` in its slot, and collapses the
    // field's other lines when `existing` was one of several.
    http_response_header& replace_header(http_response_header& existing, http_response_header prepared) noexcept;

    // Field rules and the known-header index.
    void record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept;
    void rebuild_known_header_index() noexcept;
    [[nodiscard]] http_response_header* find_header_for_update(
        std::string_view key, std::uint32_t known_bit) noexcept;
    [[nodiscard]] const http_response_header* find_header_for_read(
        std::string_view key, std::uint32_t known_bit) const noexcept;
    // Owns lookup, duplicate collapse, and index publication for every upsert.
    // Mutation receives the retained slot, or nullptr when it must append.
    template <typename Mutation>
    http_response_header& upsert_header(std::string_view key, std::uint32_t known_bit, Mutation&& mutate);
    http_response_header& prepare_header_value_storage(
        std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    void set_header_validated(std::string_view key, std::string_view value, std::uint32_t known_bit, bool stable_view = false);
    void append_header_validated(
        std::string_view key, std::string_view value, std::uint32_t known_bit);
    http_response_header& append_header_uninitialized_value(
        std::string_view key, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] http_response_header& collapse_response_headers(
        http_response_header& retained, std::uint32_t known_bit) noexcept;
    bool remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept;
    void upsert_set_cookie_header_validated(std::string_view value);
    [[nodiscard]] http_response_header* find_set_cookie_header(std::string_view wire_prefix,
        std::string_view cookie_name, bool has_path, std::string_view path,
        std::string_view domain, bool partitioned) noexcept;
    void erase_later_set_cookie_headers(http_response_header& retained,
        std::string_view cookie_name, bool has_path, std::string_view path,
        std::string_view domain, bool partitioned) noexcept;
    // Descriptor storage.
    [[nodiscard]] http_response_header* inline_data() noexcept;
    [[nodiscard]] const http_response_header* inline_data() const noexcept;
    [[nodiscard]] http_response_header* data() noexcept;
    [[nodiscard]] const http_response_header* data() const noexcept;
    [[nodiscard]] http_response_header make_owned_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit);
    [[nodiscard]] http_response_header make_uninitialized_header(
        std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    [[nodiscard]] static std::optional<http_response_header> make_static_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept;
    [[nodiscard]] std::optional<http_response_header> make_shared_header(
        std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view) const noexcept;
    [[nodiscard]] bool try_assign_owned_in_place(http_response_header& header, std::string_view name,
        std::string_view value, std::uint32_t known_bit) noexcept;
    [[nodiscard]] bool borrows_owned_header(std::string_view name, std::string_view value) const noexcept;
    http_response_header& append_header(http_response_header header);
    http_response_header& add_uninitialized_value(
        std::string_view name, std::size_t value_size, std::uint32_t known_bit);
    http_response_header& add(
        std::string_view name, std::string_view value, std::uint32_t known_bit, bool stable_view = false);
    void assign(http_response_header& header, std::string_view name, std::string_view value,
        std::uint32_t known_bit, bool stable_view = false);
    http_response_header& assign_uninitialized_value(http_response_header& header, std::string_view name,
        std::size_t value_size, std::uint32_t known_bit);
    void release_header(http_response_header& header) noexcept;
    // Drops every header from new_end onward; the dropped headers must
    // already be released.
    void truncate(iterator new_end) noexcept;
    http_response_header& append_prepared_header(http_response_header header) noexcept;
    void clear() noexcept;
    void spill(std::size_t min_capacity);
    void move_from(http_response_headers&& other) noexcept;

    std::vector<http_response_header> heap_;
    std::array<inline_storage_type, inline_capacity> inline_;
    std::size_t size_{0};
    bool spilled_{false};
    std::uint32_t known_header_bits_{0};
    std::array<std::int16_t, known_header_count> known_header_indexes_;
};

namespace detail {

class http_response_body;

class http_empty_response_body final {
public:
    [[nodiscard]] constexpr bool head_metadata_only() const noexcept {
        return head_metadata_only_;
    }

private:
    friend class http_response_body;
    explicit constexpr http_empty_response_body(bool head_metadata_only = false) noexcept
        : head_metadata_only_(head_metadata_only) {}

    bool head_metadata_only_{false};
};

class http_static_response_bytes final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class http_response_body;

    explicit constexpr http_static_response_bytes(std::string_view bytes_value) noexcept
        : bytes_(bytes_value) {}

    std::string_view bytes_;
};

class http_owned_response_bytes final {
public:
    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

    [[nodiscard]] constexpr bool file_representation() const noexcept {
        return file_representation_;
    }

private:
    friend class http_response_body;

    explicit http_owned_response_bytes(std::string_view bytes_value)
        : bytes_(bytes_value) {}

    http_owned_response_bytes(std::string&& bytes_value, bool file_representation) noexcept
        : bytes_(std::move(bytes_value)),
          file_representation_(file_representation) {}

    std::string bytes_;
    bool file_representation_{false};
};

class http_owned_response_file final {
public:
    [[nodiscard]] constexpr std::uint64_t length() const noexcept {
        return length_;
    }

    [[nodiscard]] http_response_file_view view() const& noexcept {
        return http_response_file_view(native_path_.c_str(), size_, offset_, length_, identity_, &opened_);
    }
    [[nodiscard]] http_response_file_view view() const&& = delete;

private:
    friend class http_response_body;

    http_owned_response_file(http_native_path_string_type&& native_path,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_descriptor opened) noexcept
        : native_path_(std::move(native_path)),
          size_(size),
          offset_(offset),
          length_(length),
          identity_(identity),
          opened_(std::move(opened)) {}

    http_owned_response_file(http_native_path_view_type file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_descriptor opened)
        : http_owned_response_file(http_native_path_string_type(file), size, offset,
              length, identity, std::move(opened)) {}
    http_native_path_string_type native_path_;
    std::uint64_t size_;
    std::uint64_t offset_;
    std::uint64_t length_;
    http_response_file_identity identity_;
    http_response_file_descriptor opened_;
};

class http_multipart_response_body final {
public:
    http_multipart_response_body(http_native_path_view_type path,
        std::uint64_t size, http_response_file_identity identity, http_multipart_byte_range_plan plan,
        http_response_file_descriptor opened)
        : native_path_(path),
          plan_(std::move(plan)),
          size_(size),
          identity_(identity),
          opened_(std::move(opened)) {
        if (plan_.segments().empty()) {
            throw std::invalid_argument("multipart response plan must contain segments");
        }
        bool contains_file = false;
        for (const auto& segment : plan_.segments()) {
            if (segment.kind_ == http_multipart_byte_range_plan::segment_kind::file) {
                if (segment.file_length_ == 0 || segment.file_offset_ > size_ ||
                    segment.file_length_ > size_ - segment.file_offset_) {
                    throw std::invalid_argument("multipart response file segment is out of range");
                }
                contains_file = true;
            }
        }
        if (!contains_file) {
            throw std::invalid_argument("multipart response plan must contain a file segment");
        }
    }

    [[nodiscard]] std::size_t segment_count() const noexcept {
        return plan_.segments().size();
    }
    [[nodiscard]] http_response_body_segment_view segment(std::size_t index) const {
        const auto& item = plan_.segments()[index];
        if (item.kind_ == http_multipart_byte_range_plan::segment_kind::metadata) {
            return {.bytes_ = plan_.metadata().substr(item.metadata_offset_, item.metadata_length_)};
        }
        return {.file_ = http_response_file_view(native_path_.c_str(), size_, item.file_offset_,
                    item.file_length_, identity_, &opened_)};
    }
    [[nodiscard]] std::uint64_t content_length() const noexcept {
        return plan_.content_length();
    }
    [[nodiscard]] http_response_file_view file() const noexcept {
        return http_response_file_view(native_path_.c_str(), size_, 0, size_, identity_, &opened_);
    }

private:
    friend class http_response_body;

    http_multipart_response_body(http_native_path_string_type&& native_path,
        http_multipart_byte_range_plan&& plan, std::uint64_t size,
        http_response_file_identity identity, http_response_file_descriptor opened) noexcept
        : native_path_(std::move(native_path)),
          plan_(std::move(plan)),
          size_(size),
          identity_(identity),
          opened_(std::move(opened)) {}

    http_native_path_string_type native_path_;
    http_multipart_byte_range_plan plan_;
    std::uint64_t size_{};
    http_response_file_identity identity_{http_response_file_identity::unchecked()};
    http_response_file_descriptor opened_;
};

// Owns exactly one legal buffered response-body representation. The common
// bytes()/file()/size() observations are derived from the active alternative.
class http_response_body final {
public:
    http_response_body() noexcept
        : value_(http_empty_response_body{}) {}

    http_response_body(const http_response_body&) = delete;
    http_response_body& operator=(const http_response_body&) = delete;
    http_response_body(http_response_body&& other) noexcept
        : value_(std::move(other.value_)) {
        other.set_empty();
    }
    http_response_body& operator=(http_response_body&&) = delete;

    [[nodiscard]] const http_static_response_bytes* static_bytes() const& noexcept {
        return std::get_if<http_static_response_bytes>(&value_);
    }
    [[nodiscard]] const http_static_response_bytes* static_bytes() const&& = delete;

    [[nodiscard]] const http_owned_response_bytes* owned_bytes() const& noexcept {
        return std::get_if<http_owned_response_bytes>(&value_);
    }
    [[nodiscard]] const http_owned_response_bytes* owned_bytes() const&& = delete;

    [[nodiscard]] const http_owned_response_file* owned_file() const& noexcept {
        return std::get_if<http_owned_response_file>(&value_);
    }
    [[nodiscard]] const http_owned_response_file* owned_file() const&& = delete;

    [[nodiscard]] const http_multipart_response_body* multipart_body() const& noexcept {
        return std::get_if<http_multipart_response_body>(&value_);
    }
    [[nodiscard]] const http_multipart_response_body* multipart_body() const&& = delete;

    [[nodiscard]] std::string_view bytes() const& noexcept {
        if (const auto* body = static_bytes()) {
            return body->bytes();
        }
        if (const auto* body = owned_bytes()) {
            return body->bytes();
        }
        return {};
    }
    [[nodiscard]] std::string_view bytes() const&& = delete;

    [[nodiscard]] std::optional<http_response_file_view> file() const& noexcept {
        if (const auto* body = owned_file()) {
            return body->view();
        }
        if (const auto* body = multipart_body()) {
            return body->file();
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<http_response_file_view> file() const&& = delete;

    [[nodiscard]] bool file_representation() const noexcept {
        if (const auto* body = owned_bytes()) {
            return body->file_representation();
        }
        return owned_file() != nullptr || multipart_body() != nullptr;
    }

    [[nodiscard]] bool head_metadata_only() const noexcept {
        if (const auto* body = std::get_if<http_empty_response_body>(&value_)) {
            return body->head_metadata_only();
        }
        return false;
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        if (const auto* body = owned_file()) {
            return body->length();
        }
        if (const auto* body = multipart_body()) {
            return body->content_length();
        }
        return static_cast<std::uint64_t>(bytes().size());
    }

private:
    friend class ::ruvia::http_response;
    friend struct http_response_body_access;

    using value_type = std::variant<http_empty_response_body, http_static_response_bytes,
        http_owned_response_bytes, http_owned_response_file, http_multipart_response_body>;
    static_assert(std::is_nothrow_move_constructible_v<value_type>);

    template <typename body_type>
    void replace(body_type&& body) noexcept {
        // Publish the complete alternative before a resource callback can reenter.
        value_type retired(std::move(value_));
        value_.emplace<std::remove_cvref_t<body_type>>(std::forward<body_type>(body));
    }

    void set_empty() noexcept {
        replace(http_empty_response_body{});
    }

    void set_head_metadata_only() noexcept {
        replace(http_empty_response_body(true));
    }

    void set_copy(std::string_view bytes_value) {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(bytes_value);
        replace(std::move(body));
    }

    void set_static(std::string_view bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        replace(http_static_response_bytes(bytes_value));
    }

    void set_owned(std::string&& bytes_value) noexcept {
        if (bytes_value.empty()) {
            set_empty();
            return;
        }
        http_owned_response_bytes body(std::move(bytes_value), false);
        replace(std::move(body));
    }

    // Kept even when empty: an empty file is still a file representation.
    void set_file_bytes(std::string&& bytes_value) noexcept {
        http_owned_response_bytes body(std::move(bytes_value), true);
        replace(std::move(body));
    }

    void set_owned_file(http_native_path_view_type file,
        std::uint64_t size, std::uint64_t offset, std::uint64_t length,
        http_response_file_identity identity, http_response_file_descriptor opened) {
        http_owned_response_file body(file, size, offset, length, identity, std::move(opened));
        replace(std::move(body));
    }

    void set_multipart(http_native_path_view_type file,
        std::uint64_t size, http_response_file_identity identity,
        http_multipart_byte_range_plan plan, http_response_file_descriptor opened) {
        http_multipart_response_body replacement(
            file, size, identity, std::move(plan), std::move(opened));
        replace(std::move(replacement));
    }

    value_type value_;
};

static_assert(std::is_nothrow_move_constructible_v<http_owned_response_bytes>);
static_assert(std::is_nothrow_move_constructible_v<http_owned_response_file>);
static_assert(std::is_nothrow_move_constructible_v<http_multipart_response_body>);

}  // namespace detail

// Composes one header block with one body. Header rules belong to
// http_response_headers; this type only publishes body and headers together.
class http_response final {
public:
    http_response() noexcept;

    http_response(const http_response&) = delete;
    http_response& operator=(const http_response&) = delete;
    // Moving transfers headers and body. The source remains reusable with
    // empty headers and body.
    http_response(http_response&& other) noexcept;
    http_response& operator=(http_response&& other) noexcept;

    [[nodiscard]] http_status_code status() const noexcept;
    [[nodiscard]] const http_response_headers& headers() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] const http_response_headers& headers() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const&& = delete;
    // A generic http_response is always final (200..599). Interim 1xx progress
    // messages use http_interim_response_head; 101 uses a dedicated protocol driver.
    void status(http_status_code status_code);
    // Header mutations follow http_response_headers.
    void header(std::string_view key, std::string_view value);
    void header(std::string_view key, std::string_view value, http_response_header_options options);
    void header_stable_view(std::string_view key, std::string_view value);
    void content_type(http_content_type type);
    // Format Allow from known-method bits followed by extension method values.
    // Extension methods must be nonempty HTTP tokens and may borrow current headers.
    // The resulting field is owned by this response; refresh views after mutation.
    void allow_methods(
        std::uint32_t method_mask, std::span<const std::string_view> extension_methods = {});
    void remove_header(std::string_view key);
    void set_cookie(const set_cookie_plan& plan);
    // Copy the source's application headers with RFC-aware append and cookie semantics.
    // Assignment preserves the destination Content-Type.
    void transfer_headers_from(const http_response& source, http_response_header_transfer mode);
    // Publish a header-only transaction staged from headers().clone() without
    // allocation. The staged block is left empty and independent.
    void commit_headers(http_response_headers&& staged) noexcept;
    // Borrowed byte representation; file and empty bodies return an empty view.
    [[nodiscard]] std::string_view body() const& noexcept RUVIA_LIFETIMEBOUND {
        return body_bytes();
    }
    std::string_view body() const&& = delete;
    [[nodiscard]] std::string_view body_bytes() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::string_view body_bytes() const&& = delete;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const& noexcept RUVIA_LIFETIMEBOUND;
    [[nodiscard]] std::optional<http_response_file_view> file_body() const&& = delete;
    [[nodiscard]] bool has_multipart_file_body() const noexcept;
    // True for file_body(), multipart_file_body() and file_bytes_body() bodies:
    // the bytes are a file's representation, which a sender transmits as is.
    [[nodiscard]] bool has_file_representation() const noexcept;
    // 将正文替换为仅描述 HEAD 所选表示的空正文，保留首部并释放原正文资源。
    // 序列化 HEAD 时保留显式 Content-Length；未提供长度时不推断为零。
    // 标记随移动转移；重新设置正文会清除它，不影响 GET 等方法的正文规则。
    void head_metadata_body() noexcept;
    [[nodiscard]] bool has_head_metadata_body() const noexcept;
    // File segments retain their path and identity even when their length is
    // zero. Empty byte bodies have no segments; multipart bodies retain their
    // planned sequence of file ranges and boundary bytes.
    [[nodiscard]] std::size_t body_segment_count() const noexcept;
    [[nodiscard]] http_response_body_segment_view body_segment(std::size_t index) const& RUVIA_LIFETIMEBOUND;
    http_response_body_segment_view body_segment(std::size_t index) const&& = delete;
    void body(std::string_view value);
    void owned_body(std::string&& value) noexcept;
    void static_body(std::string_view value) noexcept;
    // Own bytes read from a file: all of it, a byte range, or a multipart
    // byte-range payload of it. Unlike owned_body(), the body remains a file
    // representation, even when empty.
    void file_bytes_body(std::string&& value) noexcept;
    // Own the native path and preserve the
    // runtime's opaque identity token. `opened`, when engaged, is the descriptor the
    // runtime framed the response from; the body owns it and releases it on
    // destruction or replacement. Path and range validation precede body
    // replacement; a rejected call releases `opened`.
    void file_body(http_native_path_view_type file, http_content_length size,
        http_content_length offset, http_content_length length, http_response_file_identity identity,
        http_response_file_descriptor opened = {});
    void multipart_file_body(http_native_path_view_type file, http_content_length size,
        http_response_file_identity identity, http_multipart_byte_range_plan&& plan,
        http_response_file_descriptor opened = {});
    void content_range(
        http_content_length offset, http_content_length length, http_content_length size);
    void content_range_unsatisfied(http_content_length size);
    void add_vary_token(std::string_view token);
    void reserve_headers(std::size_t count);
    // Apply a streaming coding, or atomically replace a buffered representation
    // and its coding-dependent metadata. The coding must be a nonempty HTTP coding list;
    // outer SP/HTAB are removed before storing its field value.
    // Both set Content-Encoding, weaken a strong ETag, drop an ETag that is not a
    // quoted entity-tag, and drop Accept-Ranges; the replacement sets
    // Content-Length, the streaming form removes it. A failed replacement leaves
    // this response unchanged.
    void apply_content_encoding(std::string_view content_encoding);
    void replace_body_with_content_encoding(
        std::string&& value, std::string_view content_encoding);

private:
    friend std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response, http_protocol_version);
    friend http1_response_head_plan prepare_http1_upgrade_response_head(http_response&, const http_request&);
    friend struct detail::http_response_body_access;

    void apply_encoded_representation(std::string_view content_encoding, std::string* body);

    http_status_code status_code_{http_status::ok};
    http_response_headers headers_;
    detail::http_response_body body_;
};

}  // namespace ruvia
