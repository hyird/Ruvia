#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
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
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"

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

namespace ruvia {

class http_origin_view;
enum class cookie_prefix : std::uint8_t {
    secure,  // serializes the name as "__Secure-<name>"; requires secure
    host,    // serializes the name as "__Host-<name>"; requires secure, Path=/, no Domain
};

enum class cookie_same_site : std::uint8_t {
    strict,
    lax,
    none,  // the literal SameSite=None attribute; requires Secure
};

enum class cookie_priority : std::uint8_t {
    low,
    medium,
    high,
};

enum class cookie_attribute_policy : std::uint8_t {
    omit,
    emit,
};

// Expires value for deleting a cookie (Thu, 01 Jan 1970 00:00:00 GMT). RFC 6265
// section 4.1.1 limits max-age-av to non-zero digits, so deletion uses an
// Expires date in the past (RFC 6265 section 3.1) instead of Max-Age=0.
inline constexpr std::chrono::system_clock::time_point cookie_deletion_expires{};

// RFC 6265bis user-agent cookie-age-limit (400 days): the outbound cookie jar caps
// received lifetimes at this value. The Set-Cookie writer does not enforce it,
// since the limit binds user agents, not servers.
inline constexpr std::int64_t max_cookie_age_seconds = 34560000;

// One Cookie request cookie-pair, "name=value". The '=' is always written,
// including for an empty name, so a nameless value that itself looks like a
// cookie-pair stays behind '='. Callers join pairs with "; " (RFC 6265
// section 4.2.1).
[[nodiscard]] constexpr std::size_t cookie_request_pair_size(std::string_view name, std::string_view value) noexcept {
    return name.size() + 1U + value.size();
}

// Writes cookie_request_pair_size(name, value) bytes at out and returns the
// end of the written pair. Only inputs overlapping the written bytes need a
// snapshot: short snapshots use the stack, otherwise heap storage.
// Allocation failure leaves the output untouched.
char* write_cookie_request_pair(char* out, std::string_view name, std::string_view value);

// Cookie request pairs always include '=', including for an empty name.
// Inputs may borrow header_value, including its terminating NUL.
// Failure preserves the original contents; capacity may grow.
template <typename string>
void append_cookie_request_pair(string& header_value, std::string_view name, std::string_view value) {
    const auto start = header_value.size();
    const std::size_t separator_bytes = start == 0 ? 0 : 2;
    const auto remaining = header_value.max_size() - start;
    if (remaining < separator_bytes + 1 || name.size() > remaining - separator_bytes - 1 ||
        value.size() > remaining - separator_bytes - 1 - name.size()) {
        throw std::length_error("cookie request header length is not representable");
    }
    const auto borrowed_offset = [&](std::string_view input) -> std::optional<std::size_t> {
        if (input.empty()) {
            return std::nullopt;
        }
        const auto first = reinterpret_cast<std::uintptr_t>(header_value.data());
        const auto input_address = reinterpret_cast<std::uintptr_t>(input.data());
        if (input_address < first || input_address - first > start) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(input_address - first);
    };
    const auto name_offset = borrowed_offset(name);
    const auto value_offset = borrowed_offset(value);
    // Growing may reallocate; the value-initialized bytes keep a borrowed
    // terminator readable as NUL until the writer consumes it.
    header_value.resize(start + separator_bytes + cookie_request_pair_size(name, value));
    auto* const bytes = header_value.data();
    if (name_offset) {
        name = std::string_view(bytes + *name_offset, name.size());
    }
    if (value_offset) {
        value = std::string_view(bytes + *value_offset, value.size());
    }
    // The pair is written before the separator, which would otherwise
    // overwrite a borrowed terminator first.
    try {
        (void)write_cookie_request_pair(bytes + start + separator_bytes, name, value);
    } catch (...) {
        header_value.resize(start);
        throw;
    }
    if (separator_bytes != 0) {
        bytes[start] = ';';
        bytes[start + 1] = ' ';
    }
}

// Cookie octet validation and name-prefix rules are protocol rules shared by
// the Set-Cookie writer and outbound cookie jar. is_valid_cookie_value accepts
// the RFC 6265 cookie-value grammar, including one DQUOTE pair around the octets.
[[nodiscard]] bool is_valid_cookie_value(std::string_view value) noexcept;
// The prefix a wire cookie name starts with, matched ASCII case-insensitively
// as user agents do (RFC 6265bis 4.1.3); nullopt when it carries none.
[[nodiscard]] std::optional<cookie_prefix> cookie_name_prefix(std::string_view name) noexcept;
// RFC 6265bis 4.1.3: __Secure- requires Secure; __Host- also requires Path=/
// and no Domain. `path` is the Path attribute value, empty when absent.
[[nodiscard]] bool cookie_prefix_requirements_met(
    cookie_prefix prefix, bool secure, std::string_view path, std::string_view domain) noexcept;
[[nodiscard]] std::string_view http_cookie_prefix_text(cookie_prefix prefix) noexcept;

struct cookie_options final {
    // Cookie attributes are retained by set_cookie_plan until serialization.
    // Keep their zero-copy representation, but reject owning-string rvalues so
    // a stored options value cannot silently contain an already-dangling view.
    ::ruvia::borrowed_text path_{"/"};
    ::ruvia::borrowed_text domain_{};
    std::optional<cookie_same_site> same_site_{};
    std::optional<cookie_priority> priority_{};
    // UTC, rounded down to whole seconds; year >= 1601. Lifetime caps belong to user agents.
    std::optional<std::chrono::system_clock::time_point> expires_{};
    // Zero means expire now: written as Expires=cookie_deletion_expires (replacing
    // expires_), since max-age-av requires a non-zero first digit (RFC 6265 4.1.1).
    std::optional<std::chrono::seconds> max_age_{};
    std::optional<cookie_prefix> prefix_{};
    cookie_attribute_policy http_only_{cookie_attribute_policy::omit};
    cookie_attribute_policy secure_{cookie_attribute_policy::omit};
    cookie_attribute_policy partitioned_{cookie_attribute_policy::omit};
};

// Visits literal semicolon-separated Cookie name/value pairs. A quote has no
// special meaning in a Cookie field. Segments without '=' are skipped, names
// and values are OWS-trimmed, and returning false stops traversal. Views borrow
// the input field.
template <typename visitor_type>
inline void http_visit_cookie_pairs(std::string_view value, visitor_type&& visitor) {
    while (!value.empty()) {
        const auto semicolon = value.find(';');
        const auto part = http_trim_ows(
            semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos &&
            !visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)))) {
            return;
        }
        if (semicolon == std::string_view::npos) {
            return;
        }
        value.remove_prefix(semicolon + 1);
    }
}

