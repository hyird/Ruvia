#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>
#include <variant>

#include "ruvia/http/HttpByteRange.h"
#include "ruvia/http/HttpStatus.h"

namespace ruvia {

class HttpRequest;

// Metadata for an already selected, existing origin representation. This plan
// does not describe cache misses or select a representation. ETag text is
// borrowed only during planning; the resulting plan retains no input views.
struct HttpSelectedRepresentationMetadata final {
    std::uint64_t length{0};
    std::string_view etag{};
    std::optional<std::time_t> lastModified{};
    // Only authorizes a date-form If-Range when lastModified is also present.
    // Ordinary date preconditions do not require a strong validator.
    bool strongDateValidator{false};
};

enum class HttpRangeRequestPolicy {
    kIgnore,
    honor_byte_ranges,
};

struct HttpRepresentationResponseOptions final {
    // Origin response status before evaluating preconditions or Range.
    HttpStatusCode normalStatus{http_status::kOk};
    HttpRangeRequestPolicy rangePolicy{HttpRangeRequestPolicy::kIgnore};
};

// An exclusive origin response selection. Owns only status/slicing values;
// response body permission and wire framing are decided at write submission.
class HttpRepresentationResponsePlan final {
public:
    struct Full final {
        HttpStatusCode status;
    };
    struct NotModified final {};
    struct PreconditionFailed final {};
    using RangeUnsatisfiable = HttpByteRangeUnsatisfiable;
    using Partial = HttpResolvedByteRange;
    using multipart = http_byte_range_set;

    [[nodiscard]] const Full* full() const& noexcept;
    [[nodiscard]] const Full* full() const&& = delete;
    [[nodiscard]] const NotModified* notModified() const& noexcept;
    [[nodiscard]] const NotModified* notModified() const&& = delete;
    [[nodiscard]] const PreconditionFailed* preconditionFailed() const& noexcept;
    [[nodiscard]] const PreconditionFailed* preconditionFailed() const&& = delete;
    [[nodiscard]] const RangeUnsatisfiable* rangeUnsatisfiable() const& noexcept;
    [[nodiscard]] const RangeUnsatisfiable* rangeUnsatisfiable() const&& = delete;
    [[nodiscard]] const Partial* partial() const& noexcept;
    [[nodiscard]] const Partial* partial() const&& = delete;
    [[nodiscard]] const multipart* multipart_ranges() const& noexcept;
    [[nodiscard]] const multipart* multipart_ranges() const&& = delete;
    [[nodiscard]] HttpStatusCode status() const noexcept;

private:
    friend HttpRepresentationResponsePlan planHttpRepresentationResponse(
        const HttpRequest&, HttpSelectedRepresentationMetadata,
        HttpRepresentationResponseOptions);

    using Value = std::variant<Full, NotModified, PreconditionFailed, RangeUnsatisfiable, Partial, multipart>;

    template <typename Alternative>
    explicit HttpRepresentationResponsePlan(Alternative alternative) noexcept
        : value_(alternative) {}

    Value value_;
};

// Call after normal request checks, before processing request content or
// performing the method, with the target resource's current representation.
// The method must be supported and allowed. For extensions, the caller must
// establish that it selects or modifies that representation; non-representation
// extensions and extensions redefining conditional semantics are outside this API.
// CONNECT, OPTIONS, and TRACE ignore conditions, as do ineligible normal statuses.
[[nodiscard]] HttpRepresentationResponsePlan planHttpRepresentationResponse(
    const HttpRequest& request, HttpSelectedRepresentationMetadata representation,
    HttpRepresentationResponseOptions options = {});

}  // namespace ruvia
