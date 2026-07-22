#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia::edge {

// A parsed single byte-range against a body of `length` bytes. `satisfiable`
// carries a usable [start,end] (inclusive); `unsatisfiable` means the range lies
// outside the body (416); neither set means "ignore the Range and serve fully".
struct ByteRange final {
    bool satisfiable{false};
    bool unsatisfiable{false};
    std::size_t start{0};
    std::size_t end{0};
};

// Parse a Range request header against a known body length. Only the single
// byte-range forms of RFC 9110 section 14.1.1 are honoured; anything else
// (multi-range, another unit, a malformed spec) yields "serve fully".
[[nodiscard]] ByteRange parseSingleByteRange(std::string_view header, std::size_t length);

}  // namespace ruvia::edge