class http_response_headers;

// Validates and fixes the exact Set-Cookie field-value shape before a runtime
// allocates its output buffer. The plan borrows name, value, path and domain
// unchanged until write() completes, so owning-string and cookie_options
// temporaries are rejected at construction. Construction throws std::length_error if the wire
// length cannot be represented by std::size_t.
class set_cookie_plan final {
public:
    set_cookie_plan(std::string_view name, std::string_view value, const cookie_options& options);

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
    set_cookie_plan(name_type&&, value_type&&, const cookie_options&) = delete;

    set_cookie_plan(std::string_view, std::string_view, cookie_options&&) = delete;
    set_cookie_plan(std::string_view, std::string_view, const cookie_options&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view wire_prefix() const noexcept {
        return prefix_text_;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::string_view domain() const noexcept {
        return domain_;
    }

    // output must cover size() bytes and not overlap the borrowed inputs.
    void write(char* output) const;

private:
    friend class http_response_headers;
    struct written_fields final {
        std::string_view wire_name_;
        std::string_view path_;
        std::string_view domain_;
    };
    [[nodiscard]] written_fields write_fields(char* output) const noexcept;
    std::array<char, 32> expires_buffer_{};
    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::string_view prefix_text_;
    std::string_view priority_text_;
    std::string_view same_site_text_;
    std::uint64_t max_age_value_{0};
    std::size_t expires_size_{0};
    std::size_t size_{0};
    bool http_only_{false};
    bool secure_{false};
    bool partitioned_{false};
};

struct http_cookie_jar_options final {
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
};

// Sans-I/O RFC 6265 client cookie jar for a fixed origin. Validates Domain,
// Path, Secure, SameSite=None, Partitioned, and __Secure-/__Host- prefixes,
// caps Max-Age/Expires at 400 days, evicts expired entries, and enforces
// count and byte budgets.
class http_cookie_jar final {
public:
    explicit http_cookie_jar(const http_origin_view& origin, http_cookie_jar_options options = {});
    http_cookie_jar(http_scheme scheme, std::string_view host, http_cookie_jar_options options = {});

    void add_configured_cookie(std::string_view name, std::string_view value);

    bool absorb_set_cookie(std::string_view request_target, std::string_view set_cookie_value,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    void retain_response_cookies(std::string_view request_target,
        std::span<const http_header> headers,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    void retain_response_cookies(std::string_view request_target,
        std::span<const http_header_view> headers,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    [[nodiscard]] std::string cookie_header(std::string_view request_target,
        bool include_received = true,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

    template <typename fn_type>
    void visit_matching_cookies(std::string_view request_target, bool include_received,
        std::chrono::system_clock::time_point now, fn_type&& fn) {
        if (cookies_.empty()) {
            return;
        }
        if (next_expiry_.has_value() && now >= *next_expiry_) {
            discard_expired(now);
        }
        if (cookies_.empty()) {
            return;
        }
        const auto path = request_path_only(request_target);
        for (const auto& cookie : cookies_) {
            if (!cookie.configured_ && !include_received) {
                continue;
            }
            if (cookie.secure_ && scheme_ != http_scheme::https) {
                continue;
            }
            if (!cookie_path_matches(path, cookie.path_)) {
                continue;
            }
            fn(std::string_view(cookie.name_), std::string_view(cookie.value_));
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return cookies_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return cookies_.empty();
    }
    [[nodiscard]] std::size_t storage_bytes() const noexcept {
        return cookie_bytes_;
    }
    void clear() noexcept;

    [[nodiscard]] static bool cookie_domain_matches(
        std::string_view host, std::string_view domain) noexcept;
    [[nodiscard]] static bool cookie_path_matches(
        std::string_view request_path, std::string_view cookie_path) noexcept;
    [[nodiscard]] static std::string_view request_path_only(std::string_view target) noexcept;
    [[nodiscard]] static std::string_view default_cookie_path(std::string_view target) noexcept;

private:
    struct stored_cookie final {
        stored_cookie(std::string_view name, std::string_view value)
            : stored_cookie(name, value, "/", {}) {}
        stored_cookie(std::string_view name, std::string_view value,
            std::string_view path, std::string_view domain)
            : name_(name),
              value_(value),
              path_(path),
              domain_(domain) {}

        std::string name_;
        std::string value_;
        std::string path_;
        std::string domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool configured_{true};
    };

    struct received_cookie final {
        std::string_view name_;
        std::string_view value_;
        std::string_view path_;
        std::string_view domain_;
        std::optional<std::chrono::system_clock::time_point> expires_;
        bool secure_{false};
        bool partitioned_{false};
        bool expired_{false};
    };

    [[nodiscard]] std::optional<received_cookie> accept_set_cookie_view(
        std::string_view request_target, std::string_view set_cookie_value,
        std::chrono::system_clock::time_point now) const;
    bool store_received_cookie(const received_cookie& cookie);
    void reserve_cookie_slot();
    void discard_expired(std::chrono::system_clock::time_point now);
    [[nodiscard]] static std::size_t entry_storage_bytes(std::string_view name,
        std::string_view value, std::string_view path, std::string_view domain) noexcept;
    [[nodiscard]] bool has_capacity(
        std::size_t replaced_bytes, std::size_t replacement_bytes, bool adding) const noexcept;
    void recompute_next_expiry() noexcept;

    std::string host_;
    http_scheme scheme_{http_scheme::http};
    std::size_t max_cookies_{256};
    std::size_t max_cookie_bytes_{std::size_t{32} * 1024};
    std::vector<stored_cookie> cookies_;
    std::optional<std::chrono::system_clock::time_point> next_expiry_{};
    std::size_t cookie_bytes_{0};
};

}  // namespace ruvia

namespace ruvia {

enum class http_set_cookie_attribute : std::uint8_t {
    secure = 1U << 0,
    partitioned = 1U << 1,
    http_only = 1U << 2,
};

// Borrowed, allocation-free Set-Cookie fields for outbound client runtimes,
// following the RFC 6265 section 5.2 user-agent algorithm. A cookie without '=',
// with an empty name, over 4096 name+value bytes, or with a control byte other
// than HTAB is rejected. Unknown and oversized attributes are ignored. path() is
// empty when the last Path value is
// empty or does not start with '/', meaning the caller's default-path. An empty
// Domain value is ignored; a leading '.' is removed, so domain() may be empty
// (host-only) and is not validated beyond domain-matching by the caller. Expires follows RFC 6265 cookie-date token grammar, including its
// permitted suffixes; this does not relax the separate HTTP-date grammar.
// Partitioned is reported independently of Secure; callers own origin policy.
class http_set_cookie_view final {
public:
    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr std::string_view path() const noexcept {
        return path_;
    }

    [[nodiscard]] constexpr std::string_view domain() const noexcept {
        return domain_;
    }

    [[nodiscard]] constexpr std::optional<std::time_t> expires() const noexcept {
        return expires_;
    }

    [[nodiscard]] constexpr std::optional<std::int64_t> max_age_seconds() const noexcept {
        return max_age_seconds_;
    }

    [[nodiscard]] constexpr bool has(http_set_cookie_attribute attribute) const noexcept {
        const auto mask = static_cast<std::uint8_t>(attribute);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (attributes_ & mask) != 0U;
    }

    [[nodiscard]] constexpr bool http_only() const noexcept {
        return has(http_set_cookie_attribute::http_only);
    }

    [[nodiscard]] constexpr bool same_site_none() const noexcept {
        return same_site_ == cookie_same_site::none;
    }

    [[nodiscard]] constexpr std::optional<cookie_same_site> same_site() const noexcept {
        return same_site_;
    }

private:
    friend std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

    constexpr http_set_cookie_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    constexpr void set(http_set_cookie_attribute attribute) noexcept {
        attributes_ |= static_cast<std::uint8_t>(attribute);
    }

    std::string_view name_;
    std::string_view value_;
    std::string_view path_;
    std::string_view domain_;
    std::optional<std::time_t> expires_;
    std::optional<std::int64_t> max_age_seconds_;
    std::optional<cookie_same_site> same_site_{};
    std::uint8_t attributes_{0};
};

[[nodiscard]] std::optional<http_set_cookie_view> parse_set_cookie(std::string_view value) noexcept;

template <detail::http_temporary_owning_char_string value_type>
std::optional<http_set_cookie_view> parse_set_cookie(value_type&&) = delete;

}  // namespace ruvia

namespace ruvia {

// Wire width of an RFC 9110 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
inline constexpr std::size_t http_imf_fixdate_size = 29;

// RFC 9110 IMF-fixdate, or no value if the timestamp cannot be represented.
// The fixed-size result contains the 29 wire bytes without a terminator.
[[nodiscard]] std::optional<std::array<char, http_imf_fixdate_size>> format_http_date(std::time_t value) noexcept;

// Parse an HTTP-date (RFC 7231 section 7.1.1.1: IMF-fixdate / RFC 850 / asctime) as used by Date,
// Expires, Last-Modified, If-Modified-Since. Returns std::nullopt if malformed.
[[nodiscard]] std::optional<std::time_t> parse_http_date(std::string_view value) noexcept;

}  // namespace ruvia

namespace ruvia {

class http_response;
class http_response_headers;

enum class cache_control_directive : std::uint16_t {
    no_store = 1U << 0,
    no_cache = 1U << 1,
    no_transform = 1U << 2,
    must_revalidate = 1U << 3,
    proxy_revalidate = 1U << 4,
    private_value = 1U << 5,
    public_value = 1U << 6,
    immutable = 1U << 7,
    only_if_cached = 1U << 8,
    max_stale_any = 1U << 9,
    private_unqualified = 1U << 10,
    no_cache_unqualified = 1U << 11,
};

// Parsed HTTP Cache-Control directives (RFC 9111 section 5.2). Unknown directives are ignored.
// Boolean directives are queried by directive enum; delta-seconds directives are optional (absent = not present).
// This type reports wire directives only; cache freshness and reuse policy belong to the caller.
class cache_control final {
public:
    [[nodiscard]] constexpr bool has(cache_control_directive directive) const noexcept {
        const auto mask = static_cast<std::uint16_t>(directive);
        return mask != 0U && (mask & (mask - 1U)) == 0U && (directives_ & mask) != 0U;
    }

    [[nodiscard]] constexpr bool no_store() const noexcept {
        return has(cache_control_directive::no_store);
    }
    [[nodiscard]] constexpr bool no_cache() const noexcept {
        return has(cache_control_directive::no_cache);
    }
    [[nodiscard]] constexpr bool no_transform() const noexcept {
        return has(cache_control_directive::no_transform);
    }
    [[nodiscard]] constexpr bool must_revalidate() const noexcept {
        return has(cache_control_directive::must_revalidate);
    }
    [[nodiscard]] constexpr bool proxy_revalidate() const noexcept {
        return has(cache_control_directive::proxy_revalidate);
    }
    [[nodiscard]] constexpr bool is_private() const noexcept {
        return has(cache_control_directive::private_value);
    }
    [[nodiscard]] constexpr bool is_unqualified_private() const noexcept {
        return has(cache_control_directive::private_unqualified);
    }
    [[nodiscard]] constexpr bool is_field_qualified_private() const noexcept {
        return is_private() && !is_unqualified_private();
    }
    [[nodiscard]] constexpr bool is_unqualified_no_cache() const noexcept {
        return has(cache_control_directive::no_cache_unqualified);
    }
    [[nodiscard]] constexpr bool is_field_qualified_no_cache() const noexcept {
        return no_cache() && !is_unqualified_no_cache();
    }
    [[nodiscard]] constexpr bool is_public() const noexcept {
        return has(cache_control_directive::public_value);
    }
    [[nodiscard]] constexpr bool immutable() const noexcept {
        return has(cache_control_directive::immutable);
    }
    [[nodiscard]] constexpr bool only_if_cached() const noexcept {
        return has(cache_control_directive::only_if_cached);
    }
    [[nodiscard]] constexpr bool max_stale_any() const noexcept {
        return has(cache_control_directive::max_stale_any);
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_age() const noexcept {
        return max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> max_stale() const noexcept {
        return max_stale_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> min_fresh() const noexcept {
        return min_fresh_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> s_maxage() const noexcept {
        return s_max_age_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_while_revalidate() const noexcept {
        return stale_while_revalidate_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> stale_if_error() const noexcept {
        return stale_if_error_;
    }

    [[nodiscard]] constexpr std::size_t private_field_count() const noexcept {
        return is_unqualified_private() ? 0U : private_fields_.count_;
    }
    [[nodiscard]] constexpr std::string_view private_field(
        std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_private() ? std::string_view{} : private_fields_.at(index);
    }
    std::string_view private_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_private_field(std::string_view field_name) const noexcept;

    [[nodiscard]] constexpr std::size_t no_cache_field_count() const noexcept {
        return is_unqualified_no_cache() ? 0U : no_cache_fields_.count_;
    }
    [[nodiscard]] constexpr std::string_view no_cache_field(
        std::size_t index) const& noexcept RUVIA_LIFETIMEBOUND {
        return is_unqualified_no_cache() ? std::string_view{} : no_cache_fields_.at(index);
    }
    std::string_view no_cache_field(std::size_t) const&& = delete;
    [[nodiscard]] bool has_no_cache_field(std::string_view field_name) const noexcept;

private:
    friend class cache_control_field_parser;
    friend std::optional<cache_control> parse_targeted_cache_control(
        std::string_view value) noexcept;
    template <typename input_type>
    friend std::optional<cache_control> parse_targeted_cache_control_from_input(
        input_type input) noexcept;

    static constexpr std::size_t max_nominated_fields = 16;
    static constexpr std::size_t max_nominated_field_bytes = 256;

    struct nominated_field_set final {
        std::array<char, max_nominated_field_bytes> storage_{};
        std::array<std::uint16_t, max_nominated_fields> offsets_{};
        std::array<std::uint8_t, max_nominated_fields> lengths_{};
        std::uint16_t storage_used_{0};
        std::uint8_t count_{0};

        constexpr void clear() noexcept {
            storage_used_ = 0;
            count_ = 0;
        }

        [[nodiscard]] constexpr std::string_view at(std::size_t index) const noexcept {
            if (index >= count_) {
                return {};
            }
            return std::string_view(storage_.data() + offsets_[index], lengths_[index]);
        }

        [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
        [[nodiscard]] bool add_lowercase(std::string_view field_name) noexcept;
    };

    constexpr void set(cache_control_directive directive) noexcept {
        directives_ |= static_cast<std::uint16_t>(directive);
    }
    constexpr void clear_directive(cache_control_directive directive) noexcept {
        directives_ &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(directive));
    }

    std::uint16_t directives_{0};
    std::optional<std::uint64_t> max_age_;
    std::optional<std::uint64_t> max_stale_;
    std::optional<std::uint64_t> min_fresh_;
    std::optional<std::uint64_t> s_max_age_;
    std::optional<std::uint64_t> stale_while_revalidate_;
    std::optional<std::uint64_t> stale_if_error_;
    nominated_field_set private_fields_{};
    nominated_field_set no_cache_fields_{};
};

// Incrementally parses every Cache-Control field line as one logical directive
// list (RFC 9110 section 5.2). State spans updates so duplicate freshness
// directives keep the first occurrence even when they appear on different lines.
class cache_control_field_parser final {
public:
    void update(std::string_view field_value) noexcept;

    [[nodiscard]] cache_control finish() const noexcept {
        return value_;
    }

private:
    cache_control value_;
    bool max_age_seen_{false};
    bool max_stale_seen_{false};
    bool min_fresh_seen_{false};
    bool s_max_age_seen_{false};
    bool stale_while_revalidate_seen_{false};
    bool stale_if_error_seen_{false};
};

struct targeted_cache_control_selection final {
    cache_control control_{};
    std::string_view selected_header_{};
    bool targeted_{false};
};

class http_vary_spec;

namespace detail {
[[nodiscard]] cache_control parse_cache_control_headers(http_header_range_view headers) noexcept;
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control_headers(
    http_header_range_view headers, std::string_view header_name) noexcept;
[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control_headers(
    http_header_range_view headers, std::span<const std::string_view> target_headers) noexcept;
[[nodiscard]] std::string rewrite_http_cache_control_max_age_headers(
    http_header_range_view headers, std::uint64_t client_max_age);
[[nodiscard]] std::optional<std::string> format_http_vary_cache_key_headers(
    const http_vary_spec& vary, http_header_range_view request_headers);
[[nodiscard]] std::uint64_t compute_http_cache_age_headers(
    http_header_range_view response_headers, std::time_t request_time, std::time_t response_time,
    std::time_t now) noexcept;
void merge_http_304_response_headers_into(
    std::vector<http_header>& cached_headers, http_header_range_view not_modified_headers);
void merge_http_304_response_headers_into(
    http_response_headers& cached_headers, http_header_range_view not_modified_headers);
}  // namespace detail

// Parse a Cache-Control field value (a single line, or several joined by commas).
[[nodiscard]] cache_control parse_cache_control(std::string_view value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] cache_control parse_cache_control(const headers_type& headers) noexcept {
    return detail::parse_cache_control_headers(headers);
}

// RFC 9213 Targeted HTTP Cache Control (e.g. CDN-Cache-Control).
// Parses an RFC 8941 Structured Field Dictionary of cache directives; returns
// std::nullopt when absent, empty, or syntactically invalid per RFC 9213 §2.1.
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(
    std::string_view value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] std::optional<cache_control> parse_targeted_cache_control(
    const headers_type& headers,
    std::string_view header_name = "CDN-Cache-Control") noexcept {
    return detail::parse_targeted_cache_control_headers(headers, header_name);
}

// RFC 9213 §2.2 target-list selection: evaluates `target_headers` in priority
// order and returns the first present, valid non-empty targeted policy; falls
// back to standard Cache-Control when no targeted header matches.
template <http_header_field_source headers_type>
[[nodiscard]] targeted_cache_control_selection resolve_targeted_cache_control(
    const headers_type& headers,
    std::span<const std::string_view> target_headers) noexcept {
    return detail::resolve_targeted_cache_control_headers(headers, target_headers);
}

// RFC 9111 §5.2.2.7 & §5.2.2.3 field-qualified header stripping for shared caches.
// Returns false if unqualified `private` or `no-store` forbids shared-cache
// storage; returns true when storable (after stripping any field-qualified
// `private="field1, field2"` headers in place).
bool strip_http_cache_private_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept;
bool strip_http_cache_private_headers(
    http_response_headers& headers, const cache_control& control);
bool strip_http_cache_private_headers(
    http_response& response, const cache_control& control);
bool strip_http_cache_private_headers(std::vector<http_header>& headers) noexcept;
bool strip_http_cache_private_headers(http_response_headers& headers);
bool strip_http_cache_private_headers(http_response& response);

bool strip_http_cache_no_cache_headers(
    std::vector<http_header>& headers, const cache_control& control) noexcept;
bool strip_http_cache_no_cache_headers(
    http_response_headers& headers, const cache_control& control);
bool strip_http_cache_no_cache_headers(
    http_response& response, const cache_control& control);
[[nodiscard]] bool http_cache_control_has_no_transform(std::string_view field_value) noexcept;
template <http_header_field_source headers_type>
[[nodiscard]] bool http_cache_control_has_no_transform(const headers_type& headers) noexcept {
    return parse_cache_control(headers).no_transform();
}

struct http_cache_control_directive_view final {
    std::string_view raw_{};
    std::string_view name_{};
    std::optional<std::string_view> argument_{};
};

// Rewrites Cache-Control to preserve all directives except max-age and s-maxage
// while setting client max-age=<client_max_age>. Header-block overloads also
// clear Expires so the rewritten max-age governs client freshness.
[[nodiscard]] std::string rewrite_http_cache_control_max_age(
    std::string_view field_value, std::uint64_t client_max_age);
template <http_header_field_source headers_type>
[[nodiscard]] std::string rewrite_http_cache_control_max_age(
    const headers_type& headers, std::uint64_t client_max_age) {
    return detail::rewrite_http_cache_control_max_age_headers(headers, client_max_age);
}
void rewrite_http_response_cache_max_age(
    std::vector<http_header>& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response_headers& response_headers, std::uint64_t client_max_age);
void rewrite_http_response_cache_max_age(
    http_response& response, std::uint64_t client_max_age);

// Parsed multi-line Vary specification (RFC 9111 section 4.1). Field names are
// normalized to lowercase, deduplicated, and sorted in fixed ASCII order.
class http_vary_spec final {
public:
    http_vary_spec() noexcept = default;

    void update(std::string_view field_value);
    template <http_header_field_source headers_type>
    void update_headers(const headers_type& headers) {
        update_header_range(headers);
    }

    [[nodiscard]] constexpr bool is_wildcard() const noexcept {
        return wildcard_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !wildcard_ && field_names_.empty();
    }
    [[nodiscard]] std::span<const std::string> field_names() const& noexcept RUVIA_LIFETIMEBOUND {
        return field_names_;
    }
    std::span<const std::string> field_names() const&& = delete;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> field_names_;
    bool wildcard_{false};
};

[[nodiscard]] http_vary_spec parse_http_vary(std::string_view field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_vary_spec parse_http_vary(const headers_type& headers) {
    http_vary_spec spec;
    spec.update_headers(headers);
    return spec;
}

// Normalizes the request header values nominated by `vary` in canonical field
// order for cache-key generation. Returns std::nullopt when vary.is_wildcard().
template <http_header_field_source headers_type>
[[nodiscard]] std::optional<std::string> format_http_vary_cache_key(
    const http_vary_spec& vary, const headers_type& request_headers) {
    return detail::format_http_vary_cache_key_headers(vary, request_headers);
}

template <http_header_field_source first_type, http_header_field_source second_type>
[[nodiscard]] bool http_vary_requests_match(const http_vary_spec& vary,
    const first_type& first_request, const second_type& second_request) {
    const auto first_key = format_http_vary_cache_key(vary, first_request);
    const auto second_key = format_http_vary_cache_key(vary, second_request);
    return first_key.has_value() && second_key.has_value() && *first_key == *second_key;
}
// RFC 9111 section 4.2.3 Age calculation.
struct http_cache_age_input final {
    std::optional<std::time_t> date_value_{};
    std::optional<std::uint64_t> age_value_{};
    std::time_t request_time_{0};
    std::time_t response_time_{0};
    std::time_t now_{0};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_age(std::string_view field_value) noexcept;

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(
    const http_cache_age_input& input) noexcept {
    const auto add_saturating = [](std::uint64_t left, std::uint64_t right) constexpr noexcept {
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        return left > maximum - right ? maximum : left + right;
    };
    const std::uint64_t apparent_age =
        (input.date_value_.has_value() && input.response_time_ > *input.date_value_)
            ? static_cast<std::uint64_t>(input.response_time_) -
                  static_cast<std::uint64_t>(*input.date_value_)
            : 0U;
    const std::uint64_t response_delay =
        input.response_time_ > input.request_time_
            ? static_cast<std::uint64_t>(input.response_time_) -
                  static_cast<std::uint64_t>(input.request_time_)
            : 0U;
    const std::uint64_t corrected_age_value =
        add_saturating(input.age_value_.value_or(0U), response_delay);
    const std::uint64_t corrected_initial_age = (std::max)(apparent_age, corrected_age_value);
    const std::uint64_t resident_time =
        input.now_ > input.response_time_
            ? static_cast<std::uint64_t>(input.now_) -
                  static_cast<std::uint64_t>(input.response_time_)
            : 0U;
    return add_saturating(corrected_initial_age, resident_time);
}

[[nodiscard]] constexpr std::uint64_t compute_http_cache_age(
    std::optional<std::time_t> date_value, std::optional<std::uint64_t> age_value,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    return compute_http_cache_age(http_cache_age_input{
        .date_value_ = date_value,
        .age_value_ = age_value,
        .request_time_ = request_time,
        .response_time_ = response_time,
        .now_ = now,
    });
}

template <http_header_field_source headers_type>
[[nodiscard]] std::uint64_t compute_http_cache_age(const headers_type& response_headers,
    std::time_t request_time, std::time_t response_time, std::time_t now) noexcept {
    return detail::compute_http_cache_age_headers(
        response_headers, request_time, response_time, now);
}

// RFC 9110 section 15.4.5 and RFC 9111 section 4.3.4: representation metadata
// and framing fields that must not be emitted in a 304 Not Modified response or
// overwrite stored representation headers during 304 cache freshening.
[[nodiscard]] bool is_http_304_forbidden_header(std::string_view field_name) noexcept;

void sanitize_http_304_response_headers(std::vector<http_header>& headers);
void sanitize_http_304_response_headers(http_response_headers& headers);
void sanitize_http_304_response_headers(http_response& response);

template <http_header_field_source headers_type>
void merge_http_304_response_headers(
    std::vector<http_header>& cached_headers, const headers_type& not_modified_headers) {
    if constexpr (std::same_as<std::remove_cvref_t<headers_type>, std::vector<http_header>>) {
        if (&cached_headers == &not_modified_headers) {
            auto copy = not_modified_headers;
            detail::merge_http_304_response_headers_into(cached_headers, copy);
            return;
        }
    }
    detail::merge_http_304_response_headers_into(cached_headers, not_modified_headers);
}
template <http_header_field_source headers_type>
void merge_http_304_response_headers(
    http_response_headers& cached_headers, const headers_type& not_modified_headers) {
    if constexpr (std::same_as<std::remove_cvref_t<headers_type>, http_response_headers>) {
        if (&cached_headers == &not_modified_headers) {
            auto copy = not_modified_headers.clone();
            detail::merge_http_304_response_headers_into(cached_headers, copy);
            return;
        }
    }
    detail::merge_http_304_response_headers_into(cached_headers, not_modified_headers);
}
void merge_http_304_response_headers(
    http_response& cached_response, const http_response& not_modified_response);

}  // namespace ruvia

namespace ruvia {

struct http_conditional_method_plan final {
    bool evaluates_preconditions_;
    bool uses_not_modified_response_;
    bool evaluates_if_modified_since_;
    bool evaluates_range_;
};

[[nodiscard]] inline constexpr http_conditional_method_plan get_http_conditional_method_plan(
    http_known_method method) noexcept {
    switch (method) {
        case http_known_method::get:
            return {true, true, true, true};
        case http_known_method::head:
            return {true, true, true, false};
        case http_known_method::post:
        case http_known_method::put:
        case http_known_method::delete_value:
        case http_known_method::patch:
            return {true, false, false, false};
        case http_known_method::options:
        case http_known_method::connect:
        case http_known_method::unknown:
            return {false, false, false, false};
    }
    return {false, false, false, false};
}

// Precondition and range fields borrowed from the request. Presence of If-Range
// is tracked separately because an empty value is still a present field.
struct http_conditional_headers final {
    std::string_view if_unmodified_since_;
    std::string_view if_modified_since_;
    std::string_view range_;
    std::string_view if_range_;
    bool has_if_range_;
};

// Repeated If-Modified-Since and If-Unmodified-Since lines are ignored as
// invalid date conditions (RFC 9110 sections 13.1.3 and 13.1.4); their views
// are empty here rather than exposing the last line as an effective condition.
[[nodiscard]] http_conditional_headers get_http_conditional_headers(const http_request& request) noexcept;
http_conditional_headers get_http_conditional_headers(const http_request&&) = delete;

// One entity-tag precondition field's outcome, accumulated across every field
// line of that name. `valid` is false for a malformed field, which the caller
// must treat differently from a field that simply did not match. A valid
// wildcard is the only member across all accumulated field lines.
struct http_etag_field_condition final {
    bool present_{false};
    bool valid_{true};
    bool matched_{false};
    bool wildcard_{false};

    void update(std::string_view value, std::string_view expected, bool strong) noexcept;

    [[nodiscard]] bool matches() const noexcept {
        return valid_ && (wildcard_ || matched_);
    }
};

struct http_etag_preconditions final {
    http_etag_field_condition if_match_;
    http_etag_field_condition if_none_match_;
};

[[nodiscard]] http_etag_preconditions get_http_etag_preconditions(
    const http_request& request, std::string_view etag) noexcept;

// If-Modified-Since / If-Unmodified-Since: the "<=" comparisons of RFC 9110
// section 13.1.3 and 13.1.4. A malformed date is ignored.
[[nodiscard]] bool http_date_not_modified(
    std::string_view header_value, std::time_t modified_seconds) noexcept;

[[nodiscard]] bool http_date_unmodified(
    std::string_view header_value, std::time_t modified_seconds) noexcept;

// Whether an If-Range still authorises a range response. A date validator here
// must match EXACTLY, unlike If-Modified-Since.
[[nodiscard]] bool http_if_range_allows(std::string_view header_value, std::string_view etag,
    std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept;

// Single-value RFC 9110 §8.8.2 entity-tag validation, comparison, and weakening.
[[nodiscard]] bool is_valid_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_weak_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_strong_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string weaken_http_etag(std::string_view etag);
}  // namespace ruvia

namespace ruvia {

// Case-insensitive set of field/option tokens parsed from one or more
// Connection header lines (RFC 9110 section 7.6.1).
class http_connection_nominated_fields final {
public:
    http_connection_nominated_fields() noexcept = default;

    void update(std::string_view connection_field_value);
    template <http_header_field_source headers_type>
    void update_headers(const headers_type& headers) {
        update_header_range(headers);
    }

    [[nodiscard]] bool contains(std::string_view field_name) const noexcept;
    [[nodiscard]] constexpr bool has_close() const noexcept {
        return has_close_;
    }
    [[nodiscard]] constexpr bool has_keep_alive() const noexcept {
        return has_keep_alive_;
    }
    [[nodiscard]] constexpr bool has_upgrade() const noexcept {
        return has_upgrade_;
    }
    [[nodiscard]] constexpr bool has_te() const noexcept {
        return has_te_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return tokens_.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return tokens_.size();
    }
    [[nodiscard]] std::span<const std::string> tokens() const& noexcept RUVIA_LIFETIMEBOUND {
        return tokens_;
    }
    std::span<const std::string> tokens() const&& = delete;

    [[nodiscard]] std::vector<std::string> non_standard_options() const;

    [[nodiscard]] std::string rebuild_connection_field(
        bool retain_upgrade = false, bool include_non_standard = true) const;

private:
    void update_header_range(detail::http_header_range_view headers);

    std::vector<std::string> tokens_;
    bool has_close_{false};
    bool has_keep_alive_{false};
    bool has_upgrade_{false};
    bool has_te_{false};
};

[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    std::string_view connection_field_value);
template <http_header_field_source headers_type>
[[nodiscard]] http_connection_nominated_fields parse_http_connection_nominated_fields(
    const headers_type& headers) {
    http_connection_nominated_fields nominated;
    nominated.update_headers(headers);
    return nominated;
}

// Protocol hop-by-hop fields (RFC 9110 section 7.6.1): Connection, Keep-Alive,
// Proxy-Connection, TE, Transfer-Encoding, Trailer, Upgrade,
// Proxy-Authenticate, and Proxy-Authorization.
[[nodiscard]] bool is_http_hop_by_hop_field_name(std::string_view field_name) noexcept;

// Standard connection control/hop-by-hop tokens ("close", "keep-alive", and the
// protocol hop-by-hop field names).
[[nodiscard]] bool is_http_standard_connection_option(std::string_view option) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_or_nominated_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated, bool retain_upgrade = false) noexcept;

[[nodiscard]] bool is_http_hop_by_hop_trailer_field(std::string_view field_name,
    const http_connection_nominated_fields& nominated = http_connection_nominated_fields{}) noexcept;

[[nodiscard]] bool is_forbidden_http_request_trailer_field_name(std::string_view field_name) noexcept;
[[nodiscard]] bool is_forbidden_http_response_trailer_field_name(std::string_view field_name) noexcept;

}  // namespace ruvia

namespace ruvia {

struct http_cors_policy_config final {
    bool allow_any_origin_{false};
    // Exact WHATWG serialized origins ("https://example.com", "null"), "*", or
    // suffix domain patterns starting with "." or "*." (e.g. ".example.com").
    std::vector<std::string> allowed_origins_;
    // Domain suffixes matched on a label boundary (".example.com", "example.com",
    // "*.example.com", or scheme-qualified "https://.example.com").
    std::vector<std::string> allowed_origin_suffixes_;
    // Allowed preflight methods (e.g. "GET", "POST", "OPTIONS"), or "*" for any
    // valid method token.
    std::vector<std::string> allowed_methods_;
    // Allowed request header names, or "*" to reflect Access-Control-Request-Headers.
    std::vector<std::string> allowed_headers_;
    bool reflect_request_headers_{false};
    std::vector<std::string> expose_headers_;
    bool allow_credentials_{false};
    std::optional<std::chrono::seconds> max_age_{};
};

struct http_cors_preflight_input final {
    borrowed_text method_{"OPTIONS"};
    borrowed_text origin_{};
    borrowed_text access_control_request_method_{};
    borrowed_text access_control_request_headers_{};
};

class http_cors_preflight_result final {
public:
    http_cors_preflight_result() noexcept = default;

    http_cors_preflight_result(bool accepted, std::vector<http_header> headers) noexcept
        : headers_(std::move(headers)),
          accepted_(accepted) {}

    [[nodiscard]] constexpr bool accepted() const noexcept {
        return accepted_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return accepted_;
    }

    [[nodiscard]] std::span<const http_header> headers() const& noexcept RUVIA_LIFETIMEBOUND {
        return headers_;
    }
    std::span<const http_header> headers() const&& = delete;

    [[nodiscard]] std::vector<http_header> take_headers() && noexcept {
        accepted_ = false;
        return std::move(headers_);
    }

private:
    std::vector<http_header> headers_;
    bool accepted_{false};
};

// Stateless, pre-validated CORS policy primitive for per-request preflight
// evaluation and actual response header rewriting.
class http_cors_policy final {
public:
    explicit http_cors_policy(const http_cors_policy_config& config);

    [[nodiscard]] bool allow_any_origin() const noexcept {
        return allow_any_origin_;
    }
    [[nodiscard]] bool allow_credentials() const noexcept {
        return allow_credentials_;
    }
    [[nodiscard]] bool reflect_request_headers() const noexcept {
        return reflect_request_headers_;
    }
    [[nodiscard]] bool allows_origin(std::string_view origin) const noexcept;
    [[nodiscard]] bool allows_method(std::string_view method) const noexcept;
    [[nodiscard]] bool allows_request_header(std::string_view name) const noexcept;

    [[nodiscard]] http_cors_preflight_result evaluate_preflight(
        http_cors_preflight_input input) const;
    [[nodiscard]] http_cors_preflight_result evaluate_preflight(
        const http_request& request) const;

    // Removes controlled Access-Control-* headers from the existing response,
    // writes computed CORS headers when the origin is permitted, and merges
    // Vary tokens (such as Origin) without duplicates.
    bool rewrite_response_headers(
        borrowed_text request_origin, std::vector<http_header>& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response_headers& response_headers) const;
    bool rewrite_response_headers(
        borrowed_text request_origin, http_response& response) const;
    // Applies server-side CORS headers (including OPTIONS preflight and Allow
    // promotion) without overwriting headers already set by the route handler.
    bool apply_response_headers(const http_request& request, http_response& response) const;

private:
    struct suffix_rule final {
        std::string scheme_;
        std::string host_suffix_;
        std::optional<std::uint16_t> port_{};
        bool subdomain_only_{false};
    };

    std::vector<std::string> exact_origins_;
    std::vector<suffix_rule> suffix_rules_;
    std::vector<std::string> allowed_methods_;
    std::string joined_allowed_methods_;
    std::vector<std::string> allowed_headers_;
    std::string joined_allowed_headers_;
    std::string joined_expose_headers_;
    std::string max_age_;
    bool allow_any_origin_{false};
    bool allow_any_method_{false};
    bool reflect_request_headers_{false};
    bool allow_credentials_{false};
};

[[nodiscard]] bool is_http_cors_controlled_response_header(std::string_view name) noexcept;

void merge_http_vary_tokens(
    std::vector<http_header>& headers, std::span<const std::string_view> tokens);
void merge_http_vary_token(std::vector<http_header>& headers, std::string_view token);

}  // namespace ruvia
