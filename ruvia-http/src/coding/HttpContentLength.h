#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "ruvia/http/detail/field/HeaderTokenUtils.h"

namespace ruvia::detail {

enum class HttpContentLengthParseStatus : std::uint8_t { kOk,
    kInvalid,
    kConflicting };

// Incremental Content-Length accumulator. Receiving boundaries accept
// comma-combined values with OWS; sending boundaries parse a single untrimmed
// decimal value. Both commit only when every repeated value agrees. The value
// type preserves each protocol boundary's size_t or uint64_t range.
template <typename value_type = std::size_t>
class HttpContentLengthState final {
public:
    [[nodiscard]] HttpContentLengthParseStatus parseField(std::string_view fieldValue) noexcept {
        auto status = HttpContentLengthParseStatus::kOk;
        bool sawValue = false;
        auto parsedValue = value_;
        httpVisitCommaSeparatedQuotedItems(fieldValue, [&parsedValue, &status, &sawValue](
                                                           std::string_view item) noexcept {
            status = accumulate(item, parsedValue);
            sawValue = true;
            return status == HttpContentLengthParseStatus::kOk;
        });
        if (status == HttpContentLengthParseStatus::kOk && !sawValue) {
            return HttpContentLengthParseStatus::kInvalid;
        }
        if (status == HttpContentLengthParseStatus::kOk) {
            value_ = parsedValue;
        }
        return status;
    }

    [[nodiscard]] HttpContentLengthParseStatus parse_single_value(std::string_view field_value) noexcept {
        auto parsed_value = value_;
        const auto status = accumulate(field_value, parsed_value);
        if (status == HttpContentLengthParseStatus::kOk) {
            value_ = parsed_value;
        }
        return status;
    }

    [[nodiscard]] std::optional<value_type> value() const noexcept {
        return value_;
    }

private:
    [[nodiscard]] static HttpContentLengthParseStatus accumulate(
        std::string_view item, std::optional<value_type>& value) noexcept {
        if (item.empty()) {
            return HttpContentLengthParseStatus::kInvalid;
        }
        value_type parsed = 0;
        const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed);
        if (ec != std::errc{} || end != item.data() + item.size()) {
            return HttpContentLengthParseStatus::kInvalid;
        }
        if (value.has_value() && *value != parsed) {
            return HttpContentLengthParseStatus::kConflicting;
        }
        value = parsed;
        return HttpContentLengthParseStatus::kOk;
    }

    std::optional<value_type> value_;
};

}  // namespace ruvia::detail
