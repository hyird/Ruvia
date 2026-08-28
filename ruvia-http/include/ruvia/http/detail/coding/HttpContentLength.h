#pragma once

#include "ruvia/http/detail/field/HeaderTokenUtils.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace ruvia::detail {

enum class HttpContentLengthParseStatus : std::uint8_t { kOk, kInvalid, kConflicting };

// Incremental field-line parser shared by HTTP/1 request and response heads.
// RFC 9112 section 6.3 permits a comma-combined Content-Length only when every
// list member is a valid decimal value and all values (including values from
// repeated field lines) are identical. A failed field is transactional: it
// cannot leak a partial value, and absence is exposed only as std::nullopt.
class HttpContentLengthState final {
public:
    [[nodiscard]] HttpContentLengthParseStatus parseField(std::string_view fieldValue) noexcept {
        auto status = HttpContentLengthParseStatus::kOk;
        bool sawValue = false;
        auto parsedValue = value_;
        httpVisitCommaSeparatedQuotedItems(fieldValue, [&parsedValue, &status, &sawValue](std::string_view item) noexcept {
            if (item.empty()) {
                status = HttpContentLengthParseStatus::kInvalid;
                return false;
            }

            std::size_t parsed = 0;
            const auto [end, ec] = std::from_chars(item.data(), item.data() + item.size(), parsed);
            if (ec != std::errc{} || end != item.data() + item.size()) {
                status = HttpContentLengthParseStatus::kInvalid;
                return false;
            }
            sawValue = true;
            if (parsedValue.has_value() && *parsedValue != parsed) {
                status = HttpContentLengthParseStatus::kConflicting;
                return false;
            }
            parsedValue = parsed;
            return true;
        });
        if (status == HttpContentLengthParseStatus::kOk && !sawValue) {
            return HttpContentLengthParseStatus::kInvalid;
        }
        if (status == HttpContentLengthParseStatus::kOk) {
            value_ = parsedValue;
        }
        return status;
    }

    [[nodiscard]] std::optional<std::size_t> value() const noexcept {
        return value_;
    }

private:
    std::optional<std::size_t> value_;
};

}  // namespace ruvia::detail
