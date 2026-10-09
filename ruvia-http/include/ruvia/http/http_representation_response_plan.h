#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/http_byte_range.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

class http_request;

// Metadata for an already selected, existing origin representation. This plan
// does not describe cache misses or select a representation. ETag text is
// borrowed only during planning; the resulting plan retains no input views.
struct http_selected_representation_metadata final {
    std::uint64_t length_{0};
    std::string_view etag_{};
    std::optional<std::time_t> last_modified_{};
    // Only authorizes a date-form If-Range when last_modified is also present.
    // Ordinary date preconditions do not require a strong validator.
    bool strong_date_validator_{false};
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
    using range_unsatisfiable_type = http_byte_range_unsatisfiable;
    using partial_type = http_resolved_byte_range;
    using multipart = http_byte_range_set;

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
    [[nodiscard]] const multipart* multipart_ranges() const& noexcept;
    [[nodiscard]] const multipart* multipart_ranges() const&& = delete;
    [[nodiscard]] http_status_code status() const noexcept;

private:
    friend http_representation_response_plan plan_http_representation_response(
        const http_request&, http_selected_representation_metadata,
        http_representation_response_options);

    using value_type = std::variant<full_type, not_modified_type, precondition_failed_type, range_unsatisfiable_type, partial_type, multipart>;

    template <typename alternative_type>
    explicit http_representation_response_plan(alternative_type alternative) noexcept
        : value_(alternative) {}

    value_type value_;
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
