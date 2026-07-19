#include "ruvia/web/detail/json/JsonSkip.h"
#include "ruvia/web/detail/json/JsonString.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

// The JSON headers are otherwise self-contained, but decodeJsonString's
// allocation helper references ruvia-core's processResource(), whose real
// definition drags in mimalloc and the process memory pool. This harness never
// exercises the process default (it always passes an explicit resource), so a
// standalone stub keeps the fuzzer free of that heavyweight dependency.
namespace ruvia::detail {
std::pmr::memory_resource* processResource() noexcept {
    return std::pmr::new_delete_resource();
}
}  // namespace ruvia::detail

// Fuzzes the model-agnostic JSON machinery that consumes untrusted request
// bodies: the recursive structural scanner (objects, arrays, strings, numbers,
// literals, whitespace, and the kMaxJsonDepth nesting guard) and the string
// decoder (backslash escapes, \uXXXX units, UTF-16 surrogate pairing, and RFC
// 8259 UTF-8 validation). Both must stay in-bounds and terminate for arbitrary
// bytes -- including embedded NULs, truncated escapes, and adversarial nesting.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::string_view(
        reinterpret_cast<const char*>(data),
        size);

    // Structural scan over the whole document. skipJsonValue mutates a local
    // cursor copy, so the original view stays intact.
    {
        std::string_view cursor = input;
        (void)ruvia::detail::skipJsonValue(cursor);
    }

    // String decode: the scanner finds one string token's bounds, then the
    // decoder runs the escape/UTF-8/surrogate paths over its raw bytes.
    {
        std::string_view cursor = input;
        if (const auto token = ruvia::detail::parseJsonString(cursor)) {
            std::pmr::monotonic_buffer_resource resource;
            (void)ruvia::detail::decodeJsonString(token->raw(), &resource);
        }
    }

    return 0;
}
