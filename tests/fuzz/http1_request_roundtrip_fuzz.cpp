#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1RequestParser.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/detail/AsciiCase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kMethods[] = {
    "GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "PATCH",
};

// Framing/routing headers the writer owns and may add, drop, or reconcile
// (e.g. it emits its own Host, and a duplicate Content-Length is a legitimate
// parser rejection, not a writer bug). Excluding them keeps the "writer output
// must parse" invariant free of false positives.
[[nodiscard]] bool isWriterManagedHeader(std::string_view name) noexcept {
    using ruvia::detail::httpAsciiEqualsIgnoreCase;
    return httpAsciiEqualsIgnoreCase(name, "host") ||
        httpAsciiEqualsIgnoreCase(name, "content-length") ||
        httpAsciiEqualsIgnoreCase(name, "transfer-encoding") ||
        httpAsciiEqualsIgnoreCase(name, "connection") ||
        httpAsciiEqualsIgnoreCase(name, "te") ||
        httpAsciiEqualsIgnoreCase(name, "upgrade") ||
        httpAsciiEqualsIgnoreCase(name, "expect");
}

std::string_view takeChunk(std::string_view input, std::size_t& cursor) {
    if (cursor >= input.size()) {
        return {};
    }
    const auto length = static_cast<unsigned char>(input[cursor++]);
    const auto take = std::min<std::size_t>(length, input.size() - cursor);
    const auto chunk = input.substr(cursor, take);
    cursor += take;
    return chunk;
}

}  // namespace

// Fuzzes the outbound HTTP/1 request writer -- which had no coverage at all --
// and round-trips it against the server request parser. A request the writer
// accepts must serialize to a head the parser accepts, with the method
// preserved. Because the writer and parser share the same field validators, a
// parse failure here is a real writer framing bug, not a validation mismatch.
// Under AddressSanitizer this also exercises the writer's fixed head-buffer
// management for overflows.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const auto method = kMethods[data[0] % (sizeof(kMethods) / sizeof(kMethods[0]))];
    const auto input = std::string_view(
        reinterpret_cast<const char*>(data + 1), size - 1);

    std::size_t cursor = 0;
    std::string target("/");
    target.append(takeChunk(input, cursor));

    std::vector<ruvia::HttpHeaderView> headers;
    while (cursor < input.size() && headers.size() < 32) {
        const auto name = takeChunk(input, cursor);
        const auto value = takeChunk(input, cursor);
        if (!name.empty() && !isWriterManagedHeader(name)) {
            headers.emplace_back(name, value);
        }
    }

    ruvia::HttpClientRequest request;
    request.method = method;
    request.target = std::string_view(target);
    request.headers = std::span<const ruvia::HttpHeaderView>(headers);

    std::array<char, 8192> headBuffer;
    const auto origin = ruvia::HttpOrigin::https("example.test");
    const auto preparedResult =
        ruvia::Http1ClientRequestWriter().prepare(origin, request, headBuffer);
    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        // The writer rejected the request; there is nothing to round-trip.
        return 0;
    }

    const ruvia::Http1RequestParser parser;
    const auto result = parser.parse(prepared->head());
    const auto* parsed = result.parsed();
    if (parsed == nullptr) {
        __builtin_trap();  // writer produced a head the server parser rejects
    }
    if (parsed->request().method() != method) {
        __builtin_trap();  // method did not survive serialization
    }
    return 0;
}
