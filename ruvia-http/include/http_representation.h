#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "borrowed_text.h"
#include "http_message.h"
#include "http_range.h"
#include "http_request.h"
#include "http_status.h"

namespace ruvia {

struct http_conditional_method_plan final {
    bool evaluates_preconditions_;
    bool uses_not_modified_response_;
    bool evaluates_if_modified_since_;
    bool evaluates_range_;
};

[[nodiscard]] inline constexpr http_conditional_method_plan get_http_conditional_method_plan(http_known_method method) noexcept {
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

[[nodiscard]] http_etag_preconditions get_http_etag_preconditions(const http_request& request, std::string_view etag) noexcept;

// If-Modified-Since / If-Unmodified-Since: the "<=" comparisons of RFC 9110
// section 13.1.3 and 13.1.4. A malformed date is ignored.
[[nodiscard]] bool http_date_not_modified(std::string_view header_value, std::time_t modified_seconds) noexcept;

[[nodiscard]] bool http_date_unmodified(std::string_view header_value, std::time_t modified_seconds) noexcept;

// Whether an If-Range still authorises a range response. A date validator here
// must match EXACTLY, unlike If-Modified-Since.
[[nodiscard]] bool http_if_range_allows(std::string_view header_value, std::string_view etag, std::optional<std::time_t> modified_seconds, bool date_validator_strong) noexcept;

// Single-value RFC 9110 §8.8.2 entity-tag validation, comparison, and weakening.
[[nodiscard]] bool is_valid_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_weak_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool is_strong_http_entity_tag(std::string_view value) noexcept;
[[nodiscard]] bool http_strong_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] bool http_weak_etag_equals(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string weaken_http_etag(std::string_view etag);

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
    friend http_representation_response_plan plan_http_representation_response(const http_request&, http_selected_representation_metadata, http_representation_response_options);

    using value_type = std::variant<full_type, not_modified_type, precondition_failed_type, range_unsatisfiable_type, partial_type, multipart_type>;

    template <typename alternative_type>
    explicit http_representation_response_plan(alternative_type alternative, bool authorized_by_if_range = false) noexcept :
        value_(alternative),
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
[[nodiscard]] http_representation_response_plan plan_http_representation_response(const http_request& request, http_selected_representation_metadata representation, http_representation_response_options options = {});

}  // namespace ruvia
