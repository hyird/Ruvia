#include "ruvia/http/MultipartParser.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace {

// A fixed, valid RFC 2046 boundary. The fuzzer treats the whole input as the
// body bytes framed by this boundary, which exercises delimiter matching,
// preamble handling, part-header parsing, and body chunking without spending
// entropy on constructing a valid boundary token.
constexpr std::string_view kBoundary = "ruviafuzzboundary";

void exerciseStreamPart(const ruvia::MultipartStreamPart& part) noexcept {
    (void)part.name();
    (void)part.filename();
    (void)part.contentType();
    (void)part.body();
    (void)part.phase();
}

// Drives the streaming parser: arbitrary bytes fed in small chunks so the
// cross-chunk delimiter/header split paths are reached. Terminates because each
// poll() either consumes buffered input (bounded by total fed bytes) or reaches
// a terminal done/failure state; an explicit iteration cap backstops both.
void fuzzStreaming(
    std::string_view input,
    std::pmr::memory_resource* resource) {
    ruvia::MultipartParser parser(ruvia::MultipartBoundary(kBoundary), resource);
    const std::size_t chunkSize = 7;
    bool terminal = false;
    std::size_t polls = 0;
    const std::size_t pollBudget = input.size() + 16;
    for (std::size_t offset = 0; offset <= input.size() && !terminal; offset += chunkSize) {
        if (offset < input.size()) {
            parser.feed(input.substr(offset, chunkSize));
        } else {
            parser.finishInput();
        }
        for (;;) {
            if (++polls > pollBudget) {
                return;
            }
            const auto result = parser.poll();
            if (const auto* part = result.part()) {
                exerciseStreamPart(*part);
                continue;
            }
            if (result.failure() != nullptr || result.done() != nullptr) {
                terminal = true;
                break;
            }
            // needInput: hand the parser more bytes on the next feed().
            break;
        }
        if (offset >= input.size()) {
            break;
        }
    }
}

void fuzzBuffered(
    std::string_view input,
    std::pmr::memory_resource* resource) {
    auto result = ruvia::parseMultipartBody(
        input, ruvia::MultipartBoundary(kBoundary), resource);
    if (const auto* body = result.body()) {
        for (const auto& part : body->parts()) {
            (void)part.name();
            (void)part.filename();
            (void)part.contentType();
            (void)part.body();
        }
    } else if (const auto* failure = result.failure()) {
        (void)failure->error();
    }
}

}  // namespace

// Fuzzes both the buffered and streaming multipart/form-data parsers (RFC 2046 /
// RFC 7578). Every borrowed view must stay in-bounds and both parsers must
// terminate for arbitrary bytes, including embedded NULs, truncated delimiters,
// and adversarial part headers.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::string_view(
        reinterpret_cast<const char*>(data),
        size);

    std::pmr::monotonic_buffer_resource bufferedResource;
    fuzzBuffered(input, &bufferedResource);

    std::pmr::monotonic_buffer_resource streamingResource;
    fuzzStreaming(input, &streamingResource);

    return 0;
}
