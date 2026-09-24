#pragma once

#include <optional>
#include <string_view>

#include "ruvia/http/BorrowedText.h"

namespace ruvia {

// Validates a WHATWG serialized origin, not a general URI or origin list.
[[nodiscard]] bool isValidHttpSerializedOrigin(std::string_view value) noexcept;

// Parses a comma-delimited Access-Control-Request-Headers field without
// allocating. Empty members are ignored; at least one valid field name is
// required. Each returned view borrows the input. Drain next() until it
// returns nullopt, then check valid() to distinguish end from malformed input.
class HttpCorsRequestHeaderNames final {
public:
    explicit HttpCorsRequestHeaderNames(BorrowedText field) noexcept
        : remaining_(field.view()) {}

    [[nodiscard]] std::optional<std::string_view> next() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return finished_ && valid_;
    }

private:
    std::string_view remaining_;
    bool finished_{false};
    bool sawName_{false};
    bool valid_{false};
};

}  // namespace ruvia
