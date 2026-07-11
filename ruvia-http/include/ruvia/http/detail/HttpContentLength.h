#pragma once

#include "ruvia/http/detail/HeaderTokenUtils.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace ruvia::detail {

enum class HttpContentLengthParseStatus : std::uint8_t {
    kOk,
    kInvalid,
    kConflicting
};

// Incremental field-line parser shared by HTTP/1 request and response heads.
// RFC 9112 section 6.3 permits a comma-combined Content-Length only when every
// list member is a valid decimal value and all values (including values from
// repeated field lines) are identical.
class HttpContentLengthState final {
public:
    [[nodiscard]] HttpContentLengthParseStatus parseField(
        std::string_view fieldValue) noexcept {
        auto status = HttpContentLengthParseStatus::kOk;
        bool sawValue = false;
        httpVisitCommaSeparatedQuotedItems(
            fieldValue,
            [this, &status, &sawValue](std::string_view item) noexcept {
                if (item.empty()) {
                    status = HttpContentLengthParseStatus::kInvalid;
                    return false;
                }

                std::size_t parsed = 0;
                const auto [end, ec] = std::from_chars(
                    item.data(), item.data() + item.size(), parsed);
                if (ec != std::errc{} || end != item.data() + item.size()) {
                    status = HttpContentLengthParseStatus::kInvalid;
                    return false;
                }
                sawValue = true;
                if (present_ && value_ != parsed) {
                    status = HttpContentLengthParseStatus::kConflicting;
                    return false;
                }
                present_ = true;
                value_ = parsed;
                return true;
            });
        return status == HttpContentLengthParseStatus::kOk && !sawValue
            ? HttpContentLengthParseStatus::kInvalid
            : status;
    }

    [[nodiscard]] bool present() const noexcept {
        return present_;
    }

    [[nodiscard]] std::size_t value() const noexcept {
        return value_;
    }

private:
    std::size_t value_{0};
    bool present_{false};
};

}  // namespace ruvia::detail
