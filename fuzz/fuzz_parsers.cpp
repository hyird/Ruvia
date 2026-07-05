// Smoke-fuzz for the header-only pure parsers (URL decode, JSON string/number,
// base64url, byte ranges, multipart helpers). Deterministic PRNG so runs are
// reproducible; feed random adversarial byte strings and confirm no crash /
// (under a sanitizer build) no undefined behaviour or out-of-bounds access.
//
// Iteration count: argv[1] (default 200000, small enough for CI). Configure the
// build with -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all"
// (as build-asan does) to turn this into a UBSan fuzzer.
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/json/JsonString.h"
#include "ruvia/http/detail/json/JsonNumber.h"
#include "ruvia/http/detail/json/JsonSkip.h"
#include "ruvia/http/detail/json/JsonObjectFields.h"
#include "ruvia/detail/Base64Url.h"
#include "http/FileResponseHelpers.h"
#include "http/MultipartParsing.h"
#include "http/HeaderAcceptUtils.h"
#include "http/HeaderTokenUtils.h"
#include "http/parser/HttpRequestTarget.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

namespace d = ruvia::detail;

namespace {
std::uint64_t g = 0x9e3779b97f4a7c15ULL;
std::uint64_t next() { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return g; }

void exercise(std::string_view s, std::string_view boundary) {
    std::pmr::string out(std::pmr::get_default_resource());
    (void)d::hasUrlEncoding(s, d::UrlDecodeMode::kPercent);
    (void)d::decodeUrlComponent(s, out, d::UrlDecodeMode::kPercent);
    (void)d::decodeUrlComponent(s, out, d::UrlDecodeMode::kForm);
    (void)d::validateUrlEncoding(s);
    (void)d::urlComponentEquals(s, "abc", d::UrlDecodeMode::kForm);
    (void)d::visitUrlEncodedPairs(s, [](std::string_view, std::string_view) { return true; });

    {
        std::string_view rest = s, value;
        bool escaped = false;
        (void)d::parseJsonStringRaw(rest, value, escaped);
    }
    (void)d::decodeJsonString(s, out);
    (void)d::scanJsonNumberTokenLength(s);
    {
        std::string_view rest = s;
        long long v = 0;
        (void)d::parseJsonNumberValue(rest, v);
    }
    // Full structural JSON parse: a run of '[' / '{' exceeding kMaxJsonDepth (64)
    // must hit the depth guard, not overflow the stack -- exercise the recursive
    // skipper and the object-field visitor on adversarial nesting.
    {
        std::string_view rest = s;
        (void)d::skipJsonValue(rest);
    }
    (void)d::visitJsonObjectFields(
        s, std::pmr::get_default_resource(),
        [](std::string_view, std::string_view) { return true; });
    for (const char c : s) {
        (void)d::decodeBase64UrlChar(c);
    }
    if (s.size() >= 4) {
        std::uint32_t hv = 0;
        (void)d::readJsonHex4(s, hv);
    }
    for (const std::uint64_t sz : {std::uint64_t{0}, std::uint64_t{1000},
                                   (std::numeric_limits<std::uint64_t>::max)()}) {
        (void)d::httpParseByteRange(s, sz);
    }
    (void)d::httpParseUnsigned(s);

    (void)d::httpFindMultipartBoundaryLine(s, boundary);
    (void)d::httpFindMultipartBoundaryPrefix(s, boundary);
    std::string_view parsedBoundary;
    (void)d::httpParseMultipartBoundary(s, parsedBoundary);
    d::HttpMultipartPartHeaders headers;
    (void)d::httpParseMultipartPartHeaders(s, headers);
    (void)d::httpDispositionParameter(s, "name");
    (void)d::httpDispositionParameter(s, "filename");
    (void)d::httpAcceptsMediaType(s, "text/plain");
    (void)d::httpSelectResponseCoding(s);

    // Untrusted date strings from If-Modified-Since / If-Unmodified-Since / If-Range:
    // the composite tries IMF-fixdate, then the two obsolete offset-based formats.
    (void)d::httpParseHttpDate(s);
    // Host header validation (reg-name / IPv4 / bracketed IPv6 via inet_pton) and the
    // ';'-delimited parameter scanner shared by cookie and content-type parsing.
    (void)d::isValidHostHeader(s);
    (void)d::httpVisitSemicolonParameters(s, [](std::string_view, std::string_view) { return true; });
}
}  // namespace

int main(int argc, char** argv) {
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 200000;
    static constexpr char pool[] =
        "%\\\"\\u0123456789abcdefABCDEF.-+eE&=/,;:{}[]@ \t\r\n"
        "Content-Disposition:form-data;name=\"filename\"boundary=multipart/"
        "\x00\x7f\x80\xff";
    // Each input lives in an exact-size heap allocation (no NUL terminator, no
    // spare capacity) so a sanitizer's redzone catches any read one past the end.
    for (long i = 0; i < iterations; ++i) {
        const auto len = static_cast<std::size_t>(next() % 128);
        auto buf = std::make_unique<char[]>(len);
        for (std::size_t j = 0; j < len; ++j) buf[j] = pool[next() % (sizeof(pool) - 1)];
        const auto blen = static_cast<std::size_t>(1 + next() % 6);
        auto boundary = std::make_unique<char[]>(blen);
        for (std::size_t j = 0; j < blen; ++j) boundary[j] = pool[next() % (sizeof(pool) - 1)];
        exercise(std::string_view(buf.get(), len), std::string_view(boundary.get(), blen));
    }
    std::printf("fuzz_parsers ok: %ld iterations\n", iterations);
    return 0;
}
