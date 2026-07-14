#include "ruvia/http/detail/parser/HttpChunkParser.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Fuzzes the sans-I/O chunked transfer-coding scanner (RFC 9112 section 7.1):
// whole-message framing, chunk-size/extension parsing, and trailer validation.
// Every entry point is noexcept and must stay memory-safe and terminating for
// arbitrary bytes, including embedded NULs, partial framing, and adversarial
// chunk-size or trailer sequences.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::string_view(
        reinterpret_cast<const char*>(data),
        size);

    const auto scan = ruvia::detail::scanHttpChunkedBody(input);

    // Exercise every accessor of the tri-state result. A complete framing must
    // never report a consumed length past the input, which UBSan-guarded callers
    // downstream would slice on.
    if (const auto* complete = scan.complete()) {
        (void)complete->consumedBytes();
    }
    (void)scan.needMore();
    if (const auto* failure = scan.failure()) {
        (void)failure->error();
    }

    // The trailer validator and the standalone chunk-size parser are reachable on
    // their own from other framing paths; fuzz them directly against the same
    // bytes so partial and malformed inputs are covered without full framing.
    (void)ruvia::detail::validateHttpChunkTrailers(input);
    std::size_t chunkSize = 0;
    (void)ruvia::detail::parseHttpChunkSize(input, chunkSize);

    return 0;
}
