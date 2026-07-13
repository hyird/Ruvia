#include "ruvia/http/Http1RequestParser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::string_view(
        reinterpret_cast<const char*>(data),
        size);
    const ruvia::Http1RequestParser parser;
    const auto result = parser.parse(input);

    // Exercise every active-alternative accessor. These calls must remain safe for
    // arbitrary bytes, including embedded NULs and incomplete message boundaries.
    (void)result.kind();
    (void)result.needMore();
    (void)result.parsed();
    (void)result.failure();
    return 0;
}
